#include "utils/coding.h"

namespace lsmdb {
namespace coding {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

char* EncodeVarint32(char* dst, uint32_t v) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  static constexpr int kB = 128;
  if (v < (1 << 7)) {
    *(ptr++) = static_cast<uint8_t>(v);
  } else if (v < (1 << 14)) {
    *(ptr++) = static_cast<uint8_t>(v | kB);
    *(ptr++) = static_cast<uint8_t>(v >> 7);
  } else if (v < (1 << 21)) {
    *(ptr++) = static_cast<uint8_t>(v | kB);
    *(ptr++) = static_cast<uint8_t>((v >> 7) | kB);
    *(ptr++) = static_cast<uint8_t>(v >> 14);
  } else if (v < (1 << 28)) {
    *(ptr++) = static_cast<uint8_t>(v | kB);
    *(ptr++) = static_cast<uint8_t>((v >> 7) | kB);
    *(ptr++) = static_cast<uint8_t>((v >> 14) | kB);
    *(ptr++) = static_cast<uint8_t>(v >> 21);
  } else {
    *(ptr++) = static_cast<uint8_t>(v | kB);
    *(ptr++) = static_cast<uint8_t>((v >> 7) | kB);
    *(ptr++) = static_cast<uint8_t>((v >> 14) | kB);
    *(ptr++) = static_cast<uint8_t>((v >> 21) | kB);
    *(ptr++) = static_cast<uint8_t>(v >> 28);
  }
  return reinterpret_cast<char*>(ptr);
}

void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  char* ptr = EncodeVarint32(buf, v);
  dst->append(buf, ptr - buf);
}

char* EncodeVarint64(char* dst, uint64_t v) {
  static constexpr int kB = 128;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= kB) {
    *(ptr++) = static_cast<uint8_t>(v | kB);
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  char* ptr = EncodeVarint64(buf, v);
  dst->append(buf, ptr - buf);
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    ++len;
  }
  return len;
}

const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = static_cast<uint8_t>(*p);
    ++p;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = static_cast<uint8_t>(*p);
    ++p;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len;
  if (GetVarint32(input, &len) && input->size() >= len) {
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
  }
  return false;
}

}  // namespace coding
}  // namespace lsmdb
