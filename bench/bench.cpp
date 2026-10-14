// 基准（plan §8 Day4）：吞吐 + 验证 O(logN)。
//
// 三组测量：
//   ① SkipList 纯内存：insert / find / erase 多档 N，验证 ns/op ≈ 常数。
//     判据：N 每 10 倍，ns/op 最多涨 ~2-3 倍（log 增长极缓）；
//           若退化为单层链表（层高 bug），10 倍 N 时 ns/op 也 ~10 倍，对比一目了然。
//   ② 范围查询：不同跨度 k 下的 ns/op，验证 O(logN + k)。
//   ③ KvStore 门面：1 线程 vs 4 线程 put 吞吐 —— 展示 批锁 + group commit
//     摊薄 fsync（WAL 真落盘从每 op 一次 → 每批一次）与 内存锁 rwmutex_ 的读并发。
//
// 构建：cmake -S . -B build -DKV_BUILD_BENCH=ON && cmake --build build
// 运行：kv_bench [最大指数档]   （默认 3→4→5→6→7，即 N=1e3..1e7）
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kv/kv_store.hpp"
#include "kv/skiplist.hpp"

using kv::KvStore;
using kv::SkipList;
using Clock = std::chrono::steady_clock;

// 跑一遍 f，返回平均每 op 的纳秒数。f 内部重复 ops 次。
template <typename F>
double ns_per_op(F&& f, int64_t ops) {
  auto t0 = Clock::now();
  f();
  auto t1 = Clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(ops);
}

// sink：全局可观测累加器 —— 防编译器把被测循环体整个优化掉（DCE）。
static volatile size_t g_sink = 0;

// ---------- ① 纯内存跳表：insert / find / erase 标度 ----------
static void bench_skiplist(std::vector<int64_t>& ns) {
  std::puts("\n===== ① SkipList 纯内存吞吐（p=1/4, maxlevel=32）=====");
  std::printf("%-8s %11s %11s %11s %11s\n", "N", "insert/op", "find/op", "hotfind/op", "erase/op");
  std::printf("%-8s %11s %11s %11s %11s\n", "------", "----------", "---------", "-----------", "--------");

  for (int64_t N : ns) {
    SkipList<int64_t, int64_t> sl;
    // insert：顺序键 0..N-1（有 bug 退化单链时最坏 O(N²)，标度差距最大）
    double ins = ns_per_op([&] {
      for (int64_t k = 1; k <= N; ++k) sl.insert(k, k);
    }, N);

    // find：均匀随机键 —— 一半命中一半未命中（uniform 0..2N）。
    // ⚠️ 随 N 涨但次线性（~2-3×/档）≠ 退化（O(N) 会 ~10×/档）：
    //    指针追逐的缓存失效主导（跳表节点随机散布），比较次数本身是 O(logN)。
    std::mt19937 rng(42);
    std::uniform_int_distribution<int64_t> dist(0, 2 * N);
    double fnd = ns_per_op([&] {
      for (int64_t i = 0; i < N; ++i) { volatile auto v = sl.find(dist(rng)); (void)v; }
    }, N);

    // hotfind：固定 8 个键反复查（缓存友好），但键选取依赖运行时 i，编译器
    //   无法 hoist/常量折叠。跨档基本持平 → find 的涨幅来自缓存而非算法复杂度。
    int64_t hot_keys[8] = {7, 385, 65537 % N, N / 3, N / 2, N - 2, 9999 % N, 1234567 % N};
    int64_t hot_reps = 200000;                  // 固定迭代——每档同等有效
    double hot = ns_per_op([&] {
      int64_t hits = 0;
      for (int64_t i = 0; i < hot_reps; ++i) {
        auto v = sl.find(hot_keys[(i * 2654435761ULL) >> 60 & 7]);  // i 依赖 → 不可 cache-fold
        hits += v.has_value();
      }
      g_sink += hits;
    }, hot_reps);

    // erase：顺序删 0..N-1，等待 find 之后重建索引
    double del = ns_per_op([&] {
      for (int64_t k = 1; k <= N; ++k) sl.erase(k);
    }, N);

    std::printf("%-8s %11.1f %11.1f %11.1f %11.1f\n",
                (std::to_string(N)).c_str(), ins, fnd, hot, del);
  }
  // 判据自动提示：
  if (ns.size() >= 2) {
    std::puts("判据：insert/erase 顺序键应持平（O(logN)）；find 随机键可涨但次线性");
    std::puts("      （O(N) 退化会每 10×N 涨 ~10×）；hotfind 缓存友好应各档基本持平。");
  }
}

// ---------- ② 范围查询：O(logN + k) ----------
static void bench_range(int64_t N) {
  std::puts("\n===== ② 范围查询 [lo, lo+s]（N= 固定；采样 20 万/档，大跨度收紧）=====");
  SkipList<int64_t, int64_t> sl;
  for (int64_t k = 1; k <= N; ++k) sl.insert(k, k);

  const int64_t spans[] = {1, 10, 100, 1000, 10000};
  std::printf("%-10s %12s %14s\n", "跨度(s)", "ns/op", "每档扫描元素");
  for (int64_t s : spans) {
    // 采样数固定 20 万（足够稳）；大跨度收紧到 ≥2000 次，避免跑太长。
    // ⚠️ 曾用 reps = 2e6/(s+1)：小跨度只分到 ~2 万次采样且 s=10000 仅 199 次，
    //    TLB/分支冷启动主导 per-op 平均 → s=100≈s=1000、s=10000 逐轮乱跳，全为采样伪影。
    int64_t reps = (s <= 1024) ? 200000 : std::max<int64_t>(2000, 2000000 / (s + 1));
    double t = ns_per_op([&] {
      std::vector<std::pair<int64_t, int64_t>> out;
      out.reserve(s + 1);             // 顶格容量：clear 后复用，不让扩容进计时
      // lo 随 r 伪随机变化 → 每次查询输入不同，range() 无法被编译器 hoist 出循环
      const int64_t stride = 2654435761ULL;
      for (int64_t r = 0; r < reps; ++r) {
        out.clear();
        int64_t lo = static_cast<int64_t>((r * stride) >> 3) % (N - s);
        sl.range(lo, lo + s, out);    // 定位 O(logN) + 扫描 O(s) 都算进本轮
        // 反 DCE：只 XOR 末尾一个结果（O(1)），不把整个 out 的遍历成本带进计时
        g_sink ^= static_cast<size_t>(out.back().first);
      }
    }, reps);
    std::printf("%-10lld %12.1f %13lld\n",
                static_cast<long long>(s), t,
                static_cast<long long>(reps * (s + 1)));
  }
  std::printf("判据：ns/op 应随 s 涨（O(k) 主导），s 每 10 倍 → ns/op ≈ 10 倍内；定位 O(logN) 是常数项。\n");
}

// ---------- ③ KvStore 门面：1 vs 4 线程 put 吞吐（group commit 摊 fsync）----------
// ⚠️ 当前实现特性（如实展示）：KvStore::put 的 leader 每批必调 wal_.flush()，
//   SyncMode 的 EverySec 外围定时器尚未接线（plan「待办」遗留）——因此单线程下
//   每次 put 都是一次真 fsync；多线程下 leader 一次 flush 覆盖整批，fsync 次数
//   从「≈请求数」降到「≈批数」，正好量化 group commit 摊薄 fsync 的效果。
static void bench_kvstore_puts(int64_t per_thread, int nthreads) {
  const std::string wal_path = "bench_runtime.wal";
  std::remove(wal_path.c_str());  // 清理上次残留（.gitignore 已含 *.wal）

  KvStore store;
  if (!store.open(wal_path, kv::Wal::SyncMode::EverySec)) {
    std::fprintf(stderr, "kvstore open failed\n");
    std::abort();
  }

  auto worker = [&](int tid) {
    // 每线程写独占 key 段，避免跨线程覆盖同一 key（测吞吐，不测冲突语义）
    int64_t base = static_cast<int64_t>(tid) * per_thread;
    for (int64_t i = 0; i < per_thread; ++i)
      store.put("k" + std::to_string(base + i), "v" + std::to_string(i));
  };

  int64_t total = static_cast<int64_t>(nthreads) * per_thread;
  auto t = [&] {
    auto t0 = Clock::now();
    if (nthreads == 1) {
      worker(0);
    } else {
      std::vector<std::thread> th;
      for (int i = 0; i < nthreads; ++i) th.emplace_back(worker, i);
      for (auto& e : th) e.join();
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  }();

  store.close();
  std::remove(wal_path.c_str());
  std::printf("%12d %14.0f %12.1f\n",
              nthreads, total / t * 1e3 /* ops/s */, t * 1e6 / total /* us/op */);
}

static void bench_kvstore() {
  // 每线程 5k 条就够（当前单线程每次 put=一次 fsync，太大要跑十几分钟）
  const int64_t per_thread = 5000;
  std::puts("\n===== ③ KvStore put 吞吐（EverySec；当前 leader 每批 flush、定时器未接线）=====");
  std::printf("%12s %14s %12s\n", "线程数", "ops/s", "us/op");
  bench_kvstore_puts(per_thread, 1);
  bench_kvstore_puts(per_thread, 4);
  std::puts("对比看点：4 线程吞吐显著高于 1 线程 = group commit 把 fsync 摊到整批；");
  std::puts("         （单线程几乎每次 put 一次 fsync，是磁盘 I/O 主导的死慢）");
}

// ---------- 入口 ----------
int main(int argc, char** argv) {
  // 默认档位：N = 1e3, 1e4, 1e5, 1e6, 1e7；可用参数指定最大指数，如 kv_bench 6
  std::vector<int64_t> ns;
  int64_t max_exp = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 7;
  for (int64_t e = 3; e <= max_exp; ++e) ns.push_back(static_cast<int64_t>(std::pow(10.0, e)));

  std::puts("kv-engine bench (Day4)");
  bench_skiplist(ns);
  bench_range(static_cast<int64_t>(1e5));
  bench_kvstore();
  std::puts("\ndone.");
  return 0;
}
