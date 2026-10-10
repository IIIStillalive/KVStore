#pragma once

// ============================================================================
// Wal — 写前日志（WAL）。接口 + 记录布局 + stub，实现由用户手写（plan §3）。
//
// 写路径顺序：先 append 落盘（按 SyncMode 决定 fsync 时机），再 apply 到内存索引。
// 恢复：open → 清内存 → 重放日志叠出索引；删除也重放（保证幂等恢复）。
// 本引擎纯内存：clean close 时截断/删除日志；崩溃恢复才需要重放。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace kv {

class Wal {
public:
    enum class Op : uint8_t { Put = 1, Del = 2 };

    struct Record {
        Op op;
        std::string key;
        std::string value;   // Del 时为空
    };

    // 对应 Redis AOF 三态：Always / EverySec / No
    enum class SyncMode { Always, EverySec, No };

    explicit Wal(SyncMode mode = SyncMode::EverySec);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    bool open(const std::string& path);
    void close();       // 干净关闭：截断/删除日志（无需恢复则清掉）
    void append(Op op, const std::string& key, const std::string& value);
    std::vector<Record> replay();  // 顺序重放；尾部半条 + CRC 坏 → 截断并停止重放
    void flush();                  // 手动 fsync（SyncMode::No 用）
    bool is_open() const noexcept;
    size_t file_size() const noexcept;  // 占用字节（观察 log 增长用）

private:
    // 单条记录编码约定（长度前缀 + 4 字节 CRC），重放靠它判尾部半条：
    //   [op:1][keyLen:4][key][valueLen:4][value][crc:4]
    // 崩溃偏在写半条 / CRC 对不上 → 视为尾部半条，丢弃并停重放（恢复命门）。
    // TODO(用户): 在此加私有辅助 —— raw 落盘、CRC 计算、SyncMode 落地时机。

    SyncMode mode_;
    std::string path_;
    // TODO(用户): 文件句柄 / std::ofstream 与必要的状态字段。
};

}  // namespace kv