#pragma once

#include <string>
#include "db/slice.h"
#include "utils/hash.h"

namespace lsmdb {

class FilterPolicy {
 public:
  // 显式构造，默认推荐 bits_per_key = 10 (约 1% 误判率)
  explicit FilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    k_ = static_cast<size_t>(bits_per_key * 0.69); // 0.69 =~ ln(2)
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  ~FilterPolicy() = default;

  // 返回策略名称
  const char* Name() const { return "lsmdb.BuiltinBloomFilter"; }

  // 构建布隆过滤器并追加到 dst 末尾
  void CreateFilter(const Slice* keys, int n, std::string* dst) const {
    size_t bits = n * bits_per_key_;
    if (bits < 64) bits = 64; // 防止小数据量下误判率飙升

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_)); // 将哈希函数个数焊在末尾

    char* array = &(*dst)[init_size];
    for (int i = 0; i < n; i++) {
      // 双重哈希模拟，优化探测性能
      uint32_t h = Hash(keys[i].data(), keys[i].size(), 0xbc9f1d34);
      const uint32_t delta = (h >> 17) | (h << 15); 
      for (size_t j = 0; j < k_; j++) {
        const uint32_t bitpos = h % bits;
        array[bitpos / 8] |= (1 << (bitpos % 8));
        h += delta;
      }
    }
  }

  // 检查 Key 是否可能存在（读路径核心拦截）
  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const {
    const size_t len = bloom_filter.size();
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    const size_t bits = (len - 1) * 8;
    const size_t k = array[len - 1]; // 从尾部提取 k 值
    if (k > 30) return true;

    uint32_t h = Hash(key.data(), key.size(), 0xbc9f1d34);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }

 private:
  size_t bits_per_key_;
  size_t k_;
};

}  // namespace lsmdb