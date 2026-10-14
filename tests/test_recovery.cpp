// 崩溃重放恢复 —— WAL 层正确性（plan §6.3）。
// 【final 场景二 · 已定案】：close 保留日志、open 每次全量重放 ——
// 干净重启数据必须存活（存储引擎语义），坏尾/CRC 才是正确性命门。
// 测试方向：不依赖 KvStore，直接驱动 Wal 验证「append → recover → 截断尾部半条」。
#include <cstdio>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "kv/wal.hpp"
#include "test_util.h"

using kv::Wal;

namespace {

// 比对 op/key/val 三要素（checknum/key_size/val_size 由 recover 如实填回，不在断言内）。
bool check_records(
    const std::vector<Wal::LogRecord>& got,
    std::initializer_list<std::tuple<Wal::OP, const char*, const char*>> exp) {
  if (got.size() != exp.size()) return false;
  size_t i = 0;
  for (const auto& e : exp) {
    const Wal::LogRecord& r = got[i++];
    if (r.op != std::get<0>(e)) return false;
    if (r.key != std::get<1>(e)) return false;
    if (r.val != std::get<2>(e)) return false;
  }
  return true;
}

// 在已存在的日志末尾追加任意字节（模拟崩溃留下的半条/垃圾尾巴）。
void trash_tail(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::app);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main() {
  const std::string path = "recovery_test.wal";
  std::remove(path.c_str());

  // 1) 场景二：写一批 → close（保留日志）→ 重开 → 全量重放（数据跨干净重启存活）
  {
    Wal w(path, Wal::SyncMode::EverySec);
    CHECK(w.open(path));
    std::string buff;                              // output_buff 暂存/队刷缓冲：append 同一字节也直写句柄
    w.append(Wal::OP::Put, "alpha", "1", buff);
    w.append(Wal::OP::Put, "beta", "2", buff);     // 二进制 value 也应原样
    w.append(Wal::OP::Del, "alpha", "", buff);
    w.close();

    Wal w2(path, Wal::SyncMode::EverySec);
    CHECK(w2.open(path));
    auto recs = w2.recover();
    CHECK_EQ(recs.size(), 3u);
    CHECK(check_records(recs, {{Wal::OP::Put, "alpha", "1"},
                               {Wal::OP::Put, "beta", "2"},
                               {Wal::OP::Del, "alpha", ""}}));
  }

  // 2) 模拟崩溃：不 close 直接销毁对象 → 日志保留最近完整批；重开全量重放
  std::remove(path.c_str());
  {
    Wal w(path, Wal::SyncMode::EverySec);
    CHECK(w.open(path));
    std::string buff;
    w.append(Wal::OP::Put, "x", "1", buff);
    w.append(Wal::OP::Put, "y", "2", buff);
  }
  {
    Wal w(path, Wal::SyncMode::EverySec);
    CHECK(w.open(path));
    auto recs = w.recover();
    CHECK_EQ(recs.size(), 2u);
    CHECK(check_records(recs, {{Wal::OP::Put, "x", "1"},
                               {Wal::OP::Put, "y", "2"}}));
  }

  // 3) 尾部半条：完整批后追加垃圾字节（长度解码不过）→ recover 截断，只回 2 条
  trash_tail(path, std::string("\x01\x02\x03", 3));
  {
    Wal w(path, Wal::SyncMode::EverySec);
    CHECK(w.open(path));
    CHECK_EQ(w.recover().size(), 2u);
  }

  std::puts("test_recovery: OK");
  return 0;
}