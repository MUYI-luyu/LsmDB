#pragma once

#include <cstdint>
#include <string>

#include "db/dbformat.h"
#include "db/env.h"
#include "db/slice.h"

namespace lsmdb {

// File types
enum FileType {
  kLogFile,
  kDescriptorFile,
  kTableFile,
  kTempFile,
  kInfoLogFile,
  kCurrentFile,
  kDBLockFile,
  kOldInfoLogFile
};

// 返回指定数据库（由 "dbname" 指定）中、具有指定日志编号的日志文件名。
// 返回的文件名会以 "dbname" 作为路径前缀。
std::string LogFileName(const std::string& dbname, uint64_t number);

// 返回指定数据库中、具有指定编号的 SSTable 文件名。
std::string TableFileName(const std::string& dbname, uint64_t number);

// 返回指定数据库中、具有指定版本号（Incarnation Number）的描述符文件（Manifest）名。
// 返回的文件名会以 "dbname" 作为路径前缀。
std::string DescriptorFileName(const std::string& dbname, uint64_t number);

// 返回 CURRENT 文件名。该文件记录了当前正在使用的 Manifest 文件名。
// 返回的文件名会以 "dbname" 作为路径前缀。
std::string CurrentFileName(const std::string& dbname);

// 返回指定数据库的锁文件名。
// 返回的文件名会以 "dbname" 作为路径前缀。
std::string LockFileName(const std::string& dbname);

// 返回一个由指定数据库所有的临时文件名。
// 返回的文件名会以 "dbname" 作为路径前缀。
std::string TempFileName(const std::string& dbname, uint64_t number);

// 返回指定数据库的信息日志（Info Log）文件名。
std::string InfoLogFileName(const std::string& dbname);

// 返回指定数据库的历史信息日志（Old Info Log）文件名。
std::string OldInfoLogFileName(const std::string& dbname);

// 如果给定的文件名是一个合法的 lsmdb 文件，则将其文件类型存入 *type，
// 将其解析出的文件编号存入 *number。
// 若文件名解析成功返回 true，否则返回 false。
bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type);

// 使 CURRENT 文件指向指定编号的描述符文件（Manifest）。
Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number);

}  // namespace lsmdb
