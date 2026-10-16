// 返回值语义（plan 下一步#2 兑现）：交接棒组提交下 put=true 新增 / del=true 确有移除。
// 单线程顺序调用时每请求自成一批（队列空才放领导权），返回值应与 skiplist 真实结果一致；
// 多线程下同一性由测试钩子 flush_count 与 test_concurrency 金标准覆盖。
#include <cstdio>
#include <string>

#include "kv/kv_store.hpp"
#include "test_util.h"

using kv::KvStore;
using kv::Wal;

int main() {
  const std::string path = "applied_test.wal";
  std::remove(path.c_str());

  KvStore s;
  CHECK(s.open(path, Wal::SyncMode::No));

  // put：true=新增，false=覆盖已有 key
  CHECK(s.put("k1", "v1"));       // 首次 → 新增
  CHECK(!s.put("k1", "v2"));      // 再写 → 覆盖（值应已更新）
  auto got = s.get("k1");
  CHECK(got.has_value());
  CHECK_EQ(*got, "v2");

  // del：true=确有移除，false=key 已不存在
  CHECK(s.del("k1"));             // 存在 → 移除
  CHECK(!s.del("k1"));            // 已无 → false
  CHECK(!s.get("k1").has_value());

  // 同键 put→del 连续写，最终态与语义一致（跨多批）
  CHECK(s.put("k2", "v2"));
  CHECK(s.del("k2"));
  CHECK(!s.get("k2").has_value());

  s.close();
  std::puts("test_applied: OK");
  return 0;
}