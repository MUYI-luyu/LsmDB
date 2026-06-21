// DBImpl 的整体状态由一组 Version 共同描述。
// 其中最新的 Version 称为 current（当前版本）。
// 旧 Version 可能仍然保留，用于为正在进行的迭代器提供一致性视图（snapshot isolation）。
//
// 每个 Version 维护各个 level 上的 SSTable 文件集合。
// 所有 Version 由 VersionCatalog 统一管理与回收。
//
// Version 与 VersionCatalog 本身是线程安全的（thread-compatible），
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
#include "mvcc/version_delta.h"

namespace lsmdb {

namespace log {
class Writer;
}

class MergeTask;
class Iterator;
class MemTable;
class TableBuilder;
class TableCache;
class Version;
class VersionCatalog;
class WritableFile;

// 返回最小下标 i，使得 files[i]->largest >= key。
// 若不存在这样的文件则返回 files.size()。
// 需要："files" 为按序且互不重叠的文件列表。
int FindFile(const InternalKeyComparator& icmp,
             const std::vector<SSTableDescriptor*>& files, const Slice& key);

// 当 "files" 中某个文件与用户 key 范围 [*smallest,*largest] 有重叠时返回 true。
// smallest==nullptr 表示小于 DB 中所有 key 的 key。
// largest==nullptr 表示大于 DB 中所有 key 的 key。
// 需要：若 disjoint_sorted_files 为真，files[] 需按序且范围不重叠。
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<SSTableDescriptor*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key);

// 通过滚雪球式的贪心扫描，防止具有相同 UserKey 的多版本数据在合并时被拦腰切断在不同的层级。
// 详细说明见 version_set.cc 中的实现注释。
void AddBoundaryInputs(const InternalKeyComparator& icmp,
                       const std::vector<SSTableDescriptor*>& level_files,
                       std::vector<SSTableDescriptor*>* compaction_files);

class Version {
 public:
  struct GetStats {
    SSTableDescriptor* seek_file;  // 记录因引发“读放大”而被惩罚的第一个文件
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
      std::vector<SSTableDescriptor*>* inputs);

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
  friend class MergeTask;
  friend class VersionCatalog;

  class LevelFileNumIterator;

  explicit Version(VersionCatalog* catalog)
      : catalog_(catalog),
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
                          bool (*func)(void*, int, SSTableDescriptor*));

  VersionCatalog* catalog_;  // 此 Version 所属的 VersionCatalog
  Version* next_;     // 历史快照版本双向链表中的下一个快照
  Version* prev_;     // 历史快照版本双向链表中的上一个快照
  int refs_;          // 当前快照的活跃引用计数（Get、Iterator 等均会抢占引用）

  // 每层的文件列表，存放 Level 0 到 Level 6 每一层的所有 SSTable 文件身份证指针
  std::vector<SSTableDescriptor*> files_[config::kNumLevels];

  SSTableDescriptor* file_to_compact_;  // 因 Seek 惩罚寿命耗尽、被死锁选出的下一个急需被融化的候选合并文件
  int file_to_compact_level_;      // file_to_compact_ 所在层级

  // 主动合并核心指标：由 VersionCatalog::Finalize() 根据层级文件总大小或数量，计算出的下一层该压缩的最高评分。
  // 评分 >= 1.0 代表该层已爆仓，后台线程必须立刻发起主动合并战役。
  double compaction_score_;
  int compaction_level_;
};

class VersionCatalog {
 public:
  // 构造函数：初始化版本集合管理中枢，绑定数据库名、配置、缓存和比较器
  VersionCatalog(const std::string& dbname, const Options* options,
             TableCache* table_cache, const InternalKeyComparator*);
  VersionCatalog(const VersionCatalog&) = delete;
  VersionCatalog& operator=(const VersionCatalog&) = delete;

  ~VersionCatalog();

  // 核心提交函数：将 VersionDelta 的改动应用到当前版本，融合出新版本，
  // 将改动持久化写入 MANIFEST 文件，并在成功后将新版本切换为当前的 current_
  Status CommitDelta(VersionDelta* edit, std::mutex* mu);

  // 恢复函数：数据库启动时调用，通过读取和重放 MANIFEST 文件恢复断电前的版本状态
  Status Recover(bool* save_manifest);

  // 获取当前正在提供读服务的最新 Version 账本快照
  Version* current() const { return current_; }

  // 获取当前正在使用的 MANIFEST 文件的编号
  uint64_t CatalogFileNumber() const { return catalog_file_number_; }

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
  MergeTask* SelectMergeCandidate();

  // 手动合并函数：找出指定层级中与用户给定的 [begin, end] 范围有空间重叠的所有文件并打包返回
  MergeTask* CompactRange(int level, const InternalKey* begin,
                           const InternalKey* end);

  // 计算任意 Level >= 1 的单个文件在向下一层合并时，所能引发的最大重叠数据量。
  int64_t MaxNextLevelOverlappingBytes();

  // 为传入的合并任务（Compaction）创建多路归并迭代器，用于读取合并输入的数据
  Iterator* MakeInputIterator(MergeTask* c);

  // 检查当前是否有任何层级满足触发后台合并的条件（通过分值或擦除读热点判断）
  bool NeedsMerge() const {
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
  class Builder;    // 声明辅助类 Builder，用于将 VersionDelta 的增量改动融合进 Version

  friend class MergeTask;
  friend class Version;

  // 尝试在启动恢复时复用已有的旧 MANIFEST 文件继续追加，避免新建文件的开销
  bool ReuseManifest(const std::string& dscname, const std::string& dscbase);

  // 为新生成的 Version 计算每一层的合并分值（Score），并确定最需要压缩的层
  void Finalize(Version* v);

  // 计算一组输入文件共同包裹的最小 Key 和最大 Key 空间边界
  void GetRange(const std::vector<SSTableDescriptor*>& inputs, InternalKey* smallest,
                InternalKey* largest);

  // 联合计算两组输入文件共同包裹的最小 Key 和最大 Key 空间边界
  void GetRange2(const std::vector<SSTableDescriptor*>& inputs1,
                 const std::vector<SSTableDescriptor*>& inputs2,
                 InternalKey* smallest, InternalKey* largest);

  // 确定目标层冲突文件并尝试反向扩展源端文件集合，最终锁定本次合并的两层输入文件范围与祖父层重叠文件防线。
  void SetupOtherInputs(MergeTask* c);

  // 拍摄当前全量资产快照：将当前 current_ 中所有存活文件的信息作为基底写入新的 MANIFEST
  Status InstallFullSnapshot(log::Writer* log);

  // 将新生成的 Version 挂入大总管维护的 MVCC 循环双向链表中
  void AppendVersion(Version* v);

  Env* const env_;                    // 系统环境接口，负责磁盘文件实际读写
  const std::string dbname_;          // 数据库在磁盘上的路径名称
  const Options* const options_;      // 用户全局配置参数指针
  TableCache* const table_cache_;     // SSTable 句柄与索引元数据缓存
  const InternalKeyComparator icmp_;  // 内部键比较器（确立全库 Key 的排序规则）

  uint64_t next_file_number_;         // 下一个待分配的全局唯一文件编号
  uint64_t catalog_file_number_;     // 当前生效的 MANIFEST 文件编号
  uint64_t last_sequence_;            // 当前全库最新数据的最大序列号
  uint64_t log_number_;               // 当前活跃的 WAL 日志文件编号
  uint64_t prev_log_number_;          // 正在进行内存刷盘的 Immutable MemTable 对应的旧日志编号

  WritableFile* catalog_file_;     // 当前 MANIFEST 文件的写文件句柄
  log::Writer* catalog_writer_;       // 当前 MANIFEST 文件的日志格式化写入器
  Version dummy_versions_;            // 维护 MVCC 版本链表的循环双向链表哨兵头节点
  Version* current_;                  // 永远指向最新、当前生效的 Version 账本

  // 记录每一层下一次发起主动冷合并（Major Compaction）的断点 Key
  std::string compact_pointer_[config::kNumLevels];
};

// Compaction 结构体封装了本次后台合并（合并元数据单元）的所有战术控制信息与边界状态。
class MergeTask {
 public:
  ~MergeTask();

  // 返回本次合并的源端层级（Level X）。
  // 合并数据来自于 level_ 层和 level_+1 层，重写后将统一在 level_+1 层生成一组新的 SSTable 文件。
  int level() const { return level_; }

  // 返回本次合并产生的增量账本编辑对象（VersionDelta）。
  // 用于记录本次合并引发的文件删除（RemoveFile）与新文件添加（AddFile）记录，随后持久化至 MANIFEST。
  VersionDelta* edit() { return &edit_; }

  // 获取参与合并的指定层级的文件数量。
  // 参数 which 必须为 0 或 1：0 代表源端层（Level X），1 代表目标端层（Level X+1）。
  int num_input_files(int which) const { return inputs_[which].size(); }

  // 获取指定层级中下标为 i 的输入文件元数据指针。
  // 参数 which 必须为 0 或 1：0 代表源端层（Level X），1 代表目标端层（Level X+1）。
  SSTableDescriptor* input(int which, int i) const { return inputs_[which][i]; }

  // 获取本次合并在切分、输出单个新 SSTable 文件时的最大物理字节数限制。
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // 判定本次合并是否可以执行“纯元数据层级提升”（Trivial Move）。
  // 当源端层仅有 1 个文件，目标端层无冲突文件，且与祖父层重叠面积未超标时返回 true。
  // 此时无需物理磁盘归并读写，直接在账本中将该文件提升至下一层，I/O 复杂度为 O(0)。
  bool IsTrivialMove() const;

  // 将当前 inputs_[0] 和 inputs_[1] 中记录的所有待合并老文件的文件编号（File Number），
  // 转换为删除指令（RemoveFile）追加写入传入的增量账本 edit 对象中。
  void AddInputDeletions(VersionDelta* edit);

  // 判定给定的 user_key 在比本次合并目标端更底的所有层级中（即所有 Level >= level_ + 2 层）
  // 是否绝对不存在任何更老版本的数据。
  // 若返回 true，说明该 Key 在全地下世界已无旧版本，若其带有删除标记（墓碑Key），则可以在归并落盘时直接进行物理抹除（GC）。
  bool IsBaseLevelForKey(const Slice& user_key);

  // 判定在流式落盘写入新 SSTable 的过程中，是否应该在处理当前 internal_key 之前，
  // 强制截断当前的物理文件写入并切换生成一个新的 SSTable 文件。
  // 核心逻辑是通过累加与祖父层（Level level_+2）的冲突面积，将未来的合并“爆炸半径”控制在安全线内。
  bool ShouldStopBefore(const Slice& internal_key);

  // 释放对输入版本（input_version_）的锁死状态（递减 MVCC 引用计数）。
  // 当合并完成或失败退出时调用，允许系统物理回收那些已经被淘汰的旧 SSTable 磁盘文件。
  void ReleaseInputs();

 private:
  friend class Version;
  friend class VersionCatalog;

  // 仅允许由大总管 VersionCatalog 内部根据触发策略进行实例化构造。
  MergeTask(const Options* options, int level);

  // 本次合并任务的源端层级（Level X）。
  int level_;

  // 本次合并输出单个 SSTable 文件的最大字节数阈值。
  uint64_t max_output_file_size_;

  // 实例化合并任务那一刻所锁死的数据时空版本快照视图（带有 MVCC 引用计数防卫）。
  Version* input_version_;

  // 用于收集并在合并成功时导出的增量账本改动凭证单据。
  VersionDelta edit_;

  // 物理存储待合并文件的二维元数据指针向量数组。
  // inputs_[0] 存储源端层（Level X）的文件集合；inputs_[1] 存储目标端层（Level X+1）的冲突文件集合。
  std::vector<SSTableDescriptor*> inputs_[2];

  // 存储与当前合并 Key 范围存在交集重叠的祖父层（Level level_ + 2）文件集合。
  // 用于在 ShouldStopBefore 中前瞻性预估下一次合并的 I/O 冲突体积。
  std::vector<SSTableDescriptor*> grandparents_;

  // 处于 grandparents_ 向量中的当前单向检索步进器下标。
  size_t grandparent_index_;

  // 状态标志位，标记在当前的输出 SSTable 文件中是否已经写入了至少一条 Key-Value 记录。
  bool seen_key_;

  // 追踪当前正在写的输出 SSTable 与祖父层文件之间已经物理发生的累计重叠物理字节数。
  int64_t overlapped_bytes_;

  // 一个线性步进指针数组，其下标对应层级编号。
  // 专门用于暂存和记忆在高于本次合并层级的所有更底层（L >= level_ + 2）文件中，
  // 当前二分/线性检索所推进到的绝对位置，用以消除 IsBaseLevelForKey 判定时的重复检索开销。
  size_t level_ptrs_[config::kNumLevels];
};
}  // namespace lsmdb
