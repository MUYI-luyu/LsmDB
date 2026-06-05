#include "sstable/table/format.h"

#include <cassert>
#include <memory>

#ifdef LSMDB_HAVE_SNAPPY
#include <snappy.h>
#endif

#ifdef LSMDB_HAVE_ZSTD
#include <zstd.h>
#endif

#include "db/env.h"
#include "db/options.h"
#include "utils/coding.h"
#include "utils/crc32c.h"

namespace lsmdb {

// ── BlockHandle ──────────────────────────────────────────────

void BlockHandle::EncodeTo(std::string* dst) const {
  // 健全性检查：所有字段都已设置
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  coding::PutVarint64(dst, offset_);
  coding::PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (coding::GetVarint64(input, &offset_) &&
      coding::GetVarint64(input, &size_)) {
    return Status::OK();
  }
  return Status::Corruption("bad block handle");
}

// ── Footer ───────────────────────────────────────────────────

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(2 * BlockHandle::kMaxEncodedLength);  // Padding
  coding::PutFixed32(dst,
                     static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  coding::PutFixed32(dst,
                     static_cast<uint32_t>(kTableMagicNumber >> 32));
  assert(dst->size() == original_size + kEncodedLength);
  (void)original_size;  // Disable unused variable warning.
}

Status Footer::DecodeFrom(Slice* input) {
  if (input->size() < kEncodedLength) {
    return Status::Corruption("not an sstable (footer too short)");
  }

  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint32_t magic_lo = coding::DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = coding::DecodeFixed32(magic_ptr + 4);
  const uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                          (static_cast<uint64_t>(magic_lo)));
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }

  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) {
    result = index_handle_.DecodeFrom(input);
  }
  if (result.ok()) {
    // 跳过 "input" 中的任何剩余数据（目前只是填充）
    const char* end = magic_ptr + 8;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}

// ── ReadBlock ────────────────────────────────────────────────

Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result) {
  result->data = Slice();
  result->cachable = false;
  result->heap_allocated = false;

  // 读取块内容及类型/CRC 尾部信息。
  // 构建此结构的代码请参见 table_builder.cc。
  size_t n = static_cast<size_t>(handle.size());
  auto buf = std::unique_ptr<char[]>(new char[n + kBlockTrailerSize]);
  Slice contents;
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents,
                        buf.get());
  if (!s.ok()) {
    return s;
  }
  if (contents.size() != n + kBlockTrailerSize) {
    return Status::Corruption("truncated block read");
  }

  // 检查类型和块内容的 CRC
  const char* data = contents.data();  // Pointer to where Read put the data
  if (options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(coding::DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      return Status::Corruption("block checksum mismatch");
    }
  }

  switch (data[n]) {
    case kNoCompression:
      if (data != buf.get()) {
        // 文件实现拥有数据缓冲区（例如 mmap）。
        // 我们的暂存缓冲区通过 unique_ptr 自动释放。
        result->data = Slice(data, n);
        result->heap_allocated = false;
        result->cachable = false;  // Do not double-cache
      } else {
        result->data = Slice(buf.release(), n);
        result->heap_allocated = true;
        result->cachable = true;
      }
      break;

    case kSnappyCompression: {
#ifdef LSMDB_HAVE_SNAPPY
      size_t ulength = 0;
      if (!snappy::GetUncompressedLength(data, n, &ulength)) {
        return Status::Corruption(
            "corrupted snappy compressed block length");
      }
      auto ubuf = std::unique_ptr<char[]>(new char[ulength]);
      if (!snappy::RawUncompress(data, n, ubuf.get())) {
        return Status::Corruption(
            "corrupted snappy compressed block contents");
      }
      // buf（压缩数据）通过 unique_ptr 析构函数自动释放
      result->data = Slice(ubuf.release(), ulength);
      result->heap_allocated = true;
      result->cachable = true;
#else
      // Snappy 不可用，视为损坏
      return Status::Corruption("snappy compression not supported");
#endif
      break;
    }

    case kZstdCompression: {
#ifdef LSMDB_HAVE_ZSTD
      unsigned long long ulength = ZSTD_getFrameContentSize(data, n);
      if (ulength == ZSTD_CONTENTSIZE_ERROR ||
          ulength == ZSTD_CONTENTSIZE_UNKNOWN) {
        return Status::Corruption(
            "corrupted zstd compressed block length");
      }
      auto ubuf = std::unique_ptr<char[]>(new char[ulength]);
      size_t rc = ZSTD_decompress(ubuf.get(), ulength, data, n);
      if (ZSTD_isError(rc)) {
        return Status::Corruption(
            "corrupted zstd compressed block contents");
      }
      // buf（压缩数据）通过 unique_ptr 析构函数自动释放
      result->data = Slice(ubuf.release(), ulength);
      result->heap_allocated = true;
      result->cachable = true;
#else
      // Zstd 不可用，视为损坏
      return Status::Corruption("zstd compression not supported");
#endif
      break;
    }

    default:
      return Status::Corruption("bad block type");
  }

  return Status::OK();
}

}  // namespace lsmdb
