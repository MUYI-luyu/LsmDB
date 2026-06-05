#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>

namespace lsmdb {

class Slice {
 public:
  // -- 构造系列 --

  Slice() noexcept : data_(""), size_(0) {}

  Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}

  Slice(const char* s) noexcept : data_(s), size_(s ? std::strlen(s) : 0) {}

  Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}

  // 现代化：支持从 std::string_view 隐式构造
  Slice(std::string_view sv) noexcept : data_(sv.data()), size_(sv.size()) {}

  Slice(const Slice&) = default;
  Slice& operator=(const Slice&) = default;

  // -- 访问器 --

  const char* data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

  const char* begin() const noexcept { return data(); }
  const char* end() const noexcept { return data() + size(); }

  char operator[](size_t n) const noexcept { return data_[n]; }

  // -- 转换 --

  // 零开销转换到 std::string_view
  std::string_view ToStringView() const noexcept {
    return std::string_view(data_, size_);
  }

  // 拷贝到 std::string
  std::string ToString() const {
    return std::string(data_, size_);
  }

  // -- 切片操作 --

  // 移除前 n 个字节（原地修改）
  void remove_prefix(size_t n) noexcept {
    data_ += n;
    size_ -= n;
  }

  // 安全子切片（不原地修改，返回新 Slice）
  Slice SubSlice(size_t pos, size_t n = npos) const noexcept {
    pos = std::min(pos, size_);
    n = std::min(n, size_ - pos);
    return Slice(data_ + pos, n);
  }

  // -- 比较 --

  // 三路比较
  int Compare(const Slice& b) const noexcept {
    size_t min_len = std::min(size_, b.size_);
    int r = std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) return -1;
      if (size_ > b.size_) return 1;
    }
    return r;
  }

  // 判断是否以指定前缀开头
  bool starts_with(const Slice& x) const noexcept {
    return size_ >= x.size_ &&
           std::memcmp(data_, x.data_, x.size_) == 0;
  }

  // -- 运算符重载 --

  bool operator<(const Slice& rhs) const noexcept { return Compare(rhs) < 0; }

  bool operator==(const Slice& rhs) const noexcept {
    return size_ == rhs.size_ &&
           std::memcmp(data_, rhs.data_, size_) == 0;
  }

  bool operator!=(const Slice& rhs) const noexcept { return !(*this == rhs); }

 private:
  const char* data_;
  size_t size_;

  static constexpr size_t npos = static_cast<size_t>(-1);
};

}  // namespace lsmdb
