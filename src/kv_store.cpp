// KvStore 实现 —— 用户手写。接口见 include/kv/kv_store.hpp，设计见 plan §4/§5。
#include "kv/kv_store.hpp"

namespace kv {

KvStore::KvStore() : index_(kExpectedEntries, 0.5f) {}

KvStore::~KvStore() {
    close();
}

bool KvStore::open(const std::string& walPath, Wal::SyncMode mode) {
    // TODO(用户): 1) 打开/创建 WAL；2) 若已有日志则 replay 叠出索引；
    //   3) use_wal_ 置真。
    (void)walPath; (void)mode;
    return false;
}

void KvStore::close() {
    // TODO(用户): 若 use_wal_：刷盘 → 干净关闭时截断/清理日志。纯内存则无需保留。
}

bool KvStore::put(const std::string& k, const std::string& v) {
    // TODO(用户) 顺序：先 append 落盘(WAL)，再锁内 apply 到内存索引。
    //   equivalent_shared_lock 场景下两条 branch，注意锁内不做磁盘 IO；
    //   正确做法：WAL append（锁外/独立临界）→ unique_lock 内改跳表。
    (void)k; (void)v;
    return false;
}

std::optional<std::string> KvStore::get(const std::string& k) {
    // TODO(用户): shared_lock 读，返回 index_.find(k) 拷贝。
    //   ⚠️ 绝不把内部引用带出锁区。
    (void)k;
    return std::nullopt;
}

bool KvStore::del(const std::string& k) {
    // TODO(用户): 先 WAL append DEL，再 unique_lock 内 erase。
    (void)k;
    return false;
}

size_t KvStore::range(const std::string& lo, const std::string& hi,
                      std::vector<std::pair<std::string, std::string>>& out) {
    // TODO(用户): shared_lock 读，返回 index_.range(lo, hi, out)。
    (void)lo; (void)hi; (void)out;
    return 0;
}

size_t KvStore::size() const {
    // TODO(用户): shared_lock 读，返回 index_.size()。
    return 0;
}

void KvStore::flush() {
    // TODO(用户): 调 wal_.flush()。
}

}  // namespace kv