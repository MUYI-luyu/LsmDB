#include "db/table_cache.h"

#include <cassert>

#include "db/cache.h"
#include "db/env.h"
#include "db/filename.h"
#include "db/options.h"
#include "db/table.h"
#include "utils/coding.h"

namespace lsmdb {

using namespace coding;

namespace {
struct TableAndFile {
  RandomAccessFile* file; // 底层操作系统的只读、随机访问文件句柄
  Table* table;           // 已经解析好索引和元数据的 SSTable 对象
};

// 当某个缓存项因为 LRU 算法被驱逐或手动抹去时调用
static void DeleteTableAndFile(const Slice& /*key*/, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}
}  // namespace

TableCache::TableCache(const std::string& dbname, const Options* options,
                       int entries)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {}

TableCache::~TableCache() { delete cache_; }

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  *handle = cache_->Lookup(key);
  if (*handle == nullptr) {  // 若缓存中不存在，则需要从磁盘读取
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr;
    Table* table = nullptr;
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
      std::string old_fname = fname;
      old_fname.replace(old_fname.size() - 3, 3, "sst");
      s = env_->NewRandomAccessFile(old_fname, &file);
    }
    if (s.ok()) {
      s = Table::Open(*options_, file, file_size, &table);
    }

    if (!s.ok()) {
      assert(table == nullptr);
      delete file;
    } else {
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      *handle = cache_->Insert(key, tf, 1, &DeleteTableAndFile);
    }
  }
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number, uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != nullptr) {
    *tableptr = nullptr;
  }

  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);

  if (tableptr != nullptr) {
    *tableptr = table;
  }

  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  return result;
}

Status TableCache::Get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&,
                                             const Slice&)) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    s = t->InternalGet(options, k, arg, handle_result);
    cache_->Release(handle);
  }
  return s;
}

uint64_t TableCache::ApproximateOffsetOf(const ReadOptions& options,
                                              uint64_t file_number,
                                              uint64_t file_size,
                                              const Slice& key) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    // 若无法打开或找不到对应的 SSTable 文件，则保守地返回该文件的总大小。
    // 这可以让上层机制（如 Compaction 权重计算）认为此文件包含了极大数量的数据。
    return file_size;
  }

  Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  uint64_t result = t->ApproximateOffsetOf(key);
  cache_->Release(handle);  // 提取完偏移量后，立刻释放缓存句柄以降低引用计数

  // 如果底层计算出 key 的大致物理偏移量恰好是 0（即定位于文件的第一个数据块开头），
  // 则将其修正为 1。这是为了向外界提供一个非零的合法物理偏移量，
  // 从而有效规避高层级在进行区间大小算术减法或大小比例估算时可能引发的“除以零”或“零值异常”等边界问题。
  return result == 0 ? 1 : result;
}

void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  cache_->Erase(key);
}

}  // namespace lsmdb

