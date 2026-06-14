#include "wal/log_writer.h"

#include <cassert>
#include <cstdint>

#include "db/env.h"
#include "utils/coding.h"
#include "utils/crc32c.h"

namespace lsmdb {

using namespace coding;

namespace log {

static void InitTypeCrc(uint32_t* type_crc) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc[i] = crc32c::Value(&t, 1);
  }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
  InitTypeCrc(type_crc_);
}

Writer::~Writer() = default;

Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t slice_left = slice.size();

  // 如果必要，分片记录并发出它。如果 slice 为空，
  // 仍然希望迭代一次以发出单个零长度记录
  Status s;
  bool begin = true;
  do {
    const int block_left = kBlockSize - block_offset_;
    assert(block_left >= 0);
    if (block_left < kHeaderSize) {
      // 切换到新块
      if (block_left > 0) {
        // 填充尾部（下面的字面值依赖于 kHeaderSize 为 7）
        static_assert(kHeaderSize == 7, "");
        dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", block_left));
      }
      block_offset_ = 0;
    }

    // 不变量：块中永远不会留下少于 kHeaderSize 字节。
    assert(kBlockSize - block_offset_ - kHeaderSize >= 0);
    // avail: 计算当前 32KB 抽屉里，扣除 7 字节封条后，还能塞多少纯数据
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    // 本次写入长度 min(slice_left, avail)
    const size_t fragment_length = (slice_left < avail) ? slice_left : avail; 

    RecordType type;
    const bool end = (slice_left == fragment_length);
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    slice_left -= fragment_length;
    begin = false;
  } while (s.ok() && slice_left > 0);
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr,
                                  size_t length) {
  assert(length <= 0xffff);  // 必须放入两个字节
  assert(block_offset_ + kHeaderSize + length <= kBlockSize);

  // 格式化头
  char buf[kHeaderSize];
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  buf[6] = static_cast<char>(t);

  // 计算记录类型和有效载荷的校验和。
  uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
  crc = crc32c::Mask(crc);  // 调整以供存储
  EncodeFixed32(buf, crc);

  // 写入头和有效载荷
  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  block_offset_ += kHeaderSize + length;
  return s;
}

}  // namespace log
}  // namespace lsmdb

