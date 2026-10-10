# 评审记录 03 — KvStore 门面接口（甲方 / 需求方）

被评审：用户手写初稿（my_design/include/kv/kv_store.hpp）

## 初稿（v1）要点
- 组合 skiplist + wal，无锁 → P0：层③ 无并发。
- 后续补 `mutable std::shared_mutex rwmutex_`、`size()/empty()`、读 const + optional ✓。

## v3 进化为 group commit（组提交）—— 认可
首批提交线程做一次 write+fsync 覆盖整批、成功唤醒所有等待者。
→ N 条 put → 1 组 fsync，等效 PostgreSQL/MySQL/Raft 批刷。✓

## 🔴 关键矛盾：`insert` 跑到 `fsync` 之前 → 自打雷②的脸
用户最早期答对「先写盘后改内存，崩以磁盘为准」；但本版 put 内：
`{ unique_lock: append(); insert(); }` **先 insert(内存)**，解锁后才排队 fsync。
失败场景：B put(K=2) insert 进内存→解锁→读者 get(K) 看到 2→崩溃→该批未 fsync、
K=2 从日志丢失→重启内存重建回 K=1 → 读者见过的 2「消失」。
⇒ 违反写前日志(WAL) 的真正次序：**数据不得在日志 fsync 完成前对读者可见**。

## 正解：两段锁 + 批次水位线（fsync 仍不背锁）
1. `unique_lock`: append 全部 + 记下本批**水位线**边界 → 解锁
2. 队首(leader) 对整批到水位线做 **write + fsync**（离锁、贵操作不背锁）→ 成功
3. **再取锁**：把本批 insert 真正 apply 到内存 →
   读者只可能看到「已 fsync、不会消失」的数据
⇒ 「先落盘后内存」在每个 put 层面依然成立，且 fsync 在锁外。这是对「必答题」
   (fsync 与 insert 先后) 的正确答案：**insert 必须在 fsync 之后**。

## 备注
- `path` 改为 `const std::string&` ✓（已修）
- 需补：队列本身的线程安全 + 等待/唤醒机制（condition_variable / future 选一）+ 水位线实现
- 备选：若愿接受「读者可能见到未提交值」（弱隔离、需文档明示），则现顺序语义可保留——但这不是先写后改。

## v4 收口 —— 层③ 设计与验收通过
- 批次边界：**数量达上限 或 超时**；leader 取当前批全部 → cv 唤醒阻塞者；队列独立 `mutex+condition_variable`。
- 语义轨迹（用户螺旋上升正确）：早期后台统一 fsync=退化 EverySec → v3 自阻塞=回 Always → v4
  批量+自阻塞=**Always 且省 fsync** ✓。
- 收口两点：
  1. 有界延迟：Always 下每个 put 最坏等一个超才返回，需能讲出「延迟上界=超时」契约。
  2. leader 先**快照水位线**（fsync 期间新 append 归下一批）+ 先 flush 流缓冲再 fsync。
- **结论：三层接口全部收官。** 见评审 01/02/03 合并验收。