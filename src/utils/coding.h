#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "db/slice.h"

namespace lsmdb {
namespace coding {


// ---------- 追加写入（std::string 尾部） ----------

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

// ---------- 从 Slice 解码（input 指针前移） ----------

bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// ---------- 低级指针操作（无边界检查，高性能） ----------

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* v);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v);

int VarintLength(uint64_t v);

char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

// ---------- 内联定长编解码 ----------

inline void EncodeFixed32(char* dst, uint32_t value) {
  uint8_t* buf = reinterpret_cast<uint8_t*>(dst);
  buf[0] = static_cast<uint8_t>(value);
  buf[1] = static_cast<uint8_t>(value >> 8);
  buf[2] = static_cast<uint8_t>(value >> 16);
  buf[3] = static_cast<uint8_t>(value >> 24);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  uint8_t* buf = reinterpret_cast<uint8_t*>(dst);
  buf[0] = static_cast<uint8_t>(value);
  buf[1] = static_cast<uint8_t>(value >> 8);
  buf[2] = static_cast<uint8_t>(value >> 16);
  buf[3] = static_cast<uint8_t>(value >> 24);
  buf[4] = static_cast<uint8_t>(value >> 32);
  buf[5] = static_cast<uint8_t>(value >> 40);
  buf[6] = static_cast<uint8_t>(value >> 48);
  buf[7] = static_cast<uint8_t>(value >> 56);
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint32_t>(buf[0])) |
         (static_cast<uint32_t>(buf[1]) << 8) |
         (static_cast<uint32_t>(buf[2]) << 16) |
         (static_cast<uint32_t>(buf[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint64_t>(buf[0])) |
         (static_cast<uint64_t>(buf[1]) << 8) |
         (static_cast<uint64_t>(buf[2]) << 16) |
         (static_cast<uint64_t>(buf[3]) << 24) |
         (static_cast<uint64_t>(buf[4]) << 32) |
         (static_cast<uint64_t>(buf[5]) << 40) |
         (static_cast<uint64_t>(buf[6]) << 48) |
         (static_cast<uint64_t>(buf[7]) << 56);
}

// ---------- 内部回退路径（为 GetVarint32Ptr 优化服务） ----------

const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value);

// 快速路径：单字节优化，避免函数调用
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
  if (p < limit) {
    uint32_t result = static_cast<uint8_t>(*p);
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}

}  // namespace coding
}  // namespace lsmdb
