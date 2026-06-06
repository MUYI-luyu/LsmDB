#pragma once

#include "db/dbformat.h"
#include "db/write_batch.h"

namespace lsmdb {

class MemTable;

// WriteBatchInternal 提供了操纵 WriteBatch 内部结构的静态工具方法。
// 引入该类的目的是封装底层序列化细节，避免将这些私有管理接口暴露在公共的 WriteBatch API 中。
class WriteBatchInternal {
 public:
  // 返回当前批处理（Batch）中包含的键值操作（Entry）总数。
  static int Count(const WriteBatch* batch);

  // 显式设置批处理中键值操作的数量计数。
  static void SetCount(WriteBatch* batch, int n);

  // 获取该批处理起始条目所分配的全局基础序列号（Sequence Number）。
  static SequenceNumber Sequence(const WriteBatch* batch);

  // 设置该批处理起始条目的全局基础序列号。
  static void SetSequence(WriteBatch* batch, SequenceNumber seq);

  // 获取 WriteBatch 内部序列化缓冲区（rep_）的只读 Slice 视图。
  static Slice Contents(const WriteBatch* batch) { return Slice(batch->rep_); }

  // 返回当前批处理序列化字节流的总长度（字节数），用于评估内存/磁盘开销及触发合并策略。
  static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }

  // 用外部字节流重置当前批处理的内部缓冲区（rep_）。主要用于 WAL 日志回放恢复（Recovery）阶段。
  static void SetContents(WriteBatch* batch, const Slice& contents);

  // 反序列化解析整个批处理字节流，将提取出的 Put/Delete 操作批量写入指定的内存表（MemTable）。
  static Status InsertInto(const WriteBatch* batch, MemTable* memtable);

  // 将 src 批处理中的所有序列化条目高效追加到 dst 批处理的末尾，并同步更新计数。
  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace lsmdb
