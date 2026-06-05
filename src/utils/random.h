#pragma once

#include <cstdint>

namespace lsmdb {

// 一个非常简单的伪随机数生成器。
// 虽然不是特别擅长生成真正的随机位，但足以满足本模块的需求。
class Random {
 private:
  uint32_t seed_;

 public:
  explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
    // 避免不良种子值
    if (seed_ == 0 || seed_ == 2147483647L) {
      seed_ = 1;
    }
  }
  uint32_t Next() {
    static const uint32_t M = 2147483647L;  // 2^31-1
    static const uint64_t A = 16807;        // bits 14, 8, 7, 5, 2, 1, 0
    // 计算: seed_ = (seed_ * A) % M,  其中 M = 2^31-1
    // seed_ 不能为 0 或 M，否则所有后续计算值将分别为 0 或 M。
    // 对于所有其他值，seed_ 将循环遍历 [1, M-1] 中的每个数字。
    uint64_t product = seed_ * A;

    // 利用事实 ((x << 31) % M) == x 计算 (product % M)
    seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
    // 第一次约减可能溢出 1 位，因此可能需要重复。
    // mod == M 不可能；使用 > 允许基于符号位的更快测试。
    if (seed_ > M) {
      seed_ -= M;
    }
    return seed_;
  }
  // 返回 [0..n-1] 范围内的均匀分布值
  // 需求: n > 0
  uint32_t Uniform(int n) { return Next() % n; }

  // 以约 "1/n" 的概率返回 true，否则返回 false。
  // 需求: n > 0
  bool OneIn(int n) { return (Next() % n) == 0; }

  // 偏斜：从范围 [0,max_log] 中均匀选择 "base"，然后
  // 返回 "base" 个随机位。效果是从范围 [0,2^max_log-1] 中以
  // 指数偏差偏向较小值的方式选取一个数。
  uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }
};

}  // namespace lsmdb

