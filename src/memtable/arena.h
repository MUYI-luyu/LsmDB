#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lsmdb {

// 简易内存池（Arena）。
// 通过预分配大块内存并从中切分小对象，避免频繁堆分配。
// 本类不是线程安全的，调用方需外部同步。
class Arena {
 public:
  Arena() = default;

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  ~Arena();

  // 分配未对齐的字节块
  char* Allocate(size_t bytes);

  // 分配对齐的字节块（对齐到至少 sizeof(void*) 字节）
  char* AllocateAligned(size_t bytes);

  // 返回估计的总内存使用量（线程安全）
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;

  std::vector<char*> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace lsmdb
