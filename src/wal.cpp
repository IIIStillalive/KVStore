// WAL 实现 —— 用户手写。接口见 include/kv/wal.hpp，设计见 plan §3。
#include "kv/wal.hpp"

namespace kv {

Wal::Wal(SyncMode mode) : mode_(mode) {}

Wal::~Wal() {
    // TODO(用户): 若文件仍打开，关掉句柄。
}

bool Wal::open(const std::string& path) {
    // TODO(用户): 打开/创建日志文件（二进制），记入 path_ 与句柄。
    (void)path;
    return false;
}

void Wal::close() {
    // TODO(用户): flush 后干净关闭；按规则截断/清空日志（本引擎纯内存，无需保留）。
}

void Wal::append(Op op, const std::string& key, const std::string& value) {
    // TODO(用户): 按 [op:1][keyLen:4][key][valueLen:4][value][crc:4] 编码顺序追加；
    //   按 SyncMode 决定是否 fsync（Always 每条刷盘 / EverySec 定秒批刷 / No 交给 flush）。
    //   ⚠️ 编码要用字符串长度而不是 strlen（二进制 value 可能含 '\0'）。
    (void)op; (void)key; (void)value;
}

std::vector<Wal::Record> Wal::replay() {
    // TODO(用户): 顺序读，逐条解码；读到长度越界 / CRC 不符 → 判尾部半条，截断并停止。
    return {};
}

void Wal::flush() {
    // TODO(用户): 手动 fsync（SyncMode::No 场景调用方主动落盘）。
}

bool Wal::is_open() const noexcept { return false; }  // TODO(用户): 用句柄状态判断

size_t Wal::file_size() const noexcept { return 0; }  // TODO(用户): 返回当前文件字节数

}  // namespace kv