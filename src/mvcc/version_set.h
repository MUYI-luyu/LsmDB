// DBImpl 的整体状态由一组 Version 共同描述。
// 其中最新的 Version 称为 current（当前版本）。
// 旧 Version 可能仍然保留，用于为正在进行的迭代器提供一致性视图（snapshot isolation）。
//
// 每个 Version 维护各个 level 上的 SSTable 文件集合。
// 所有 Version 由 VersionSet 统一管理与回收。
//
// Version 与 VersionSet 本身是线程安全的（thread-compatible），
// 但所有对外访问仍需由上层保证互斥同步。

#pragma once

#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "db/dbformat.h"
#include "db/env.h"
#include "db/options.h"
#include "db/status.h"
#include "mvcc/version_edit.h"

namespace lsmdb {

namespace log {
class Writer;
}

class Iterator;
class MemTable;
class TableBuilder;
class TableCache;
class Version;
class VersionSet;
class WritableFile;

// 返回最小下标 i，使得 files[i]->largest >= key。
// 若不存在这样的文件则返回 files.size()。
// 需要："files" 为按序且互不重叠的文件列表。
int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files, const Slice& key);

// 当 "files" 中某个文件与用户 key 范围 [*smallest,*largest] 有重叠时返回 true。
// smallest==nullptr 表示小于 DB 中所有 key 的 key。
// largest==nullptr 表示大于 DB 中所有 key 的 key。
// 需要：若 disjoint_sorted_files 为真，files[] 需按序且范围不重叠。
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key);

// 通过滚雪球式的贪心扫描，防止具有相同 UserKey 的多版本数据在合并时被拦腰切断在不同的层级。
// 详细说明见 version_set.cc 中的实现注释。
void AddBoundaryInputs(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>& level_files,
                       std::vector<FileMetaData*>* compaction_files);

class Version {
 public:
  struct GetStats {
    FileMetaData* seek_file;  // 记录因引发“读放大”而被惩罚的第一个文件
    int seek_file_level;      // 被惩罚文件所在的物理层级
    GetStats() : seek_file(nullptr), seek_file_level(0) {}
  };

  // 为当前快照版本下的所有 SSTable 生成对应的迭代器，并追加到 *iters。
  // Level 0 文件的迭代器独立追加，Level 1~6 则每层各生成一个二级级联迭代器（ConcatenatingIterator）。
  // 要求：该 Version 的成员变量 files_ 已填充完成并沉淀。
  void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

  // 在当前快照的磁盘文件（L0~L6）中检索指定 key 的值。
  // 若命中有效数据则填入 *val，若命中删除墓碑或未找到则返回对应 Status，并在发生白读时填充 *stats。
  // 要求：前台调用，不能持有大总管全局互斥锁（以实现完全无锁的并发点查）。
  Status Get(const ReadOptions&, const LookupKey& key, std::string* val,
             GetStats* stats);

  // 接收点查或采样提报的白读文件，削减其允许Seek次数（寿命）。
  // 如果额度耗尽且当前无候选，则原位锁死该文件为被动合并目标，并返回 true。
  // 要求：必须已持有大总管全局互斥锁（涉及到修改内部冷压缩变量）。
  bool UpdateStats(const GetStats& stats);

  // 迭代器扫描（Scan）时每读取1MB触发一次的空间切面采样。
  // 若发现该 Key 至少跨越两个重叠文件，证明存在读放大，则对首个前置文件进行 Seek 寿命削减。
  // 要求：必须已持有大总管全局互斥锁。
  bool RecordReadSample(Slice key);

  // 引用计数管理（避免 Version 在活动迭代器期间被销毁）
  void Ref();
  void Unref();

  // 找出指定层级（level）中，其物理区间与 [begin, end] 存在任何重叠的所有 SSTable 文件指针。
  // begin==nullptr 代表区间无左边界，end==nullptr 代表区间无右边界。
  void GetOverlappingInputs(
      int level,
      const InternalKey* begin,  // nullptr 表示在所有 key 之前
      const InternalKey* end,    // nullptr 表示在所有 key 之后
      std::vector<FileMetaData*>* inputs);

  // 判断指定层级（level）中，是否存在任意 SSTable 的物理区间与用户键区间 [*smallest_user_key, *largest_user_key] 重叠。
  // 传入 nullptr 代表对应的用户键边界无限延伸。
  bool OverlapInLevel(int level, const Slice* smallest_user_key,
                      const Slice* largest_user_key);

  // 自适应多维评估：为新写满的 MemTable 冲刷（Flush）寻找最合适的下落层级。
  // 核心逻辑：若无重叠，优先推向更深层（最多到L2），以降低 L0 的合并高压，但需卡死单层重叠字节数上限，防止引发后续严重的写放大。
  int PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                 const Slice& largest_user_key);

  // 返回指定层级的 SSTable 文件总数。
  int NumFiles(int level) const { return files_[level].size(); }

  // 格式化输出当前 Version 账本快照中每层的完整 SSTable 结构视图。
  std::string DebugString() const;

 private:
  friend class VersionSet;

  class LevelFileNumIterator;

  explicit Version(VersionSet* vset)
      : vset_(vset),
        next_(this), // 双向循环链表：初始化时自己指向自己
        prev_(this),
        refs_(0),
        file_to_compact_(nullptr),
        file_to_compact_level_(-1),
        compaction_score_(-1),
        compaction_level_(-1) {}

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  ~Version();

  // 为 Level 1~6 这种物理完全有序且无交叉的单层文件，生成一个高效的级联迭代器。
  Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

  // 磁盘点查的底层雷达网：在 L0 进行全量时空降序筛选，在 L1~6 进行层级二分精准定位。
  // 只要锁定物理区间包裹 user_key 的嫌疑文件，就回调安检门 func。
  // 若 func 返回 false，说明捕获有效状态，立即提前拉闸截断整个遍历流。
  void ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                          bool (*func)(void*, int, FileMetaData*));

  VersionSet* vset_;  // 此 Version 所属的 VersionSet
  Version* next_;     // 历史快照版本双向链表中的下一个快照
  Version* prev_;     // 历史快照版本双向链表中的上一个快照
  int refs_;          // 当前快照的活跃引用计数（Get、Iterator 等均会抢占引用）

  // 每层的文件列表，存放 Level 0 到 Level 6 每一层的所有 SSTable 文件身份证指针
  std::vector<FileMetaData*> files_[config::kNumLevels];

  FileMetaData* file_to_compact_;  // 因 Seek 惩罚寿命耗尽、被死锁选出的下一个急需被融化的候选合并文件
  int file_to_compact_level_;      // file_to_compact_ 所在层级

  // 主动合并核心指标：由 VersionSet::Finalize() 根据层级文件总大小或数量，计算出的下一层该压缩的最高评分。
  // 评分 >= 1.0 代表该层已爆仓，后台线程必须立刻发起主动合并战役。
  double compaction_score_;
  int compaction_level_;
};

class VersionSet {
 public:
  // 构造函数：初始化版本集合管理中枢，绑定数据库名、配置、缓存和比较器
  VersionSet(const std::string& dbname, const Options* options,
             TableCache* table_cache, const InternalKeyComparator*);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  ~VersionSet();

  // 核心提交函数：将 VersionEdit 的改动应用到当前版本，融合出新版本，
  // 将改动持久化写入 MANIFEST 文件，并在成功后将新版本切换为当前的 current_
  Status LogAndApply(VersionEdit* edit, std::mutex* mu);

  // 恢复函数：数据库启动时调用，通过读取和重放 MANIFEST 文件恢复断电前的版本状态
  Status Recover(bool* save_manifest);

  // 获取当前正在提供读服务的最新 Version 账本快照
  Version* current() const { return current_; }

  // 获取当前正在使用的 MANIFEST 文件的编号
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }

  // 生成一个全库唯一的、单调递增的新文件编号（用于新建 .sst, .log 等文件）
  uint64_t NewFileNumber() { return next_file_number_++; }

  // 尝试回收复用刚申请的文件编号（如果该编号申请后发现并未使用）
  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) {
      next_file_number_ = file_number;
    }
  }

  // 返回指定层级（Level）当前的 SSTable 文件总数量
  int NumLevelFiles(int level) const;

  // 返回指定层级（Level）所有 SSTable 文件的总物理大小（字节数）
  int64_t NumLevelBytes(int level) const;

  // 获取当前数据库中最新写入数据的最大序列号（Sequence Number）
  uint64_t LastSequence() const { return last_sequence_; }

  // 更新全局最大的序列号
  void SetLastSequence(uint64_t s) {
    assert(s >= last_sequence_);
    last_sequence_ = s;
  }

  // 显式标记某个文件编号已被占用，防止后续分配时重复
  void MarkFileNumberUsed(uint64_t number);

  // 返回当前处于活跃状态、正在接收前台写入的 WAL 日志编号
  uint64_t LogNumber() const { return log_number_; }

  // 返回处于退役冲刷状态、正在做 Minor Compaction 的旧日志编号（若无则返回 0）
  uint64_t PrevLogNumber() const { return prev_log_number_; }

  // 核心调度函数：自动评估各层健康度，为后台合并线程挑选并返回一组需要进行压缩的文件集合
  // Compaction* PickCompaction();

  // 手动合并函数：找出指定层级中与用户给定的 [begin, end] 范围有空间重叠的所有文件并打包返回
  // Compaction* CompactRange(int level, const InternalKey* begin,
  //                          const InternalKey* end);

  // 计算任意 Level >= 1 的单个文件在向下一层合并时，所能引发的最大重叠数据量。
  int64_t MaxNextLevelOverlappingBytes();

  // 为传入的合并任务（Compaction）创建多路归并迭代器，用于读取合并输入的数据
  // Iterator* MakeInputIterator(Compaction* c);

  // 检查当前是否有任何层级满足触发后台合并的条件（通过分值或擦除读热点判断）
  bool NeedsCompaction() const {
    Version* v = current_;
    return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);
  }

 // 垃圾回收盘点：遍历当前链表中所有存活的版本，把它们引用的所有有效 SST 文件号存入 live 集合
  void AddLiveFiles(std::set<uint64_t>* live);

  // 估算指定 Key 在对应 Version 中的大致磁盘物理偏移位置
  uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);

  struct LevelSummaryStorage {
    char buffer[100];
  };
  // 格式化输出每层文件数量的简要单行字符串摘要（如 "files[ 3 10 0 0 0 0 0 ]"）
  const char* LevelSummary(LevelSummaryStorage* scratch) const;

 private:
  class Builder;    // 声明辅助类 Builder，用于将 VersionEdit 的增量改动融合进 Version

  friend class Version;

  // 尝试在启动恢复时复用已有的旧 MANIFEST 文件继续追加，避免新建文件的开销
  bool ReuseManifest(const std::string& dscname, const std::string& dscbase);

  // 为新生成的 Version 计算每一层的合并分值（Score），并确定最需要压缩的层
  void Finalize(Version* v);

  // 计算一组输入文件共同包裹的最小 Key 和最大 Key 空间边界
  void GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                InternalKey* largest);

  // 联合计算两组输入文件共同包裹的最小 Key 和最大 Key 空间边界
  void GetRange2(const std::vector<FileMetaData*>& inputs1,
                 const std::vector<FileMetaData*>& inputs2,
                 InternalKey* smallest, InternalKey* largest);

  // 确定目标层冲突文件并尝试反向扩展源端文件集合，最终锁定本次合并的两层输入文件范围与祖父层重叠文件防线。
  // void SetupOtherInputs(Compaction* c);

  // 拍摄当前全量资产快照：将当前 current_ 中所有存活文件的信息作为基底写入新的 MANIFEST
  Status WriteSnapshot(log::Writer* log);

  // 将新生成的 Version 挂入大总管维护的 MVCC 循环双向链表中
  void AppendVersion(Version* v);

  Env* const env_;                    // 系统环境接口，负责磁盘文件实际读写
  const std::string dbname_;          // 数据库在磁盘上的路径名称
  const Options* const options_;      // 用户全局配置参数指针
  TableCache* const table_cache_;     // SSTable 句柄与索引元数据缓存
  const InternalKeyComparator icmp_;  // 内部键比较器（确立全库 Key 的排序规则）

  uint64_t next_file_number_;         // 下一个待分配的全局唯一文件编号
  uint64_t manifest_file_number_;     // 当前生效的 MANIFEST 文件编号
  uint64_t last_sequence_;            // 当前全库最新数据的最大序列号
  uint64_t log_number_;               // 当前活跃的 WAL 日志文件编号
  uint64_t prev_log_number_;          // 正在进行内存刷盘的 Immutable MemTable 对应的旧日志编号

  WritableFile* descriptor_file_;     // 当前 MANIFEST 文件的写文件句柄
  log::Writer* descriptor_log_;       // 当前 MANIFEST 文件的日志格式化写入器
  Version dummy_versions_;            // 维护 MVCC 版本链表的循环双向链表哨兵头节点
  Version* current_;                  // 永远指向最新、当前生效的 Version 账本

  // 记录每一层下一次发起主动冷合并（Major Compaction）的断点 Key
  std::string compact_pointer_[config::kNumLevels];
};
}  // namespace lsmdb
