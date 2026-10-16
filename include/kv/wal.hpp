#pragma once
// ============================================================================
// 层② Wal — 写前日志（WAL）
// 【最终规范 v1.0 · 2026-10-08】
// 持久化语义（定案）：【场景二 WAL-only】——close 保留日志、open 每次全量重放。
// 正确性命门 = CRC + 坏尾处理(F9)；EOF 收尾标记 = 完整性信号（非正确性命门）。
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <mutex>
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

    explicit Wal(std::string path, SyncMode mode = SyncMode::EverySec);
    Wal() = default;                 // 未 open：file_=nullptr；路径交给 open()（KvStore 默认构造用）
    ~Wal();
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    bool open(const std::string& path);          // 打开/创建日志文件；失败返 false
    void append(OP op, const std::string& key,   // 追加一条（只写 OS 页缓存/流缓冲，
                const std::string& value, std::string& output_buff);       // 落盘时机由 SyncMode 决定）
    std::vector<LogRecord> recover();            // 重放全部记录交上层；坏尾(长度/Crc)
                                                 // 截断并停止；含 EOF 收尾标记处理
    void close();                                // flush + 写 EOF 收尾标记（不删日志）
    bool flush();                                // 手动 fsync：fflush+fsync（EverySec 由外部计时器/队刷调）
    bool flush_to_os();                          // 仅 fflush：C 缓冲 → OS 页缓存，不 fsync；
                                                 //   EverySec leader 每批返回前调（磁盘 fsync 交定时器）
    bool is_open() const noexcept;
    size_t file_size() const noexcept;           // 观测 log 增长用
    size_t flush_count() const noexcept;         // 测试/诊断钩子：fsync 成功次数（三态判据）
    size_t to_os_count() const noexcept;         // 测试/诊断钩子：flush_to_os 成功次数（EverySec 判据）

private:
    bool CRC32(const LogRecord&) const;          // 校验单条整记录
    bool flush_locked();                         // flush 的锁内实现（file_mu_ 已持有）

    // 布局单一来源 encode()：[op][keyLen][key][valLen][val]（不含 crc）。
    // append 写本布局 + 末尾 crc、recover 按此解码、CRC32 按此重序列化——禁止各自手拼。
    std::string encode(OP op, const std::string& key,
                       const std::string& value) const;

    // 完整磁盘记录 = encode() + [crc:4]：
    //   [op:1][keyLen:4][key][valueLen:4][value][crc:4]
    // 重放读到长度越界或 CRC 不符 → 判尾部半条，丢弃并停（F9）。
    // 字段宽/字节序：op 1 字节、长度 4 字节小端（src/wal.cpp read_le32/append_le32）；
    // CRC 计算用 src/wal.cpp 匿名命名空间的 crc32(std::string)（查表，初值/收尾
    // 0xFFFFFFFF 函数内自含，喂入 encode() 产出的完整字节串即可）。
    // EOF 收尾标记（close 写、recover 跳过）：固定 9 字节 [0xFF][keyLen:4=0][valLen:4=0]，
    // 无 crc——完整性信号，非正确性命门；其后可能是后续会话追加的新记录，须跳过继续。
    SyncMode mode_ = SyncMode::EverySec;  // fsync 时机：Always=append 内即 flush；EverySec/No=外部计时器/队刷调 flush()
    size_t flush_success_ = 0;             // flush_locked 成功自增（file_mu_ 保护）；fsync 三态判据
    size_t flush_to_os_count_ = 0;       // flush_to_os 成功自增（file_mu_ 保护）；EverySec 每批到 OS 判据
    std::string path_;           // 日志路径（open 记下；recover 独立按它重放）
    std::FILE* file_ = nullptr;  // 写句柄（fopen "ab" 二进制追加）：append 直写、close 写 EOF 收尾标记
    // file_mu_ 串行化对 file_ 的一切访问（append 的 fwrite、close/flush 的 fflush+fsync+fclose）。
    // 必需：上层 group-commit 的 leader 会在 KvStore 锁外调 flush()，而并发 put 的 append 在
    // 锁内 fwrite——两者对同一 FILE* 必须互斥（C stdio 句柄非线程安全）。
    mutable std::mutex file_mu_;
};


}  // namespace kv
