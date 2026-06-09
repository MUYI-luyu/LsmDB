// FilterBlock 存储在 Table 文件的尾部附近。它将表中所有数据块的 filter
// （例如 Bloom filter）合并为一个完整的 filter block。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "db/slice.h"
#include "utils/hash.h"

namespace lsmdb {

class FilterPolicy;

// FilterBlockBuilder 用于构造特定 Table 的所有 filter。
// 它生成一个单一的字符串，作为 Table 中的一个特殊 block 存储。
// 对 FilterBlockBuilder 的调用序列必须满足正则表达式：
//      (StartBlock AddKey*)* Finish
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy*);

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  void StartBlock(uint64_t block_offset);
  void AddKey(const Slice& key);
  [[nodiscard]] Slice Finish();

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;
  std::string keys_;                // 展平的键内容（所有键拼接）
  std::vector<size_t> start_;       // 每个键在 keys_ 中的起始索引
  std::string result_;              // 目前已计算的 filter 数据
  std::vector<Slice> tmp_keys_;     // policy_->CreateFilter() 的参数
  std::vector<uint32_t> filter_offsets_;  // 每个 filter 在 result_ 中的偏移量
};

// FilterBlockReader 用于从已持久化的 filter block 中查询 key 是否可能匹配。
class FilterBlockReader {
 public:
  // 要求："contents" 和 *policy 必须在 *this 的生命周期内保持有效。
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents);

  // 检查指定 block_offset 对应的 filter 中是否可能包含该 key。
  [[nodiscard]] bool KeyMayMatch(uint64_t block_offset, const Slice& key);

 private:
  const FilterPolicy* policy_;
  const char* data_;     // filter 数据起始指针（block 头部）
  const char* offset_;   // offset 数组起始指针（block 尾部）
  size_t num_;           // offset 数组的条目数
  size_t base_lg_;       // 编码参数（见 .cc 中的 kFilterBaseLg）
};

}  // namespace lsmdb
