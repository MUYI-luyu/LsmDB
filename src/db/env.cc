#include "db/env.h"

namespace lsmdb {

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

Logger::~Logger() = default;

}  // namespace lsmdb
