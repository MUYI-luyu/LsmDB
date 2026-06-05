#pragma once

#include <cstdint>
#include <vector>

#include "db/slice.h"

namespace lsmdb {

struct Options;

class BlockBuilder {
 public:
  explicit BlockBuilder(const Options* options);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // 重置内容，就像 BlockBuilder 刚刚被构造一样。
  void Reset();

  // 要求：自上次调用 Reset() 以来，Finish() 未被调用。
  // 要求：key 大于任何之前添加的 key
  void Add(const Slice& key, const Slice& value);

  // 完成块的构建并返回一个指向块内容的 slice。返回的 slice 将在此
  // 构建器的生命周期内或直到调用 Reset() 时保持有效。
  [[nodiscard]] Slice Finish();

  // 返回我们正在构建的块的当前（未压缩）大小的估计值。
  [[nodiscard]] size_t CurrentSizeEstimate() const noexcept;

  // 如果自上次调用 Reset() 以来没有添加任何条目，则返回 true
  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

 private:
  const Options* options_;
  std::string buffer_;              // 目标缓冲区
  std::vector<uint32_t> restarts_;  // 重启点偏移数组
  int counter_;                     // 自上次重启以来发出的条目数
  bool finished_;                   // Finish() 是否已被调用？
  std::string last_key_;
};

}  // namespace lsmdb
