// Env 是 LsmDB 实现用于访问操作系统功能（如文件系统等）的接口。
// 调用者可能希望在打开数据库时提供自定义的 Env 对象以获得更精细的控制；
// 例如，对文件系统操作进行速率限制。
//
// 所有 Env 实现均支持多线程安全并发访问，无需任何外部同步。

#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

#include "db/status.h"

namespace lsmdb {

class FileLock;
class Logger;
class RandomAccessFile;
class SequentialFile;
class WritableFile;

class Env {
 public:
  Env();

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  virtual ~Env();

  // 返回适合当前操作系统的默认环境。
  // 高级用户可以通过提供自己的 Env 实现来替代此默认环境。
  //
  // Default() 的返回值属于 LsmDB，调用者不得删除它。
  static Env* Default();

  // 创建一个顺序读取指定文件的对象。
  // 成功时，将指向新文件的指针存入 *result 并返回 OK。
  // 失败时，将 nullptr 存入 *result 并返回非 OK 状态。
  // 如果文件不存在，应返回 NotFound 状态。
  //
  // 返回的文件一次只能被一个线程访问。
  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) = 0;

  // 创建支持随机读取指定文件的对象。
  // 成功时，将指向新文件的指针存入 *result 并返回 OK。
  // 失败时，将 nullptr 存入 *result 并返回非 OK 状态。
  // 如果文件不存在，应返回 NotFound 状态。
  //
  // 返回的文件可以被多个线程并发访问。
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) = 0;

  // 创建写入指定名称新文件的对象。
  // 如果同名文件已存在，则删除并创建新文件。
  // 成功时，将指向新文件的指针存入 *result 并返回 OK。
  // 失败时，将 nullptr 存入 *result 并返回非 OK 状态。
  //
  // 返回的文件一次只能被一个线程访问。
  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) = 0;

  // 创建可追加写入的文件对象：如果文件已存在，则追加写入；
  // 如果文件不存在，则创建新文件。
  // 成功时，将指向新文件的指针存入 *result 并返回 OK。
  // 失败时，将 nullptr 存入 *result 并返回非 OK 状态。
  //
  // 返回的文件一次只能被一个线程访问。
  //
  // 如果此 Env 不支持追加写入，可能返回 IsNotSupportedError。
  // Env 的使用者（包括 LsmDB 实现）必须准备好处理不支持追加的 Env。
  virtual Status NewAppendableFile(const std::string& fname,
                                   WritableFile** result);

  // 返回指定文件是否存在。
  virtual bool FileExists(const std::string& fname) = 0;

  // 将指定目录的子目录/文件名称列表存入 *result。
  // 名称相对于 "dir"。
  // *result 的原始内容会被丢弃。
  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) = 0;

  // 删除指定文件。
  //
  // 默认实现调用 DeleteFile 以支持旧版 Env 实现。
  // 更新后的 Env 实现必须覆盖 RemoveFile 并忽略 DeleteFile 的存在。
  // 调用 Env API 的更新代码应调用 RemoveFile 而非 DeleteFile。
  virtual Status RemoveFile(const std::string& fname);

  // 已废弃：现代 Env 实现应覆盖 RemoveFile。
  //
  // 默认实现调用 RemoveFile，以支持在现代 Env 实现上调用此方法的旧版用户代码。
  // 现代用户代码应调用 RemoveFile。
  virtual Status DeleteFile(const std::string& fname);

  // 创建指定目录。
  virtual Status CreateDir(const std::string& dirname) = 0;

  // 删除指定目录。
  //
  // 默认实现调用 DeleteDir 以支持旧版 Env 实现。
  // 更新后的 Env 实现必须覆盖 RemoveDir 并忽略 DeleteDir 的存在。
  // 调用 Env API 的更新代码应调用 RemoveDir 而非 DeleteDir。
  virtual Status RemoveDir(const std::string& dirname);

  // 已废弃：现代 Env 实现应覆盖 RemoveDir。
  //
  // 默认实现调用 RemoveDir，以支持在现代 Env 实现上调用此方法的旧版用户代码。
  // 现代用户代码应调用 RemoveDir。
  virtual Status DeleteDir(const std::string& dirname);

  // 将指定文件的大小存入 *file_size。
  virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) = 0;

  // 将文件 src 重命名为 target。
  virtual Status RenameFile(const std::string& src,
                            const std::string& target) = 0;

  // 锁定指定文件。用于防止多个进程并发访问同一个数据库。
  // 失败时，将 nullptr 存入 *lock 并返回非 OK 状态。
  //
  // 成功时，将代表已获取锁的对象指针存入 *lock 并返回 OK。
  // 调用者应调用 UnlockFile(*lock) 释放锁。
  // 如果进程退出，锁将自动释放。
  //
  // 如果其他进程已持有该锁，立即返回失败，不等待锁释放。
  //
  // 如果指定文件不存在，可能会创建它。
  virtual Status LockFile(const std::string& fname, FileLock** lock) = 0;

  // 释放之前通过 LockFile 成功获取的锁。
  // 前置条件：lock 是由成功的 LockFile() 调用返回的。
  // 前置条件：lock 尚未被释放。
  virtual Status UnlockFile(FileLock* lock) = 0;

  // 安排后台线程执行 "(*function)(arg)"。
  //
  // "function" 可能在一个未指定的线程中运行。
  // 添加到同一 Env 的多个函数可能在不同线程中并发运行。
  // 即，调用者不能假设后台工作项是串行化的。
  virtual void Schedule(void (*function)(void* arg), void* arg) = 0;

  // 启动一个新线程，在新线程中调用 "function(arg)"。
  // 当 "function(arg)" 返回时，线程将被销毁。
  virtual void StartThread(void (*function)(void* arg), void* arg) = 0;

  // *path 被设置为可用于测试的临时目录。
  // 该目录可能刚刚被创建，也可能已存在。
  // 同一进程的后续调用将返回相同的目录。
  virtual Status GetTestDirectory(std::string* path) = 0;

  // 创建并返回用于存储信息性消息的日志文件。
  virtual Status NewLogger(const std::string& fname, Logger** result) = 0;

  // 返回自某个固定时间点以来的微秒数。仅用于计算时间增量。
  virtual uint64_t NowMicros() = 0;

  // 使线程休眠/延迟指定的微秒数。
  virtual void SleepForMicroseconds(int micros) = 0;
};

// 用于顺序读取文件的抽象
class SequentialFile {
 public:
  SequentialFile() = default;

  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  virtual ~SequentialFile();

  // 从文件中读取最多 "n" 个字节。
  // "scratch[0..n-1]" 可能被此例程写入。
  // 将 "*result" 设置为已读取的数据（包括实际读取不足 n 字节的情况）。
  // 可能将 "*result" 指向 "scratch[0..n-1]" 中的数据，
  // 因此在 "*result" 被使用时 "scratch" 必须保持有效。
  // 如果遇到错误，返回非 OK 状态。
  //
  // 需要：外部同步
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

  // 跳过文件中的 "n" 个字节。这保证不会比读取相同数据慢，
  // 且可能更快。
  //
  // 如果到达文件末尾，跳过将在文件末尾停止，并返回 OK。
  //
  // 需要：外部同步
  virtual Status Skip(uint64_t n) = 0;
};

// 用于随机读取文件内容的抽象
class RandomAccessFile {
 public:
  RandomAccessFile() = default;

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  virtual ~RandomAccessFile();

  // 从文件 "offset" 处开始读取最多 "n" 个字节。
  // "scratch[0..n-1]" 可能被此例程写入。
  // 将 "*result" 设置为已读取的数据（包括实际读取不足 n 字节的情况）。
  // 可能将 "*result" 指向 "scratch[0..n-1]" 中的数据，
  // 因此在 "*result" 被使用时 "scratch" 必须保持有效。
  // 如果遇到错误，返回非 OK 状态。
  //
  // 支持多个线程并发安全使用。
  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const = 0;
};

// 用于顺序写入文件的抽象。
// 实现必须提供缓冲，因为调用方可能以小片段追加写入。
class WritableFile {
 public:
  WritableFile() = default;

  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  virtual ~WritableFile();

  virtual Status Append(const Slice& data) = 0;
  virtual Status Close() = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
};

// 用于写日志消息的接口
class Logger {
 public:
  Logger() = default;

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  virtual ~Logger();

  // 向日志文件写入一条具有指定格式的条目。
  virtual void Logv(const char* format, std::va_list ap) = 0;
};

// 标识一个已锁定的文件
class FileLock {
 public:
  FileLock() = default;

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  virtual ~FileLock();
};

// 如果 info_log 非空，将指定数据写入到 *info_log 中。
void Log(Logger* info_log, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((__format__(__printf__, 2, 3)))
#endif
    ;

// 工具例程：将 "data" 写入指定名称的文件。
Status WriteStringToFile(Env* env, const Slice& data,
                         const std::string& fname);

// 工具例程：将指定文件的内容读入 *data。
Status ReadFileToString(Env* env, const std::string& fname,
                        std::string* data);

// 将所有调用转发到另一个 Env 的 Env 实现。
// 适用于希望仅覆盖另一个 Env 部分功能的客户端。
class EnvWrapper : public Env {
 public:
  // 初始化一个将所有调用委托给 *t 的 EnvWrapper。
  explicit EnvWrapper(Env* t) : target_(t) {}
  virtual ~EnvWrapper();

  // 返回此 Env 转发所有调用的目标对象。
  Env* target() const { return target_; }

  // 以下样板代码将所有方法转发给 target()。
  Status NewSequentialFile(const std::string& f, SequentialFile** r) override {
    return target_->NewSequentialFile(f, r);
  }
  Status NewRandomAccessFile(const std::string& f,
                             RandomAccessFile** r) override {
    return target_->NewRandomAccessFile(f, r);
  }
  Status NewWritableFile(const std::string& f, WritableFile** r) override {
    return target_->NewWritableFile(f, r);
  }
  Status NewAppendableFile(const std::string& f, WritableFile** r) override {
    return target_->NewAppendableFile(f, r);
  }
  bool FileExists(const std::string& f) override {
    return target_->FileExists(f);
  }
  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* r) override {
    return target_->GetChildren(dir, r);
  }
  Status RemoveFile(const std::string& f) override {
    return target_->RemoveFile(f);
  }
  Status CreateDir(const std::string& d) override {
    return target_->CreateDir(d);
  }
  Status RemoveDir(const std::string& d) override {
    return target_->RemoveDir(d);
  }
  Status GetFileSize(const std::string& f, uint64_t* s) override {
    return target_->GetFileSize(f, s);
  }
  Status RenameFile(const std::string& s, const std::string& t) override {
    return target_->RenameFile(s, t);
  }
  Status LockFile(const std::string& f, FileLock** l) override {
    return target_->LockFile(f, l);
  }
  Status UnlockFile(FileLock* l) override { return target_->UnlockFile(l); }
  void Schedule(void (*f)(void*), void* a) override {
    return target_->Schedule(f, a);
  }
  void StartThread(void (*f)(void*), void* a) override {
    return target_->StartThread(f, a);
  }
  Status GetTestDirectory(std::string* path) override {
    return target_->GetTestDirectory(path);
  }
  Status NewLogger(const std::string& fname, Logger** result) override {
    return target_->NewLogger(fname, result);
  }
  uint64_t NowMicros() override { return target_->NowMicros(); }
  void SleepForMicroseconds(int micros) override {
    target_->SleepForMicroseconds(micros);
  }

 private:
  Env* target_;
};

}  // namespace lsmdb
