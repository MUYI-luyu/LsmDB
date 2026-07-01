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
  // BlockHandle 编码的最大长度
  static constexpr int kMaxEncodedLength = 10 + 10;

  BlockHandle();

  // 文件中该块的偏移量。
  [[nodiscard]] uint64_t offset() const noexcept { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  // 存储块的大小。
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
  // Footer 的编码长度。注意，Footer 的序列化总是占用恰好这么多字节。
  // 它由两个 BlockHandle 和一个魔数组成。
  static constexpr int kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  Footer() = default;

  // 表的 metaindex 块对应的 BlockHandle
  [[nodiscard]] const BlockHandle& metaindex_handle() const noexcept {
    return metaindex_handle_;
  }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  // 表的 index 块对应的 BlockHandle
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

// kTableMagicNumber 是通过运行
//    echo LsmDB | sha1sum
// 并取其前 64 位生成的。
static constexpr uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 1 字节类型 + 32 位 CRC
static constexpr size_t kBlockTrailerSize = 5;

struct BlockContents {
  Slice data;           // 数据的实际内容
  bool cachable;        // 数据是否可缓存 
  bool heap_allocated;  // 调用者是否应 delete[] data.data()
};

// 从 `file` 中读取由 `handle` 指定的块。失败返回非 OK；成功时填充 *result 并返回 OK。
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

inline BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace lsmdb
