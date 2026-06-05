#pragma once

#include <cstdint>
#include <string>

#include "db/slice.h"
#include "db/status.h"

namespace lsmdb {

class Block;
class RandomAccessFile;
struct ReadOptions;

// BlockHandle 是指向存储数据块或元块的文件范围的指针。
class BlockHandle {
 public:
  // Maximum encoding length of a BlockHandle
  static constexpr int kMaxEncodedLength = 10 + 10;

  BlockHandle();

  // The offset of the block in the file.
  [[nodiscard]] uint64_t offset() const noexcept { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  // The size of the stored block
  [[nodiscard]] uint64_t size() const noexcept { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  uint64_t offset_;
  uint64_t size_;
};

// Footer encapsulates the fixed information stored at the tail
// end of every table file.
class Footer {
 public:
  // Encoded length of a Footer.  Note that the serialization of a
  // Footer will always occupy exactly this many bytes.  It consists
  // of two block handles and a magic number.
  static constexpr int kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  Footer() = default;

  // The block handle for the metaindex block of the table
  [[nodiscard]] const BlockHandle& metaindex_handle() const noexcept {
    return metaindex_handle_;
  }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  // The block handle for the index block of the table
  [[nodiscard]] const BlockHandle& index_handle() const noexcept {
    return index_handle_;
  }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// kTableMagicNumber was picked by running
//    echo LsmDB | sha1sum
// and taking the leading 64 bits.
static constexpr uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 1-byte type + 32-bit crc
static constexpr size_t kBlockTrailerSize = 5;

struct BlockContents {
  Slice data;           // Actual contents of data
  bool cachable;        // True iff data can be cached
  bool heap_allocated;  // True iff caller should delete[] data.data()
};

// Read the block identified by "handle" from "file".  On failure
// return non-OK.  On success fill *result and return OK.
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

// Implementation details follow.  Clients should ignore,

inline BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace lsmdb
