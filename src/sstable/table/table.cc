#include "db/table.h"

#include "db/cache.h"
#include "db/comparator.h"
#include "db/dbformat.h"
#include "db/env.h"
#include "db/filter_policy.h"
#include "db/options.h"
#include "sstable/table/block.h"
#include "sstable/table/filter_block.h"
#include "sstable/table/format.h"
#include "sstable/iterator/two_level_iterator.h"
#include "utils/coding.h"

namespace lsmdb {

struct Table::Rep {
  ~Rep() {
    delete filter;
    delete[] filter_data;
    delete index_block;
  }

  Options options;
  Status status;
  RandomAccessFile* file;
  uint64_t cache_id;
  FilterBlockReader* filter;
  const char* filter_data;

  BlockHandle metaindex_handle;  // Meta Index Block 在磁盘文件中的物理坐标
  Block* index_block;
};

// 打开一个指定的 SSTable 文件，通过读取其末尾的 Footer 并加载
// 核心数据索引块到内存中，初始化并返回一个只读的 Table 对象。
Status Table::Open(const Options& options, RandomAccessFile* file,
                   uint64_t size, Table** table) {
  *table = nullptr;
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // 读取 Index Block
  BlockContents index_block_contents;
  ReadOptions opt;
  if (options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);

  if (s.ok()) {
    // 成功读取 Footer 和 Index Block：准备初始化只读 Table 对象
    Block* index_block = new Block(index_block_contents);
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;
    rep->cache_id =
        (options.block_cache ? options.block_cache->NewId() : 0);
    rep->filter_data = nullptr;
    rep->filter = nullptr;
    *table = new Table(rep);
    (*table)->ReadMeta(footer);
  }

  return s;
}

// 根据 Footer 提供的坐标读取 Meta Index Block，并在其中通过
// filter 策略名称查找对应的 Bloom Filter 块的磁盘坐标。
void Table::ReadMeta(const Footer& footer) {
  if (rep_->options.filter_policy == nullptr) {
    return;  // 无需任何元数据
  }

  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents contents;
  if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents)
           .ok()) {
    // 不传播错误，因为元数据对于基础读取操作来说并非必不可少
    return;
  }
  Block* meta = new Block(contents);

  Iterator* iter = meta->NewIterator(BytewiseComparator());
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  iter->Seek(key);
  if (iter->Valid() && iter->key() == Slice(key)) {
    ReadFilter(iter->value());
  }
  delete iter;
  delete meta;
}

// 接收 Bloom Filter 块的磁盘坐标并反序列化，发起物理 I/O 将
// filter 位数组整体读入堆内存，并绑定到 FilterBlockReader 上供后续点查熔断使用。
void Table::ReadFilter(const Slice& filter_handle_value) {
  Slice v = filter_handle_value;
  BlockHandle filter_handle;
  if (!filter_handle.DecodeFrom(&v).ok()) {
    return;
  }

  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents block;
  if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
    return;
  }
  if (block.heap_allocated) {
    rep_->filter_data = block.data.data();  // 后续析构时需释放
  }
  rep_->filter =
      new FilterBlockReader(rep_->options.filter_policy, block.data);
}

Table::~Table() { delete rep_; }

// 专门用来销毁堆上的 Block（数据块/索引块）对象
static void DeleteBlock(void* arg, void* /*ignored*/) {
  delete reinterpret_cast<Block*>(arg);
}

// 用作全局块缓存（Block Cache）的逐出销毁回调
static void DeleteCachedBlock(const Slice& /*key*/, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

// 用于减少全局块缓存（Block Cache）中某个条目的引用计数
static void ReleaseBlock(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

// 将一个索引迭代器的值（即编码的 BlockHandle）转换为对相应块内容的迭代器。
Iterator* Table::BlockReader(void* arg, const ReadOptions& options,
                             const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  // 有意在索引值中预留了一些富余空间，以便未来添加更多特性

  if (s.ok()) {
    BlockContents contents;
    if (block_cache != nullptr) {  // 系统配置了全局缓存
      char cache_key_buffer[16];
      coding::EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      coding::EncodeFixed64(cache_key_buffer + 8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {  // 缓存命中
        block = reinterpret_cast<Block*>(
            block_cache->Value(cache_handle));
      } else {
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(
                key, block, block->size(), &DeleteCachedBlock);
          }
        }
      }
    } else {
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }

  Iterator* iter;
  if (block != nullptr) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == nullptr) {
      // 未进全局缓存，注册 DeleteBlock 回调，迭代器销毁时释放 Block
      iter->RegisterCleanup(&DeleteBlock, block, nullptr);
    } else {
      // 在全局缓存中，注册 ReleaseBlock 回调，迭代器销毁时归还缓存引用
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

Status Table::InternalGet(const ReadOptions& options, const Slice& k,
                          void* arg,
                          void (*handle_result)(void*, const Slice&,
                                                const Slice&)) {
  Status s;
  Iterator* iiter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(k);
  if (iiter->Valid()) {
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    bool filter_checked = false;
    if (filter != nullptr && handle.DecodeFrom(&handle_value).ok()) {
      // 使用用户键（而非完整的内部键）进行布隆过滤器判断。
      // 完整的内部键包含序列号，查询键（max_seq）与实际存储键的序列号
      // 不同，会导致布隆过滤器产生假阴性，错误判定键不存在。
      Slice user_key = ExtractUserKey(k);
      filter_checked = !filter->KeyMayMatch(handle.offset(), user_key);
    }
    if (!filter_checked) {
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        (*handle_result)(arg, block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
      delete block_iter;
    }
  }
  if (s.ok()) {
    s = iiter->status();
  }
  delete iiter;
  return s;
}

uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // 无法解码索引块中的 BlockHandle，
      // 返回 metaindex 块的偏移量（接近文件尾部）
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key 的字典序在文件中最后一个 key 之后，
    // 返回 metaindex 块的偏移量（接近文件尾部）
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace lsmdb
