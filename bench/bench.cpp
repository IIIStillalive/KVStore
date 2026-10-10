// 基准（plan §8 Day4）：吞吐 + 验证 O(logN)。
// 测试方向（实现后填充）：把 N 从 1e3 → 1e7 拉几档，插入/查找耗时应近似线性增速
// 而非跳表退化（若有 bug 退化为单层链表，跨度大时慢成 O(N)，对比明显）。
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>

#include "kv/skiplist.hpp"

int main() {
  // TODO(用户): 填真实测量：多档 N，分别计时 insert 与 find，打印每档 ns/op。
  //   - insert：依次插入 0..N，记录平均耗时。
  //   - find：随机查 N 次，记录平均耗时。
  //   - 判断：随 N 每 10 倍，单次耗时近似持平（log)，而非线性恶化。
  std::puts("bench: TODO(用户) 填实现");
  return 0;
}