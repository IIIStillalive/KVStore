// KvStore 实现 —— 接口见 include/kv/kv_store.hpp。
// 并发模型（交接棒）：批锁 mutex_ 护 WAL append + 组提交队列 + 领导权(worked/promoted)；
// 内存锁 rwmutex_ 护跳表（读 shared / 写 unique）。put/del 统一走私有 write()。
#include "kv/kv_store.hpp"
#include <chrono>
#include <mutex>
namespace kv {

KvStore::KvStore() = default;

KvStore::~KvStore() {
    close();
}

bool KvStore::open(const std::string& walPath, Wal::SyncMode mode) {
    //   1) 幂等保护：已 open 再 open → 非法（须先 close；防句柄/日志状态被覆盖）。
    //   2) mode_ 记下（驱动组提交批边界/超时策略）。
    //   3) wal_.open 打开/创建日志（"ab"，不存在则建）→ recover() 全量重放：
    //      坏尾自动物理裁剪，只返回完整记录；逐条 apply 进 skiplist_（Put→insert，Del→erase）。
    //      —— recover 是恢复的唯一调用点（Wal 层不持内存索引，见 wal.hpp 契约）。
    if (opened_) return false;                // 重复 open 拒绝，不静默覆盖

    mode_ = mode;
    if (!wal_.open(walPath)) return false;    // 打不开日志 → open 失败（不可恢复错误，直接认栽）
    opened_ = wal_.is_open();                 // 句柄态转发

    // 场景二：open 每次全量重放重建内存索引；日志保留不截断。重放顺序 = 日志顺序，天然幂等。
    for (const Wal::LogRecord& r : wal_.recover()) {
        if (r.op == Wal::OP::Put) skiplist_.insert(r.key, r.val);
        else                      skiplist_.erase(r.key);   // recover 只产 Put/Del（其它 op 已当坏尾裁）
    }
    // EverySec：启动每 1s 一次的上层 fsync（覆盖 leader 跳过的刷盘，丢失窗口 ≤1s）。
    //   定时器只抢批锁（flush() 内部）不调 put/del → 锁序单向 mutex_→rwmutex_ 不破。
    if (mode_ == Wal::SyncMode::EverySec) {
        stop_ = false;                // 重开时清零旧信号
        timer_ = std::thread([this]() {
            while (!stop_) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (stop_) break;     // close 提前给信号，不再多刷一次
                flush();
            }
        });
    }
    return opened_;
}

void KvStore::close() {
    // 幂等：非 open 直接 return。否则先停定时器再 wal_.close()
    //   （一键 flush + EOF 收尾标记，日志保留不截断！场景二）。重复 close 幂等。
    if(!opened_) return;  //幂等保护
    stop_ = true;                          // 请求定时器退出
    if (timer_.joinable()) timer_.join();  // 等它走完（可能正在 flush()），再关句柄
    wal_.close();
    opened_ = false;  //置标志位false，防止文件无法open
}

bool KvStore::is_open() const noexcept {
    return opened_;
}

bool KvStore::put(const std::string& key, const std::string& val) {
    return write(Wal::OP::Put, key, val);
}

bool KvStore::del(const std::string& key) {
    return write(Wal::OP::Del, key, "");
}

bool KvStore::write(Wal::OP op, const std::string& key, const std::string& val) {
    // 写路径（两锁 group commit · 交接棒），设计见头注。
    // 每个写者是「成员」或「leader」之一：
    //   - 成员：入队后等本批 is_done（leader 已代 apply）→ 直接拿自己的 applied 返回。
    //   - leader：自助晋升（无人在班）或被交接（promoted），任期 = 一次快照 + 一次刷盘
    //     + 批量 apply，随后置 is_done 并点名下一任 → 有界，无饿死、无尾孤儿、无惊群。
    std::string output_buf;
    auto reg = std::make_shared<WriteRequest>();
    reg->op = op;
    reg->key = key;
    reg->val = val;

    std::unique_lock<std::mutex> lock(mutex_);   // [段1｜mutex_]
    wal_.append(op, key, val, output_buf);       // 编码+fwrite 进日志流（file_mu_ 串行）
    writequeue.push(reg);                        // 请求入队（字节已进日志流，等待组提交）

    // 就位：等待「入批(is_done) 或 被交接(promoted)」，二者都不成立 → 自己是 leader。
    // ⚠️ 谓词二取一构成全集：每个入队请求要么被某任 leader 收进批(→is_done)、要么被点名
    //    交接(→promoted)——无第三类，尾部不再有孤儿；spurious 唤醒由 while 重查全条件兜住。
    // 旧设计把条件变量放进 WriteRequest 与请求同生共死 → 同一 malloc 地址先 destroy 后复用，
    // 新请求对象被误判成旧 cv 触发 TSAN 告警（ASan/引用计数证实无真实竞争）。现用 KvStore 共享门铃：
    // 谓词不变，通信对象生命周期覆盖所有 waiter。
    while (worked && !reg->is_done && !reg->promoted)
        cv_.wait(lock);
    if (reg->is_done) return reg->applied;       // 成员：leader 已代 apply，拿自己的增删结果
    if (!reg->promoted) worked = true;           // 自助晋升（无人当班）

    // ── leader 任期开始 ──
    std::vector<std::shared_ptr<WriteRequest>> batch;   // 快照：只收「此刻队列」为一批，
    while (!writequeue.empty()) {                       // 快照后的散员留给下任（绝无并入）
        batch.push_back(writequeue.front());
        writequeue.pop();
    }
    lock.unlock();                               // [段2｜离锁]
    // a′ 方案：不批 write，只批 fsync。append 已把每条记录 fwrite 进日志流（C 缓冲），
    // leader 按 mode 决定「把整批推到哪个水位」；flush 在 Wal 内部 file_mu_ 串行
    // （与并发 append 的 fwrite 互斥），可以安全地离 KvStore 锁执行——读写者不被阻塞。
    //   ⚠️ 三档水位 = 丢失窗口（fsync 频率）：
    //      Always   → wal_.flush()       （fflush+fsync，返回前到磁盘，窗口≈0）
    //      EverySec → wal_.flush_to_os()（fflush 到 OS 页缓存返回；磁盘 fsync 交 1s 定时器，
    //                                     丢窗口只计磁盘段 ≤1s —— 对齐 Redis everysec）
    //      No       → 全部跳过（C 缓冲留待手动 flush()，丢窗口无上限）
    bool ok = true;
    if (mode_ == Wal::SyncMode::Always)
        ok = wal_.flush();
    else if (mode_ == Wal::SyncMode::EverySec)
        ok = wal_.flush_to_os();

    {   // [段3] 全批一次性 apply：一把 rwmutex_ 写锁，成员不再各自抢锁插/删（无惊群）。
        //   返回 reg->applied 真值：put=skiplist 是否新增、del=是否确有移除。
        std::unique_lock<std::shared_mutex> w(rwmutex_);
        for (auto& it : batch)
            it->applied = (it->op == Wal::OP::Put)
                              ? skiplist_.insert(it->key, it->val)
                              : skiplist_.erase(it->key);
    }

    // 收尾 + 交接 + 唤醒：全部在批锁内完成。⚠️ 此处刻意「锁内敲门」——成员被唤醒后只做
    // 两件事：读 is_done 拿自己的 applied、直接返回；不再抢 rwmutex_ 各自插/删跳表
    // （旧「锁内唤醒=惊群」的批判针对的正是成员自 apply，该前提已随交接棒消除）。
    // 锁内 notify 无丢失唤醒：等待谓词（is_done/promoted）已在同一次持锁内置真；
    // 多醒者（谓词为假）醒来重查后 futex 重睡，代价一次往返，不为跳表锁争抢。
    // 敲的是共享门铃 cv_（KvStore 持有）：通信对象与请求解耦，存活期覆盖所有 waiter，
    // 不存在「notify 撞上请求销毁」的窗口（TSAN 曾 4 处 cond_destroy ∥ broadcast 告警，
    // 本质是 per-request cv 地址复用的影子误判，门铃归店后根除）。
    lock.lock();
    for (auto& it : batch) { it->ok = ok; it->is_done = true; }  // 本批成员统一出口
    if (!writequeue.empty()) {
        writequeue.front()->promoted = true;      // 交接棒：最早的散员做下一任 leader
    } else {
        worked = false;                           // 无人等待才放下领导权
    }
    cv_.notify_all();                             // 一次广播：谓词已真者离开（含被交接者）
    lock.unlock();
    return reg->applied;   // leader 自己的增删结果
}

std::optional<std::string> KvStore::get(const std::string& key) const {
    // 读 shared；返回 skiplist_.find(key) 拷贝（绝不把内部引用带出锁区）。
    {
        std::shared_lock<std::shared_mutex> lock(rwmutex_);
        return skiplist_.find(key);
    }
    
}

size_t KvStore::range(const std::string& lo, const std::string& hi,
                      std::vector<std::pair<std::string, std::string>>& out) const {
    // shared_lock；skiplist_.range(lo, hi, out)。
    {
        std::shared_lock<std::shared_mutex> lock(rwmutex_);
        return skiplist_.range(lo, hi, out);
    }
}

bool KvStore::empty() const noexcept {
    // shared_lock；返回 skiplist_.empty()。
    {
        std::shared_lock<std::shared_mutex> lock(rwmutex_);
        return skiplist_.empty();
    }
    
}

size_t KvStore::size() const noexcept {
    // shared_lock；返回 skiplist_.size()。
    {
        std::shared_lock<std::shared_mutex> lock(rwmutex_);
        return skiplist_.size();
    }
}

void KvStore::flush() {
    // 手动 fsync（SyncMode::No 用）：持批锁调 wal_.flush() ——
    //   与 put/del 的 leader 刷盘同一把锁、同一条 Wal 串行通道（file_mu_ 在 Wal 内部），
    //   批锁保证「本调用之前已 append 的全部写请求」此刻整份落盘；
    //   之后进场的新请求由各自的 leader 周期/批次刷盘覆盖，互不干扰。
    // 未 open 时 wal_ 无句柄，flush_locked() 内部安全返回 false，无需在此额外守卫。
    std::unique_lock<std::mutex> lock(mutex_);
    wal_.flush();
}

size_t KvStore::flush_count() const noexcept {
    // 测试/诊断钩子：透传 Wal 的 fsync 成功计数（线程安全在 Wal::file_mu_ 内）。
    return wal_.flush_count();
}

size_t KvStore::to_os_count() const noexcept {
    // 测试/诊断钩子：透传 Wal 的 flush_to_os 成功计数（EverySec「返回已到 OS」判据）。
    return wal_.to_os_count();
}

}  // namespace kv
