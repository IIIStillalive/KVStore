#pragma once
// ============================================================================
// 层① SkipList — 内存有序索引（header-only 模板）
// 【最终规范 v1.0 · 2026-10-08】由 my_design 设计定稿合并而来。
// 契约：覆盖式插入、optional 区分空/不存在、range 记为有序拷贝。
// mag = 固定 32@p=1/4（对齐 Redis ZSet，非随容量推导）。
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
    SkipList();                  // 固定 maxlevel=32、p=0.25、bottom 从 level=1 起
    ~SkipList();
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // 覆盖式 upsert：key 已存在则替换 value。
    // 契约：返回 true = 新增（size++）；false = 覆盖（size 不变）。
    bool insert(const K& key, const V& value);

    // 契约：命中返回 value 拷贝；未命中返回 nullopt。绝不泄露内部指针/引用。
    bool erase(const K& key);    // 返回是否有节点被移除
    std::optional<V> find(const K& key) const;

    // 闭区间 [lo,hi]，底层有序链顺序扫描，结果有序、独立拷贝。返回写出条数。
    size_t range(const K& lo, const K& hi,
                 std::vector<std::pair<K,V>>& out) const;

    bool   empty() const noexcept;   // == size()==0
    size_t size()  const noexcept;   // 已插入元素个数（哨兵不计）

private:
    struct Node {
        K key;
        V value;
        std::vector<Node*> next;   // next[i]=第 i 层后继；next[0] 为底链
        int level;                  // 节点实际层高（分配时一次定死，不增不降）
        // 不变量：next.size()==level
    };

    // 从 header 顶层向下逐层下沉，把「待插删位置在每层的前驱」填进 prev[0..currentlevel]。
    // 关键：同一趟下沉须记所有层，不只底层。
    void updatePrev(const K& key, std::vector<Node*>& prev);

    Node*  head_;          // 哨兵，level=maxlevel，next 全空
    int    maxlevel = 32;
    int    currentlevel = 1;   // 当前实际最高层（1..32）
    int    randomLevel();      // G(p)：level 从 1 起、以 p_ 逐层上升、封顶 maxlevel
    float  p_ = 0.25;
    size_t size_ = 0;
    Cmp    cmp_;
    std::mt19937 rng_;         // 随机源（分布稳定，别用 rand）
};

// ==== TODO(实现)：/随机层高 /updatePrev /insert /erase /find /range /析构回收 ====
}  // namespace kv