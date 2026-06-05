#pragma once

#include <string>

#include "db/slice.h"

namespace lsmdb {

// 键比较器的抽象基类。必须保证线程安全。
class Comparator {
 public:
  virtual ~Comparator() = default;

  // 三路比较：
  //   < 0  iff a < b
  //   == 0 iff a == b
  //   > 0  iff a > b
  virtual int Compare(const Slice& a, const Slice& b) const = 0;

  // 比较器名称，用于校验一致性
  virtual const char* Name() const = 0;

  // 如果 *start < limit，将 *start 修改为 [start, limit) 区间内的一个尽可能短的字符串。
  // 简单实现可以直接 return，什么也不做。
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const = 0;

  // 将 *key 修改为一个 >= *key 的尽可能短的字符串。
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// 返回一个内置的字节序逐字节比较器（字典序）。
const Comparator* BytewiseComparator();

}  // namespace lsmdb
