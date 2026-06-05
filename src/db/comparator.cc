#include "db/comparator.h"

#include <cstdint>
#include <string>

namespace lsmdb {

namespace {

class BytewiseComparatorImpl : public Comparator {
 public:
  BytewiseComparatorImpl() = default;

  const char* Name() const override { return "lsmdb.BytewiseComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
    return a.Compare(b);
  }

  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override {
    // 查找公共前缀长度
    size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           ((*start)[diff_index] == limit[diff_index])) {
      diff_index++;
    }

    if (diff_index >= min_length) {
      // 如果一个字符串是另一个的前缀，则不做任何操作
    } else {
      uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
      uint8_t limit_byte = static_cast<uint8_t>(limit[diff_index]);
      if (diff_byte < limit_byte) {
        // 尝试找到介于 start 和 limit 之间的分隔符
        // 通过递增第一个不同的字节
        if (diff_byte < 0xff && diff_byte + 1 < limit_byte) {
          (*start)[diff_index]++;
          start->resize(diff_index + 1);
        }
      }
    }
  }

  void FindShortSuccessor(std::string* key) const override {
    // 找到第一个可以递增的字节
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
      const uint8_t byte = (*key)[i];
      if (byte != static_cast<uint8_t>(0xff)) {
        (*key)[i] = byte + 1;
        key->resize(i + 1);
        return;
      }
    }
    // *key 全是 0xff 字节，直接保留原样。
  }
};

}  // namespace

const Comparator* BytewiseComparator() {
  static const BytewiseComparatorImpl impl;
  return &impl;
}

}  // namespace lsmdb
