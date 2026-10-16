# SkipList 内存 KV 存储引擎 — 实现方案（独立仓库 kv-engine）

状态：`[Day1 ✅ 跳表全绿 · Day2 ✅ WAL 全绿 · Day3 ✅ 完成（Windows ×3 + WSL2 TSAN 零竞争报告）· Day4 ✅ bench 三表 + README 回填 · 秋招打磨 ✅ README 去教学化 + 注释清理 + flush() 落地]`
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

### Day4（2026-10-14 开工）
- **bench 起草 ✅**：`bench/bench.cpp` 三组测量——① 跳表 insert/find/erase 多档 N（1e3→1e7）标度验证（判据：10 倍 N 时 ns/op 涨 2~3 倍=O(logN），涨 ~10 倍=退化链表 bug）；② range 跨度 k 递增扫 O(logN+k)；③ KvStore 1 vs 4 线程 put 吞吐（group commit 摊 fsync）。
- **README 重写 ✅**：状态从「Day1 手写中」更新为交付版——三层层设计（跳表/WAL/并发）+ 两锁并发详述 + 构建/TSAN/bench 命令 + 面试穿透点 4 组 + 边界明确。
- **跑数回填 ✅**（2026-10-14 WSL2 真机）：① insert 1e3→1e7 仅 149.5→209.2ns = O(logN) 无退化；erase 持平 ~62ns；find 次线性（~2.4×/档，缓存主导）、hotfind 持平 ~9→20ns 反证；② range 单调收敛线性（大跨度 ~9.7×/10 倍 s）；③ put 4 线程 1469 ops/s ≈ 1.8× 单线程（group commit）。三表已贴 README。
  - 踩坑记录：range 段曾受**编译器 hoisting**（小跨度 ns 值不真实）与**采样不足方差**双重折磨——lo 随轮次伪随机变化 + 固定 20 万采样（大跨度 ≥2000）后才稳定；id=「测量伪影 ≠ 算法」判据在 README 有注。
- 剩余：校对（P1）；可选 Bloom 前置过滤（P2）。

### 秋招打磨（2026-10-15，岗位方向：分布式存储）
- **README 重写 ✅**：去掉教学/面试措辞，改分布式存储内核定位（write-ahead ordering / fsync 策略 / group commit / 坏尾 torn-write）；三张 bench 表保留为实证亮点；Roadmap 列 分段日志/共识复制/网络层/布隆索引 四个演进方向。
- **P0#1 过时注释清理 ✅**：`include/ + src/` 全部 `TODO(用户)`/"stub"/"教学"标记清除——跳表 6 个"待实现"标头改为真实描述（算法主体未动）；`kv_store.cpp` 里「死锁未解决/WriteRequest.data_ 未删/open 未 recover」这条@handoff 已与现状矛盾（均已完成），删除。
- **P0#2 `KvStore::flush()` ✅**：stub 落地——批锁 `mutex_` 内 `wal_.flush()`，与 put/del leader 刷盘同锁序。scratch 验证：SyncMode::No 下 put→flush→不 close 崩溃→重放两键俱在；4 测试全绿。
  - ⚠️ 现状说明：put 的 leader 近期仍每批必刷盘（与 mode_ 无关），故 flush() 此刻与 leader 刷盘冗余，为 README 声明的 `No 交 OS + 手动 flush()` 接口占位，语义待 EverySec/No gate leader 时才能完全体现。

### fsync 三态收口（EverySec 接线）—— 设计定案 ✅（2026-10-15，用户与 Claude 对齐）
目标：让 README 声明的 fsync 三态为真（对齐 Redis appendfsync Always/EverySec/No）。
- **目标语义**：`Always`=每批返回前 fsync 完成（现状）；`EverySec`=返回即顶点页缓存、fsync ≤1 次/s；`No`=仅手动 flush()，无自动 fsync。
- **最小改动**：
  1. `put()/del()` `[段2]` leader flush 按 mode 分流——`Always` 照旧 `wal_.flush()`；`EverySec`/`No` 跳过（append 已在 `[段1]` `fwrite` 进日志流 C 缓冲；**fsync 频率决定丢失窗口**：EverySec 由定时器 `flush()` 的 fflush+fsync ≤1s 覆盖，No 由手动 `flush()` 覆盖）。
  2. `[段3]` 散员的兜底 flush 同按 mode 分流（另两态不需要——散员字节已在各自 `[段1]` fwrite 进日志流，由定时器/手动 `flush()` 覆盖）；**drain-to-empty 不变量原样保留**（只挪「是否 flush」这个动作，不挪 drain——散员仍必须被 drain，只是不再补 fsync）。
  3. `open()`/`close()` 启动/停掉定时器线程（`EverySec` 专属：`std::thread` + `atomic<bool> stop_`，间隔 1s 调现有 `KvStore::flush()`）。
- **正确性不变式（均不破坏）**：队列空 ⟺ 无等待者；先写盘后改内存（append 恒先上页缓存，内存 apply 在其后）；锁序单向 mutex_→rwmutex_（定时器只抢批锁、不调 put/del → 无 AB-BA）。
- **put()/del() 返回值语义降级（已接受，Redis 同款）**：`Always` 下为「本批 fsync 成功」；`EverySec`/`No` 下弱化为「已接受进页缓存」。接口注释同步改。
- **测试判据（确定性、不依赖墙钟，断言行为不测耗时）**：给 Wal 加测试可见计数器 `flush_count`（每 fsync 成功 +1）——`Always`:N put → ≥批次数；`No`:N put → 0，手动 `flush()` 一次 → 1；`EverySec`:put 后立即 0 → sleep 1.2s 后 ≥1。
- **实现 + 验证 ✅（2026-10-16）**：`[段2]` 门控由用户手写；`[段3]` gate、定时器生命周期（open 启动/close join）、Wal `flush_count` 钩子、`tests/test_sync_mode` 由 Claude 在用户授权下补齐。Windows MinGW 5 测试全绿——test_sync_mode 断言三态 fsync 次数（Always=100、No=0→1、EverySec=0→≥1），无回归。

**EverySec 语义精化 ✅（2026-10-16，P2→定案）**：回答「Sec 模式如何保证先刷后返」时明确——EverySec **不保证**「返回前已落盘」，其担保是「丢窗口 ≤1s」。精化三档水位为**返回到账位**，对齐 Redis `appendfsync everysec`（每命令 write 到页缓存 + 每秒 fsync）：
- `Always` → `wal_.flush()`（fflush+fsync，返回已到磁盘）
- `EverySec` → **`wal_.flush_to_os()`**（新方法：仅 fflush 到 OS 页缓存，返回已到 OS；磁盘 fsync 交 1s 定时器）
- `No` → 跳过（C 缓冲留待手动 `flush()` 的 fflush+fsync）
- 新增 Wal 钩子 `to_os_count`；`test_sync_mode` 加判据：EverySec 100 put → `to_os_count≥100`（每批到 OS）、`flush_count==0`→睡 1.2s→≥1；No → `to_os_count==0`。6 测试全绿。

### 交接棒重设计（2026-10-16 ✅，替代 [段3] drain-to-empty）
用户两连击指出 drain 方案的两个结构性问题后重构：① 唤醒方式不对——锁内 notify wrapper 会抢批锁+争 rwmutex_（惊群，组提交的 apply 摊薄失效）；② drain 循环**退化成持续 leader**——持续负载下 leader 无限续期、首批成员饿死、「队列空 ⟺ 无等待者」在持续到达下不可证明。
- **新机制（定案）**：leader 任期 = 一次快照 + 一次刷盘(仅 Always) + 一把 rwmutex_ 代 apply 全批，**有界**。入队请求只落二态之一——被某任 leader 收进批(→`is_done`) 或 被点名交接(→`promoted`)（下任 leader = 队列最老散员），无双关、无可证性依赖；`worked=false` 仅在队列空时由收尾置。唤醒全部移锁外：成员 `notify_all` + 下任 `notify_one`。
- **踩坑（已修）**：交接只置 `promoted=true` 不 notify → 被点名者睡死在自己的 cv_ 上（test_concurrency 挂死，现象 = 老版尾孤儿的接班变体）。修复：交接必须连敲门（收尾锁外 `next_leader->cv_.notify_one()`）。
- **返回值语义一次兑现（plan 下一步#2）**：`put`/`del` 统一走私有 `write(op,key,val)`，`reg->applied` = skiplist 真值（put=true 新增 / del=true 确有移除）；`reg->ok`（fsync 结果）降级为内部字段（Always 真判定，EverySec/No=已接受）。写请求切 3→6 字段：`op/key/val/ok/applied/is_done/promoted`。新增 `tests/test_applied` 断言语义。
- **验证**：test_concurrency（4×5000）修复后 PASS；6 测试 Windows 全绿（skiplist/kv/recovery/concurrency/sync_mode/applied）。待办：WSL2 TSAN 复跑（新并发面：交接棒 + 定时器并发）——规范轮次在 Linux。

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
2. **put/del 返回值语义 ✅（2026-10-16 交接棒重设计一并兑现）**：接口回 `reg->applied`（put=true 新增 / del=true 确有移除），`reg->ok`（fsync）降级内部；`test_applied` 断言语义。
3. **close 并发接线**：当前仅 `wal_.close()`；并发下有等待者时应排空/唤醒（现测试在 join 后调 close，无并发 close，暂安全）。
4. **清理**：`test_concurrency` 关尾部 `remove("concurrency_test.wal")`（WAL 已入 `.gitignore`，可不改）。

### 待实现里程碑
- Day2：WAL `src/wal.cpp`（append/CRC/replay/clean-close 截断）+ `test_recovery` 转绿。
- Day3：KvStore `src/kv_store.cpp`（两锁并发）+ `test_concurrency` 转绿（✅ Windows ×3；WSL2 TSAN 收尾即完成）。
- Day4：bench + README 面试点整理 +（可选）Bloom 前置过滤。（✅ bench 三表跑出 + README 回填；Bloom 列为 P2 未做）

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

## 10. TSAN 收口记录（2026-10-16）

**现象**：`test_concurrency` 在 `-fsanitize=thread` 下 83% 挂：4 处 `data race`，全部是
`pthread_cond_destroy`（`WriteRequest::~WriteRequest`）∥ `pthread_cond_broadcast` /
`Signal`（`write()` 收尾段），且「内存块 size=144，分配者 = 正在 notify 的 leader 线程自己」。

**排查三连**：
1. **引用计数分析**：`batch` / `next_leader` 持 `shared_ptr` 期间对象不可能析构；batch 只在本
   `write()` 返回时销毁。broadcast（锁内）恒早于任何析构。结构上无 UAF 路径。
2. **ASan+UBSan 16 万次写（8 轮 × 4 线程 × 5000）零错误**：ASan 对 UAF 是确定性检测，
   若「notify 已销毁对象」为真必有报告。程序 8+ 轮 TSAN/非 TSAN 从不崩溃。
3. **归因**：libstdc++ 13 的 `std::condition_variable` 用 futex 直接字实现，构造不经
   `pthread_cond_init` → TSan 感知不到「同一 malloc 地址先 destroy 后复用的新 cv」，
   把新对象的唤醒读配上旧对象销毁写，属于 shadow 影子误报（`_Sp_counted_ptr_inplace`
   把对象内联进控制块，加剧地址复用）。

**收敛（根治误报而非压制）**：条件变量本质是通信基础设施，与请求「同生共死」才是错配。
把每请求一个 `cv_` 升级为 **KvStore 持有的共享门铃**（成员 `cv_`，open 后一次构造、
close 前无在途写者），等待谓词 `is_done || promoted` 不变，收尾段一次 `cv_.notify_all()`
锁内广播。多醒者谓词为假 → futex 重睡，不抢 rwmutex_，无惊群；交接由谓词 `promoted`
承载，广播的一次唤醒天然覆盖被交接者（不再需要单独 `notify_one`）。

**验证**：`test_concurrency` TSAN 5/5 轮零警告（exit 0）；WSL2 ctest 6/6 通过；
Windows MinGW 6/6 通过。README 的「TSAN 零竞争」从此为真。

## 11. 缓存失效专项 bench（2026-10-16）

**动因**：千万级（10⁷）下 find 效率下降，定性为跳表指针追逐的 Cache Miss。
**做法**（用户选定「先测后改」）：新增 `bench/bench_cache.cpp`（`kv_bench_cache` target），
同数据三组对照——跳表 coldfind（随机 key）／ hotfind（8 热 key，cache 友好下限）／
排序数组二分（cache 友好结构的量化下界），string 短 key（SSO 内，真实 KV 形态）。

**实测**（Windows MinGW g++ 8.1 -O2 / WSL2 g++ 12 两环境趋势一致）：

| N     | cold/op | hot/op | range 吞吐 | array 二分 | 跳表/数组 |
|-------|---------|--------|-----------|-----------|----------|
| 1e5   | ~1.2 µs | ~250 ns | ~8 个/µs  | ~290 ns   | 3.7–4.2× |
| 1e6   | ~2.9 µs | ~350 ns | ~5 个/µs  | ~530 ns   | 4.1–6.0× |
| 1e7   | ~5.7 µs | ~350 ns | ~3.7 个/µs| ~900 ns   | 5.8–6.8× |

**结论（判据已核验）**：cold 随 10⁵→10⁷ 涨 ~4.7–5×（1e7 ≈ 5.7 µs/op），hot 几乎持平
（~1.6–2.2×）→ 涨幅全部来自每次指针跳转的主存往返，非算法复杂度；跳表冷查比数组
二分慢 3.7×→6.8× 且随量级拉开 → 「有序索引＋find 高频＋千万级」三条件齐备时换
B 树/数组的量化判据。README §④ 已录表 + WSL 复跑锚点 + 判据三句。

**方案状态（2026-10-16 用户决策）**：P2 紧凑节点优化**先停**。高数据量的应对方向改为**「换结构 + 分层级」**——数据量越过阈值后不再让单一跳表硬扛 cache miss，转而采用分层/多级索引。形态待后续明确，候选包括：
- 内存跳表（热层）＋ 磁盘 B 树（冷层），按数据量/温度分层；
- Bloom 过滤层拦「不存在」键 + 精查层（呼应 §7 缓存穿透防护）；
- 或直接切排序数组/B 树（§④ 判据：三条件齐备时换结构）。
启动判据即 §④ 实测：1e7 冷查 ~5.7 µs/op、比数组二分慢 6.8×。