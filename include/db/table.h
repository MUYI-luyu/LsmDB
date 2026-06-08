#pragma once

#include <cstdint>

#include "db/iterator.h"

namespace lsmdb {

class Block;
class BlockHandle;
class Footer;
struct Options;
class RandomAccessFile;
struct ReadOptions;
class TableCache;

// Table 是一个不可变（Immutable）的有序映射，将 String 映射到另一个 String。
// Table 是线程安全的，多个线程可以并发调用而不需要外部加锁同步。
class Table {
 public:
  // 尝试打开存储在 "file" 中、字节范围为 [0..file_size) 的 SSTable 文件，
  // 并读取解析用于定位数据的核心元数据条目（如 Index Block）。
  //
  // 如果成功，返回 Status::OK() 并将 "*table" 指向新创建的 Table 对象。
  // 当不再需要该对象时，调用者必须手动 delete "*table"。
  // 如果在打开或解析表时出错，将 "*table" 设置为 nullptr 并返回错误状态。
  //
  // 注意：此函数不会接管 "file" 的生命周期。调用者必须确保
  // 在返回的 Table 对象存活的整个生命周期内，"file" 指针保持有效且不被销毁。
  static Status Open(const Options& options, RandomAccessFile* file,
                     uint64_t file_size, Table** table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  ~Table();

  // 返回一个可以在当前表内容上进行点查和扫描的迭代器（Iterator）。
  // 注意：NewIterator() 返回的迭代器最初是"无效的"（Invalid），
  // 调用者在使用它读取数据之前，必须先调用 Seek/SeekToFirst 等方法将其激活。
  [[nodiscard]] Iterator* NewIterator(const ReadOptions&) const;

  // 给定一个 key，返回该 key 的数据在磁盘文件里开始位置的近似字节偏移量（Offset）。
  // （如果 key 存在，则是它的确切位置；若不存在，则是它如果存在时应该在的位置）。
  // 返回的值是基于文件物理字节计算的，因此已经包含了数据压缩等带来的体积变化效果。
  // 例如：表中最后一个 key 的近似偏移量，会非常接近文件的总长度。
  [[nodiscard]] uint64_t ApproximateOffsetOf(const Slice& key) const;

 private:
  friend class TableCache;
  struct Rep;  // 经典的 Pimpl 架构设计，隐藏内部动态状态

  static Iterator* BlockReader(void*, const ReadOptions&, const Slice&);

  explicit Table(Rep* rep) : rep_(rep) {}

  // 核心点查接口：在 Seek(key) 查到对应的 Entry 后，
  // 自动回调 (*handle_result)(arg, key, value)。
  // 如果布隆过滤器判定该 key 绝对不在当前文件中，则根本不会触发此调用（直接熔断）。
  Status InternalGet(const ReadOptions&, const Slice& key, void* arg,
                     void (*handle_result)(void* arg, const Slice& k,
                                           const Slice& v));

  void ReadMeta(const Footer& footer);
  void ReadFilter(const Slice& filter_handle_value);

  Rep* const rep_;  // 存放 Index Block 账本、布隆过滤器指针等常驻内存控制状态
};

}  // namespace lsmdb
