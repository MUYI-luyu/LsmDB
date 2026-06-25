#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <string>

#include "db/cache.h"
#include "db/db.h"
#include "db/dbformat.h"
#include "db/env.h"
#include "db/snapshot.h"
#include "utils/mutexlock.h"
#include "wal/log_writer.h"

namespace lsmdb {

class MemTable;
class TableCache;
class Version;
class VersionDelta;
class VersionCatalog;
class FilterPolicy;

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  ~DBImpl() override;

  // DB 接口的实现
  Status Put(const WriteOptions&, const Slice& key,
             const Slice& value) override;
  Status Delete(const WriteOptions&, const Slice& key) override;
  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, const Slice& key,
             std::string* value) override;
  Iterator* NewIterator(const ReadOptions&) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;
  bool GetProperty(const Slice& property, std::string* value) override;
  void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) override;
  void CompactRange(const Slice* begin, const Slice* end) override;

  // 额外的用于测试的方法，这些方法不在公开的 DB 接口中

  // 压实在指定层级中与 [*begin, *end] 存在重叠的所有文件
  void TEST_CompactRange(int level, const Slice* begin, const Slice* end);

  // 强制当前 memtable 的内容进行压实。
  Status TEST_CompactMemTable();

  // 针对数据库的当前状态返回一个内部迭代器。
  // 此迭代器的键是 internal keys (参见 format.h)。
  // 返回的迭代器在不再需要时应该被删除。
  Iterator* TEST_NewInternalIterator();

  // 对于任意层级 >= 1 的文件，返回其在下一层级的最大数据重叠量 (以字节为单位)。
  int64_t TEST_MaxNextLevelOverlappingBytes();

  // 记录在指定内部键处读取采样的字节数。
  // 大约每读取 config::kReadBytesPeriod 个字节进行一次采样。
  void RecordReadSample(Slice key);

 private:
  friend class DB;
  struct MergeTaskState;
  struct Writer;

  // 手动压实 (Manual Compaction) 的状态信息
  struct ManualMerge {
    int level;                 // 当前正在进行手动压实的层级 (Level)
    bool done;                 // 该层级的压实任务是否已经全部完成
    const InternalKey* begin;  // 要压实的起始键，若为 nullptr 则表示从该层级的最小键开始
    const InternalKey* end;    // 要压实的结束键，若为 nullptr 则表示一直压实到该层级的最大键
    InternalKey tmp_storage;   // 临时存储区，用于记录当前已完成合并的断点位置，以便在被后台打断后能继续追踪进度
  };

  // 每一层的压实统计信息。stats_[level] 存储了生成目标 "level" 数据的合并统计信息。
  struct MergeStats {
    MergeStats() : micros(0), bytes_read(0), bytes_written(0) {}

    // 累加函数，用于将单次合并的开销或增量数据合并到当前层级的总统计中
    void Add(const MergeStats& c) {
      this->micros += c.micros;
      this->bytes_read += c.bytes_read;
      this->bytes_written += c.bytes_written;
    }

    int64_t micros;        // 累计消耗的微秒数（即执行压实操作所花费的总 CPU/IO 时间）
    int64_t bytes_read;    // 累计读取的字节数（包括从当前层和下一层读入参与归并排序的总数据量）
    int64_t bytes_written; // 累计写入的字节数（归并去重后，最终写入到目标层级 SSTable 的总数据量）
  };

  // ==================================
  // 1. 初始化与崩溃恢复
  // ==================================
  Status NewDB();
  // 从持久化存储中恢复描述符 (descriptor)。可能会执行大量工作来恢复最近记录在日志中的更新操作。
  // 任何对MANIFEST 文件的修改操作都会被添加到 *edit 中。
  Status Recover(VersionDelta* edit, bool* save_manifest);
  Status RecoverLogFile(uint64_t log_number, bool last_log, bool* save_manifest,
                        VersionDelta* edit, SequenceNumber* max_sequence);

  // ==================================
  // 2. 前台写入与流量控制
  // ==================================
  Status ApplyBackpressure(bool force /* 即便有可用空间也要进行压实？ */);
  // 从 writers_ 队列中捞出多个写请求，打包成一个大 Batch。
  WriteBatch* CoalesceWriteBatch(Writer** last_writer);

  // ==================================
  // 3. 后台压实状态机
  // ==================================
  void MaybeScheduleMerge();  // 触发器
  static void BGWork(void* db);    // 线程池回调入口
  void BackgroundCall();           // 锁保护与异常包装层
  void BackgroundMerge();     // 任务分配器 (Minor 还是 Major)

  // 将内存中的写入缓冲 (write buffer) 压实至磁盘。
  // 当且仅当成功时，切换至一个新的日志文件/memtable 并且写入一个新的 MANIFEST 文件。
  // 错误会被记录在 bg_error_ 中。
  void CompactMemTable();
  Status WriteLevel0Table(MemTable* mem, VersionDelta* edit, Version* base);

  Status DoMergeWork(MergeTaskState* compact);
  Status OpenMergeOutputFile(MergeTaskState* compact);
  Status FinishMergeOutputFile(MergeTaskState* compact, Iterator* input);
  Status InstallMergeResults(MergeTaskState* compact);
  void CleanupMerge(MergeTaskState* compact);


  // ==================================
  // 4. 全局辅助与基础设施 
  // ==================================
  Iterator* NewInternalIterator(const ReadOptions&,
                                SequenceNumber* latest_snapshot,
                                uint32_t* seed);
  void MaybeIgnoreError(Status* s) const;
  void RecordBackgroundError(const Status& s);

  // 删除任何不需要的文件和过时的内存条目。
  void RemoveObsoleteFiles();

  const Comparator* user_comparator() const {
    return internal_comparator_.user_comparator();
  }

  // 构造后即为常量
  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const Options options_;  // options_.comparator == &internal_comparator_
  // 是否拥有 info_log（日志句柄）的所有权（若为 true，析构时 DBImpl 负责 delete 销毁该日志对象）
  const bool owns_info_log_;
  // 是否拥有 Block Cache（数据块缓存）的所有权（若为 true，析构时 DBImpl 负责释放对应的 Cache 内存）
  const bool owns_cache_;
  const std::string dbname_;

  // table_cache_ 内部提供了它自己的同步机制
  TableCache* const table_cache_;

  // 用于锁定持久化的 DB 状态。当且仅当成功获取锁时才为非空。
  FileLock* db_lock_;

  // 下方的状态变量由 mutex_ 互斥锁保护
  std::mutex mutex_;
  std::atomic<bool> shutting_down_;
  CondVar background_work_finished_signal_;
  MemTable* mem_;               // 当前正在接收写入的活跃内存表
  MemTable* imm_;               // 已经写满、冻结，等待转换成 SSTable 的只读内存表
  std::atomic<bool> has_imm_;   // 原子变量，方便后台线程快速检查 imm_ 是否存在，减少锁竞争
  WritableFile* logfile_;       // 当前 WAL (Write Ahead Log) 的文件句柄
  uint64_t logfile_number_;     // 当前 WAL 的文件编号
  log::Writer* log_;            // 日志写入器包装类
  uint32_t seed_;               // 用于采样计算

  std::deque<Writer*> writers_; // 并发写入的等待队列（保存了每个线程的 WriteBatch 和条件变量）
  WriteBatch* tmp_batch_;       // 用于 Group Commit（组提交）合并写入时的临时缓冲区
  SnapshotList snapshots_;      // 活跃的快照链表，用于实现 MVCC（多版本并发控制）

  // 核心账本管理器，管理所有 SSTable 的生命周期与版本变迁
  VersionCatalog* const catalog_; 
  
  // 正在生成的 SSTable 文件编号集合（防止被 GC 误删）
  std::set<uint64_t> pending_outputs_; 

  // 是否已经调度或者正在运行一个后台压实作业？
  bool background_merge_scheduled_;

  // 记录用户手动触发的 Compaction 任务信息
  ManualMerge* manual_merge_;     
  
  // 记录后台写入或合并时发生的严重磁盘错误（如磁盘满）
  Status bg_error_;

  MergeStats stats_[config::kNumLevels];
};

// 使得 db 的选项合规并合理化。如果 result.info_log 不等于 src.info_log，那么调用方应该删除前者。
Options SanitizeOptions(const std::string& db,
                        const InternalKeyComparator* icmp,
                        const Options& src);

}  // namespace lsmdb
