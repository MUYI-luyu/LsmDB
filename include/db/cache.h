#pragma once

#include <cstdint>

#include "db/slice.h"

namespace lsmdb {

class Cache;

// 创建一个具有固定容量的新缓存。此 Cache 实现使用最近最少使用（LRU）淘汰策略。
Cache* NewLRUCache(size_t capacity);

class Cache {
 public:
  Cache() = default;

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // 销毁所有现有条目，通过调用构造时传入的 "deleter" 函数。
  virtual ~Cache();

  // 不透明句柄，表示缓存中存储的一个条目。
  struct Handle {};

  // 将 key->value 映射插入缓存，并分配指定的负载计入总缓存容量。
  //
  // 返回一个指向该映射的句柄。当返回的映射不再需要时，
  // 调用者必须调用 this->Release(handle)。
  //
  // 当插入的条目不再需要时，key 和 value 将被传递给 "deleter"。
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) = 0;

  // 如果缓存中没有 "key" 的映射，返回 nullptr。
  //
  // 否则返回一个指向该映射的句柄。当返回的映射不再需要时，
  // 调用者必须调用 this->Release(handle)。
  virtual Handle* Lookup(const Slice& key) = 0;

  // 释放之前由 Lookup() 返回的映射。
  // 要求：handle 之前未被释放过。
  // 要求：handle 必须是由 *this 上的方法返回的。
  virtual void Release(Handle* handle) = 0;

  // 返回由成功的 Lookup() 返回的句柄所封装的值。
  // 要求：handle 之前未被释放过。
  // 要求：handle 必须是由 *this 上的方法返回的。
  virtual void* Value(Handle* handle) = 0;

  // 如果缓存包含 key 的条目，将其擦除。注意，底层条目将
  // 一直保留，直到所有现有的句柄都被释放。
  virtual void Erase(const Slice& key) = 0;

  // 返回一个新的数字 ID。可供共享同一缓存的多个客户端使用，
  // 以划分键空间。通常客户端在启动时分配一个新 ID，
  // 并将该 ID 作为其缓存键的前缀。
  virtual uint64_t NewId() = 0;

  // 移除所有未处于活跃使用状态的缓存条目。内存受限的应用
  // 可调用此方法来减少内存使用。默认的 Prune() 实现不执行任何操作。
  // 强烈建议子类覆盖默认实现。
  virtual void Prune() {}

  // 返回缓存中存储的所有元素的总负载的估算值。
  virtual size_t TotalCharge() const = 0;
};

}  // namespace lsmdb
