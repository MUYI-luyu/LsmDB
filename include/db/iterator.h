#pragma once

// 迭代器从一个数据源产生一系列键/值对。
// 本文件定义了迭代器的抽象接口。
// 本库将提供多个实现，包括访问 Table 或 DB 内容的迭代器。
//
// 对同一个 Iterator，多个线程可以安全调用 const 方法，
// 前提是底层容器在此期间不被修改。
// 如果任何线程可能调用非 const 方法（或修改底层容器），
// 则访问该 Iterator 的所有线程必须使用外部同步（如互斥锁）。

#include <cassert>
#include <cstdint>

#include "db/slice.h"
#include "db/status.h"

namespace lsmdb {

class Iterator {
 public:
  Iterator();

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual ~Iterator();

  // 当迭代器定位在一个键/值对上时为 true，否则无效。
  virtual bool Valid() const = 0;

  // 定位到数据源中第一个键。当且仅当数据源非空时，
  // 调用后迭代器为 Valid()。
  virtual void SeekToFirst() = 0;

  // 定位到数据源中最后一个键。当且仅当数据源非空时，
  // 调用后迭代器为 Valid()。
  virtual void SeekToLast() = 0;

  // 定位到数据源中 >= target 的第一个键。
  // 当且仅当数据源包含 >= target 的条目时，调用后迭代器为 Valid()。
  virtual void Seek(const Slice& target) = 0;

  // 前进到下一个条目。调用后 Valid() 为真 iff 迭代器没有定位在最后一个条目上。
  // 前置条件: Valid()
  virtual void Next() = 0;

  // 后退到前一个条目。调用后 Valid() 为真 iff 迭代器没有定位在第一个条目上。
  // 前置条件: Valid()
  virtual void Prev() = 0;

  // 返回当前条目的键。返回的 Slice 的底层存储仅在下次修改迭代器之前有效。
  // 前置条件: Valid()
  virtual Slice key() const = 0;

  // 返回当前条目的值。返回的 Slice 的底层存储仅在下次修改迭代器之前有效。
  // 前置条件: Valid()
  virtual Slice value() const = 0;

  // 如果发生错误，返回它。否则返回 ok 状态。
  virtual Status status() const = 0;

  // 允许注册清理函数，在迭代器销毁时被调用。
  // 注意：与前所有方法不同，此方法不是抽象方法，子类不应覆盖它。
  using CleanupFunction = void (*)(void* arg1, void* arg2);
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

 private:
  // 清理函数存储在单链表中。链表的头节点内嵌在迭代器中。
  struct CleanupNode {
    // 如果节点未被使用则返回 true。只有头节点可能未被使用。
    bool IsEmpty() const { return function == nullptr; }
    // 调用清理函数。
    void Run() {
      assert(function != nullptr);
      (*function)(arg1, arg2);
    }

    CleanupFunction function;
    void* arg1;
    void* arg2;
    CleanupNode* next;
  };
  CleanupNode cleanup_head_;
};

// 返回一个空迭代器（不产生任何内容）。
Iterator* NewEmptyIterator();

// 返回一个带有指定状态的错误迭代器。
Iterator* NewErrorIterator(const Status& status);

}  // namespace lsmdb
