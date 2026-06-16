#include "db/filename.h"

#include <cassert>
#include <cstdio>

#include "db/env.h"
#include "utils/logging.h"

namespace lsmdb {

static std::string MakeFileName(const std::string& dbname, uint64_t number,
                                const char* suffix) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

std::string LogFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "ldb");
}

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

std::string LockFileName(const std::string& dbname) {
  return dbname + "/LOCK";
}

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "dbtmp");
}

std::string InfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG";
}

std::string OldInfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG.old";
}

bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type) {
  Slice rest(filename);
  std::string prefix;
  // 跳过开头的 "/"
  if (!rest.empty() && rest[0] == '/') {
    rest.remove_prefix(1);
  }

  // 解析并剥离出数据库名称（dbname）前缀（即最后一个 '/' 之前的所有内容）
  // 解析文件名时并不需要 dbname
  size_t slash_pos = rest.ToString().rfind('/');
  if (slash_pos != std::string::npos) {
    rest.remove_prefix(slash_pos + 1);
  }

  // 此时 rest 只剩纯文件名。接下来开始解析以下几种模式：
  // 1、 number.type  2、 MANIFEST-number  3、 CURRENT  4、 LOG  5、 LOCK
  if (rest == "CURRENT") {
    *number = 0;
    *type = kCurrentFile;
    return true;
  } else if (rest == "LOCK") {
    *number = 0;
    *type = kDBLockFile;
    return true;
  } else if (rest == "LOG" || rest == "LOG.old") {
    *number = 0;
    *type = (rest == "LOG") ? kInfoLogFile : kOldInfoLogFile;
    return true;
  } else if (rest.starts_with("MANIFEST-")) {
    // 如果是以 "MANIFEST-" 开头，先剥离掉这个前缀
    rest.remove_prefix(strlen("MANIFEST-"));
    uint64_t num;
    if (!rest.empty()) {
      char* end = nullptr;
      // 将剩下的字符串转为 10 进制的 uint64_t
      num = std::strtoull(rest.data(), &end, 10);
      // 必须确保整个剩余字符串全部被成功解析为数字（指针走到末尾）
      if (end == rest.data() + rest.size()) {
        *number = num;
        *type = kDescriptorFile;
        return true;
      }
    }
  } else {
    // 解析 "number.type" 这种带有后缀的文件模式（如 000005.sst）
    // 寻找点号 '.' 的位置
    const char* dot = nullptr;
    for (size_t i = 0; i < rest.size(); i++) {
      if (rest[i] == '.') {
        dot = rest.data() + i;
        break;
      }
    }
    if (dot != nullptr) {
      // 提取点号前面的数字部分
      size_t num_len = dot - rest.data();
      std::string num_str(rest.data(), num_len);
      char* end = nullptr;
      uint64_t num = std::strtoull(num_str.c_str(), &end, 10);
      // 确保点号前面的字符全部是合法的数字
      if (end == num_str.c_str() + num_len) {
        Slice suffix(dot + 1, rest.size() - num_len - 1);
        if (suffix == "log") {
          *number = num;
          *type = kLogFile;
          return true;
        } else if (suffix == "ldb" || suffix == "sst") {
          *number = num;
          *type = kTableFile;
          return true;
        } else if (suffix == "dbtmp") {
          *number = num;
          *type = kTempFile;
          return true;
        }
      }
    }
  }
  return false;
}

Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number) {
  // 根据数据库名和指定的描述符编号，生成完整的 Manifest 文件路径
  std::string manifest = DescriptorFileName(dbname, descriptor_number);
  // 剥离 dbname 前缀，从而只获取 Manifest 的纯文件名（不含路径）
  size_t slash_pos = manifest.rfind('/');
  Slice manifest_slice(manifest);
  if (slash_pos != std::string::npos) {
    // 移动切片指针，跳过最后一个 '/' 及其前面的所有路径
    manifest_slice.remove_prefix(slash_pos + 1);
  }

  // 生成一个临时文件路径（例如：dbname/000005.dbtmp）
  std::string tmp = TempFileName(dbname, descriptor_number);
  // 将 Manifest 纯文件名外加一个换行符 "\n"，安全地写入到上述临时文件中
  Status s = WriteStringToFile(env, manifest_slice.ToString() + "\n", tmp);
  if (s.ok()) {
    s = env->RenameFile(tmp, CurrentFileName(dbname));
  }
  return s;
}

}  // namespace lsmdb