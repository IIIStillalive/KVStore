# kv-engine

手写内存 KV 存储引擎：**跳表**有序索引（O(logN) 增删查 + 范围查询）、**WAL** 断电恢复、
**读写锁 + 批次 group commit** 并发。教学性质，面试直通 Redis ZSet / AOF / 缓存穿透三考点。

## 设计总览

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

### ① 排序索引 — SkipList
- header-only 模板，`maxlevel=32, p=1/4`（对齐 Redis ZSet，内存/速度折中）。
- `updatePrev()` 核心辅助：从 header 顶层向下逐层下沉，记录每层待插/删位置的前驱。
- 覆盖式 `insert`（同 key 覆 value）、`find` 返回 `optional<V>` 拷贝、`erase`、
  闭区间 `range(lo,hi)`。
- 复杂度：查 / 删 **O(logN)**，范围查询 **O(logN + k)**。

### ② 持久化 — WAL（Write-Ahead Log）
- 记录布局（小端）：`[op:1][keyLen:4][key][valLen:4][val][crc32:4]`；
  CRC 覆盖整条 record，保证**坏尾可检**。
- 写路径固定顺序：**先 append 日志，后 apply 内存**（AOF/命令重放式）。
- 恢复语义（本引擎场景二）：`open` 每次全量重放 WAL 重建索引；`close` 保留日志
  （写 9 字节 EOF 标记 0xFF），崩溃恢复时坏尾自动物理截断、EOF 后跳过继续。
- fsync 三态（Redis AOF 同款）：`Always` 每条落盘 / `EverySec` 每秒批量 /
  `No` 交 OS + 手动 `flush()`。

### ③ 并发 — 两锁 group commit
- **批锁 `mutex_`**：`wal_.append`（fwrite 页缓存）+ 写请求入 `writequeue` + leader 选举。
- **内存锁 `rwmutex_`**：跳表读 `shared` / 写 `unique`；读并发，写互斥。
- 写路径三段：
  1. `[段1｜mutex_]` append → 请求入队；
  2. `[段2｜离锁]`   leader 对整批只调一次 `wal_.flush()`（fsync 摊到整批）；
  3. `[段3｜mutex_]` 锁内反复 drain 队列到 **空** → notify → `worked=false` 下轮才可竞选。
- **关键不变量：队列空 ⟺ 无等待者。** 等待者的 `reg` 恒留在队列里，故 flush 窗口内
  进场者要么被当前 leader 认领、要么在 `worked=false` 后自晋升——无「无人晋升」死锁。
- **锁序单向 `mutex_ → rwmutex_`**，读者只持 `rwmutex_` 从不反向拿 `mutex_`
  → 无 AB-BA 死锁；`Wal::file_` 由 Wal 内部 `file_mu_` 串行，两层不嵌套。
- **公开接口绝不返回内部引用**：`get` 返回 `optional<string>` 拷贝、`range` 返回 `vector` 拷贝。

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

**Bench**（吞吐 + O(logN) 标度验证）：

```bash
cmake -S . -B build -DKV_BUILD_BENCH=ON && cmake --build build -j
./build/kv_bench        # 默认测 N = 1e3..1e7 共 5 档
```

## Benchmark 结果（WSL2，执行 `./build/kv_bench 7`）[^1]

### ① SkipList 纯内存吞吐（p=1/4, maxlevel=32；ns/op）

| N      | insert/op | find/op | hotfind/op | erase/op |
|--------|-----------|---------|------------|----------|
| 1e3    | 149.5     | 56.1    | 8.5        | 64.6     |
| 1e4    | 141.2     | 107.9   | 9.7        | 61.4     |
| 1e5    | 181.1     | 344.3   | 12.4       | 71.5     |
| 1e6    | 187.0     | 880.7   | 18.7       | 63.1     |
| 1e7    | 209.2     | 1770.0  | 20.1       | 61.5     |

判据核验：
- **insert / erase 基本持平**（1e3→1e7 仅 ~1.4× / ~1.0×，数据量涨了 1 万倍）→ **O(logN)**。
  若层高有 bug 退化成单层链表，顺序插入会变 O(N²)，每档涨幅 ~10×，对照一目了然。
- **find 涨 ~31× / 4 档 ≈ 2.4×/档（次线性）**：随机键查找的涨幅来自**指针追逐的缓存未命中**
  （跳表节点 malloc 随机散布），不是算法复杂度——O(N) 退化会每 10×N 涨 ~10×。
- **hotfind 基本持平**（8.5→20.1）反证了上面的判断：缓存友好时跨档只涨 2×。

### ② 范围查询 [lo, lo+s]（N=1e5，ns/op）

| 跨度 s   | ns/op    |
|----------|----------|
| 1        | 805.8    |
| 10       | 1733.8   |
| 100      | 4392.8   |
| 1000     | 24157.1  |
| 10000    | 233734.3 |

- s 每 10 倍 → ns/op ≈ **1~2×（小跨度，定位 O(logN) 常数项 ~0.8µs 主导）→ ~5.5× → ~9.7×（大跨度，O(k) 主导）**
  ——整条曲线单调向上并收敛到线性，符合 **O(logN + k)**。
- 注：lo 每轮伪随机变化，避免编译器把 range 提升出循环（hoisting）；s=100/1000 曾因采样
  不足出现平台（s=100≈1000 均 ~20µs），加大采样后消除——是测量伪影，非算法问题。

### ③ KvStore put 吞吐（SyncMode::EverySec，leader 每批 flush、外围定时器未接线）

| 线程数 | ops/s | us/op |
|--------|-------|-------|
| 1      | 807   | 1238.5 |
| 4      | 1469  | 680.8  |

- 4 线程 ≈ **1.8× 单线程吞吐**：批锁 + leader 离锁刷盘把 fsync 从「每 op 一次」摊到「每批一次」，
  多线程下磁盘 I/O 不再逐条卡在锁上 → **group commit 生效**。
- 如实标注：当前实现（plan「待办」遗留）leader 每批都调 `wal_.flush()`，单线程下每批≈1 条
  ~~故单线程等价每次 put 一次真 fsync~~，数字偏保守；定时器接线后每线程吞吐会上一个量级。

[^1]: `kv_bench` 输出三张表（跳表标度、范围跨度、并发吞吐），核对判据：
N 每 10 倍，跳表 ns/op 最多涨 2~3 倍（O(logN)），而不是 10 倍（退化单链 bug）；
kv 4 线程吞吐应显著高于 1 线程（group commit 摊薄 fsync）。

## 面试穿透点

### 1. 跳表为什么 O(logN)？Redis 为什么选跳表不选红黑树 / B+树？
- **期望层高**：每节点以 p=1/4 向上晋升，期望层高 O(1) 空间；从顶层下沉，每层
  `updatePrev` 前进的步数期望 O(logN) → 查/删平均 O(logN)。
- **B+树**：为磁盘外存设计（高扇出、一页多键、顺序 IO），内存里无磁盘优势，实现复杂。
- **红黑树**：内存均衡 O(logN)，但区间查询要先 O(logN) 定位再中序遍历；无法便捷 rank /
  按区间枚举。
- **跳表**：区间查询天然（底层有序链顺序扫）、可加 span 字段做 O(logN) rank、p 可调、
  bug 好排查（无旋转），Redis 综合「实现简单 + ZRANGE 支持 + 可扩展」胜出。

### 2. WAL 与 Redis AOF 的对比
- 同：都是「写先落盘、后生效」的 append-only 恢复日志。
- Redis AOF：记录写命令本身，恢复时重放命令；`appendfsync` 三态即上文
  （always / everysec / no）；AOF rewrite 压缩是配套手段。
- 本引擎：record 用「长度前缀 + CRC」而非命令文本——key 可含二进制，无解析歧义，
  CRC 校验坏尾可安全截断。「先日志后内存」「长度前缀防半条」「CRC 断坏尾」三条是命门。

### 3. 缓存穿透防护（本引擎可作延伸点）
1. **参数校验 / 布隆过滤器**：不存在的 key 先在引擎外挡掉 → 可加 `bloom_exists(key)` 前置过滤。
2. **空值缓存**：把「key 不存在」也缓存片刻，防穿透打穿下游 DB。
3. **互斥锁单飞**：同一 key 缺失只允许一个线程查下游，其余等缓存回填 —— 可用细粒度锁投影。

### 4. 并发难点（面试可讲）
- Redis 本身单线程，靠 IO 多路复用规避锁；本引擎为学习跳表并发用读写锁做正确性入门。
- group commit：把每 op 一次的 fsync 摊到整批 —— 批锁 + leader 离锁刷盘，
  **fsync 永不背在 KvStore 锁上**（读吞吐不被磁盘拖死）。
- 尾部孤儿死锁（曾实测复现）：flush 窗口内进场者无人晋升 → 「队列不空不放手」解法，
  不变量 `队列空 ⟺ 无等待者` 是正确性核心。

## 边界 / 明确不做
- 不做 AOF rewrite 压缩、多文件多段日志、事务级原子性 / 隔离级别。
- 不做 lock-free / 每节点细化（列为"未来可扩展"）。
- 不做磁盘索引（内存优先；磁盘 B+树是另一门功课）。
- key / value 均为 `std::string`（Skiplist 模板内部通用，门面定 string）。

设计与工作契约详见 `plan/task_plan_skipkv.md`；README 起草、评审与排障由 Claude 负责，
算法主体由我手写（教学项目）。