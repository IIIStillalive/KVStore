#pragma once

// ============================================================================
// KvStore — 门面：跳表索引 + WAL + 并发。设计见 plan §4/§5。
//
// 并发设计（已定案 · 两锁 group commit · 交接棒）：【批锁 mutex_ + 内存锁 rwmutex_】
//   mutex_   批锁：wal_.append（编码+fwrite 进日志流）+ writequeue + 领导权（worked/promoted）
//   rwmutex_ 内存锁：skiplist_ 读 shared / 写 unique
//   put/del（写路径，统一走私有 write(op,key,val)）：
//     [段1｜mutex_]  wal_.append（编码 + fwrite 进日志流）→ 请求入 writequeue
//                    → 等待「入批(is_done) 或 被交接(promoted)」；二者都不成立 → 自己当 leader
//     [段2｜离锁]    leader 按 mode 把整批推到对应水位（a′：不批 write 只批 fsync）：
//                   Always → wal_.flush()（fflush+fsync，返回前到磁盘，窗口≈0）
//                   EverySec → wal_.flush_to_os()（fflush 到 OS 页缓存；磁盘 fsync 交 1s
//                   定时器 → 丢窗口只计磁盘段 ≤1s，Redis appendfsync everysec 同款）
//                   No → 跳过（C 缓冲留待手动 flush()，丢窗口无上限）
//                   fsync 频率 = 丢失窗口。
//     [段3]          leader 一把 rwmutex_ 写锁代 apply 全批 → 回 mutex_（同一次持锁）置
//                    is_done + 交接队列头 promoted，然后锁内敲一次共享门铃 cv_.notify_all()
//                    ——谓词已真者（本批成员 + 被交接者）拿结果离开；其余多醒者谓词为假，
//                    futex 重睡，不抢 rwmutex_，故无惊群。门铃由 KvStore 持有，永不与请求
//                    同生共死，从根上排除「销毁中唤醒」类竞争。
//   put/del 返回值（reg->applied）：本请求的增删结果——put=true 新增 / false 覆盖；
//   del=true 确有移除 / false key 不存在。reg->ok = 本批 fsync 结果（仅 Always 真判定，
//   EverySec/No=已接受进日志流=true）。get / range / size / empty → rwmutex_ shared
//   （读可并发；返回拷贝，绝不带引用出锁）
//
// ⚠️ 关键约定：
//   1. 公开方法绝不返回内部引用——get 返回 optional<string> 拷贝、range 返回 vector 拷贝。
//   2. fsync 永不背在 KvStore 锁上：leader 离锁调 wal_.flush()。
//   3. 锁序单向 mutex_ → rwmutex_，读者只持 rwmutex_ 从不反向拿 mutex_ → 无 AB-BA 死锁。
//   4. Wal::file_ 由 Wal 内部 file_mu_ 串行化（append 的 fwrite 与 leader/close 的 flush 互斥），
//      KvStore 层无需再加 WAL 专属锁，也不会两层锁嵌套死锁（Wal 永不反向持 KvStore 锁）。
//   5. 孤儿/饿死不变量：任一入队请求恒落在二态之一——被某任 leader 收进批(→is_done)，
//      或被点名交接(→promoted)。leader 任期 = 一次快照 + 一次刷盘 + 批量 apply，有界；
//      队列空才置 worked=false（接棒由队列头新 leader 继续，绝无无人认领的尾孤儿）。
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "kv/skiplist.hpp"
#include "kv/wal.hpp"
namespace kv {

class KvStore {
public:
    KvStore();                       // 初始 closed；需 open() 才可用
    ~KvStore();

    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;

    bool open(const std::string& walPath, Wal::SyncMode mode);
    void close();                     // 场景二：flush → 写 EOF 收尾标记 → 关句柄；日志保留不截断
    bool is_open() const noexcept;

    // ---- 对外 API ----
    bool put(const std::string& key, const std::string& val);  // true=新增，false=覆盖已有 key
    std::optional<std::string> get(const std::string& key) const;
    bool del(const std::string& key);                          // true=确有移除，false=key 不存在
    size_t range(const std::string& lo, const std::string& hi,
                 std::vector<std::pair<std::string, std::string>>& out) const;
    bool   empty() const noexcept;
    size_t size() const noexcept;
    void   flush();                   // 手动 fsync（SyncMode::No 用；EverySec 定时器回调）
    size_t flush_count() const noexcept;  // 测试/诊断钩子：WAL fsync 成功次数（fsync 行为判据）
    size_t to_os_count() const noexcept;  // 测试/诊断钩子：WAL flush_to_os 成功次数（EverySec 判据）

private:
    // put/del 契约（two-phase group commit · 交接棒）见头注。
    // 一次写请求的状态：reg 入 writequeue 并与调用线程绑定；leader 刷盘+代 apply 后
    //   置 is_done（本批成员被唤醒拿结果），或点名队列头 promoted（下任 leader）。
    struct WriteRequest {
        Wal::OP        op = Wal::OP::Put;  // 本请求操作（leader 代 apply 需要）
        std::string    key, val;           // 本请求载荷（同上）
        bool ok       = false;  // 本批 fsync 结果（Always 真判定；EverySec/No=已接受进日志流=true）
        bool applied  = false;  // 本请求在跳表上的增删结果（put=true 新增 / del=true 确有移除）
        bool is_done  = false;  // 已入批完成：成员总出口（等待谓词的一半）
        bool promoted = false;  // 被点名做下一任 leader：交接棒（等待谓词的另一半）
        // ⚠️ 无 cv_ 成员：条件变量是「通信基础设施」，由 KvStore 持有共享门铃 cv_（见下）。
        //    若把 cv 放进请求对象（与请求同生共死），同一地址先 destroy 后复用出新请求时，
        //    对旧 cv 的销毁写会与新 cv 的唤醒读被误判为同一对象的竞争（TSAN 实测 4 处告警
        //    全来源于此；ASan 16 万次写零 UAF 证实无真实竞争）。门铃归店所有后该窗口消失。
    };

    SkipList<std::string, std::string> skiplist_;
    Wal    wal_;
    mutable std::shared_mutex rwmutex_;
    Wal::SyncMode mode_ = Wal::SyncMode::EverySec;  // open 记下；驱动 [段2] 刷盘策略
    bool opened_ = false;  //这个应该是磁盘文件是否存在，不同于Wal的日志文件
    std::mutex mutex_;   //信号量保护锁
    std::condition_variable cv_;  // 组提交共享门铃：与 mutex_ 配对，等待谓词 = is_done || promoted。
                                  //   open 后一次构造、close（destroy）前不再有在途写者，
                                  //   生命周期覆盖所有 waiter——任何线程都不会在此事后用其 cv。

    std::queue<std::shared_ptr<struct WriteRequest>> writequeue;  //需要组提交的队列
    // EverySec 定时器：每 1s 调 flush()（覆盖 leader 跳过的 fsync，丢失窗口 ≤1s）。
    //   open(EverySec) 清零 stop_ 并启动、close() 置位 stop_ 后 join；
    //   只抢批锁不调 put/del → 锁序单向不变（无 AB-BA）。
    std::thread timer_;              // 仅 EverySec 模式启动
    std::atomic<bool> stop_{false};  // 停止信号（open 清零，close 置位后 join）

    bool worked = false;  // 领导权标记：true=有人当班（含已交接给下任），false=无人 → 自助晋升
    bool write(Wal::OP op, const std::string& key, const std::string& val);  // put/del 统一入口
};

}  // namespace kv
