#pragma once

// ============================================================================
// SkipList<K, V, Cmp> — 内存有序索引（header-only 模板，定义必须对本 TU 可见）
//
// 学习契约：本文件只有「接口 + 数据结构 + 可编译的 stub」，算法主体由你手写。
// 结构见 plan/task_plan_skipkv.md §2；每个 TODO(用户) 是你需要填充的核心逻辑。
//
// 复杂度目标：
//   查 / 删     平均 O(logN)，从 header 顶层逐层下沉
//   范围查询    O(logN + k)，k = 结果数
// ============================================================================

#include <cstddef>
#include <functional>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace kv {

template<typename K, typename V, typename Cmp = std::less<K>>
class SkipList {
public:
    // maxLevel 由预计条目数估算，不要写死 32：
    //   maxLevel = ceil(log_{1/p}(expected_entries))，至少 1
    explicit SkipList(size_t expected_entries, float p = 0.5f);

    ~SkipList();

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // 覆盖式插入：key 已存在则替换 value。
    // 返回 true 表示「新增节点」，false 表示「覆盖已有 key」。
    bool insert(const K& key, const V& value);

    // 命中返回 value 的拷贝；未命中返回 nullopt。
    // ⚠️ 绝不返回内部引用/指针 —— 并发线程锁外使用会悬垂/数据竞争。
    std::optional<V> find(const K& key) const;

    // 删除 key；返回是否确有节点被移除。
    bool erase(const K& key);

    // 闭区间 [lo, hi]，在底层有序链顺序扫描。
    // 返回实际写入 out 的条数。
    size_t range(const K& lo, const K& hi,
                 std::vector<std::pair<K, V>>& out) const;

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    struct Node {
        K key;
        V value;
        int level;                   // 实际层高，1..maxLevel_
        std::vector<Node*> next;     // next[0] 是最底层有序链
    };

    // 核心辅助：从 header 顶层向下逐层下沉，把「待插删位置在每一层的前驱
    // 指针」填进 prev[0..currentLevel_]。
    // ⚠️ 跳表最容易写错的地方：每一层都要正确下沉，不可只沉底层。
    void updatePrev(const K& key, std::vector<Node*>& prev) const;

    // 随机层高：从 1 起步，以概率 p_ 逐层上升，封顶 maxLevel_。
    // 用 std::mt19937（分布稳定），不要用 rand()。
    int randomLevel();

    Node*  header_;        // 哨兵，level = maxLevel_
    int    currentLevel_;  // 1..maxLevel_，当前实际最高层
    int    maxLevel_;
    float  p_;
    size_t size_;
    Cmp    cmp_;
    std::mt19937 rng_;
};

// ---------------------------------------------------------------------------
// TODO(用户) 实现区：以下 stub 保证可编译，但逻辑未实现，测试当前为「红」。
// 你把每个函数体按 plan §2 替换成真实实现即可逐步转绿。
// ---------------------------------------------------------------------------

template<typename K, typename V, typename Cmp>
SkipList<K, V, Cmp>::SkipList(size_t expected_entries, float p)
    : header_(nullptr),
      currentLevel_(1),
      maxLevel_(1),
      p_(p),
      size_(0),
      cmp_(),
      rng_(std::random_device{}()) {
    // TODO(用户):
    //   1) maxLevel_ = ceil(log_{1/p_}(expected_entries))，至少 1。
    //      例：p=0.5 时即 log2(expected_entries)。
    //   2) 建哨兵：new Node{ K{}, V{}, maxLevel_, vector<Node*>(maxLevel_, nullptr) }。
    //      若 p != 0.5，randomLevel 的分布要与 maxLevel_ 匹配（否则层高不达上限）。
    // 下方只是可编译的临时占位（单层哨兵，退化为有序链表）：
    header_ = new Node{ K{}, V{}, 1, std::vector<Node*>(1, nullptr) };
}

template<typename K, typename V, typename Cmp>
SkipList<K, V, Cmp>::~SkipList() {
    // TODO(用户): 沿底层链释放所有节点，防止裸指针泄漏
    //   （配合 erase 一起写；释放顺序见 plan §2）。
}

template<typename K, typename V, typename Cmp>
void SkipList<K, V, Cmp>::updatePrev(const K& key,
                                     std::vector<Node*>& prev) const {
    // TODO(用户): 从 header 顶层下沉到第 0 层，记每层最后「后继 >= key」的节点。
    //   1) prev 只需覆盖 0..currentLevel_；
    //   2) 每层向下一层时指针要正确下跳，这是最容易写错的地方。
}

template<typename K, typename V, typename Cmp>
int SkipList<K, V, Cmp>::randomLevel() {
    // TODO(用户): level=1; 以 p_ 概率 level++（rng_ 驱动），封顶 maxLevel_。返回 level。
    return 1;
}

template<typename K, typename V, typename Cmp>
bool SkipList<K, V, Cmp>::insert(const K& key, const V& value) {
    // TODO(用户):
    //   1) updatePrev(key, prev) 定位前驱们；
    //   2) 若 prev[0]->next[0] 的 key == key → 覆盖其 value，返回 false；
    //   3) 否则 randomLevel() 起飞，从下往上建节点并逐层接入 prev 链，size_++，
    //      返回 true；若新层超过 currentLevel_ 记得提升。
    return false;
}

template<typename K, typename V, typename Cmp>
std::optional<V> SkipList<K, V, Cmp>::find(const K& key) const {
    // TODO(用户): 从顶层下沉查 key；命中返回 value 拷贝，未命中返回 nullopt。
    return std::nullopt;
}

template<typename K, typename V, typename Cmp>
bool SkipList<K, V, Cmp>::erase(const K& key) {
    // TODO(用户): updatePrev 定位 → 若存在则把各层 next 越过去 → 释放节点，size_--。
    //   可选：某层已无后继时回落 currentLevel_（不是必须，保持简单即可）。
    return false;
}

template<typename K, typename V, typename Cmp>
size_t SkipList<K, V, Cmp>::range(const K& lo, const K& hi,
                                  std::vector<std::pair<K, V>>& out) const {
    // TODO(用户): 底层链 lower_bound(lo) 顺序扫到 >hi；把结果 push 进 out，返回条数。
    return 0;
}

}  // namespace kv