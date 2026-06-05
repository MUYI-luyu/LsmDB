#pragma once

#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>

#include "db/slice.h"

namespace lsmdb {

// 操作结果状态封装，用于标识操作的成功/失败及错误信息。
class Status {
 public:
  // 成功状态
  Status() noexcept : state_(nullptr) {}

  ~Status() { delete[] state_; }

  Status(const Status& rhs) : state_(rhs.state_ ? CopyState(rhs.state_) : nullptr) {}
  Status& operator=(const Status& rhs) {
    if (state_ != rhs.state_) {
      delete[] state_;
      state_ = rhs.state_ ? CopyState(rhs.state_) : nullptr;
    }
    return *this;
  }

  Status(Status&& rhs) noexcept : state_(rhs.state_) { rhs.state_ = nullptr; }
  Status& operator=(Status&& rhs) noexcept {
    std::swap(state_, rhs.state_);
    return *this;
  }

  static Status OK() { return Status(); }

  static Status NotFound(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kNotFound, msg, msg2);
  }

  static Status Corruption(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kCorruption, msg, msg2);
  }

  static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kNotSupported, msg, msg2);
  }

  static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kInvalidArgument, msg, msg2);
  }

  static Status IOError(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kIOError, msg, msg2);
  }

  bool ok() const { return state_ == nullptr; }
  bool IsNotFound() const { return code() == kNotFound; }

  std::string ToString() const;

 private:
  enum Code {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5
  };

  Code code() const {
    return (state_ == nullptr) ? kOk : static_cast<Code>(state_[4]);
  }

  Status(Code code, const Slice& msg, const Slice& msg2);
  static const char* CopyState(const char* s);

  // state_ 编码：
  //   [0..3]  = message 长度（大端 uint32）
  //   [4]     = code
  //   [5..]   = message 内容
  const char* state_;
};

inline std::string Status::ToString() const {
  if (state_ == nullptr) return "OK";
  const char* type;
  switch (code()) {
    case kOk:         return "OK";
    case kNotFound:   type = "NotFound: "; break;
    case kCorruption: type = "Corruption: "; break;
    case kNotSupported:  type = "Not implemented: "; break;
    case kInvalidArgument: type = "Invalid argument: "; break;
    case kIOError:    type = "IO error: "; break;
    default:          type = "Unknown: "; break;
  }
  uint32_t len;
  memcpy(&len, state_, sizeof(len));
  return std::string(type) + std::string(state_ + 5, len);
}

inline Status::Status(Code code, const Slice& msg, const Slice& msg2) {
  size_t len1 = msg.size();
  size_t len2 = msg2.size();
  size_t size = len1 + (len2 ? (2 + len2) : 0);
  char* result = new char[size + 5];
  uint32_t encoded_len = static_cast<uint32_t>(size);
  memcpy(result, &encoded_len, sizeof(encoded_len));
  result[4] = static_cast<char>(code);
  memcpy(result + 5, msg.data(), len1);
  if (len2) {
    result[5 + len1] = ':';
    result[6 + len1] = ' ';
    memcpy(result + 7 + len1, msg2.data(), len2);
  }
  state_ = result;
}

inline const char* Status::CopyState(const char* s) {
  uint32_t len;
  memcpy(&len, s, sizeof(len));
  char* result = new char[len + 5];
  memcpy(result, s, len + 5);
  return result;
}

}  // namespace lsmdb
