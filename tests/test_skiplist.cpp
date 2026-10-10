// 结构单测 —— 跳表正确性（plan §6.1）。
// 当前为「红」：skiplist.hpp 里算法是 stub，逐个解锁后转绿。
#include <string>
#include <vector>

#include "kv/skiplist.hpp"
#include "test_util.h"

using kv::SkipList;

int main() {
  SkipList<int, std::string> sl(1u << 16);

  // 1) 插入后 get 命中 / 未命中
  CHECK(sl.insert(3, "three"));
  CHECK(sl.insert(1, "one"));
  CHECK(sl.insert(2, "two"));
  CHECK_EQ(sl.size(), 3u);
  CHECK(sl.find(2).has_value());
  CHECK_EQ(*sl.find(2), "two");
  CHECK(!sl.find(9).has_value());     // 未命中
  CHECK(!sl.empty());

  // 2) 重复 key → 覆盖 value，且返回 false
  CHECK(!sl.insert(2, "TWO"));
  CHECK_EQ(sl.size(), 3u);            // 覆盖不新增
  CHECK_EQ(*sl.find(2), "TWO");

  // 3) 范围查询闭区间 [lo,hi]（含边界、有序）
  std::vector<std::pair<int, std::string>> out;
  CHECK_EQ(sl.range(2, 3, out), 2u);
  CHECK_EQ(out[0].first, 2);
  CHECK_EQ(out[1].first, 3);

  // 4) 删除：存在则移除、size 减一；不存在返回 false
  CHECK(sl.erase(1));
  CHECK(!sl.find(1).has_value());
  CHECK(!sl.erase(1));                 // 已删除不再有
  CHECK_EQ(sl.size(), 2u);

  // 5) 结构不变量：底层链随 key 严格有序（覆盖插入多条后仍成立）
  out.clear();
  sl.range(-1e9, 1e9, out);           // 全量
  for (size_t i = 1; i < out.size(); ++i)
    CHECK(out[i - 1].first < out[i].first);

  std::puts("test_skiplist: OK");
  return 0;
}