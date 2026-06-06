// WriteBatch 保存要原子地应用于数据库的一组更新。
//
// 更新将按照添加到 WriteBatch 的顺序应用。例如，在写入以下
// 批处理后，"key" 的值将是 "v3"：
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// 多个线程可以在没有外部同步的情况下调用 WriteBatch 上的 const 方法，
// 但如果任何线程可能调用非 const 方法，则所有访问同一 WriteBatch 的
// 线程都必须使用外部同步。

#pragma once

#include <string>

#include "db/slice.h"
#include "db/status.h"

namespace lsmdb {

class WriteBatch {
 public:
  // 用于与内存表交互，虚函数重写避免头文件交叉引用
  class Handler {
   public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch();

  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;

  ~WriteBatch();

  void Put(const Slice& key, const Slice& value); // 仅在内部的 std::string rep_ 字符串尾部，追加一段二进制字节
  void Delete(const Slice& key);
  void Clear();
  size_t ApproximateSize() const;
  void Append(const WriteBatch& source);
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;
};

}  // namespace lsmdb
