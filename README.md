# kv-engine

手写内存 KV 存储引擎：**跳表**有序索引（O(logN) 增删查 + 范围查询）、**WAL** 断电恢复、
**shared_mutex** 并发。面试直通 Redis ZSet / AOF / 缓存穿透三考点。

## 状态
**Day1-led，正在手写核心代码。** `plan/task_plan_skipkv.md` 是设计与工作契约；
算法主体由你手写，Claude 提供接口 / 测试方向 / Reivew。

## 结构
```
include/kv/skiplist.hpp     # 跳表（header-only，TODO 实现核心逻辑）
include/kv/wal.hpp          # WAL 接口
include/kv/kv_store.hpp     # 门面：索引 + WAL + 锁
src/wal.cpp, src/kv_store.cpp
tests/                      # test_skiplist / test_kv / test_recovery / test_concurrency
bench/bench.cpp
```

## 构建 / 测试
```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
# 并发 + TSAN（Clang/GCC）：
cmake -S . -B build -DKV_ENABLE_TSAN=ON && cmake --build build -j
ctest --test-dir build -R test_concurrency
```

> README 的「面试穿透点」小节待实现完成后补全（跳表 vs 红黑树 vs B+树、
> WAL/AOF always-everysec-no、缓存穿透防护）——见 plan §7。

## 面试穿透点（详见 plan）
- 跳表为何 O(logN)、为何 Redis 选它不选红黑树/B+树。
- WAL 与 Redis AOF 三态（always/everysec/no）对比。
- 缓存穿透：布隆前滤 / 空值缓存 / 单飞互斥（本引擎可扩展点）。