// fsync 三态行为判据 —— 断言「fsync / flush_to_os 发生次数」（行为），不依赖墙钟测耗时（磁盘速度无关）。
// 目标语义（对齐 Redis appendfsync）：
//   Always   = 每批返回前 fsync（此处单线程每 put 一组批 → flush_count == put 数）
//   No       = 无自动 fsync，仅手动 flush()（flush_count == 手动次数；leader 也不到 OS）
//   EverySec = flush ≤ 1 次/s（put 后立即 0 → 睡 1.2s → ≥1）；但每批返回前 fflush 到 OS
//              （to_os_count == 批数），磁盘 fsync 交 1s 定时器
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "kv/kv_store.hpp"
#include "test_util.h"

using kv::KvStore;
using kv::Wal;

int main() {
  const std::string path = "sync_mode_test.wal";
  std::remove(path.c_str());

  // 1) Always：N 次 put → 每批（此处单线程即每 put）都 fsync → flush_count == 100。
  {
    KvStore s;
    CHECK(s.open(path, Wal::SyncMode::Always));
    for (int i = 0; i < 100; ++i) {
      const std::string k = "k" + std::to_string(i);
      CHECK(s.put(k, k));
    }
    CHECK_EQ(s.flush_count(), 100u);   // 单线程顺序 put = 每 put 一组批 → 100 次 fsync
    CHECK(s.flush_count() >= 1u);      // 语义判据：每批返回前至少 fsync 一次
    s.close();
  }

  // 2) No：N 次 put → 0 次 fsync；手动 flush() 一次 → 1 次。
  {
    KvStore s;
    CHECK(s.open(path, Wal::SyncMode::No));
    for (int i = 0; i < 100; ++i) {
      const std::string k = "n" + std::to_string(i);
      CHECK(s.put(k, k));
    }
    CHECK_EQ(s.flush_count(), 0u);     // 无 leader 刷盘、无定时器、无手动调用
    CHECK_EQ(s.to_os_count(), 0u);     // No：leader 连到 OS 都跳过（C 缓冲留待手动 flush）
    s.flush();
    CHECK_EQ(s.flush_count(), 1u);
    s.close();
  }

  // 3) EverySec：put 后立即 0 次 fsync（1s 定时器尚未到点，100 put 远快于 1s）
  //    → 睡 1.2s（跨过一次 tick）→ ≥1。只能断言「≥1」：tick 计数不确定，断言 ==1 是
  //      实现耦合。
  {
    KvStore s;
    CHECK(s.open(path, Wal::SyncMode::EverySec));
    for (int i = 0; i < 100; ++i) {
      const std::string k = "e" + std::to_string(i);
      CHECK(s.put(k, k));
    }
    CHECK_EQ(s.flush_count(), 0u);     // 定时器未到点（put 远快于 1s 间隔）
    CHECK(s.to_os_count() >= 100u);    // 每批返回前 fflush 到 OS（100 put = 100 批）→ 到 OS 判据
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK(s.flush_count() >= 1u);      // 到过一次点 → 至少一次 fsync
    s.close();                         // join 定时器，防测试进程悬挂
  }

  std::puts("test_sync_mode: OK");
  return 0;
}