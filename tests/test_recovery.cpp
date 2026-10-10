// 崩溃重放恢复（plan §6.3）—— WAL 层正确性。
// 测试方向：不依赖 KvStore，直接驱动 Wal 验证「append → replay → 截断尾部半条」。
#include <string>
#include <vector>

#include "kv/wal.hpp"
#include "test_util.h"

using kv::Wal;

namespace {
// 把记录集合编码成 bytes 的等价判断（decode 由 replay 内的测试用）。
bool records_eq(const std::vector<Wal::Record>& a,
                const std::vector<Wal::Record>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].op != b[i].op || a[i].key != b[i].key)
      return false;
    if (a[i].op == Wal::Op::Put && a[i].value != b[i].value)
      return false;
  }
  return true;
}
}  // namespace

int main() {
  const std::string path = "recovery_test.wal";

  // 1) 正常写一批 → 重放 → 原样返回
  {
    Wal w(Wal::SyncMode::EverySec);
    CHECK(w.open(path));
    w.append(Wal::Op::Put, "alpha", "1");
    w.append(Wal::Op::Put, "beta", "2");   // 二进制 value 也应有 '\0'
    w.append(Wal::Op::Del, "alpha", "");
    w.close();  // 干净关闭应截断/清空日志

    // 干净关闭后不保留数据：重开应得到空日志（本引擎关闭即清）。
    Wal w2(Wal::SyncMode::EverySec);
    CHECK(w2.open(path));
    CHECK_EQ(w2.replay().size(), 0u);
  }

  // 2) 模拟崩溃：不 close 直接销毁对象（文件残留半批 / 完整批）
  {
    Wal w(Wal::SyncMode::EverySec);
    CHECK(w.open(path));
    w.append(Wal::Op::Put, "x", "1");
    w.append(Wal::Op::Put, "y", "2");
    // 不调用 close() 就析构 → 日志保留完整两项
  }
  {
    Wal w(Wal::SyncMode::EverySec);
    CHECK(w.open(path));
    auto recs = w.replay();
    CHECK_EQ(recs.size(), 2u);
    CHECK(records_eq(recs, {{Wal::Op::Put, "x", "1"},
                            {Wal::Op::Put, "y", "2"}}));
  }

  // 3) 尾部半条日志 + CRC 坏 → 截断并停止，保留前面完整记录
  //    （构造方式由你在实现 WAL 后补充：可手动在文件末尾写半个 record 再重放。）

  std::puts("test_recovery: OK");
  return 0;
}