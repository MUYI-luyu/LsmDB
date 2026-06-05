// BlockBuilder 生成块，其中的键是前缀压缩的：
//
// 当我们存储一个键时，我们丢弃了与上一个字符串共享的前缀。
// 这有助于显著减少空间需求。
// 此外，一旦每 K 个键，我们就不应用前缀压缩，而是存储整个键。
// 本项目将此称为"重启点"。块的末尾存储所有重启点的偏移量，
// 可以用于在搜索特定键时执行二分查找。值以原样形式存储（未压缩），
// 紧跟在相应键的后面。
//
// 特定键值对的条目形式为：
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// 对于重启点，shared_bytes == 0。
//
// 块的括号形式为：
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] 包含块内第 i 个重启点的偏移量。

#include "sstable/table/block_builder.h"

#include <algorithm>
#include <cassert>

#include "db/comparator.h"
#include "db/options.h"
#include "utils/coding.h"

namespace lsmdb {

BlockBuilder::BlockBuilder(const Options* options)
    : options_(options), restarts_(), counter_(0), finished_(false) {
  assert(options->block_restart_interval >= 1);
  restarts_.push_back(0);  // 第一个重启点在偏移量 0 处
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // 第一个重启点在偏移量 0 处
  last_key_.clear();
  counter_ = 0;
  finished_ = false;
}

size_t BlockBuilder::CurrentSizeEstimate() const noexcept {
  return (buffer_.size() +                       // 原始数据缓冲区
          restarts_.size() * sizeof(uint32_t) +  // 重启数组
          sizeof(uint32_t));                     // 重启数组长度
}

Slice BlockBuilder::Finish() {
  // 附加重启数组
  for (size_t i = 0; i < restarts_.size(); i++) {
    coding::PutFixed32(&buffer_, restarts_[i]);
  }
  coding::PutFixed32(&buffer_, restarts_.size());
  finished_ = true;
  return Slice(buffer_);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  Slice last_key_piece(last_key_);
  assert(!finished_);
  assert(counter_ <= options_->block_restart_interval);
  assert(buffer_.empty()  // 还没有值吗？
         || options_->comparator->Compare(key, last_key_piece) > 0);
  size_t shared = 0;
  if (counter_ < options_->block_restart_interval) {
    // 计算与上一个 key 共享的前缀长度
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  } else {
    // 达到重启间隔：在此处设置一个新的重启点
    restarts_.push_back(buffer_.size());
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  // 向 buffer_ 写入 "<shared><non_shared><value_size>"
  coding::PutVarint32(&buffer_, shared);
  coding::PutVarint32(&buffer_, non_shared);
  coding::PutVarint32(&buffer_, value.size());

  // 写入 key 的差分部分（非共享部分），紧接着写入 value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // 更新 last_key_ 状态
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  counter_++;
}

}  // namespace lsmdb
