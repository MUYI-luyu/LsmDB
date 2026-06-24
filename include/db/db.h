#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "db/iterator.h"
#include "db/options.h"

namespace lsmdb {

struct Options;
struct ReadOptions;
struct WriteOptions;
class WriteBatch;

class Snapshot {
 protected:
  virtual ~Snapshot();
};

// 一个键的范围
struct Range {
  Range() = default;
  Range(const Slice& s, const Slice& l) : start(s), limit(l) {}

  Slice start;
  Slice limit;
};

// DB 表示一个持久化的有序键值存储接口（key-value store）。
// 所有操作在不需要外部同步的情况下都是线程安全的，可被并发调用。
//
// DB 提供基本的 CRUD 操作以及范围查询、迭代器、快照等能力。
// 底层实现基于 LSM-tree。
class DB {
 public:
  static Status Open(const Options& options, const std::string& name,
                     DB** dbptr);

  DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual ~DB();

  virtual Status Put(const WriteOptions& options, const Slice& key,
                     const Slice& value) = 0;
  virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;
  virtual Status Get(const ReadOptions& options, const Slice& key,
                     std::string* value) = 0;
  virtual Iterator* NewIterator(const ReadOptions& options) = 0;
  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  virtual bool GetProperty(const Slice& property, std::string* value) = 0;

  virtual void GetApproximateSizes(const Range* range, int n,
                                   uint64_t* sizes) = 0;
  virtual void CompactRange(const Slice* begin, const Slice* end) = 0;
};

Status DestroyDB(const std::string& name, const Options& options);
Status RepairDB(const std::string& dbname, const Options& options);

}  // namespace lsmdb
