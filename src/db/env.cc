#include "db/env.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace lsmdb {

Env::Env() = default;

Env::~Env() = default;

Status Env::NewAppendableFile(const std::string& fname, WritableFile** result) {
  return Status::NotSupported(Slice("NewAppendableFile"), Slice(fname));
}

Status Env::RemoveFile(const std::string& fname) {
  return Status::NotSupported(Slice("RemoveFile"), Slice(fname));
}

Status Env::DeleteFile(const std::string& fname) {
  return RemoveFile(fname);
}

Status Env::RemoveDir(const std::string& dirname) {
  return Status::NotSupported(Slice("RemoveDir"), Slice(dirname));
}

Status Env::DeleteDir(const std::string& dirname) {
  return RemoveDir(dirname);
}

SequentialFile::~SequentialFile() = default;

RandomAccessFile::~RandomAccessFile() = default;

WritableFile::~WritableFile() = default;

FileLock::~FileLock() = default;

Logger::~Logger() = default;

EnvWrapper::~EnvWrapper() = default;

void Log(Logger* info_log, const char* format, ...) {
  if (info_log != nullptr) {
    std::va_list ap;
    va_start(ap, format);
    info_log->Logv(format, ap);
    va_end(ap);
  }
}

Status WriteStringToFile(Env* env, const Slice& data,
                          const std::string& fname) {
  WritableFile* file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (s.ok()) {
    s = file->Close();
  }
  delete file;
  return s;
}

Status ReadFileToString(Env* env, const std::string& fname,
                         std::string* data) {
  data->clear();
  SequentialFile* file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  static const int kBufferSize = 8192;
  char* space = new char[kBufferSize];
  while (true) {
    Slice fragment;
    s = file->Read(kBufferSize, &fragment, space);
    if (!s.ok()) {
      break;
    }
    data->append(fragment.data(), fragment.size());
    if (fragment.size() < kBufferSize) {
      break;  // No more data to read
    }
  }
  delete[] space;
  delete file;
  return s;
}

}  // namespace lsmdb
