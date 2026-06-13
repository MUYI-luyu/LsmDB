#pragma once

#include <cstdint>

#include "db/slice.h"
#include "db/status.h"
#include "wal/log_format.h"

namespace lsmdb {

class WritableFile;

namespace log {

class Writer {
 public:
  // 创建一个将数据追加到 "*dest" 的 writer。
  // "*dest" 必须初始为空。
  // 在使用本 Writer 期间，"*dest" 必须保持活跃。
  explicit Writer(WritableFile* dest);

  // 创建一个将数据追加到 "*dest" 的 writer。
  // "*dest" 必须具有初始长度 "dest_length"。
  // 在使用本 Writer 期间，"*dest" 必须保持活跃。
  Writer(WritableFile* dest, uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer();

  Status AddRecord(const Slice& slice);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_;  // 块中的当前偏移量

  // 所有支持的记录类型的 crc32c 值。这些值是
  // 预先计算的，以减少计算存储在头部的
  // 记录类型 crc 的开销。
  uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace log
}  // namespace lsmdb

