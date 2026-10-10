#pragma once
// ============================================================================
// 层② Wal — 写前日志（WAL）
// 【最终规范 v1.0 · 2026-10-08】
// 持久化语义（定案）：【场景二 WAL-only】——close 保留日志、open 每次全量重放。
// 正确性命门 = CRC + 坏尾处理(F9)；EOF 收尾标记 = 完整性信号（非正确性命门）。
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace kv {

class Wal {
public:
    enum class OP { Put = 1, Del = 2 };
    enum class SyncMode { Always, EverySec, No };

    struct LogRecord {
        uint32_t checknum;      // 本条记录 CRC（须覆盖 长度+载荷 全字节，非仅 op）
        OP       op;
        uint32_t key_size;
        uint32_t val_size;
        std::string key;        // 二进制安全：长度用 key_size，勿 strlen
        std::string val;        // Del 时为空
    };

    explicit Wal(SyncMode mode = SyncMode::EverySec);
    ~Wal();
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    bool open(const std::string& path);          // 打开/创建日志文件；失败返 false
    void append(OP op, const std::string& key,   // 追加一条（只写 OS 页缓存/流缓冲，
                const std::string& value);       // 落盘时机由 SyncMode 决定）
    std::vector<LogRecord> recover();            // 重放全部记录交上层；坏尾(长度/Crc)
                                                 // 截断并停止；含 EOF 收尾标记处理
    void close();                                // flush + 写 EOF 收尾标记（不删日志）
    void flush();                                // 手动 fsync（EverySec 由外部计时器/队刷调）
    bool is_open() const noexcept;
    size_t file_size() const noexcept;           // 观测 log 增长用

private:
    bool CRC32(const LogRecord&) const;          // 校验单条整记录

    // 单条磁盘编码（长度前缀 + CRC）：
    //   [op:1][keyLen:4][key][valueLen:4][value][crc:4]
    // 重放读到长度越界或 CRC 不符 → 判尾部半条，丢弃并停（F9）。
    // TODO(实现)：文件句柄(二进制写、顺序追加)、SyncMode 三态 fsync 时机、
    //             EOF 收尾标记的读写/识别。
    SyncMode mode_;
    std::string path_;
};

}  // namespace kv