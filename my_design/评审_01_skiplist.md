# 评审记录 01 — SkipList 接口（甲方 / 需求方）

日期：2026-10-08
范围：层① 有序结构接口 `my_design/include/kv/skiplist.hpp`
评审人：甲方（Claude）
被评审：用户手写初稿

## 结论
骨架里有 3 处判断正确，但有 **3 个 P0（编译不过/直接缺陷）**、**2 个 P1（需求漏项）**，
另有 5 个甲方追问需要你在下一版设计里自己答清楚。当前版本甲方**不予验收**。

## 🔴 P0 — 编译不过 / 直接缺陷
1. `SkipList& SkipList(SkipList&) = delete;` —— 构造函数不能带返回类型。
   这是一行「返回 SkipList& 的成员函数 + 与类同名」的错误声明，会编译报错。
   想禁拷贝应写 `SkipList(const SkipList&) = delete;`（构造器无返回类型 + 取 const&）。
2. `bool change(const K& key);` —— 缺新值参数。F1 要求能「覆盖已有 key 的 value」，
   这个「改」只给 key 不给 value，一个字面意义的缺陷：改不了值。
   需澄清：要么 `change(key, newValue)`，要么说明 `insert_` 本身就是覆盖式 upsert。
3. `kv_store.hpp` 第 7 行 `#include "kv/wal.hpp` —— 缺右引号，编译错。

## 🟠 P1 — 需求漏项
4. 缺 `size()` / `empty()`（F4）。且类内无 `size_` 成员，实现时无法 O(1) 答条数。
5. 缺成员 `cmp_`（Cmp 实例）与 `p_`（升层概率）。模板 Cmp 未在类内落实例 → 比较器是死的；
   `randomLevel` 的概率与最大层上限无处安放。

## 🟡 甲方追问（自己答，下一版带上）
- Q1 `insert_` 对已存在 key：拒绝 or 覆盖？覆盖→`change` 冗余；拒绝→F1 覆盖靠谁？
- Q2 `head_` 是哨兵还是真实首节点？`maxlevel` 从哪定（无容量/概率参数）？层高随容量增长依据？
- Q3 `next.size()==level` 不变量谁维护？
- Q4 `insert` 怎么在同一趟下沉里记住「每层前驱」以接新节点？
- Q5 读侧已用 const 表达共享；写侧是否「只改 next、key 不可变」？这决定门面锁粒度。

## ✅ 已判断正确的部分（保留）
- `find` 返回 `std::optional<V>` 值拷贝 → 区分「存在但空/不存在」，不泄内部指针（F2+F13）✓
- `range` const + 拷贝出参 → 结果有序、独立拷贝（F6）✓
- 泛型模板，层①始与门面解耦 ✓

## 验收判定
当前版本：**不通过**。修好 P0、补 P1、把 Q 写下草案后可复审。

---

## 复审 01b（2026-10-08，用户修订后）

### 已闭环 ✓
- 拷贝构造/赋值修复为 `const& = delete` ✓
- `change` 并入 `insert`，覆盖式 upsert（Q1 闭环）✓
- 补 `size()` / `isempty()`（F4）✓
- 落 `p_` / `cmp_` / `rng_` 成员 ✓

### 仍阻塞（P0）
1. `siez_t size_;` 拼写错（应为 `size_t`）→ 编译错
2. `size()` / `isempty()` 未标 `const`（读操作应 const，门面 shared_lock 以 const 查询用）建议 `size_t size() const noexcept; bool empty() const noexcept;`

### 仍欠账
- Q2：`p_` 初值、`maxlevel` 来源与「随容量增长」依据（构造仍无参）
- `insert` 的 bool 返回语义未钉：true = 操作成功，还是 = 新增 unp vs 覆盖？（门面/WAL 记账要用）

### 顺手
- `isempty()` → `empty()`（命名一致性）
- `kv_store.hpp` 缺右引号仍未修

---

## 复审 01c（2026-10-08，最终版）→ **层①批准放行**（修 2 处后）

### 已改对并确认
- `size_t size_;` 拼写 ✔；`empty()`/`size()` 带 `const noexcept` ✔
- `maxlevel=32`、`p_=0.25` 成员内置、不动态（决策认可，对齐 Redis ZSet）✔
- 拷贝构造/赋值 `const& = delete` ✔；`change` 并入 `insert` ✔
- **`insert` bool 语义：true=新增、false=覆盖** ✔（记账有依据）

### 批准前还剩（2 处 P0/P1）
1. **`struct Node{...}` 缺结尾 `;`（第 36 行）→ 编译错**，需补 `};`
2. `int currentlevel;` 无初值 → 建议 `= 1`

### 事实订正
节点层高是**几何分布**（G(p)，期望 1/p=4 层，≥h 层概率 p^(h-1)），**不是正态分布**。

### 层② 待设计
WAL：F8–F11 + 三态刷盘。评审文件见 `评审_02_wal.md`。