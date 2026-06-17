#pragma once

#include <cstdint>
#include <string>

#include "db/cache.h"
#include "db/dbformat.h"
#include "db/env.h"
#include "db/slice.h"
#include "db/table.h"

namespace lsmdb {

class Env;

// 一个辅助类，用于缓存已打开的、对应于 SSTable 文件的 Table 对象。
// 线程安全（所有公有方法都是 const 的，并且可以并发调用）。
class TableCache {
 public:
  TableCache(const std::string& dbname, const Options* options,
             int entries);
  ~TableCache();

  // 返回一个针对指定文件编号的迭代器（对应物理文件的长度必须正好是 "file_size" 字节）。
  // 如果 "tableptr" 非空，*tableptr 将指向该返回迭代器底层的 Table 对象；如果未命中缓存，则指向 nullptr。
  // 返回的迭代器在不再需要时应该被手动 delete 释放。
  // 调用者绝不能修改返回的 "tableptr"。
  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number,
                        uint64_t file_size, Table** tableptr = nullptr);

  // 如果在指定的文件中定位（Seek）内部键 "k" 找到了对应的记录（Entry），
  // 则调用回调函数 (*handle_result)(arg, found_key, found_value)。
  Status Get(const ReadOptions& options, uint64_t file_number,
             uint64_t file_size, const Slice& k, void* arg,
             void (*handle_result)(void*, const Slice&, const Slice&));

  // 清除（驱逐）指定文件编号在缓存中对应的任何缓存条目。
  void Evict(uint64_t file_number);
  // 返回某个 key 在特定 SSTable 文件中大概的物理偏移量（Offset）。
  uint64_t ApproximateOffsetOf(const ReadOptions& options,
                                uint64_t file_number,
                                uint64_t file_size,
                                const Slice& key);


 private:
  // 底层核心方法：根据文件号和大小查找 Table。
  // 如果缓存没有则从磁盘打开，最终通过 handle 返回缓存项的指针（增加了引用计数）。
  Status FindTable(uint64_t file_number, uint64_t file_size,
                   Cache::Handle** handle);

  Env* const env_;           // 操作系统环境接口，用于打开物理文件
  const std::string dbname_; // 数据库路径名称
  const Options* options_;   // 数据库配置项（包含了用户自定义的比较器等）
  Cache* cache_;             // 底层通用的缓存抽象层（通常是 LRU Cache）
};

}  // namespace lsmdb
