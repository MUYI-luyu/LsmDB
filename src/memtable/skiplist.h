#pragma once

// 线程安全性
// -----------
// 写操作需要外部同步。
// 读操作需要保证 SkipList 在读进行期间不会被销毁。
// 除此之外，读操作不需要任何内部锁定或同步。


#include <atomic>
#include <cassert>
#include <cstdlib>

#include "memtable/arena.h"
#include "utils/random.h"

namespace lsmdb {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  // 创建一个新的 SkipList 对象，使用 "cmp" 来比较键，
  // 并使用 "*arena" 分配内存。在 arena 中分配的对象
  // 必须在 skiplist 对象的整个生命周期内保持分配状态。
  explicit SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // 将键插入列表中。
  // 需求：当前列表中不存在与键比较相等的任何元素。
  void Insert(const Key& key);

  // 当且仅当列表中存在与键比较相等的条目时返回 true。
  bool Contains(const Key& key) const;

  // 遍历跳表的内容
  class Iterator {
   public:
    // 初始化指定列表上的迭代器。
    // 返回的迭代器无效。
    explicit Iterator(const SkipList* list);

    // 当且仅当迭代器位于有效节点时返回 true。
    bool Valid() const;

    // 返回当前位置的键。
    // 需求：Valid() 必须为真
    const Key& key() const;

    // 前进到下一个位置。
    // 需求：Valid() 必须为真
    void Next();

    // 后退到前一个位置。
    // 需求：Valid() 必须为真
    void Prev();

    // 前进到键 >= target 的第一个条目
    void Seek(const Key& target);

    // 定位到列表中的第一个条目。
    // 迭代器的最终状态当且仅当列表非空时为 Valid()。
    void SeekToFirst();

    // 定位到列表中的最后一个条目。
    // 迭代器的最终状态当且仅当列表非空时为 Valid()。
    void SeekToLast();

   private:
    const SkipList* list_;
    Node* node_;
    // 有意使其可复制
  };

 private:
  enum { kMaxHeight = 12 };  // 最大高度

  inline int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  // 比较两个键是否相等
  bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

  // 如果 key 大于存储在 "n" 中的数据，返回 true
  bool KeyIsAfterNode(const Key& key, Node* n) const;

  // 返回最早出现在 key 处或之后的节点。
  // 如果没有这样的节点，返回 nullptr。
  //
  // 如果 prev 非空，为 [0..max_height_-1] 中的每个 level 填充
  // prev[level]，指向该 level 处的前一个节点。
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  // 返回键 < key 的最新节点。
  // 如果没有这样的节点，返回 head_。
  Node* FindLessThan(const Key& key) const;

  // 返回列表中的最后一个节点。
  // 如果列表为空，返回 head_。
  Node* FindLast() const;

  // 构造后不可变
  Comparator const compare_;
  Arena* const arena_;  // 用于节点分配的 Arena

  Node* const head_;

  // 仅由 Insert() 修改。由读取者竞争读取，但陈旧的
  // 值是可以接受的。
  std::atomic<int> max_height_;  // 整个列表的高度

  // 仅由 Insert() 读写。
  Random rnd_;
};

// 实现细节如下
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  // 链接的访问器/修改器。包装在方法中，以便我们可以
  // 根据需要添加适当的屏障。
  Node* Next(int n) {
    assert(n >= 0);
    // 使用 'acquire load' 确保：
    // 我们观察到的返回 Node 是完全初始化后的版本。
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    // 使用 'release store' 确保：
    // 在此指针被其他线程看到之前，该 Node 的所有初始化工作（如 key 的赋值）已经完成。
    next_[n].store(x, std::memory_order_release);
  }

  // 可以在少数位置安全使用的无屏障变体。
  Node* NoBarrier_Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_relaxed);
  }
  void NoBarrier_SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_relaxed);
  }

 private:
  // 长度等于节点高度的数组。next_[0] 是最低级别的链接。
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  // 为新节点分配对齐的内存
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  return new (node_memory) Node(key);
}

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) {
  list_ = list;
  node_ = nullptr;
}

template <typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const {
  return node_ != nullptr;
}

template <typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const {
  assert(Valid());
  return node_->key;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next() {
  assert(Valid());
  node_ = node_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
  // 我们不使用显式的 "prev" 链接，而是搜索最后一个
  // 在 key 之前的节点。
  assert(Valid());
  node_ = list_->FindLessThan(node_->key);
  if(node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
  node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
  node_ = list_->FindLast();
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // 以 1/kBranching 的概率增加高度
  static const unsigned int kBranching = 4;
  int height = 1;
  while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
    height++;
  }
  assert(height > 0);
  assert(height <= kMaxHeight);
  return height;
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
  // null n 被认为是无穷大
  return (n != nullptr) && (compare_(n->key, key) < 0);
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key,
                                              Node** prev) const {
  int level = GetMaxHeight() - 1;
  Node* x = head_;
  while(true){
    Node* next = x->Next(level);
    if(KeyIsAfterNode(key, next)){
      x = next;
    } else {
      if(prev != nullptr) prev[level] = x;
      if(level == 0) {
        return next;
      } else {
        level--;
      }
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
  int level = GetMaxHeight() - 1;
  Node* x = head_;
  while(true){
    Node* next = x->Next(level);
    if(KeyIsAfterNode(key, next)){
      x = next;
    } else {
      if(level == 0){
        return x;
      } else {
        level--;
      }
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast()
    const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return x;
      } else {
        // 切换到下一个列表
        level--;
      }
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(0 /* 任何键都可以 */, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {// 初始化随机数生成器
  for (int i = 0; i < kMaxHeight; i++) {
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);

  // 不允许重复插入
  assert(x == nullptr || !Equal(key, x->key));
  int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // NoBarrier_SetNext() 就足够了，因为当我们在 prev[i] 中发布指向 "x" 的指针时，会添加一个屏障。
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  if (x != nullptr && Equal(key, x->key)) {
    return true;  // 找到匹配的键
  } else {
    return false;  // 未找到
  }
}

}  // namespace lsmdb
