#pragma once

#include <string>

#include "db/dbformat.h"
#include "db/iterator.h"
#include "memtable/arena.h"
#include "memtable/skiplist.h"

namespace lsmdb {

class InternalKeyComparator;
class MemTableIterator;

class MemTable {
 public:
  // MemTable 是引用计数的。初始引用计数为零，调用者必须至少调用一次 Ref()。
  explicit MemTable(const InternalKeyComparator& comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // 增加引用计数。
  void Ref() { ++refs_; }

  // 减少引用计数。如果不存在更多引用，则删除。
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) {
      delete this;
    }
  }

  // 返回此数据结构使用的数据字节数的估计。在 MemTable 被修改时调用是安全的。
  size_t ApproximateMemoryUsage();

  // 返回一个迭代器，产生 memtable 的内容。
  //
  // 调用者必须确保底层 MemTable 在返回的迭代器活跃期间保持活跃。
  // 此迭代器返回的键是由 db/format.{h,cc} 模块中的 AppendInternalKey 编码的内部键。
  Iterator* NewIterator();

  // 在指定的序列号和指定类型下，向 memtable 添加一个条目，将键映射到值。
  // 如果 type==kTypeDeletion，通常 value 将为空。
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  // 如果 memtable 包含 key 的值，将其存储在 *value 中并返回 true。
  // 如果 memtable 包含 key 的删除，在 *status 中存储 NotFound() 错误并返回 true。
  // 否则，返回 false。
  bool Get(const LookupKey& key, std::string* value, Status* s);

 private:
  friend class MemTableIterator;

  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  typedef SkipList<const char*, KeyComparator> Table;

  ~MemTable();  // Private since only Unref() should be used to delete it

  KeyComparator comparator_;
  int refs_;
  Arena arena_;
  Table table_;
};

}  // namespace lsmdb
