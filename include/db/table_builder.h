// TableBuilder 提供构建不可变有序映射（SSTable）的接口。
//
// 多个线程可在没有外部同步的情况下对 TableBuilder 调用常数方法，
// 但如果任何线程可能调用非常数方法，则所有访问同一 TableBuilder
// 的线程都必须使用外部同步。

#pragma once

#include <cstdint>

#include "db/options.h"
#include "db/status.h"

namespace lsmdb {

class BlockBuilder;
class BlockHandle;
class WritableFile;

class TableBuilder {
 public:
  // 创建一个构建器，其构建的表内容将被写入到 *file 中。
  // 不会关闭 file。关闭 file 的责任由调用者负责，应在调用 Finish() 后进行。
  TableBuilder(const Options& options, WritableFile* file);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // 要求：已调用 Finish() 或 Abandon() 之一。
  ~TableBuilder();

  // 变更此构建器使用的选项。注意：仅部分选项字段可以在构造后变更。
  // 如果某字段不允许动态变更，且其在构造时传入的值与此方法设置的值不同，
  // 此方法将返回错误且不改变任何字段。
  Status ChangeOptions(const Options& options);

  // 向正在构建的表添加 key-value 对。
  // 要求：key 按比较器顺序大于任何之前添加的 key。
  // 要求：尚未调用 Finish() 或 Abandon()。
  void Add(const Slice& key, const Slice& value);

  // 高级操作：将任何缓冲的 key-value 对刷新到文件。
  // 可用于确保两个相邻条目不会落在同一数据块中。
  // 大多数调用者不需要使用此方法。
  // 要求：尚未调用 Finish() 或 Abandon()。
  void Flush();

  // 如果检测到任何错误，返回非 OK 状态。
  [[nodiscard]] Status status() const;

  // 完成表的构建。此方法返回后不再使用传递给构造器的 file。
  // 要求：尚未调用 Finish() 或 Abandon()。
  Status Finish();

  // 放弃此构建器的内容。此方法返回后不再使用传递给构造器的 file。
  // 如果调用者不打算调用 Finish()，则必须在销毁此构建器前调用 Abandon()。
  // 要求：尚未调用 Finish() 或 Abandon()。
  void Abandon();

  // 到目前为止 Add() 的调用次数。
  [[nodiscard]] uint64_t NumEntries() const;

  // 到目前为止文件已写入的大小。如果在成功调用 Finish() 后调用，
  // 返回最终生成文件的大小。
  [[nodiscard]] uint64_t FileSize() const;

 private:
  bool ok() const { return status().ok(); }
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

  struct Rep;
  Rep* rep_;
};

}  // namespace lsmdb
