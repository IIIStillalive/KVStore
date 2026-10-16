# kv-engine — 内存键值存储内核

嵌入式内存 KV 存储引擎：**跳过索引列表（SkipList）** 做有序数据面，**Write-Ahead Log（WAL）** 做崩溃一致，**两锁 group commit** 做并发写入。写路径严格「先落 WAL、后 apply 内存」；断电后按 WAL 全量重放重建索引（坏尾自动检测截断）。面向**分布式存储的存储内核/数据面**定位：单机内核已具备后续接多副本复制、分段日志所需的基本组件。

## 特性亮点

- **O(logN) 有序索引**：跳过列表，`maxlevel=32, p=1/4`。增/删/查 O(logN)，闭区间范围查询 O(logN+k)。10⁷ 级数据实证标度（见 Benchmark）。
- **WAL 断电恢复**：记录 `[op | keyLen | key | valLen | val | crc32]`，长度前缀 + CRC 覆盖全记录；崩溃产生的坏尾（torn write）自动物理截断。fsync 三档策略：`Always` / `EverySec` / `No`（手动 flush），覆盖从强一致到聚合批刷的取舍。
- **两锁 group commit**：写请求在批锁内 append 入队，leader 离锁对整批只刷一次盘——fsync 从「每 op 一次」摊到「每批一次」（InnoDB / PostgreSQL group commit 同款思路）。读走 `shared_mutex` 并发执行，写互斥，锁序单向无 AB-BA。
- **可验证的数据面**：10 万随机操作对照 `std::map` 金标准全绿；WSL2 TSAN 并发测试零数据竞争；bench 标度判据核验（见下）。

## 架构

```
                ┌──────────────────────────────────────┐
    put/get/del │  KvStore（门面：锁编排 + WAL 联动）     │
    range/size  │  ┌──────────┐   ┌──────────────────┐  │
                │  │ rwmutex_ │   │ mutex_（批锁）      │  │
                │  │ 内存索引锁 │   │ wal_.append+批队列 │  │
                │  └────┬─────┘   └────────┬─────────┘  │
                └───────┼──────────────────┼────────────┘
                        │                  │
              SkipList<K,V>          Wal（append-only）
              有序索引（memory）       编码[op|len|k|v|CRC]
```

## 内核设计

### 内存索引 — SkipList
- header-only 模板；`updatePrev()` 从顶层逐层下沉，记录每层待插/删位置的前驱。
- 覆盖式 `insert` / `find` 返回 `optional<V>` 拷贝 / `erase` / 闭区间 `range(lo,hi)`。

### 持久化 — WAL
- **write-ahead ordering**：任何写操作先 append 到日志、成功后才 apply 内存索引——崩溃任意时刻重放日志即得一致态，重放天然幂等（Put/Del 覆盖）。
- **坏尾处理**：正常关闭写 9 字节 EOF 标记（0xFF）；崩溃遗留的半条记录 CRC 校验失败，恢复时物理截断，EOF 之后跳过继续。
- **fsync 策略**：`Always` 每条落盘 / `EverySec` 每秒批量 / `No` 交 OS + 手动 `flush()`；`close` 保留日志不截断（场景二语义）。

### 并发 — 两锁 group commit
- 写路径三段：
  1. `[段1｜mutex_]` append → 请求入 `writequeue`；
  2. `[段2｜离锁]`   leader 对整批只调一次 `wal_.flush()`（fsync 摊到整批，**不背在 KvStore 锁上**）；
  3. `[段3｜mutex_]` 锁内反复 drain 队列到**空** → notify → `worked=false`，下轮才可重新竞选。
- **活性保证**：不变量「队列空 ⟺ 无等待者」。等待者恒留队列，flush 窗口内进场者要么被当前 leader 认领、要么在 `worked=false` 后自晋升——消除「flush 窗口内无人晋升」的尾部孤儿等待（曾实测复现并根因定位）。
- **锁序**：单向 `mutex_ → rwmutex_`，读者只持 `rwmutex_` 从不反向 → 无 AB-BA 死锁；`Wal::file_` 由引擎内 `file_mu_` 串行，两层不嵌套。
- **API 不泄引用**：`get` 返回 `optional<string>` 拷贝、`range` 返回 `vector` 拷贝，内部索引不对外暴露。

## 构建 / 测试

```bash
cmake -S . -B build            # in-source（cmake .）会污染仓库，务必 -B build
cmake --build build -j
ctest --test-dir build --output-on-failure    # test_skiplist/test_kv/test_recovery/test_concurrency
```

**TSAN 并发验证**（MinGW 无 libtsan，规范轮次在 WSL2 原生 ext4 跑）：

```bash
cp -r /mnt/f/C++项目/kv-engine ~/kv-engine-tsan && cd ~/kv-engine-tsan
cmake -S . -B build -DKV_ENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build -R test_concurrency --output-on-failure
```

**Bench**（吞吐 + O(logN) 标度验证；`kv_bench [最大指数档]`，默认 5 档 1e3..1e7）：

```bash
cmake -S . -B build -DKV_BUILD_BENCH=ON && cmake --build build -j
./build/kv_bench
```

## Benchmark（WSL2 原生 ext4，g++ 12）

### ① SkipList 纯内存吞吐（ns/op）

| N      | insert/op | find/op | hotfind/op | erase/op |
|--------|-----------|---------|------------|----------|
| 1e3    | 149.5     | 56.1    | 8.5        | 64.6     |
| 1e4    | 141.2     | 107.9   | 9.7        | 61.4     |
| 1e5    | 181.1     | 344.3   | 12.4       | 71.5     |
| 1e6    | 187.0     | 880.7   | 18.7       | 63.1     |
| 1e7    | 209.2     | 1770.0  | 20.1       | 61.5     |

验证方式（标度判据）：数据量涨 10⁴×，insert 仅 149.5→209.2 ns（1.4×）、erase 持平——**O(logN) 实证**；对照：若层高退化单链，顺序插入将劣化为 O(N²)，每档涨幅 ~10×。随机键 find 次线性（~2.4×/档），来源为指针追逐的缓存未命中而非复杂度——同为随机键下 hotfind（固定热键）跨档持平（8.5→20.1）佐证。

### ② 范围查询 [lo, lo+s]（N=1e5，ns/op）

| 跨度 s   | ns/op    |
|----------|----------|
| 1        | 805.8    |
| 10       | 1733.8   |
| 100      | 4392.8   |
| 1000     | 24157.1  |
| 10000    | 233734.3 |

小跨度段由 O(logN) 定位常数 ~0.8µs 主导，大跨度收敛线性——**O(logN + k)** 实证，可用跨度扫描成本与真实分布对齐（本引擎 value 内联于节点，顺序扫描缓存友好）。

### ③ KvStore put 吞吐（group commit 批刷 fsync）

| 线程数 | ops/s | us/op |
|--------|-------|-------|
| 1      | 807   | 1238.5 |
| 4      | 1469  | 680.8  |

4 线程 ≈ **1.8× 单线程**：批锁 + leader 离锁刷盘把 fsync 摊到整批，磁盘 I/O 不再逐条卡在锁上。注：当前 `EverySec` 由 leader 每批 flush 实现（定时聚合待接线），数字为保守下限。

### ④ 缓存失效专项：千万级 Cache Miss 的 find 代价（string 键）

同数据、同探测的对照组——跳表冷查（随机 key）/ 热查（8 固定 key）/ 排序数组二分（cache 友好下界）。环境：Windows 10 x64、MinGW-w64 g++ 8.1.0、`-O2`（ms 绝对值与编译器/内存相关；跨档**趋势与倍率**是机器无关的结构性结论）：

| N      | 跳表 cold/op | 跳表 hot/op | range 吞吐 | 数组二分/op | 跳表/数组 |
|--------|-------------|-------------|-----------|-------------|----------|
| 1e5    | 1198 ns     | 252 ns      | 8.5 个/µs | 285 ns      | 4.2×     |
| 1e6    | 3176 ns     | 357 ns      | 4.2 个/µs | 529 ns      | 6.0×     |
| **1e7**| **5632 ns** | 401 ns      | 3.6 个/µs | 826 ns      | **6.8×** |

- **cold/op 随 10⁵→10⁷ 涨 4.7×，hot/op 几乎持平（仅 1.6×）**：数据量 100× 而热查涨幅远小于量级 → 冷查涨幅全部来自「每次指针跳转的主存往返（~80–100 ns）」，不是复杂度退化（跳表节点随机散布，找不到空间局部性）。
- **range 吞吐 8.5 → 3.6 个/µs**：底层链每前进一步即一次 cache miss 的主存往返，顺序扫描拿不到连续内存的硬件预取。
- **跳表冷查比排序数组二分慢 4.2× → 6.8×**：千万级下即便 cache 友好的数组也 miss，但连续存储把 miss 率压低一个量级——这是「有序索引 + find 高频 + 千万级」三条件齐备时**换 B 树 / 数组**的量化判据。

> WSL2 + g++ 12（与 ①~③ 同环境）复跑：1e7 档 cold 5814 ns / hot 342 ns / range 3.9 个/µs / array 1002 ns，倍率 3.7×→5.8×，跨档趋势与上表一致（cold 涨约 5×、hot 持平）。

```bash
cmake -S . -B build -DKV_BUILD_BENCH=ON && cmake --build build -j
./build/kv_bench_cache          # 默认 1e5..1e7；1e7 档需 ≥2 GB 可用内存
```

> 复现环境：WSL2 + 原生 ext4（9p 会引入文件系统层噪声），`./build/kv_bench 7`。

## 设计权衡

- **跳过列表 vs 红黑树 vs B+树**：B+树为磁盘外存（高扇出、一页多键）设计，内存中无磁盘优势且实现复杂；红黑树区间查询需定位后再中序遍历；跳表层高期望 O(1) 空间、支持按 span 做 O(logN) rank、参数可调，区间枚举天然——选作内存有序索引。
- **WAL 记录格式（长度前缀 + CRC）vs 命令文本**：长度前缀使 key/value 可含任意二进制、无解析歧义；CRC 覆盖整条记录，坏尾可安全截断。非命令重放，恢复无需编解码层。
- **组提交而非逐条 fsync**：单条 fsync 受盘 IOPS 上限约束，组提交把吞吐从「IOPS 受限」移到「批尺寸受限」（InnoDB / PostgreSQL 同思路）。

## Roadmap：迈向分布式存储

当前为单机内核（未实现以下，作为演进方向）：

1. **分段日志 + compaction**：现 WAL 无限 append，需分段 + 后台压缩（对齐 RocksDB manifest / 日志段治理）。
2. **多副本复制 / 共识**：WAL 已提供全局有序的 append-only 序列，可对接 Raft 等并发共识做日志复制落副本。
3. **网络协议层**：socket/epoll 数据面接入，对外服务化。
4. **布隆索引 / 空值缓存**：不存在键前置过滤，治理读放大的缓存穿透。

设计与评审记录：`plan/`。测试基线：`test_kv`（10 万随机 op 对照 std::map 金标准）、`test_recovery`（干净重启 / 崩溃重放 / 坏尾截断）、`test_concurrency`（4 线程混跑 + WSL2 TSAN 零竞争）。