# SkipList 内存 KV 存储引擎 — 实现方案（独立仓库 kv-engine）

状态：`[Day1 ✅ 跳表全绿 · Day2 ✅ WAL 全绿 · Day3 ✅ 完成（Windows ×3 + WSL2 TSAN 零竞争报告）→ Day4：bench + README]`
目标：3–5 天。用户手写核心代码、边写边学；Claude 负责 分层设计 / Review / 排障。
存放：**独立 Git 仓库**已在本机 F:\C++项目\kv-engine 建好（本文件已在该仓库 `plan/`）。

---

## 进度记录（2026-10-14 更新）
- **层① 完成 ✅**：skiplist.hpp 全部算法由用户手写（randomLevel / updatePrev / insert / find / erase / range / 析构）。
  - `test_skiplist`（结构单测）→ OK；`test_kv`（10 万随机 op 对照 std::map 金标准）种子 12345 / 6174 / 987654 全绿。
  - 遍历轮次修复要点：相等 key 前进条件（advance-on-`<`）、下沉不横移、新层前驱填 `prev[k]=head_`、连接上界 `min(newlevel,currentlevel)`、析构无双重释放。
- 已提交并 push（用户操作 git）。
- 注：层高最终定案为**固定 32@p=1/4（对齐 Redis ZSet）**，覆盖了本节 §2 早期「随容量推算 maxLevel」的做法；以 `my_design/final/kv/skiplist.hpp` 为权威（此更新如上轮评审确定）。
- **层② WAL 完成 ✅**：`recover`（坏尾 CRC 截断 + EOF 标记跨会话跳过 + F9 物理裁剪）由 Claude 实现，经 5 场景 scratch 全绿；`open/append/close/flush/is_open/析构` 由 Claude 补齐写侧句柄（`FILE*`「ab」+ 真落盘 `fflush`+`fsync`/`_commit`）；`file_size` 由用户实现（ifstream seek/tell）。`test_recovery` 三块全绿（干净重启重放 / 崩溃重放 / 坏尾截断）。
  - 踩坑记录：recover 初版两个布局错位 bug（valLen 按固定偏移读、crc 偏移漏 valLen 字段位）——已改为按字段序列位置推导；`<filesystem>` 头文件在 MinGW g++8.1 自身编译不过（fs_path.h operator!=），弃用。
  - 待办（P2，可选）：append 的 output_buff → 队刷线程（condition_variable）异步落盘；SyncMode 三态的外围定时器编排在 KvStore 层接线。

### Day3（2026-10-14 主体完成 ✅）
- **接口对齐**（Claude）：`kv_store.hpp` 补 `is_open()`/`empty()`、get/range/size 改 const、删 `kExpectedEntries`；`Wal() = default` + `mode_` 类内默认；规范头 `my_design/final/kv/*.hpp` 同步。
- **两锁模型定案并实现**：`mutex_`（批锁：wal_.append + writequeue + leader 选举）+ `rwmutex_`（内存锁：读 shared / 写 unique）。写路径两段：
  - `[段1|mutex_]` append（编码+立即 fwrite）→ 入 writequeue → 查 worked；leader：drain 整批 → 离锁 `wal_.flush()`（Wal `file_mu_` 串行）→ `[段3]` 锁内反复 drain 到空 + 条件二次 flush → notify → `worked=false`；follower：cv 唤醒后自插自己的记录（rwmutex_ unique）。
  - **修复① 读写在两把锁上裸奔**：get(shared rwmutex_) vs insert(mutex_) 不互斥 → 4 处 skiplist_ 写全部包 rwmutex_ unique；锁序单向 `mutex_→rwmutex_`、读者永不反向 → 无 AB-BA。
  - **修复② 尾部孤儿死锁（实测复现过）**：leader 的 flush 窗口内进场者无人晋升→永久睡（`test_concurrency` 25s 超时挂死）。解法「**队列不空不放手**」：`worked=false` 与「锁内 drain 到空」同一临界区；等待者 reg 恒留队列 ⇒ 队列空⟺无等待者；push 者要么被当前 leader 认领、要么在 worked=false 后自查晋升 —— 无第三类，尾部无线程进入也不挂。
  - open：wal_.open + recover() 全量重放叠 skiplist_；close：wal_.close() + opened_=false。
- **验证**：`test_concurrency`（4 线程×5000 put/get/del）Windows ×3 连跑全绿；`test_skiplist/test_kv/test_recovery` 无回归。

### 下一步（Day3 收尾）
1. **WSL2 TSAN 收口 ✅**（2026-10-14 通过：`test_concurrency Passed 42.17s`，TSAN 零数据竞争报告、无挂死）。MinGW g++8.1 无 libtsan；规范轮次在 WSL2 跑（源码 `_WIN32` 分叉已含 POSIX fsync 分支，Linux 可直接编译）：
   ```bash
   # ⚠️ 踩坑 2026-10-14：勿在源码根目录 `cmake .`（in-source 会污染工作区，
   #   `.gitignore` 已挡根目录残留）。必须先 build 再 ctest——configure 只写了 Makefile。
   # ⚠️ 拷到 WSL 原生 ext4 再跑，别在 /mnt 的 9p 上做（慢且偶发怪问题）。
   cp -r /mnt/f/C++项目/kv-engine ~/kv-engine-tsan && cd ~/kv-engine-tsan
   cmake -S . -B build -DKV_ENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j$(nproc)
   ctest --test-dir build -R test_concurrency --output-on-failure
   ```
   已跑通命令本身。注意 `~/kv-engine-tsan` 是副本，若 `/mnt/f` 再有改动需重新拷/同步。
2. **put/del 返回值语义**：当前是整批 flush 成功与否（`reg->success`），接口契约「true=新增 / 确有移除」未兑现（方向：WriteRequest 携带 op/key，apply 时记录 insert/erase 真值）。
3. **close 并发接线**：当前仅 `wal_.close()`；并发下有等待者时应排空/唤醒（现测试在 join 后调 close，无并发 close，暂安全）。
4. **清理**：`test_concurrency` 关尾部 `remove("concurrency_test.wal")`（WAL 已入 `.gitignore`，可不改）；`kv_store.hpp` 头注从「单锁」更新为「批锁+内存锁」两锁模型。

### 待实现里程碑
- Day2：WAL `src/wal.cpp`（append/CRC/replay/clean-close 截断）+ `test_recovery` 转绿。
- Day3：KvStore `src/kv_store.cpp`（两锁并发）+ `test_concurrency` 转绿（✅ Windows ×3；WSL2 TSAN 收尾即完成）。
- Day4：bench + README 面试点整理 +（可选）Bloom 前置过滤。

学习契约：核心算法（跳表、WAL、并发）由用户手写实现；Claude 只给接口、测试方向、踩坑提醒、Review。

---

## 0. 项目一句话

一个内存 KV 引擎：**跳表做有序索引**（O(logN) 增删查 + 范围查询），**WAL 保断电恢复**，**读写锁控并发**。面试直通 Redis ZSet / AOF / 缓存穿透三考点。

## 1. 目录结构（新仓库）

```
kv-engine/
├── CMakeLists.txt
├── README.md                    # 设计 + 面试穿透点 + 用法
├── include/kv/
│   ├── skiplist.hpp             # 模板化跳表（核心，header-only）
│   ├── wal.hpp  ── wal.cpp      # WAL：append / replay / checksum / 截断清理
│   └── kv_store.hpp ─ kv_store.cpp # 门面：put/get/del/range + 锁 + WAL 联动
├── src/
│   ├── wal.cpp
│   └── kv_store.cpp
├── tests/
│   ├── test_skiplist.cpp        # 结构单测
│   ├── test_kv.cpp              # 接口 + 随机对照 std::map
│   ├── test_recovery.cpp        # 崩溃重放恢复
│   └── test_concurrency.cpp     # 多线程混跑 + Tsan
├── bench/bench.cpp              # 吞吐 / O(logN) 验证
├── plan/                        # 本方案 + 进度
└── deps/                        # （可选）googletest fetched，或手写断言
```

跳表模板化放 header-only（模板必须可见）；WAL / KVStore 分离 .cpp（普通类型）。

## 2. 数据结构层 — SkipList

### Node / 结构
```cpp
template<typename K, typename V>
struct SkipNode {
    K          key;
    V          value;
    int        level;            // 实际层高（1..）
    std::vector<SkipNode*> next; // next[0] 是最底有序链
};
```

### 类（header-only）
```cpp
template<typename K, typename V, typename Cmp>
class SkipList {
    // 成员
    SkipNode<K,V>* header_;   // 哨兵，level = maxLevel
    int   currentLevel_;      // 当前实际最高层（从 1 计）
    float p_         = 0.5;   // 晋升概率；Redis ZSet 用 1/4（省内存）
    int   maxLevel_;          // 由容量估算：maxLevel = ceil(log_{1/p}(n上限))
    size_t size_;
};
```
- `maxLevel` 建议按预计条目数算（如 1<<16 条目 → log2≈16 层），**不要写死 32**，让面试时能讲出「层高随容量增长的依据」。
- `randomLevel()`：1 起步，以概率 p 逐层上升，封顶 maxLevel。用 `std::mt19937`（分布稳定），别用 `rand()`。
- `updatePrev(int k)`: 核心辅助——从 header 顶层向下，逐层记录「待插入位置的前驱」数组 `prev[0..currentLevel]`。**这是跳表最容易写错的地方（每层都要正确下沉）**。

### 反同刻/等 key 语义
- 允不允许重复 key：不允许（KV 语义）。插入同 key = 覆盖 value。
- 范围查询闭区间 `[lo, hi]`：从 bottom 层 `lower_bound` 顺序扫到 `> hi`。`size_t range(lo, hi, vector<pair<K,V>>& out)`。

### 复杂度与面试穿透
- 查 / 删：平均 O(logN)，从 header 顶层下沉。
- 范围查询：O(logN + k)，k=结果数（Redis ZRANGEBYSCORE 就是这思路）。
- **为什么 Redis ZSet 选跳表而非红黑树 / B+树**（面试必背）：
  1. **B+树**为磁盘外存设计（高扇出、一页多键、顺序 IO），内存里没有磁盘访问优势，且实现复杂。
  2. **红黑树**内存均衡好、O(logN)，但**区间查询**要先 O(logN) 定位再中序遍历，且不支持便捷「给出排名 / 按区间枚举」。
  3. **跳表**：多层有序链表，结构简单、易调试、易扩展——(a) 区间查询天然（底层有序链顺序扫）；(b) 加 `span` 字段可 O(logN) 求 rank；(c) p 可调平衡内存与速度；(d) bug 好排查（相对红黑树旋转）。Redis 选择它正是在「实现简单 + 能支持 ZRANGE 系列 + 可扩展」上的综合胜出。

## 3. 持久化层 — WAL（Write-Ahead Log）

### 写路径（关键顺序：先日志，后内存；或日志先，再 apply 到内存）
```
put(k,v):  日志追加 [op=PUT][len][k][len][v]  (+ CRC)  →  fsync 策略 → 更新内存跳表
del(k):    日志追加 [op=DEL][len][k]                 →  fsync 策略 → 删除内存跳表
```
- 每条 log 记录用**长度前缀 + 4 字节 CRC**：崩溃发生在写半条时，重放读到长度不对或 CRC 失败 → 视为尾部半条，**丢弃并停**（这是恢复正确性的命门）。
- 落盘顺序：append 是顺序写，性能良好。**同步点**讲究：每条 fsync 最安全但最慢。

### fsync 策略（做成可配，学 Redis AOF 三态）
| 策略 | 行为 | 代价 / 用途 |
|---|---|---|
| Always | 每条写后 fsync | 最稳，吞吐低（面试讲：Redis always 即此）|
| EverySec | 每秒批量 fsync | 折中（Redis everysec 默认）|
| No | 交给 OS / 调用方手动 flush | 最快，靠 OS 页缓存，断电窗最长 |

- 恢复：`open(path)` → 清空内存 → 顺序重放 WAL → 建出跳表。**删除也重放**（保证幂等重放后状态与崩溃前一致）。
- WAL 无限增长问题：**clean close 时截断/删除日志**（本引擎为纯内存，干净关闭则无需恢复）；崩溃恢复无压缩 → 标注为 P2 边界（真引擎对 AOF rewrite，这里不追求）。

### 面试穿透：WAL vs Redis AOF / 刷盘对比
- 同：都是「写先落盘、后生效」的 append-only 恢复日志。
- WAL（DB 传统）：先日志后数据页，崩溃可回滚/前滚。
- Redis AOF：记录**写命令本身**，恢复时重放命令；AOF rewrite(压缩)、appendfsync(always/everysec/no) 三态即上文。
- 本引擎取的是「AOF 命令重放」的简化为「record 重放」：key 若为二进制，用长度前缀而非命令文本，天然无解析歧义。

## 4. 并发层 — 读写锁 / 批次 group commit

**【已定案 2026-10-13】：两段锁 + 批次水位线 group commit**（而非下述简单整表锁；作为 Day3 主线）。要点：
- 写路径三段：`[段1|批锁内] wal.append + 入批登记水位线` → `[离锁] leader 对整批 fsync` → `[段2|rwmutex_ unique] 整批 apply 进跳表` → cv 唤醒本批等待者。读者(shared_lock)永不见未落盘值。
- `Wal::file_` 非线程安全：append 的 fwrite 与 flush 的 fsync 必须串行同一把批锁；批锁与内存锁(rwmutex_)不嵌套 → 无 AB-BA 死锁。
- 批边界 = 条数上限 或 超时（按 SyncMode 编排）；延迟上界 = 一批超时。
- 简单整表 shared_mutex 仍可作为上手第一版实现，功能等价（组提交只是把 fsync 摊到整批）。

### 进阶（P1，三/四天内可选）
- **细粒度分段**：把跳表底层链按区间分 *M* 段，每段一把锁；范围 / 跨段写要小心锁顺序防死锁（先锁 header 层方向一致）。
- **每节点锁**：锁住被插入/删除节点的邻域——难度陡增，跨层维护 invariants 易错。**时间紧别做**，作为「未来可扩展」在文档标注。
- 提示：**Redis 本身是单线程**，所以它不用锁——真正的并发难点在「如果把它变成多线程」对比理解。面试可讲「Redis 靠单线程 + 分字典 / IO 多路复用规避锁；本引擎为学习跳表并发，用读写锁做正确性入门」。

### 踩坑（并发 + 跳表叠加时尤其致命）
- `get` 绝不能返回内部 `V&` / 节点指针——外部线程在锁外使用 = 悬垂/数据竞争。**返回 `std::optional<V>` 拷贝**。
- `range` 返回 `std::vector` 拷贝，别暴露迭代器。
- 锁内任何 `fsync` / 磁盘 IO 会长时间持锁 → 吞吐崩。日志追加可放锁外，内存 apply 锁内。（WAL append 与内存更新的临界区分要和锁的位置一起想清楚。）

## 5. 门面接口（KvStore）
```cpp
class KvStore {
    bool open(const std::string& walPath, DurableMode mode);
    void close();                          // 干净关闭：截断/清 WAL
    bool put(const std::string& k, const std::string& v);
    std::optional<std::string> get(const std::string& k);
    bool del(const std::string& k);
    size_t range(const std::string& lo, const std::string& hi,
                 std::vector<std::pair<std::string,std::string>>& out);
    size_t size() const;
    void flush();                          // 手动 fsync（No 模式用）
};
```

## 6. 测试 / 验证路线（每个里程碑配套，用户跑）
1. **结构单测**：插入 n 条 → 每层链单调、`get` 命中/未命中、删除真实移除、重复 key 覆盖。
2. **随机对照**：随机 10 万次 put/del/get，全程与 `std::map` 结果逐一比对（这是跳表正确性的金标准）。
3. **恢复测试**：写一批 → 不 close、模拟崩溃（进程内直接丢弃内存）→ `open` 重放 → 断言与崩溃前一致；再测「尾部半条日志 + CRC 坏」被正确截断。
4. **并发测试**：若干线程 put/get/del 混跑，`-fsanitize=thread` 跑无报错；与单线程基准比对吞吐衰减。

## 7. 面试穿透总结（README 必写）
- 跳表为何取 logN、为何 Redis 选它不选红黑树/B+树（上文 2 节）。
- WAL / AOF always-everysec-no 对比（上文 3 节）。
- **缓存穿透防护**（本引擎可作的延伸点，面试一问就能讲）：
  1. 参数校验 / 布隆过滤器（不存在 key 先挡在引擎外）→ 本引擎可实现 `bloom_exists(key)` 前置过滤。
  2. 空值缓存（把「key 不存在」也缓存一小段时间，防穿透击穿 DB）。
  3. 互斥锁单飞（同一 key 的缺失只允许一个线程去查下游，其余等缓存填好）——用上文细粒度锁思路可投影。

## 8. 里程碑拆分（用户排期自拟）
- Day1：跳表 insert/find/remove/range + 单测 + 随机对照。（P0）
- Day2：WAL append/重放/CRC/clean-close 截断 + 恢复测试。（P0）
- Day3：KvStore 门面 + shared_mutex 并发 + Tsan。（P0）
- Day4：bench + README + 面试点整理＋（可选）Bloom 前置过滤。（P1）
- Day5：缓冲期 / 踩坑复盘 / 代码 Review 收口。（P2）

## 9. 边界 / 不做（明说，防 scope 蔓延）
- 不做 AOF rewrite 压缩、不做多文件多段日志、不做事务级原子性 / 持久化隔离级别。
- 不做 lock-free / 每节点细化（标注为"未来可扩展"，时间紧不动）。
- 不做磁盘索引（本引擎内存优先；磁盘 B+树是另一门功课）。
- key/value 均 `std::string`（模板跳表内部通用，门面定 string 即可）。