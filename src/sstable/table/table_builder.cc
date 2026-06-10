#include "db/table_builder.h"

#include <cassert>

#ifdef LSMDB_HAVE_SNAPPY
#include <snappy.h>
#endif

#ifdef LSMDB_HAVE_ZSTD
#include <zstd.h>
#endif

#include "db/comparator.h"
#include "db/dbformat.h"
#include "db/env.h"
#include "db/filter_policy.h"
#include "db/options.h"
#include "sstable/table/block_builder.h"
#include "sstable/table/filter_block.h"
#include "sstable/table/format.h"
#include "utils/coding.h"
#include "utils/crc32c.h"

namespace lsmdb {

// 存储 TableBuilder 构建 SSTable 时的全局上下文，包括文件操作句柄、
// 各种 Block 组装器、物理偏移量计算器，以及用于优化索引项大小的延迟处理缓冲区。
struct TableBuilder::Rep {
  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        offset(0),
        data_block(&options),
        index_block(&index_block_options),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == nullptr
                         ? nullptr
                         : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) {
    // Index Block 关闭前缀压缩（重启间隔为 1），确保每个索引条目的 key
    // 都是完整存储的，从而保证二分查找可直接在索引条目上安全定位。
    index_block_options.block_restart_interval = 1;
  }

  Options options;
  Options index_block_options;
  WritableFile* file;        // 底层文件追加写入接口
  uint64_t offset;           // 当前文件已写入的字节数
  Status status;
  BlockBuilder data_block;   // 对用户数据进行前缀压缩的 BlockBuilder
  BlockBuilder index_block;  // 记录每个 Data Block 的缩短版隔离键及物理坐标
  std::string last_key;      // 上一个已写入的 Data Block 的最后一个 key
  int64_t num_entries;       // 已写入的 KV 记录总数
  bool closed;               // 状态机守护：确保 Finish() 或 Abandon() 仅调用一次
  FilterBlockBuilder* filter_block;  // Bloom Filter 生成器
  bool pending_index_entry;         // 标记上一个 Data Block 已落盘但索引项尚未写入
  BlockHandle pending_handle;       // 上一个已落盘 Data Block 的物理坐标

  std::string compressed_output;    // 复用缓冲区：压缩操作的临时输出目标，
                                    // 避免每次写块时重复分配内存
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {
  if (rep_->filter_block != nullptr) {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(rep_->closed);  // 捕获调用者忘记调用 Finish() 或 Abandon() 的错误
  delete rep_->filter_block;
  delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) {
  // 注意：如果 Options 新增了字段，应同步更新此函数，
  // 阻止在表构建中途变更不应变更的字段。
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgument(
        "changing comparator while building table");
  }

  // 注意：所有活跃的 BlockBuilder 内部持有对 rep_->options 的引用，
  // 因此后续的 Add/Flush 操作会自动使用新的选项。
  rep_->options = options;
  rep_->index_block_options = options;
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->num_entries > 0) {
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }

  if (r->pending_index_entry) {
    assert(r->data_block.empty());
    // 使用 FindShortestSeparator 缩短索引键，以减小 Index Block 体积
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    r->pending_handle.EncodeTo(&handle_encoding);
    r->index_block.Add(r->last_key, Slice(handle_encoding));
    r->pending_index_entry = false;
  }

  if (r->filter_block != nullptr) {
    // Use user key (without sequence number) for Bloom filter
    Slice user_key = ExtractUserKey(key);
    r->filter_block->AddKey(user_key);
  }

  r->last_key.assign(key.data(), key.size());
  r->num_entries++;
  r->data_block.Add(key, value);

  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  if (estimated_block_size >= r->options.block_size) {
    Flush();
  }
}

void TableBuilder::Flush() {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {
    r->pending_index_entry = true;
    r->status = r->file->Flush();
  }
  if (r->filter_block != nullptr) {
    r->filter_block->StartBlock(r->offset);
  }
}

// 将 BlockBuilder 中的内容压缩（可选）后写入文件。
// 物理布局：block_data + type(1byte) + crc(4byte)
void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  // 文件格式包含一系列块，每个块结构为：
  //   block_data: uint8[n]
  //   type:       uint8
  //   crc:        uint32
  assert(ok());
  Rep* r = rep_;
  Slice raw = block->Finish();

  Slice block_contents;
  CompressionType type = r->options.compression;
  switch (type) {
    case kNoCompression:
      block_contents = raw;
      break;

    case kSnappyCompression: {
#ifdef LSMDB_HAVE_SNAPPY
      std::string* compressed = &r->compressed_output;
      snappy::Compress(raw.data(), raw.size(), compressed);
      if (compressed->size() >= raw.size() - (raw.size() / 8u)) {
        // Snappy 压缩效果不佳，或压缩率未达 12.5% 以上，
        // 则存储未压缩形式
        block_contents = raw;
        type = kNoCompression;
      } else {
        block_contents = *compressed;
      }
#else
      // Snappy 不可用；存储未压缩版本
      block_contents = raw;
      type = kNoCompression;
#endif
      break;
    }

    case kZstdCompression: {
#ifdef LSMDB_HAVE_ZSTD
      std::string* compressed = &r->compressed_output;
      size_t max_dst = ZSTD_compressBound(raw.size());
      compressed->resize(max_dst);
      size_t rc = ZSTD_compress(&(*compressed)[0], max_dst, raw.data(),
                                raw.size(),
                                r->options.zstd_compression_level);
      if (ZSTD_isError(rc) ||
          rc >= raw.size() - (raw.size() / 8u)) {
        // Zstd 压缩效果不佳，或压缩率未达 12.5% 以上，
        // 则存储未压缩形式
        block_contents = raw;
        type = kNoCompression;
      } else {
        compressed->resize(rc);
        block_contents = *compressed;
      }
#else
      // Zstd 不可用；存储未压缩版本
      block_contents = raw;
      type = kNoCompression;
#endif
      break;
    }
  }
  WriteRawBlock(block_contents, type, handle);
  r->compressed_output.clear();
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents,
                                 CompressionType type, BlockHandle* handle) {
  Rep* r = rep_;
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  r->status = r->file->Append(block_contents);
  if (r->status.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    uint32_t crc =
        crc32c::Value(block_contents.data(), block_contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // 将 type 字节纳入 CRC 校验
    coding::EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
    if (r->status.ok()) {
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status TableBuilder::status() const { return rep_->status; }

Status TableBuilder::Finish() {
  Rep* r = rep_;
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // 写入 filter block
  if (ok() && r->filter_block != nullptr) {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression,
                  &filter_block_handle);
  }

  // 写入 metaindex block
  if (ok()) {
    BlockBuilder meta_index_block(&r->options);
    if (r->filter_block != nullptr) {
      // 添加从 "filter.<Name>" 到 filter 数据位置的映射
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(key, handle_encoding);
    }

    // TODO: 添加统计信息与其他 meta block
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  // 写入 index block
  if (ok()) {
    if (r->pending_index_entry) {
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, Slice(handle_encoding));
      r->pending_index_entry = false;
    }
    WriteBlock(&r->index_block, &index_block_handle);
  }

  // 写入 Footer
  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

void TableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const { return rep_->num_entries; }

uint64_t TableBuilder::FileSize() const { return rep_->offset; }

}  // namespace lsmdb
