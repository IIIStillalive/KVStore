#pragma once

// ============================================================================
// KvStore — 门面：跳表索引 + WAL + 并发。设计见 plan §4/§5。
//
// 并发设计（已定案 · 两锁 group commit）：【批锁 mutex_ + 内存锁 rwmutex_】
//   mutex_   批锁：wal_.append（编码+fwrite 页缓存）+ writequeue + leader 选举
//   rwmutex_ 内存锁：skiplist_ 读 shared / 写 unique
//   put/del（写路径）：
//     [段1｜mutex_]  wal_.append（编码 + fwrite 页缓存）→ 请求入 writequeue
//     [段2｜离锁]    leader 对整批只调 wal_.flush()（真落盘；a′ 方案：不批 write 只批 fsync）
//     [段3｜mutex_]  锁内反复 drain 队列到空 + 条件二次 flush（兜底 flush 窗口内散员）
//                    → 各请求 is_done=true notify → worked=false（队列空才放手）
//     apply：leader/follower 各自把本记录 insert/erase 进 skiplist_（持 rwmutex_ unique）
//   get / range / size / empty → rwmutex_ shared（读可并发；返回拷贝，绝不带引用出锁）
//
// ⚠️ 关键约定：
//   1. 公开方法绝不返回内部引用——get 返回 optional<string> 拷贝、range 返回 vector 拷贝。
//   2. fsync 永不背在 KvStore 锁上：leader 离锁调 wal_.flush()。
//   3. 锁序单向 mutex_ → rwmutex_，读者只持 rwmutex_ 从不反向拿 mutex_ → 无 AB-BA 死锁。
//   4. Wal::file_ 由 Wal 内部 file_mu_ 串行化（append 的 fwrite 与 leader/close 的 flush 互斥），
//      KvStore 层无需再加 WAL 专属锁，也不会两层锁嵌套死锁（Wal 永不反向持 KvStore 锁）。
// ============================================================================

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
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
    bool put(const std::string& key, const std::string& val);  // 覆盖式；true=新增
    std::optional<std::string> get(const std::string& key) const;
    bool del(const std::string& key);
    size_t range(const std::string& lo, const std::string& hi,
                 std::vector<std::pair<std::string, std::string>>& out) const;
    bool   empty() const noexcept;
    size_t size() const noexcept;
    void   flush();                   // 手动 fsync（SyncMode::No 用）

private:
    // put/del 契约（two-phase group commit）见头注。
    // 一次写请求的等待条目：reg 入 writequeue 并与调用线程绑定，
    // leader 刷盘后逐条置 is_done + notify，调用线程据此从 cv 唤醒。
    struct WriteRequest{
        std::condition_variable cv_;  //信号量
        bool success = {false};
        bool is_done{false};  //标志位
        explicit WriteRequest(){}  //使用move效率更高
    };

    SkipList<std::string, std::string> skiplist_;
    Wal    wal_;
    mutable std::shared_mutex rwmutex_;
    Wal::SyncMode mode_ = Wal::SyncMode::EverySec;  // open 记下；驱动批边界/超时策略
    bool opened_ = false;  //这个应该是磁盘文件是否存在，不同于Wal的日志文件
    std::mutex mutex_;   //信号量保护锁

    std::queue<std::shared_ptr<struct WriteRequest>> writequeue;  //需要进行io的队列

    bool worked = false;  //哪个线程先抢到哪个就是leader，这也代表fsync是否空闲
};

}  // namespace kv
