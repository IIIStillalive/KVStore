// 并发混跑（plan §6.4）：多线程 put/get/del，-fsanitize=thread 下无数据竞争。
// 测试方向：每个线程认领若干 key 自写自读（避免断言竞态），结束后
// 再用 单线程顺序重放 对每个线程的写入做最终一致性校验。
// 配置：cmake -DKV_ENABLE_TSAN=ON ..  再跑  ctest -R test_concurrency
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "kv/kv_store.hpp"
#include "test_util.h"

int main() {
  const int kThreads = 4;
  const int kPerThread = 5000;

  kv::KvStore store;
  CHECK(store.open("concurrency_test.wal", kv::Wal::SyncMode::No));

  std::vector<std::thread> pool;
  std::atomic<bool> stop{false};
  for (int t = 0; t < kThreads; ++t) {
    pool.emplace_back([&, t] {
      // 每线程只碰自己的 key 空间，线程内自写自读（无跨线程断言竞态）
      for (int i = 0; i < kPerThread; ++i) {
        std::string k = "t" + std::to_string(t) + "_" + std::to_string(i);
        store.put(k, k);              // 写
        auto got = store.get(k);
        if (got) CHECK_EQ(*got, k);   // 读自证一致
        if (i % 2) store.del(k);      // 偶发删除
      }
    });
  }
  for (auto& th : pool) th.join();

  // 最终一致性：若某 key 没被删，应能读到原值；被删则读不到。
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      std::string k = "t" + std::to_string(t) + "_" + std::to_string(i);
      auto got = store.get(k);
      if (i % 2) {
        CHECK(!got.has_value());      // 偶数索引被删
      } else {
        CHECK(got.has_value());
        CHECK_EQ(*got, k);
      }
    }
  }

  store.close();
  std::puts("test_concurrency: OK");
  return 0;
}