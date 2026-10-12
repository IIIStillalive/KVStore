// 接口 + 随机对照 std::map —— 跳表正确性的金标准（plan §6.2）。
// 这是「测试方向」：正确实现完成后此测试为强验证，未通过即算法有 bug。
//
// 用 SkipList 而非 KvStore，是为了先隔离验证索引层（不牵扯 WAL/锁）。
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "kv/skiplist.hpp"
#include "test_util.h"

using kv::SkipList;

int main() {
  // 随机序列种子固定 → 可复现；换个种子跑也能过才说明算法自身正确。
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> op(0, 2);   // 0=put 1=del 2=get
  std::uniform_int_distribution<int> key(-1000, 1000);
  std::uniform_int_distribution<int> val(0, 999);

  SkipList<int, int> sl;
  std::map<int, int> ref;  // 对照基准

  // 每个 op 同步作用于 sl 与 ref，断言结果完全一致。
  for (int i = 0; i < 100000; ++i) {
    int k = key(rng);
    switch (op(rng)) {
      case 0: {  // put（覆盖式语义）
        int v = val(rng);
        sl.insert(k, v);
        ref[k] = v;
        break;
      }
      case 1: {  // del
        bool a = sl.erase(k);
        bool b = ref.erase(k) > 0;
        CHECK_EQ(a, b);
        break;
      }
      default: {  // get
        auto a = sl.find(k);
        auto it = ref.find(k);
        CHECK_EQ(a.has_value(), it != ref.end());
        if (a.has_value()) CHECK_EQ(*a, it->second);
        break;
      }
    }
    CHECK_EQ(sl.size(), ref.size());
  }

  // 全程结束后再全量比对一遍（覆盖 put 的返回语义不逐条校，最终一致性保证）。
  std::vector<std::pair<int, int>> out;
  sl.range(-2000000000, 2000000000, out);
  CHECK_EQ(out.size(), ref.size());
  size_t j = 0;
  for (auto it = ref.begin(); it != ref.end(); ++it, ++j) {
    CHECK_EQ(out[j].first, it->first);
    CHECK_EQ(out[j].second, it->second);
  }

  std::puts("test_kv: OK");
  return 0;
}