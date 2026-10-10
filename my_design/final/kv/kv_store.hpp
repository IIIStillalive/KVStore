#pragma once
// ============================================================================
// 层③ KvStore — 门面：组合 跳表 + WAL + 读写锁并发
// 【最终规范 v1.0 · 2026-10-08】
// 持久化（定案）：open 每次全量重放重建内存；put 用 【两段锁 + 批次水位线】
// 的 group commit：append(锁内)→ leader 离锁 write+fsync 整批 → 再取锁 apply 内存。
// 关键次序：insert 内存必须在 fsync 之后（WAL 写前原则，读者永不见未落盘值）。
// ============================================================================
#include <optional>
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

    // 打开 + 全量重放恢复；失败返 false（别在构造里恢复，报错无门）。
    bool open(Wal::SyncMode mode, const std::string& path);
    void close();
    bool is_open() const noexcept;

    // ---- 对外 API ----
    bool put(const std::string& key, const std::string& val);  // 覆盖式；true=新增
    std::optional<std::string> get(const std::string& key) const;
    bool del(const std::string& key);
    size_t range(const std::string& lo, const std::string& hi,
                 std::vector<std::pair<std::string,std::string>>& out) const;
    bool   empty() const noexcept;
    size_t size() const noexcept;

private:
    // put 契约（two-phase group commit）：
    //   [段1｜unique_lock]  wal_.append(op,key,val)；记下本批水位线 → 解锁
    //   [离锁]             leader 对整批到水位线 write(flush)+fsync；成功即永不再丢
    //   [段2｜unique_lock]  把本批 insert/erase apply 进 skiplist_（insert 必须在 fsync 后）
    //   cv 唤醒本批全部等待者（队列自身独立 mutex+cv，与 rwmutex_ 分开）
    // batch 边界 = 数量达上限 或 超时；fsync 期间新 append 归下一批。
    // 有界延迟：Always 下单个 put 最坏等一整个批超时才返回（延迟上界=超时）。

    // 并发契约：
    //   get/range/size/empty → shared_lock(rwmutex_)：只读共享，返回值拷贝，锁内不带引用出。
    //   put/del/close        → unique_lock：写独占。
    //   fsync/磁盘 IO 永不背锁。

    SkipList<std::string,std::string> skiplist_;
    Wal wal_;
    mutable std::shared_mutex rwmutex_;   // 保护内存索引（读写）
    bool opened_ = false;
    // TODO(实现)：group-commit 批队列(mutex+cv)、水位线、等待者注册/唤醒。
};

}  // namespace kv