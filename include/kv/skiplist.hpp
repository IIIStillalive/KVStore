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
    // 固定 maxlevel=32、p=1/4（对齐 Redis ZSet，非随容量推导）。
    // 【final 规范 v1.0】：无参构造，成员全部就地初始化为默认值。
    SkipList();

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

    // 核心辅助：从 head_ 顶层向下逐层下沉，把「待插删位置在每一层的前驱
    // 指针」填进 prev[0..currentlevel]。
    // ⚠️ 跳表最容易写错的地方：每一层都要正确下沉，不可只沉底层。
    void updatePrev(const K& key, std::vector<Node*>& prev);

    // 随机层高：从 1 起步，以概率 p_ 逐层上升，封顶 maxlevel。
    // 用 std::mt19937（分布稳定），不要用 rand()。
    int randomLevel();

    Node*  head_;          // 哨兵，level = maxlevel
    int    maxlevel   = 32;
    int    currentlevel = 1;   // 1..32，当前实际最高层
    float  p_         = 0.25f;
    size_t size_      = 0;
    Cmp    cmp_;     //通用比较器
    std::mt19937 rng_;         // 随机源（分布稳定，别用 rand）
};

// ---------------------------------------------------------------------------
// TODO(用户) 实现区：以下 stub 保证可编译，但逻辑未实现，测试当前为「红」。
// 你把每个函数体按 plan §2 替换成真实实现即可逐步转绿。
// ---------------------------------------------------------------------------

template<typename K, typename V, typename Cmp>
SkipList<K, V, Cmp>::SkipList()
    // 成员已就地初始化：maxlevel=32, currentlevel=1, p_=0.25f, size_=0
    : rng_(std::random_device{}()) {
    // TODO(用户):
    //   1) 建哨兵：head_ = new Node{ K{}, V{}, maxlevel, vector<Node*>(maxlevel, nullptr) };
    //      哨兵 level=maxlevel，next 全空（每层都从它开始下沉）。
    head_ = new Node{K{}, V{}, maxlevel, std::vector<Node*> (maxlevel, nullptr)};
}

template<typename K, typename V, typename Cmp>
SkipList<K, V, Cmp>::~SkipList() {
    // TODO(用户): 沿底层链释放所有节点，防止裸指针泄漏
    //   （配合 erase 一起写；释放顺序见 plan §2）。

    auto curr = head_;
    while(curr){
        Node* next = curr->next[0];
        delete curr;
        curr = next;
    }
}

template<typename K, typename V, typename Cmp>
void SkipList<K, V, Cmp>::updatePrev(const K& key,
                                     std::vector<Node*>& prev) {
    // TODO(用户): 从 header 顶层下沉到第 0 层，记每层最后「后继 >= key」的节点。
    //   1) prev 只需覆盖 0..currentLevel_；
    //   2) 每层向下一层时指针要正确下跳，这是最容易写错的地方。
    auto it = head_;
    int level = currentlevel - 1;
    while(true){
        while(it->next[level] && cmp_(it->next[level]->key, key)){
            it = it->next[level];
        }

        //  到了这里说明要么break or it->next == nullptr
        //  所以这层的要存的节点就是it
        prev[level] = it;

        //接下来下沉
        --level;
        if(level < 0) break;
        
    }
}

template<typename K, typename V, typename Cmp>
int SkipList<K, V, Cmp>::randomLevel() {
    // TODO(用户): level=1; 以 p_ 概率 level++（rng_ 驱动），封顶 maxLevel_。返回 level。

    //我的想法是使用std::mt19937生成0-1.0之间的概率，当概率小于P_时++，小于时结束
    std::uniform_real_distribution<double> udist(0.0, 1.0);
    int level = 1;
    while(udist(rng_) < p_){
        level++;
    }
    level = (level > maxlevel ? maxlevel : level);
    return level;
}

template<typename K, typename V, typename Cmp>
bool SkipList<K, V, Cmp>::insert(const K& key, const V& value) {
    // TODO(用户):
    //   1) updatePrev(key, prev) 定位前驱们；
    //   2) 若 prev[0]->next[0] 的 key == key → 覆盖其 value，返回 false；
    //   3) 否则 randomLevel() 起飞，从下往上建节点并逐层接入 prev 链，size_++，
    //      返回 true；若新层超过 currentLevel_ 记得提升。
    std::vector<Node*> prev(maxlevel, nullptr);
    updatePrev(key, prev);
    if(prev[0]->next[0] && prev[0]->next[0]->key == key){
        prev[0]->next[0]->value = value;
        return false;  //insert负责了插入和更改两个功能，所以要检查这个前置节点的key是否与当下一致;
    }
    
    //插入
    int newlevel = randomLevel();  //返回当下新节点的高度
    Node* newnode = new Node{key, value, newlevel, std::vector<Node*>(newlevel, nullptr)};
    int min = currentlevel > newlevel ? newlevel : currentlevel;
    if(newlevel > currentlevel){  //修改当前跳表的实际最高层
        for(int i = currentlevel; i < newlevel; ++i){
            head_->next[i] = newnode;
            
        }
        currentlevel = newlevel;
        
    }
    for(int level = 0; level < min; ++ level){
        newnode->next[level] = prev[level]->next[level];
        prev[level]->next[level] = newnode;
    }
    
    this->size_++;
    return true;
}

template<typename K, typename V, typename Cmp>
std::optional<V> SkipList<K, V, Cmp>::find(const K& key) const {
    // TODO(用户): 从顶层下沉查 key；命中返回 value 拷贝，未命中返回 nullopt。
    auto it = head_;  //遍历指针
    int level = currentlevel - 1;
    while(true){
        while(it->next[level] && cmp_(it->next[level]->key, key)){
            // 存在和小于就前进
            it = it->next[level];
        }
        //出循环说明： 1.上索引没找到  2.没满足条件-> 1.大于 2.等于
        if(it->next[level] && it->next[level]->key == key) return it->next[level]->value;
        //不满足就下沉
        --level;
        if(level < 0) break;
    }
    return std::nullopt;
}

template<typename K, typename V, typename Cmp>
bool SkipList<K, V, Cmp>::erase(const K& key) {
    // TODO(用户): updatePrev 定位 → 若存在则把各层 next 越过去 → 释放节点，size_--。
    //   可选：某层已无后继时回落 currentLevel_（不是必须，保持简单即可）。
    std::vector<Node*> prev(maxlevel, nullptr);
    updatePrev(key, prev);
    Node* curr = prev[0]->next[0];
    if(curr && curr->key == key){
        //存在就删除
        int level = curr->level - 1;  //根据层高来改变prev的后继;
        for(int i = level; i >= 0; --i){
            prev[i]->next[i] = curr->next[i];  //越过
        }
        while(currentlevel > 1 && head_->next[currentlevel-1] == nullptr){
            --currentlevel;
        }
        delete curr;  //删除节点，释放资源
        size_--;  //size变化
        return true;
    }
    return false;
}

template<typename K, typename V, typename Cmp>
size_t SkipList<K, V, Cmp>::range(const K& lo, const K& hi,
                                  std::vector<std::pair<K, V>>& out) const {
    // TODO(用户): 底层链 lower_bound(lo) 顺序扫到 >hi；把结果 push 进 out，返回条数。

    //利用查询找到第层的lo，然后一直判断push
    auto it = head_;  //遍历指针
    int level = currentlevel - 1;
    while(true){
        while(it->next[level] && cmp_(it->next[level]->key, lo)){
            it = it->next[level];
        }

        //下沉
        --level;
        if(level < 0) break;
    }
    //当循环退出的时候it->next[0]->key 只能是>= lo 
    //接下只要判断是否存在以及是否大于hi即可
    while(it->next[0] && it->next[0]->key <= hi){
        out.emplace_back(it->next[0]->key, it->next[0]->value);
        it= it->next[0];
    }
    return out.size();
}

}  // namespace kv