// Copyright (c) 2024 LsmDB Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// POSIX Env 实现 — 为整个库提供 Env::Default() 的基础设施。
// 此前该实现被错放在单测文件 db_test.cc 中，导致 -O2 优化下
// 静态初始化顺序失控（Static Initialization Order Fiasco）。
// 归位到核心库后，虚表初始化顺序得以保证。

#include "db/env.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/status.h"

namespace lsmdb {

namespace {

static Status FDSetCloexec(int fd) {
  int flags = fcntl(fd, F_GETFD, 0);
  if (flags == -1) {
    return Status::IOError("fcntl(F_GETFD)", std::strerror(errno));
  }
  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
    return Status::IOError("fcntl(F_SETFD, FD_CLOEXEC)", std::strerror(errno));
  }
  return Status::OK();
}

class PosixSequentialFile : public SequentialFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  PosixSequentialFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) {}
  ~PosixSequentialFile() override { fclose(file_); }

  Status Read(size_t n, Slice* result, char* scratch) override {
    Status s;
    size_t r = fread_unlocked(scratch, 1, n, file_);
    *result = Slice(scratch, r);
    if (r < n) {
      if (!feof(file_)) {
        s = Status::IOError(filename_, std::strerror(errno));
      }
    }
    return s;
  }

  Status Skip(uint64_t n) override {
    if (fseek(file_, static_cast<long>(n), SEEK_CUR)) {
      return Status::IOError(filename_, std::strerror(errno));
    }
    return Status::OK();
  }
};

class PosixRandomAccessFile : public RandomAccessFile {
 private:
  std::string filename_;
  int fd_;

 public:
  PosixRandomAccessFile(const std::string& fname, int fd)
      : filename_(fname), fd_(fd) {}
  ~PosixRandomAccessFile() override { close(fd_); }

  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    Status s;
    ssize_t r = pread(fd_, scratch, n, static_cast<off_t>(offset));
    *result = Slice(scratch, (r < 0) ? 0 : static_cast<size_t>(r));
    if (r < 0) {
      s = Status::IOError(filename_, std::strerror(errno));
    }
    return s;
  }
};

class PosixWritableFile : public WritableFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  PosixWritableFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) {}
  ~PosixWritableFile() override {
    if (file_ != nullptr) fclose(file_);
  }

  Status Append(const Slice& data) override {
    size_t r = fwrite(data.data(), 1, data.size(), file_);
    if (r != data.size()) {
      return Status::IOError(filename_, std::strerror(errno));
    }
    return Status::OK();
  }

  Status Close() override {
    Status s;
    if (fclose(file_) != 0) {
      s = Status::IOError(filename_, std::strerror(errno));
    }
    file_ = nullptr;
    return s;
  }

  Status Flush() override {
    if (fflush(file_) != 0) {
      return Status::IOError(filename_, std::strerror(errno));
    }
    return Status::OK();
  }

  Status Sync() override {
    if (fflush(file_) != 0) {
      return Status::IOError(filename_, std::strerror(errno));
    }
    if (fsync(fileno(file_)) != 0) {
      return Status::IOError(filename_, std::strerror(errno));
    }
    return Status::OK();
  }
};

class PosixLogger : public Logger {
 private:
  FILE* file_;

 public:
  explicit PosixLogger(FILE* f) : file_(f) { setvbuf(f, nullptr, _IONBF, 0); }
  ~PosixLogger() override { fclose(file_); }

  void Logv(const char* format, std::va_list ap) override {
    const uint64_t thread_id =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
    char buffer[500];
    for (int iter = 0; iter < 2; iter++) {
      char* base;
      int bufsize;
      if (iter == 0) {
        bufsize = sizeof(buffer);
        base = buffer;
      } else {
        bufsize = 65536;
        base = new char[bufsize];
      }
      char* p = base;
      char* limit = base + bufsize;
      struct timeval now_tv;
      gettimeofday(&now_tv, nullptr);
      const time_t seconds = now_tv.tv_sec;
      struct tm t;
      localtime_r(&seconds, &t);
      p += std::snprintf(p, limit - p,
          "%04d/%02d/%02d-%02d:%02d:%02d.%06d %llx ",
          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
          t.tm_hour, t.tm_min, t.tm_sec,
          static_cast<int>(now_tv.tv_usec),
          static_cast<long long unsigned int>(thread_id));
      if (p < limit) {
        va_list backup_ap;
        va_copy(backup_ap, ap);
        int done = std::vsnprintf(p, limit - p, format, backup_ap);
        va_end(backup_ap);
        if (done >= 0) {
          p += done;
        } else {
          if (base != buffer) delete[] base;
          continue;
        }
      }
      if (p >= limit) p = limit - 1;
      *p++ = '\n';
      size_t write_amt = p - base;
      size_t written = 0;
      while (written < write_amt) {
        size_t n = fwrite(base + written, 1, write_amt - written, file_);
        if (n <= 0) break;
        written += n;
      }
      if (base != buffer) delete[] base;
      break;
    }
  }
};

class PosixFileLock : public FileLock {
 public:
  int fd_;
  std::string name_;
};

class PosixEnv : public Env {
 public:
  PosixEnv();
  ~PosixEnv() override;

  Status NewSequentialFile(const std::string& fname,
                           SequentialFile** result) override {
    FILE* f = std::fopen(fname.c_str(), "r");
    if (f == nullptr) {
      *result = nullptr;
      return Status::NotFound(fname, std::strerror(errno));
    }
    *result = new PosixSequentialFile(fname, f);
    return Status::OK();
  }
  
  // 打开一个文件，并包装成一个“可随机读取的文件对象”返回给上层。
  Status NewRandomAccessFile(const std::string& fname,
                             RandomAccessFile** result) override {
    *result = nullptr;
    int fd = open(fname.c_str(), O_RDONLY);
    if (fd < 0) return Status::NotFound(fname, std::strerror(errno));
    Status s = FDSetCloexec(fd);
    if (!s.ok()) { close(fd); return s; }
    *result = new PosixRandomAccessFile(fname, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         WritableFile** result) override {
    FILE* f = std::fopen(fname.c_str(), "w");
    if (f == nullptr) {
      *result = nullptr;
      return Status::IOError(fname, std::strerror(errno));
    }
    *result = new PosixWritableFile(fname, f);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           WritableFile** result) override {
    FILE* f = std::fopen(fname.c_str(), "a");
    if (f == nullptr) {
      *result = nullptr;
      return Status::IOError(fname, std::strerror(errno));
    }
    *result = new PosixWritableFile(fname, f);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    return access(fname.c_str(), F_OK) == 0;
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return Status::IOError(dir, std::strerror(errno));
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    if (unlink(fname.c_str()) != 0)
      return Status::IOError(fname, std::strerror(errno));
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    if (mkdir(dirname.c_str(), 0755) != 0)
      return Status::IOError(dirname, std::strerror(errno));
    return Status::OK();
  }

  Status RemoveDir(const std::string& dirname) override {
    if (rmdir(dirname.c_str()) != 0)
      return Status::IOError(dirname, std::strerror(errno));
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      return Status::IOError(fname, std::strerror(errno));
    }
    *size = static_cast<uint64_t>(sbuf.st_size);
    return Status::OK();
  }

  Status RenameFile(const std::string& src,
                    const std::string& target) override {
    if (rename(src.c_str(), target.c_str()) != 0)
      return Status::IOError(src, std::strerror(errno));
    return Status::OK();
  }

  Status LockFile(const std::string& fname, FileLock** lock) override {
    *lock = nullptr;

    // 同进程锁拦截：防止同一个进程内对同一文件重复加锁.
    // fcntl(F_SETLK) 是进程级别的，相同进程的第二次调用会静默覆盖前一次锁。
    // 若不在用户态拦截，DestroyOpenDB 和 Locking 等单测将无法正确检测锁冲突。
    {
      std::lock_guard<std::mutex> l(lock_mu_);
      if (!locked_files_.insert(fname).second) {
        return Status::IOError("lock " + fname,
                               "already held by this process");
      }
    }

    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      std::lock_guard<std::mutex> l(lock_mu_);
      locked_files_.erase(fname);
      return Status::IOError(fname, std::strerror(errno));
    }
    Status s = FDSetCloexec(fd);
    if (!s.ok()) {
      close(fd);
      std::lock_guard<std::mutex> l(lock_mu_);
      locked_files_.erase(fname);
      return s;
    }
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(fd, F_SETLK, &fl) == -1) {
      close(fd);
      std::lock_guard<std::mutex> l(lock_mu_);
      locked_files_.erase(fname);
      return Status::IOError("lock " + fname, std::strerror(errno));
    }
    PosixFileLock* my_lock = new PosixFileLock;
    my_lock->fd_ = fd;
    my_lock->name_ = fname;
    *lock = my_lock;
    return Status::OK();
  }

  Status UnlockFile(FileLock* lock) override {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(my_lock->fd_, F_SETLK, &fl) == -1)
      return Status::IOError("unlock", std::strerror(errno));
    close(my_lock->fd_);
    {
      std::lock_guard<std::mutex> l(lock_mu_);
      locked_files_.erase(my_lock->name_);
    }
    delete my_lock;
    return Status::OK();
  }

  void Schedule(void (*function)(void* arg), void* arg) override {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push({function, arg});
    if (!started_) {
      started_ = true;
      bg_thread_ = std::thread(BGThreadWrapper, this);
    }
    cv_.notify_one();
  }

  void StartThread(void (*function)(void* arg), void* arg) override {
    std::thread t(function, arg);
    t.detach();
  }

  Status GetTestDirectory(std::string* path) override {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *path = env;
    } else {
      char buf[100];
      std::snprintf(buf, sizeof(buf), "/tmp/lsmdb_test_%d",
                    static_cast<int>(geteuid()));
      *path = buf;
    }
    CreateDir(*path);
    return Status::OK();
  }

  Status NewLogger(const std::string& fname, Logger** result) override {
    FILE* f = std::fopen(fname.c_str(), "w");
    if (f == nullptr) {
      *result = nullptr;
      return Status::IOError(fname, std::strerror(errno));
    }
    *result = new PosixLogger(f);
    return Status::OK();
  }

  uint64_t NowMicros() override {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  void SleepForMicroseconds(int micros) override {
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool started_;
  std::atomic<bool> exiting_;
  std::thread bg_thread_;
  std::queue<std::pair<void (*)(void*), void*>> queue_;

  // 同进程文件锁追踪：fcntl 锁是 per-process 的，
  // 相同进程第二次加锁会静默覆盖前一次。此集合确保在同进程内
  // 对同一文件只允许加一次锁，第二次会返回 "already held" 错误。
  std::mutex lock_mu_;
  std::set<std::string> locked_files_;

  static void BGThreadWrapper(void* arg) {
    reinterpret_cast<PosixEnv*>(arg)->BGThread();
  }
  void BGThread() {
    while (true) {
      void (*function)(void*) = nullptr;
      void* arg = nullptr;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return !queue_.empty() || exiting_.load(); });
        if (exiting_.load() && queue_.empty()) return;
        function = queue_.front().first;
        arg = queue_.front().second;
        queue_.pop();
      }
      (*function)(arg);
    }
  }
};

PosixEnv::PosixEnv() : started_(false), exiting_(false) {}

PosixEnv::~PosixEnv() {
  exiting_.store(true);
  cv_.notify_one();
  if (bg_thread_.joinable()) bg_thread_.join();
}

}  // anonymous namespace

// 单例 — Env::Default() 返回此实例
static PosixEnv default_posix_env;

Env* Env::Default() { return &default_posix_env; }

}  // namespace lsmdb
