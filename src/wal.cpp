// ============================================================================
// 层② WAL 实现 —— 接口见 include/kv/wal.hpp；规范见 my_design/final/kv/wal.hpp。
// 持久化语义（定案 · 场景二）：close 保留日志；open 每次全量重放。
// ============================================================================
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
// 注：勿 #include <filesystem>——MinGW g++ 8.1 的该头文件自身编译不过（fs_path.h operator!=）。
//     file_size() 已用 ifstream 实现，无需它。
#ifdef _WIN32
#include <io.h>    // _fileno / _commit（真落盘）
#else
#include <unistd.h>  // fileno / fsync（真落盘）
#endif

#include "kv/wal.hpp"
namespace kv {

namespace {

// 将 uint32_t 以小端字节序追加到 out。
// ⚠️ append / recover / CRC32 必须共用同一条「字节序 + 字段宽」规则。
void append_le32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

// CRC32（IEEE 802.3 / zlib 同款，反射多项式 0xEDB88320）：查表，整条喂入。
// 初值/收尾 0xFFFFFFFF 在此函数内自含（zlib 标准实现方式），
// append 对 encode() 字节串算、recover 同样对拼回的完整字节算 —— CRC 只有这一份代码。
uint32_t crc32(const std::string& data) {
    static const auto table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data)
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// 与 append_le32 配对的小端解码：读 4 字节为 uint32_t。字段宽/字节序规则与写必须一致。
uint32_t read_le32(const std::string& s, size_t off) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 3])) << 24);
            //两次转换是为了去掉char类型的符号
            //为什么是uint_8，是因为取的正好是一个字节8位
}

// 真落盘：fflush 只到 OS 页缓存，再经原生接口刷到磁盘。
//   POSIX  fsync(fileno)；Windows  _commit(_fileno)。
int fsync_native(std::FILE* f) {
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return ::fsync(::fileno(f));
#endif
}

// EOF 收尾标记的 op 字节：完整标记为 [0xFF][keyLen:4=0][valLen:4=0] 共 9 字节
// （close 写、recover 跳过；完整性信号，非正确性命门）。
constexpr uint8_t kEofOp = 0xFFu;

// 布局长度常数（与 encode() 一一对应）：
//   HDR        = [op:1][keyLen:4][valLen:4] = 9 —— 仅 EOF 标记（两长度皆 0）独占全长；
//   DATA_OFFSET = [op:1][keyLen:4] = 5 —— 真实记录 key 首字节相对记录起点的偏移
//                （valLen 字段在 key 之后，不在固定头部）。
constexpr size_t HDR         = 1 + 4 + 4;   //EOF长度
constexpr size_t DATA_OFFSET = 1 + 4;

}  // namespace

Wal::Wal(std::string path, SyncMode mode) :  mode_(mode), path_(path){
    // 句柄先置空（file_ 成员默认），随后 open() 真正打开/创建日志。

    //初始化就要查看文件是否有赶紧退出标志
    open(path_);
}

Wal::~Wal() {
    // close 语义由调用方主导：析构只释放写句柄（fclose 把 C 缓冲刷到 OS 页缓存，
    // 但不 fsync、不写 EOF 标记）。「不 close 就销毁」即模拟崩溃，已写字节进程内仍可重放。
    if (file_) {
        std::lock_guard<std::mutex> g(file_mu_);
        if (file_) { std::fclose(file_); file_ = nullptr; }
    }
}

bool Wal::open(const std::string& path) {
    // 以二进制「追加写」方式打开/创建日志并留存句柄 file_；失败返 false。
    // 重开前先收旧句柄，避免同对象二次 open 时文件共享冲突。
    if (file_) { std::fclose(file_); file_ = nullptr; }
    path_ = path;
    file_ = std::fopen(path.c_str(), "ab");
    if (!file_) {
        std::cerr << "open failed: " << path << "\n";
        return false;
    }
    return true;
}

void Wal::append(OP op, const std::string& key, const std::string& value, std::string& output_buff) {
    // 编码 [op:1][keyLen:4][key][valueLen:4][value][crc:4]：crc 为 encode 字节串的 CRC32，
    // 一律 append_le32 小端（recover/CRC32 同口径，勿用宿主机字节序 reinterprit_cast）。
    // ⚠️ 用 key.size()/value.size() 写长度，勿 strlen（二进制 value 可含 '\0'）。
    //   同名字节 ① 进调用方 output_buff（暂存/队刷缓冲）② 立即 fwrite 上 file_（OS 页缓存）；
    //   真落盘时机由 SyncMode 决定（Always=本函数内即 flush；EverySec/No=外部 leader/manual flush()）。
    //   ⚠️ 文件以二进制追加模式打开，写偏移自动在末尾。
    //   file_mu_：append 的 fwrite 与 并发 flush()（上层组提交 leader 在锁外调）互斥于同一 FILE*。
    std::string data = encode(op, key, value);
    const uint32_t crc_32 = crc32(data);
    output_buff.append(data);
    append_le32(output_buff, crc_32);

    std::lock_guard<std::mutex> g(file_mu_);
    if (file_) {                              //磁盘侧：与 output_buff 完全相同的两段字节
        std::fwrite(data.data(), 1, data.size(), file_);
        std::string crcenc;
        append_le32(crcenc, crc_32);
        std::fwrite(crcenc.data(), 1, crcenc.size(), file_);
    }
    if (mode_ == SyncMode::Always) flush_locked();   // 每条即刻落盘；EverySec/No 由外部计时/队刷调 flush()
}

std::vector<Wal::LogRecord> Wal::recover() {
    // 场景二：open 每次全量重放。坏尾（读不齐头/长度越界/未知 op/CRC 不符）→
    // 裁掉 good_end 之后的内容并停（F9）；EOF 收尾标记只是信号，跳过继续。
    // 按 path_ 独立读，不依赖 open() 的句柄状态；需裁剪时用同路径 trunc 重写前缀。
    std::vector<LogRecord> out;

    if (file_) std::fflush(file_);             // 先写后读同对象：自家 C 缓冲推到 OS，防遮蔽
    std::ifstream f(path_, std::ios::binary);
    if (!f) return out;                          // 文件不存在/打不开 → 无可重放

    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    f.close();

    size_t pos = 0;
    size_t good_end = 0;                         // 最后一个完整记录/EOF 标记的字节终点
    bool   need_truncate = false;

    while (pos < buf.size()) {
        // 1) 头 9 字节读不齐 → 尾部半条，截断停（F9）
        if (pos + HDR > buf.size()) { need_truncate = true; break; }
        const uint8_t op = static_cast<uint8_t>(buf[pos]);

        // 2) EOF 收尾标记：仅凭 op 判据（无 key/val 字段），跳过继续（非命门；其后可追加）
        if (op == kEofOp) { pos += HDR; good_end = pos; continue; }

        // 3) op 只允许 Put/Del，别的一律当坏尾
        if (op != static_cast<uint8_t>(OP::Put) &&
            op != static_cast<uint8_t>(OP::Del)) { need_truncate = true; break; }

        // 4) 布局 [op:1][keyLen:4][key][valLen:4][val][crc:4]（小端）：依序推导各字段位置，
        //    valLen 在 key 之后、crc 在 val 之后——禁止再按固定 HDR 掐头（会错位）
        const uint32_t keyLen  = read_le32(buf, pos + 1);
        const size_t   key_pos = pos + DATA_OFFSET;   // key 首字节
        const size_t   vlen_pos = key_pos + keyLen;   // valLen 字段
        if (vlen_pos + 4 > buf.size()) { need_truncate = true; break; }   // valLen 读不齐 → 坏尾
        const uint32_t valLen = read_le32(buf, vlen_pos);
        const size_t   val_pos = vlen_pos + 4;        // val 首字节
        const size_t   crc_pos = val_pos + valLen;    // crc 字段
        if (crc_pos + 4 > buf.size()) { need_truncate = true; break; }    // 整条读不齐 → 坏尾
        const size_t total = crc_pos + 4 - pos;

        // 5) CRC 校验：覆盖 [op..val] 全部字节，与 append 的 crc32(encode(...)) 同口径
        const uint32_t stored = read_le32(buf, crc_pos);
        if (stored != crc32(buf.substr(pos, crc_pos - pos))) {
            need_truncate = true;                    // 坏尾，截断停（F9）
            break;
        }

        // 6) 拼出记录，checknum/key_size/val_size 如实填回
        LogRecord r;
        r.op       = (op == static_cast<uint8_t>(OP::Put)) ? OP::Put : OP::Del;
        r.key_size = keyLen;
        r.val_size = valLen;
        r.key.assign(buf, key_pos, keyLen);
        r.val.assign(buf, val_pos, valLen);  //使用std::string::assign高效，不会创建临时的string副本
        r.checknum = stored;
        out.push_back(std::move(r));

        pos += total;
        good_end = pos;
    }

    // 7) 坏尾物理裁剪；干净文件不动（保留末尾 EOF 标记）
    if (need_truncate && good_end < buf.size()) {
        std::ofstream wf(path_, std::ios::binary | std::ios::trunc);
        if (wf) wf.write(buf.data(), static_cast<std::streamsize>(good_end));
    }
    return out;
}

void Wal::close() {
    // 场景二：flush → 写 EOF 收尾标记（完整性信号，非正确性命门）→ 再 flush → 关句柄。
    // 日志保留不删；后续会话再 open 时新记录追加在标记之后（recover 跳过标记继续）。
    if (!file_) return;
    std::lock_guard<std::mutex> g(file_mu_);
    flush_locked();
    const char marker[9] = { static_cast<char>(kEofOp), 0, 0, 0, 0, 0, 0, 0, 0 };
    std::fwrite(marker, 1, sizeof marker, file_);
    flush_locked();                            // 标记也要真落盘
    std::fclose(file_);
    file_ = nullptr;
}

bool Wal::flush() {
    // 真落盘两段式：fflush（C 缓冲 → OS 页缓存）→ fsync_native（OS 页缓存 → 磁盘）。
    // Linux fsync(fileno)；Windows _commit(_fileno)。
    std::lock_guard<std::mutex> g(file_mu_);
    return flush_locked();
}

bool Wal::flush_locked() {
    // 承调用方已持 file_mu_（append 的 Always、close、flush 共同路径）。
    if (!file_) return false;
    return std::fflush(file_) == 0 && fsync_native(file_) == 0;
}

bool Wal::is_open() const noexcept {
    // 句柄是否已 open。
    return file_ != nullptr;
}

size_t Wal::file_size() const noexcept {
    // 当前日志字节数（观测增长用）。
    //c++17方法
    //std::uintmax_t size = std::filesystem::file_size(path_);
    //c++stream
    std::ifstream f(path_, std::ios::binary);
    if(!f) return -1;
    f.seekg(0, std::ios::end);
    std::streampos size = f.tellg();
    return static_cast<size_t>(static_cast<std::streamoff>(size));
}

std::string Wal::encode(OP op, const std::string& key,
                        const std::string& value) const {
    // 单条字节布局（不含 CRC 字段）：[op:1][keyLen:4][key][valLen:4][val]
    // ⚠️ 布局单一来源：append 写、recover 读、CRC32 校三方共用，禁止各自手拼。
    std::string bytes;
    bytes.push_back(static_cast<char>(op));
    append_le32(bytes, static_cast<uint32_t>(key.size()));
    bytes += key;
    append_le32(bytes, static_cast<uint32_t>(value.size()));
    bytes += value;
    return bytes;
}

bool Wal::CRC32(const LogRecord& rec) const {
    // 按 encode() 重序列化（不含 crc 字段），匿名命名空间 crc32 整条重算并与 checknum 比对。
    return crc32(encode(rec.op, rec.key, rec.val)) == rec.checknum;
}

}  // namespace kv
