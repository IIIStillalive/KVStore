// 基准 ④：缓存失效专项 —— 量化千万级 Cache Miss 对 find 的真实代价。
//
// 背景：跳表节点随机散布在堆上（每节点 1~2 次独立分配），find 每次下沉/前进
//      都是指针追逐；数据量越过 L2/L3 后，瓶颈从「比较次数 O(logN)」变成
//      「每次 jump 的主存往返（~80-100ns）」。本基准用同一份数据给出三组对照：
//        ① 跳表 coldfind（随机 key，命中+未命中）→ 随 N 次线性上涨 = miss 主导
//        ② 跳表 hotfind（8 个热 key）→ 各档基本持平 = 热集命中时的真实下限
//        ③ 排序数组二分（cache 友好的对照实现）→ 同数据、同探测，量化「跳表
//           比 cache 友好结构慢多少倍」→ 这是「何时该换 B 树/数组」的判据数据。
// 真实场景：string key/value（SSO 内 8 字节，无额外堆分配），非整型 key 的玩具测。
//
// 构建：cmake -S . -B build -DKV_BUILD_BENCH=ON && cmake --build build
// 运行：kv_bench_cache [最大指数档]   默认 7 → N = 1e5/1e6/1e7
//  ⚠️ 1e7 档跳表节点 ~1.3GB + 排序 keys ~0.4GB：请确认可用内存 ≥ 2GB。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "kv/skiplist.hpp"

using kv::SkipList;
using Clock = std::chrono::steady_clock;

// 反 DCE：全局累加器，防编译器把被测循环整体优化掉。
static volatile size_t g_sink = 0;

template <typename F>
double ns_per_op(F&& f, int64_t ops) {
  auto t0 = Clock::now();
  f();
  auto t1 = Clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(ops);
}

// 生成 8 字符随机 key（a-z0-9），SSO 内、无堆分配 —— 与真实短 key 内存形态一致。
static std::string rand_key(std::mt19937& rng, int n = 8) {
  static const char* alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string s;
  s.reserve(n);
  std::uniform_int_distribution<int> d(0, 35);
  for (int i = 0; i < n; ++i) s.push_back(alphabet[d(rng)]);
  return s;
}

static void bench_cache(int64_t max_exp) {
  std::puts("\n===== ④ 缓存失效专项：千万级 Cache Miss 的 find 代价 =====");
  std::printf("%-8s %12s %12s %13s %15s %10s\n",
              "N", "sl cold/op", "sl hot/op", "range 个/us", "array 二分/op", "sl/array x");
  std::printf("%-8s %12s %12s %13s %15s %10s\n",
              "------", "----------", "----------", "-----------",
              "-------------", "----------");

  for (int64_t e = 5; e <= max_exp; ++e) {
    const int64_t N = static_cast<int64_t>(std::pow(10.0, e));

    // ---- 构造数据：N 个随机 key，跳表 + 排序数组各持一份 ----
    SkipList<std::string, std::string> sl;
    std::vector<std::string> keys;
    keys.reserve(N);
    {
      std::mt19937 gen(42);
      for (int64_t i = 0; i < N; ++i) {
        std::string k = rand_key(gen);
        keys.push_back(k);
        sl.insert(k, k);            // 跳表插入（随机散布 = 判决缓存 miss 的来源）
      }
    }
    std::sort(keys.begin(), keys.end());   // 数组二分前提：有序

    // ---- 探测集：5 万命中(真实 key) + 5 万必 miss(末尾 '~' 不在 key 字符集) ----
    const int64_t PROBE = 50000;
    std::vector<std::string> hit(PROBE), miss(PROBE);
    {
      std::mt19937 rng(0xdeadbeef);
      for (int64_t i = 0; i < PROBE; ++i) {
        hit[i]   = keys[static_cast<size_t>((i * 2654435761ULL) % static_cast<uint64_t>(N))];
        miss[i]  = rand_key(rng);
        miss[i].back() = '~';              // 末尾 '~' 不出现在 key 字符集 → 必然 miss
      }
    }
    std::mt19937 probe_rng(7);             // 探测序随机化：保证任意两次访问都不相邻
    // 交替取命中/必 miss 探针，并按 probe_rng 打乱访问顺序 —— 真正「冷」。
    struct Sz { int64_t i = 0; std::mt19937* r; const std::vector<std::string>* rhs;
                const std::vector<std::string>* rms;
                const std::string& peek() {
                  return ((i++ + static_cast<int64_t>((*r)())) & 1)
                             ? (*rms)[(*r)() % 50000]
                             : (*rhs)[(*r)() % 50000];
                } };
    Sz pick; pick.r = &probe_rng; pick.rhs = &hit; pick.rms = &miss;
    auto coldf = [&] {
      int64_t cnt = 0;
      for (int64_t i = 0; i < PROBE * 2; ++i) cnt += sl.find(pick.peek()).has_value();
      g_sink += static_cast<size_t>(cnt);
    };
    double cold = ns_per_op(coldf, PROBE * 2);

    // ② hotfind：8 个热 key（从 key 集中固定挑），缓存友好 → 各档应持平。
    const int64_t hot_keys[8] = {0, N / 8, N / 4, N / 3, N / 2, 2 * N / 3, 7 * N / 8, N - 1};
    const int64_t HOT = 2000000;
    double hot = ns_per_op([&] {
      int64_t h = 0;
      for (int64_t i = 0; i < HOT; ++i) {
        auto& k = keys[hot_keys[(i * 2654435761ULL >> 24) & 7]];
        h += sl.find(k).has_value();
      }
      g_sink += static_cast<size_t>(h);
    }, HOT);

    // ③ range：区间 [keys[j], keys[j+999]]（排序数组里正好跨 1000 个节点），测吞吐。
    const int64_t SPAN = 1000, REPS = 2000;
    std::vector<std::pair<std::string, std::string>> out;
    std::mt19937 r2(99);
    double range_us = ns_per_op([&] {
      size_t total = 0;
      for (int64_t r = 0; r < REPS; ++r) {
        int64_t j = static_cast<int64_t>(r2()) % (N - SPAN);
        out.clear();
        sl.range(keys[static_cast<size_t>(j)],
                 keys[static_cast<size_t>(j + SPAN)], out);
        total += out.size();
      }
      g_sink += total;
    }, REPS);
    double throughput = (SPAN + 1) * 1000.0 / range_us;       // 个/µs

    // ①对照：排序数组二分（cache 友好的下界），同探测集。
    double bin = ns_per_op([&] {
      int64_t cnt = 0;
      for (int64_t i = 0; i < PROBE * 2; ++i)
        cnt += std::binary_search(keys.begin(), keys.end(), pick.peek());
      g_sink += static_cast<size_t>(cnt);
    }, PROBE * 2);

    std::printf("%-8s %12.1f %12.1f %13.1f %15.1f %9.1fx\n",
                (std::to_string(N)).c_str(), cold, hot, throughput, bin, cold / bin);
  }

  std::puts("判据：① sl cold/op 随 N 每 10× 次线性涨（1.5~3×）= 指针追逐 miss 主导；");
  std::puts("      ② sl hot/op 各档基本持平 = 热集命中的真实下限；");
  std::puts("      ③ sl/array 倍率 = 跳表在冷查上比 cache 友好结构慢的倍数，");
  std::puts("        ≈ 一次跳转的主存往返数。这是『有序索引 + find 高频 + 千万级』");
  std::puts("        三条件齐备时抛弃跳表换 B 树/数组的量化判据。");
}

int main(int argc, char** argv) {
  int64_t max_exp = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 7;
  std::puts("kv-engine bench (cache-miss)");
  bench_cache(max_exp);
  std::puts("\ndone.");
  return 0;
}