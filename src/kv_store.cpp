// KvStore 实现 —— 接口见 include/kv/kv_store.hpp；设计定案见 my_design/final/kv/kv_store.hpp。
// 并发模型：批锁 mutex_ 护 WAL append + 组提交队列；内存锁 rwmutex_ 护跳表（读 shared / 写 unique）。
#include "kv/kv_store.hpp"
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
    // mode_ 记下（衡量组提交批边界/超时策略的输入，当前未参与收批决策）。
    return opened_;
}

void KvStore::close() {
    // 幂等：非 open 直接 return；否则 wal_.close()
    //   （一键 flush + EOF 收尾标记，日志保留不截断！场景二）。重复 close 幂等。
    if(!opened_) return;  //幂等保护
    wal_.close();
    opened_ = false;  //置标志位false，防止文件无法open
}

bool KvStore::is_open() const noexcept {
    return opened_;
}

bool KvStore::put(const std::string& key, const std::string& val) {
    // 写路径（a′ 只批 fsync）：
    //   当前结构：
    //   [段1｜mutex_]  wal_.append（编码+fwrite 页缓存）→ 请求入 writequeue
    //   leader：drain 整批 → 离锁 wal_.flush()（fsync，Wal 内部 file_mu_ 串行）→ 回锁 notify。
    //   follower：等自己的 reg->is_done 睡眠，唤醒后**自插自己的记录**。
    //   ⚠️ file_ 的并发安全由 Wal::file_mu_ 保证，KvStore 层不持 WAL 专属锁。
    //先日志append，然后组提交,最后insert;
    //两段锁
    std::string output_buf;
    Wal::OP op(Wal::OP::Put);


    std::unique_lock<std::mutex> lock(mutex_);  //写独占锁,加锁
    wal_.append(op, key, val, output_buf);  //这里每个线程进行了
 
    std::shared_ptr<struct WriteRequest> reg = std::make_shared<struct WriteRequest>();
    writequeue.push(reg);  //提交到队列

    if(worked){
        //如果已经有leader在工作了，说明此线程属于下一个刷新轮次，唤醒在竞争leader时刻
        reg->cv_.wait(lock, [&reg](){ return reg->is_done;});  //应该是自己的任务被完成才醒来
        {
            std::unique_lock<std::shared_mutex> lock(rwmutex_);  //读写锁中写
            skiplist_.insert(key, val);  //follower线程插入
        }

        return reg->success;  //follower的出口
    }

    worked = true;  //当下线程为leader；
    //整合queue的内容data，这也需要在锁内
    std::vector<std::shared_ptr<WriteRequest>> batch;
    while(!writequeue.empty()){
        batch.push_back(writequeue.front());
        writequeue.pop();
    }

    lock.unlock();  //解锁
    // a′ 方案：不批 write，只批 fsync。append 已把每条记录 fwrite 上页缓存，
    // leader 只需把整批 fsync 落盘；flush 在 Wal 内部 file_mu_ 串行（与并发 append
    // 的 fwrite 互斥），可以安全地离 KvStore 锁执行 —— 读写者不被 fsync 阻塞。
    bool ok = wal_.flush();  //交给wal来刷盘

    lock.lock();  //【段3·排除尾部孤儿】队列不空不放手
    // flush 窗口内（unlock→relock）进场的新请求已 push+wait，若无人晋升将永久睡。
    // 解法：把 writequeue 在锁内反复 drain 到空，才许置 worked=false——
    //   「队列空 ⟺ 无等待者」：等待者的 reg 一直留在 writequeue 里；
    //   因此 push 在 worked=true 期间的线程，要么被本循环认领（当前 leader notify），
    //   要么在 worked=false 之后才 push（自查时晋升为 leader）——无第三类，
    //   即便之后无线程进入也不会挂死。
    //   散员 fwrite 发生在本轮 flush 之后 → 认领到几个就要补刷一次覆盖其字节（WAL 写前不变量）。
    bool need_drain;
    do {
        need_drain = false;
        while (!writequeue.empty()) {           // 兜底散员：认领 flush 窗口内进场者
            batch.push_back(writequeue.front());
            writequeue.pop();
            need_drain = true;                  // 有散员 → 其字节未经本轮 fsync 覆盖
        }
        if (need_drain) {                       // 补一次性 fsync，回锁认领下一波
            lock.unlock();
            ok = wal_.flush() && ok;
            lock.lock();
        }
    } while (need_drain);

    for(auto& item : batch){
        item->success = ok;
        item->is_done = true;
        item->cv_.notify_one();  //唤醒
    }
    worked = false;  //队列已空（锁内），才进入下一个轮次

    {
        std::unique_lock<std::shared_mutex> lock(rwmutex_);
        skiplist_.insert(key, val);  //leader线程插入到跳表
    }

    return reg->success;
}

std::optional<std::string> KvStore::get(const std::string& key) const {
    // 读 shared；返回 skiplist_.find(key) 拷贝（绝不把内部引用带出锁区）。
    {
        std::shared_lock<std::shared_mutex> lock(rwmutex_);
        return skiplist_.find(key);
    }
    
}

bool KvStore::del(const std::string& key) {
    // 同 put 两段：append(Del,key,"") → 离锁 flush → rwmutex_ 内 erase；
    //   返回本批 apply 时 erase 的「确有移除」bool。
    std::string output_buf;
    Wal::OP op(Wal::OP::Del);

    std::unique_lock<std::mutex> lock(mutex_);  //写独占锁,加锁
    wal_.append(op, key, "", output_buf);  //追加日志
    std::shared_ptr<struct WriteRequest> reg = std::make_shared<struct WriteRequest>();
    writequeue.push(reg);  //提交到队列
    if(worked){
        //如果已经有leader在工作了，说明此线程属于下一个刷新轮次，唤醒在竞争leader时刻
        reg->cv_.wait(lock, [&reg](){ return reg->is_done;});  //应该是自己的任务被完成才醒来
        {
            std::unique_lock<std::shared_mutex> lock(rwmutex_);  //读写锁中写
            skiplist_.erase(key);  //follower线程删除
        }

        return reg->success;  //follower的出口
    }

    worked = true;  //当下线程为leader；
    //整合queue的内容data，这也需要在锁内
    std::vector<std::shared_ptr<WriteRequest>> batch;
    while(!writequeue.empty()){
        batch.push_back(writequeue.front());
        writequeue.pop();
    }
    lock.unlock();  //解锁
    // a′ 方案：不批 write，只批 fsync。append 已把每条记录 fwrite 上页缓存，
    // leader 只需把整批 fsync 落盘；flush 在 Wal 内部 file_mu_ 串行（与并发 append
    // 的 fwrite 互斥），可以安全地离 KvStore 锁执行 —— 读写者不被 fsync 阻塞。
    bool ok = wal_.flush();  //交给wal来刷盘

    lock.lock();  //【段3·排除尾部孤儿】队列不空不放手
    // 与 put 同款：flush 窗口内进场的散员在锁内反复 drain 到空才置 worked=false；
    // 「队列空 ⟺ 无等待者」，push 者要么被本循环认领、要么自晋升——尾部不再有孤儿。
    bool need_drain;
    do {
        need_drain = false;
        while (!writequeue.empty()) {           // 兜底散员：认领 flush 窗口内进场者
            batch.push_back(writequeue.front());
            writequeue.pop();
            need_drain = true;                  // 有散员 → 其字节未经本轮 fsync 覆盖
        }
        if (need_drain) {                       // 补一次性 fsync，回锁认领下一波
            lock.unlock();
            ok = wal_.flush() && ok;
            lock.lock();
        }
    } while (need_drain);

    for(auto& item : batch){
        item->success = ok;
        item->is_done = true;
        item->cv_.notify_one();  //唤醒
    }
    worked = false;  //队列已空（锁内），才进入下一个轮次

    {
        std::unique_lock<std::shared_mutex> lock(rwmutex_);
        skiplist_.erase(key);  //leader线程插入到跳表
    }
    return reg->success;
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

}  // namespace kv
