#pragma once

#include <cstddef>
#include <cstdint>

#include "db/iterator.h"

namespace lsmdb {

struct BlockContents;
class Comparator;

class Block {
 public:
  // 使用指定的内容初始化块。
  // Block 从 BlockContents::heap_allocated 获取 data_ 的所有权语义，
  // 与 format.h 中定义的 BlockContents 状态机严格对接。
  explicit Block(const BlockContents& contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  ~Block();

  [[nodiscard]] size_t size() const noexcept { return size_; }
  [[nodiscard]] Iterator* NewIterator(const Comparator* comparator);

 private:
  class Iter;

  uint32_t NumRestarts() const;

  const char* data_;
  size_t size_;
  uint32_t restart_offset_;  // data_ 中重启数组的偏移量
  bool owned_;               // 当 heap_allocated == true 时，块拥有 data_，析构时需 delete[]
};

}  // namespace lsmdb
