#pragma once

// ============================================================================
// KvStore — 门面：跳表索引 + WAL + 读写锁并发。实现由用户手写（plan §4/§5）。
//
// 并发第一版：整表 shared_mutex。
//   get / range / size  → shared_lock（读共享）
//   put / del / flush   → unique_lock（写独占）
//
// ⚠️ 关键约定：公开方法绝不返回内部引用——get 返回 optional<string> 拷贝、
// range 返回 vector 拷贝。锁内不做 fsync / 磁盘 IO（会把吞吐打崩）。
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
    KvStore();
    ~KvStore();

    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;

    bool open(const std::string& walPath, Wal::SyncMode mode);
    void close();  // 干净关闭：截断/清 WAL

    bool put(const std::string& k, const std::string& v);
    std::optional<std::string> get(const std::string& k);
    bool del(const std::string& k);
    size_t range(const std::string& lo, const std::string& hi,
                 std::vector<std::pair<std::string, std::string>>& out);

    size_t size() const;
    void flush();  // 手动 fsync（SyncMode::No 用）

private:
    static constexpr size_t kExpectedEntries = 1u << 16;  // 索引容量估算

    SkipList<std::string, std::string> index_;
    Wal wal_;
    mutable std::shared_mutex mtx_;
    bool use_wal_ = false;
};

}  // namespace kv