#pragma once

#include <set>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "db/status.h"

namespace lsmdb {

class VersionCatalog;

// SSTable 文件的元数据
struct SSTableDescriptor {
  // 默认构造函数：初始化文件生命线计数
  SSTableDescriptor() : refs(0), allowed_seeks(1 << 30), file_size(0) {}

  // 根据文件大小初始化 Seek 寿命阈值
  void InitAllowedSeeks() {
    allowed_seeks = static_cast<int>(file_size / 16384);
    if (allowed_seeks < 100) allowed_seeks = 100;
  }

  int refs;             // 物理生命线：当前有多少个活跃的快照（Version）正在引用此文件
  int allowed_seeks;    // 触发合并前允许的 Seek 次数（白读惩罚生命值，降至0之下触发被动冷合并）
  uint64_t number;      // 全局唯一标识：SSTable 文件的文件编号
  uint64_t file_size;   // 空间账本：文件的物理大小（字节数）
  InternalKey smallest; // 空间左边界：该 SSTable 中负责承载的最小内部键
  InternalKey largest;  // 空间右边界：该 SSTable 中负责承载的最大内部键
};

// 增量修改单：记录两个版本交替期间发生的状态和资产纯变量变更
class VersionDelta {
 public:
  VersionDelta() { Clear(); }
  ~VersionDelta() = default;

  void Clear();

  void SetComparatorName(const Slice& name) { // 设置键比较器的名称（状态覆盖）
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  void SetLogNumber(uint64_t num) {           // 设置当前活跃的 WAL 日志文件编号（状态覆盖）
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {       // 设置正在做合并刷盘的旧 Immutable MemTable 对应的日志编号（状态覆盖）
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  void SetNextFile(uint64_t num) {            // 设置全盘下一个可用的全局自增文件编号（状态覆盖）
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {  // 设置当前全库最新的全局最高序列号（状态覆盖）
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  void SetCompactPointer(int level, const InternalKey& key) { // 记录指定层级下次开始压缩的物理断点 Key
    compact_pointers_.push_back(std::make_pair(level, key));
  }

  // 将指定的 SSTable 文件（包含身份证元数据）追加到指定层级的新生名单中
  // 前置要求：当前这个修改单（VersionDelta）还没有被持久化保存到磁盘（参见 VersionCatalog::SaveTo）
  // 前置要求："smallest" 和 "largest" 必须是该物理文件中真实承载的最小和最大内部键
  void AddFile(int level, uint64_t file, uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest) {
    SSTableDescriptor f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
    f.InitAllowedSeeks();
    new_files_.push_back(std::make_pair(level, f));
  }

  // 将指定的文件编号从指定层级的资产中注销并拉入死亡名单
  void RemoveFile(int level, uint64_t file) {
    deleted_files_.insert(std::make_pair(level, file));
  }

  // 二进制序列化：将修改单中所有的纯变量及资产变动揉碎，压扁追加到目标字符串中
  void EncodeTo(std::string* dst) const;
  // 二进制反序列化：从一段物理切面字节流中还原出修改单的完整元数据资产变更视图
  Status DecodeFrom(const Slice& src);

  std::string DebugString() const;

 private:
  friend class VersionCatalog;

  typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;

  std::string comparator_;       // 比较器名称元数据
  uint64_t log_number_;          // WAL 日志编号元数据
  uint64_t prev_log_number_;     // 备用/前置日志编号元数据
  uint64_t next_file_number_;    // 系统下一个自增文件号元数据
  SequenceNumber last_sequence_; // 全局最高断点序列号元数据
  bool has_comparator_;          // 标识修改单内是否包含比较器变更
  bool has_log_number_;          // 标识修改单内是否包含日志编号变更
  bool has_prev_log_number_;     // 标识修改单内是否包含前置日志编号变更
  bool has_next_file_number_;    // 标识修改单内是否包含下一个文件号变更
  bool has_last_sequence_;       // 标识修改单内是否包含序列号变更

  std::vector<std::pair<int, InternalKey>> compact_pointers_; // 历史合并断点指针变更集
  DeletedFileSet deleted_files_;                              // 死亡名单：本轮演进注销的文件集合
  std::vector<std::pair<int, SSTableDescriptor>> new_files_;       // 新生名单：本轮演进诞生并落盘的文件集合
};

}  // namespace lsmdb
