#include "db/db_impl.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "db/builder.h"
#include "db/db.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/env.h"
#include "db/filter_policy.h"
#include "db/filename.h"
#include "db/status.h"
#include "db/table.h"
#include "db/table_builder.h"
#include "db/table_cache.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "sstable/iterator/merger.h"
#include "sstable/iterator/two_level_iterator.h"
#include "sstable/table/block.h"
#include "mvcc/version_catalog.h"
#include "utils/coding.h"
#include "utils/logging.h"
#include "utils/mutexlock.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"

namespace lsmdb {

const int kNumNonTableCacheFiles = 10;

// 为每个等待写入的线程保存的状态信息
struct DBImpl::Writer {
  explicit Writer(std::mutex* mu)
      : batch(nullptr), sync(false), done(false), cv(mu) {}

  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  CondVar cv;
};

struct DBImpl::MergeTaskState {
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };

  Output* current_output() { return &outputs[outputs.size() - 1]; }

  explicit MergeTaskState(MergeTask* c)
      : merge(c),
        smallest_snapshot(0),
        outfile(nullptr),
        builder(nullptr),
        total_bytes(0) {}

  MergeTask* const merge;

  // 小于此序列号的旧版本数据可被安全丢弃
  SequenceNumber smallest_snapshot;

  std::vector<Output> outputs;

  WritableFile* outfile;
  TableBuilder* builder;

  uint64_t total_bytes;
};

// 修正用户提供的配置选项，使其保持在合理范围内
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}

Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const Options& src) {
  Options result = src;
  result.comparator = icmp;
  ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
  ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
  ClipToRange(&result.block_size, 1 << 10, 4 << 20);
  if (result.info_log == nullptr) {
    src.env->CreateDir(dbname);
    src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
    Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
    if (!s.ok()) {
      result.info_log = nullptr;
    }
  }
  if (result.block_cache == nullptr) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  if (result.filter_policy == nullptr &&
      result.comparator != nullptr &&
      std::string(result.comparator->Name()) == "lsmdb.BytewiseComparator") {
    // 仅对默认字节序比较器启用布隆过滤器，避免自定义比较器的键等价语义不一致
    // bits_per_key=10 → 哈希函数数 k≈7，误判率 ~1%
    result.filter_policy = new FilterPolicy(10);
  }
  return result;
}

static int TableCacheSize(const Options& sanitized_options) {
  return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      options_(SanitizeOptions(dbname, &internal_comparator_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      owns_filter_policy_(options_.filter_policy != raw_options.filter_policy),
      dbname_(dbname),
      table_cache_(new TableCache(dbname_, &options_, TableCacheSize(options_))),
      db_lock_(nullptr),
      shutting_down_(false),
      background_work_finished_signal_(&mutex_),
      mem_(nullptr),
      imm_(nullptr),
      has_imm_(false),
      logfile_(nullptr),
      logfile_number_(0),
      log_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_merge_scheduled_(false),
      manual_merge_(nullptr),
      catalog_(new VersionCatalog(dbname_, &options_, table_cache_,
                               &internal_comparator_)) {}

DBImpl::~DBImpl() {
  // 等待后台工作完成
  mutex_.lock();
  shutting_down_.store(true, std::memory_order_release);
  while (background_merge_scheduled_) {
    background_work_finished_signal_.Wait();
  }
  mutex_.unlock();

  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }

  delete catalog_;
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete table_cache_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
  if (owns_filter_policy_) {
    delete options_.filter_policy;
  }
}

Status DBImpl::NewDB() {
  VersionDelta new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Sync();
    }
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->RemoveFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // 无需更改
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}

void DBImpl::RemoveObsoleteFiles() {
  if (!bg_error_.ok()) {
    return;
  }

  std::set<uint64_t> live = pending_outputs_;
  catalog_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  env_->GetChildren(dbname_, &filenames);
  uint64_t number;
  FileType type;
  std::vector<std::string> files_to_delete;
  for (std::string& filename : filenames) {
    if (ParseFileName(filename, &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          keep = ((number >= catalog_->LogNumber()) ||
                  (number == catalog_->PrevLogNumber()));
          break;
        case kDescriptorFile:
          keep = (number >= catalog_->CatalogFileNumber());
          break;
        case kTableFile:
          keep = (live.find(number) != live.end());
          break;
        case kTempFile:
          keep = (live.find(number) != live.end());
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }

      if (!keep) {
        files_to_delete.push_back(std::move(filename));
        if (type == kTableFile) {
          table_cache_->Evict(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n", static_cast<int>(type),
            static_cast<unsigned long long>(number));
      }
    }
  }

  mutex_.unlock();
  for (const std::string& filename : files_to_delete) {
    env_->RemoveFile(dbname_ + "/" + filename);
  }
  mutex_.lock();
}

Status DBImpl::Recover(VersionDelta* edit, bool* save_manifest) {
  env_->CreateDir(dbname_);
  assert(db_lock_ == nullptr);
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.create_if_missing) {
      Log(options_.info_log, "Creating DB %s since it was missing.",
          dbname_.c_str());
      s = NewDB();
      if (!s.ok()) {
        if (db_lock_ != nullptr) {
          env_->UnlockFile(db_lock_);
          db_lock_ = nullptr;
        }
        return s;
      }
    } else {
      if (db_lock_ != nullptr) {
        env_->UnlockFile(db_lock_);
        db_lock_ = nullptr;
      }
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      if (db_lock_ != nullptr) {
        env_->UnlockFile(db_lock_);
        db_lock_ = nullptr;
      }
      return Status::InvalidArgument(dbname_,
                                     "exists (error_if_exists is true)");
    }
  }

  s = catalog_->Recover(save_manifest);
  if (!s.ok()) {
    if (db_lock_ != nullptr) {
      env_->UnlockFile(db_lock_);
      db_lock_ = nullptr;
    }
    return s;
  }
  SequenceNumber max_sequence(0);

  const uint64_t min_log = catalog_->LogNumber();
  const uint64_t prev_log = catalog_->PrevLogNumber();
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    if (db_lock_ != nullptr) {
      env_->UnlockFile(db_lock_);
      db_lock_ = nullptr;
    }
    return s;
  }

  std::set<uint64_t> expected;
  catalog_->AddLiveFiles(&expected);
  uint64_t number;
  FileType type;
  std::vector<uint64_t> logs;
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      expected.erase(number);
      if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
        logs.push_back(number);
    }
  }
  if (!expected.empty()) {
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%d missing files; e.g.",
                  static_cast<int>(expected.size()));
    if (db_lock_ != nullptr) {
      env_->UnlockFile(db_lock_);
      db_lock_ = nullptr;
    }
    return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
  }

  std::sort(logs.begin(), logs.end());

  for (size_t i = 0; i < logs.size(); i++) {
    s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit,
                       &max_sequence);
    if (!s.ok()) {
      if (db_lock_ != nullptr) {
        env_->UnlockFile(db_lock_);
        db_lock_ = nullptr;
      }
      return s;
    }
    catalog_->MarkFileNumberUsed(logs[i]);
  }

  if (catalog_->LastSequence() < max_sequence) {
    catalog_->SetLastSequence(max_sequence);
  }

  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
                              bool* save_manifest, VersionDelta* edit,
                              SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;
    void Corruption(size_t bytes, const Status& s) override {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == nullptr ? "(ignoring error) " : ""), fname,
          static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) *this->status = s;
    }
  };

  std::string fname = LogFileName(dbname_, log_number);

  SequentialFile* file;
  Status status = env_->NewSequentialFile(fname, &file);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : nullptr);
  log::Reader reader(file, &reporter, true /*checksum*/, 0 /*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long)log_number);

  std::string scratch;
  Slice record;
  WriteBatch batch;
  int compactions = 0;
  MemTable* mem = nullptr;
  while (reader.ReadRecord(&record, &scratch) && status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(record.size(),
                          Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == nullptr) {
      mem = new MemTable(internal_comparator_);
      mem->Ref();
    }
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) +
                                    WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      compactions++;
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
      mem->Unref();
      mem = nullptr;
      if (!status.ok()) {
        break;
      }
    }
  }

  delete file;

  if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
    assert(logfile_ == nullptr);
    assert(log_ == nullptr);
    assert(mem_ == nullptr);
    uint64_t lfile_size;
    if (env_->GetFileSize(fname, &lfile_size).ok() &&
        env_->NewAppendableFile(fname, &logfile_).ok()) {
      Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
      log_ = new log::Writer(logfile_, lfile_size);
      logfile_number_ = log_number;
      if (mem != nullptr) {
        mem_ = mem;
        mem = nullptr;
      } else {
        mem_ = new MemTable(internal_comparator_);
        mem_->Ref();
      }
    }
  }

  if (mem != nullptr) {
    if (status.ok()) {
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
    }
    mem->Unref();
  }

  return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionDelta* edit,
                                Version* base) {
  const uint64_t start_micros = env_->NowMicros();
  SSTableDescriptor meta;
  meta.number = catalog_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long)meta.number);

  Status s;
  {
    mutex_.unlock();
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    mutex_.lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long)meta.number, (unsigned long long)meta.file_size,
      s.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);

  int level = 0;

  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != nullptr) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.number, meta.file_size, meta.smallest,
                  meta.largest);
  }

  MergeStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  return s;
}

void DBImpl::CompactMemTable() {
  assert(imm_ != nullptr);

  VersionDelta edit;
  Version* base = catalog_->current();
  base->Ref();
  Status s = WriteLevel0Table(imm_, &edit, base);
  base->Unref();

  if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  if (s.ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);
    s = catalog_->CommitDelta(&edit, &mutex_);
  }

  if (s.ok()) {
    imm_->Unref();
    imm_ = nullptr;
    has_imm_.store(false, std::memory_order_release);
    RemoveObsoleteFiles();
  } else {
    RecordBackgroundError(s);
  }
}

void DBImpl::RecordBackgroundError(const Status& s) {
  if (bg_error_.ok()) {
    bg_error_ = s;
    background_work_finished_signal_.SignalAll();
  }
}

void DBImpl::MaybeScheduleMerge() {
  if (background_merge_scheduled_) {
    // 已被调度
  } else if (shutting_down_.load(std::memory_order_acquire)) {
    // DB 正在被删除
  } else if (!bg_error_.ok()) {
    // 已经出错了
  } else if (imm_ == nullptr && manual_merge_ == nullptr &&
             !catalog_->NeedsMerge()) {
    // 没有工作可做
  } else {
    background_merge_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}

void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  MutexLock l(&mutex_);
  assert(background_merge_scheduled_);
  if (shutting_down_.load(std::memory_order_acquire)) {
  } else if (!bg_error_.ok()) {
  } else {
    BackgroundMerge();
  }

  background_merge_scheduled_ = false;

  MaybeScheduleMerge();
  background_work_finished_signal_.SignalAll();
}

void DBImpl::BackgroundMerge() {
  if (imm_ != nullptr) {
    CompactMemTable();
    return;
  }

  MergeTask* c;
  bool is_manual = (manual_merge_ != nullptr);
  InternalKey manual_end;
  if (is_manual) {
    ManualMerge* m = manual_merge_;
    c = catalog_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == nullptr);
    if (c != nullptr) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    Log(options_.info_log,
        "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        m->level, (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
    c = catalog_->SelectMergeCandidate();
  }

  Status status;
  if (c == nullptr) {
    // 无事可做
  } else if (!is_manual && c->IsTrivialMove()) {
    // TrivialMove 优化：文件直接移至下一层级
    assert(c->num_input_files(0) == 1);
    SSTableDescriptor* f = c->input(0, 0);
    c->edit()->RemoveFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest,
                       f->largest);
    status = catalog_->CommitDelta(c->edit(), &mutex_);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    VersionCatalog::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number), c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(), catalog_->LevelSummary(&tmp));
  } else {
    MergeTaskState* compact = new MergeTaskState(c);
    status = DoMergeWork(compact);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    CleanupMerge(compact);
    c->ReleaseInputs();
    RemoveObsoleteFiles();
  }
  delete c;

  if (status.ok()) {
    // 完成
  } else if (shutting_down_.load(std::memory_order_acquire)) {
    // 忽略关闭期间的 compaction 错误
  } else {
    Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
  }

  if (is_manual) {
    ManualMerge* m = manual_merge_;
    if (!status.ok()) {
      m->done = true;
    }
    if (!m->done) {
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    manual_merge_ = nullptr;
  }
}

// ──────────────────── Compaction 辅助函数 ────────────────────

void DBImpl::CleanupMerge(MergeTaskState* compact) {
  if (compact->builder != nullptr) {
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == nullptr);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const MergeTaskState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  delete compact;
}

Status DBImpl::OpenMergeOutputFile(MergeTaskState* compact) {
  assert(compact != nullptr);
  assert(compact->builder == nullptr);
  uint64_t file_number;
  {
    mutex_.lock();
    file_number = catalog_->NewFileNumber();
    pending_outputs_.insert(file_number);
    MergeTaskState::Output out;
    out.number = file_number;
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);
    mutex_.unlock();
  }

  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile);
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return s;
}

Status DBImpl::FinishMergeOutputFile(MergeTaskState* compact,
                                     Iterator* input) {
  assert(compact != nullptr);
  assert(compact->outfile != nullptr);
  assert(compact->builder != nullptr);

  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  Status s = input->status();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = nullptr;

  if (s.ok()) {
    s = compact->outfile->Sync();
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = nullptr;

  if (s.ok() && current_entries > 0) {
    // 验证生成的表可被正确打开
    Iterator* iter =
        table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log, "Generated table #%llu@%d: %lld keys, %lld bytes",
          (unsigned long long)output_number, compact->merge->level(),
          (unsigned long long)current_entries,
          (unsigned long long)current_bytes);
    }
  }
  return s;
}

Status DBImpl::InstallMergeResults(MergeTaskState* compact) {
  Log(options_.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->merge->num_input_files(0), compact->merge->level(),
      compact->merge->num_input_files(1), compact->merge->level() + 1,
      static_cast<long long>(compact->total_bytes));

  compact->merge->AddInputDeletions(compact->merge->edit());
  const int level = compact->merge->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const MergeTaskState::Output& out = compact->outputs[i];
    compact->merge->edit()->AddFile(level + 1, out.number, out.file_size,
                                    out.smallest, out.largest);
  }
  return catalog_->CommitDelta(compact->merge->edit(), &mutex_);
}

Status DBImpl::DoMergeWork(MergeTaskState* compact) {
  const uint64_t start_micros = env_->NowMicros();
  int64_t imm_micros = 0;

  Log(options_.info_log, "Compacting %d@%d + %d@%d files",
      compact->merge->num_input_files(0), compact->merge->level(),
      compact->merge->num_input_files(1),
      compact->merge->level() + 1);

  assert(catalog_->NumLevelFiles(compact->merge->level()) > 0);
  assert(compact->builder == nullptr);
  assert(compact->outfile == nullptr);
  if (snapshots_.empty()) {
    compact->smallest_snapshot = catalog_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
  }

  Iterator* input = catalog_->MakeInputIterator(compact->merge);

  // 在实际做 compaction 工作时释放锁
  mutex_.unlock();

  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    // 优先处理 imm_ 的刷盘工作
    if (has_imm_.load(std::memory_order_relaxed)) {
      const uint64_t imm_start = env_->NowMicros();
      mutex_.lock();
      if (imm_ != nullptr) {
        CompactMemTable();
        background_work_finished_signal_.SignalAll();
      }
      mutex_.unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }

    Slice key = input->key();
    if (compact->merge->ShouldStopBefore(key) &&
        compact->builder != nullptr) {
      status = FinishMergeOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // 处理键值：判断是否可丢弃（被更新版本覆盖 / 墓碑可清理）
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
              0) {
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // 被同键更新的条目隐藏
        drop = true;
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->merge->IsBaseLevelForKey(ikey.user_key)) {
        // 墓碑标记在底层无数据可删除，可安全丢弃
        drop = true;
      }

      last_sequence_for_key = ikey.sequence;
    }

    if (!drop) {
      if (compact->builder == nullptr) {
        status = OpenMergeOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, input->value());

      if (compact->builder->FileSize() >=
          compact->merge->MaxOutputFileSize()) {
        status = FinishMergeOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }

    input->Next();
  }

  if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.ok() && compact->builder != nullptr) {
    status = FinishMergeOutputFile(compact, input);
  }
  if (status.ok()) {
    status = input->status();
  }
  delete input;
  input = nullptr;

  MergeStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->merge->num_input_files(which); i++) {
      stats.bytes_read += compact->merge->input(which, i)->file_size;
    }
  }
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }

  mutex_.lock();
  stats_[compact->merge->level() + 1].Add(stats);

  if (status.ok()) {
    status = InstallMergeResults(compact);
  }
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  VersionCatalog::LevelSummaryStorage tmp;
  Log(options_.info_log, "compacted to: %s", catalog_->LevelSummary(&tmp));
  return status;
}

// ──────────────────── 迭代器基础设施 ────────────────────

namespace {

struct IterState {
  std::mutex* const mu;
  Version* const version;
  MemTable* const mem;
  MemTable* const imm;

  IterState(std::mutex* mutex, MemTable* mem, MemTable* imm, Version* version)
      : mu(mutex), version(version), mem(mem), imm(imm) {}
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->lock();
  state->mem->Unref();
  if (state->imm != nullptr) state->imm->Unref();
  state->version->Unref();
  state->mu->unlock();
  delete state;
}

}  // anonymous namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  mutex_.lock();
  *latest_snapshot = catalog_->LastSequence();

  // 收集所有需要的子迭代器：memtable → imm → SSTables
  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != nullptr) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  catalog_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  catalog_->current()->Ref();

  IterState* cleanup = new IterState(&mutex_, mem_, imm_, catalog_->current());
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

  *seed = ++seed_;
  mutex_.unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  uint32_t ignored_seed;
  return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return catalog_->MaxNextLevelOverlappingBytes();
}

// ──────────────────── 读操作 ────────────────────

Status DBImpl::Get(const ReadOptions& options, const Slice& key,
                   std::string* value) {
  Status s;
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  if (options.snapshot != nullptr) {
    snapshot =
        static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
  } else {
    snapshot = catalog_->LastSequence();
  }

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = catalog_->current();
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // 在读取文件和 memtable 时解锁
  {
    mutex_.unlock();
    LookupKey lkey(key, snapshot);
    if (mem->Get(lkey, value, &s)) {
      // 命中活跃 memtable
    } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
      // 命中不可变 memtable
    } else {
      s = current->Get(options, lkey, value, &stats);
      have_stat_update = true;
    }
    mutex_.lock();
  }

  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleMerge();
  }
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  current->Unref();
  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  return NewDBIterator(this, user_comparator(), iter,
                       (options.snapshot != nullptr
                            ? static_cast<const SnapshotImpl*>(options.snapshot)
                                  ->sequence_number()
                            : latest_snapshot),
                       seed);
}

void DBImpl::RecordReadSample(Slice key) {
  MutexLock l(&mutex_);
  if (catalog_->current()->RecordReadSample(key)) {
    MaybeScheduleMerge();
  }
}

// ──────────────────── 快照管理 ────────────────────

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(catalog_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  MutexLock l(&mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

// ──────────────────── 写操作 ────────────────────

Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  Writer w(&mutex_);
  w.batch = updates;
  w.sync = options.sync;
  w.done = false;

  MutexLock l(&mutex_);
  writers_.push_back(&w);
  // 当前写请求必须成为 batch leader 才能执行 write group
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }

  // 可能在构建 batch / 写入过程中短暂释放锁
  Status status = ApplyBackpressure(updates == nullptr);
  uint64_t last_sequence = catalog_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok() && updates != nullptr) {
    // 将队列中一段连续 writer 合并成一个 batch group
    WriteBatch* write_batch = CoalesceWriteBatch(&last_writer);
    WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(write_batch);

    {
      // 释放锁执行三步写入：WAL → 可选 fsync → MemTable
      mutex_.unlock();
      status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
      bool sync_error = false;
      if (status.ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.ok()) {
          sync_error = true;
        }
      }
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(write_batch, mem_);
      }
      mutex_.lock();
      if (sync_error) {
        RecordBackgroundError(status);
      }
    }
    if (write_batch == tmp_batch_) tmp_batch_->Clear();

    catalog_->SetLastSequence(last_sequence);
  }

  // 通知 batch group 中所有等待的 writer
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }

  // 唤醒写入队列的新头部
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

WriteBatch* DBImpl::CoalesceWriteBatch(Writer** last_writer) {
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != nullptr);

  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // 动态容量上限：Leader 小请求时限制合并膨胀以控制延迟抖动
  size_t max_size = 1 << 20;  // 硬上限 1MB
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);  // 软上限：Leader 体积 + 128KB
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;

    if (w->sync && !first->sync) {
      // 不合并 sync 与 non-sync 请求，否则会破坏持久性承诺
      break;
    }

    if (w->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        break;
      }

      if (result == first->batch) {
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

// 流控与背压：L0 文件数限速 / MemTable 写满时切换 / 等待后台刷盘
Status DBImpl::ApplyBackpressure(bool force) {
  assert(!writers_.empty());
  bool allow_delay = !force;
  Status s;
  while (true) {
    if (!bg_error_.ok()) {
      s = bg_error_;
      break;
    } else if (allow_delay && catalog_->NumLevelFiles(0) >=
                                  config::kL0_SlowdownWritesTrigger) {
      // L0 文件数触及慢写阈值：每次写入休眠 1ms 以降速
      mutex_.unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;
      mutex_.lock();
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      // MemTable 还有空间
      break;
    } else if (imm_ != nullptr) {
      // 前一个 MemTable 仍在刷盘，等待后台完成
      background_work_finished_signal_.Wait();
    } else if (catalog_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // L0 文件数触及硬限制，必须等待压实
      background_work_finished_signal_.Wait();
    } else {
      // MemTable 写满且 imm_ 空闲：执行 MemTable 切换
      assert(catalog_->PrevLogNumber() == 0);
      uint64_t new_log_number = catalog_->NewFileNumber();
      WritableFile* lfile = nullptr;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      if (!s.ok()) {
        catalog_->ReuseFileNumber(new_log_number);
        break;
      }

      delete log_;
      s = logfile_->Close();
      if (!s.ok()) {
        RecordBackgroundError(s);
      }
      delete logfile_;

      logfile_ = lfile;
      logfile_number_ = new_log_number;
      log_ = new log::Writer(lfile);

      imm_ = mem_;
      has_imm_.store(true, std::memory_order_release);
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      force = false;

      MaybeScheduleMerge();
    }
  }
  return s;
}

// ──────────────────── 压实入口（手动触发 / 测试辅助） ────────────────────

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = catalog_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable();
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,
                               const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualMerge manual;
  manual.level = level;
  manual.done = false;
  if (begin == nullptr) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.load(std::memory_order_acquire) &&
         bg_error_.ok()) {
    if (manual_merge_ == nullptr) {
      manual_merge_ = &manual;
      MaybeScheduleMerge();
    } else {
      background_work_finished_signal_.Wait();
    }
  }
  while (background_merge_scheduled_) {
    background_work_finished_signal_.Wait();
  }
  if (manual_merge_ == &manual) {
    manual_merge_ = nullptr;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // nullptr batch 意味着等待之前写操作的完成
  Status s = Write(WriteOptions(), nullptr);
  if (s.ok()) {
    MutexLock l(&mutex_);
    while (imm_ != nullptr && bg_error_.ok() &&
           !shutting_down_.load(std::memory_order_acquire)) {
      background_work_finished_signal_.Wait();
    }
    if (imm_ != nullptr) {
      s = bg_error_;
    }
  }
  return s;
}

// ──────────────────── 属性查询与统计 ────────────────────

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("lsmdb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      std::snprintf(buf, sizeof(buf), "%d",
                    catalog_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "                               Compactions\n"
                  "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
                  "--------------------------------------------------\n");
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = catalog_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        std::snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
                      level, files, catalog_->NumLevelBytes(level) / 1048576.0,
                      stats_[level].micros / 1e6,
                      stats_[level].bytes_read / 1048576.0,
                      stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = catalog_->current()->DebugString();
    return true;
  } else if (in == "approximate-memory-usage") {
    size_t total_usage = options_.block_cache->TotalCharge();
    if (mem_) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%llu",
                  static_cast<unsigned long long>(total_usage));
    value->append(buf);
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
  MutexLock l(&mutex_);
  Version* v = catalog_->current();
  v->Ref();
  for (int i = 0; i < n; i++) {
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = catalog_->ApproximateOffsetOf(v, k1);
    uint64_t limit = catalog_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }
  v->Unref();
}

// ──────────────────── 以下为 DB 基类接口实现 ────────────────────

Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() = default;

Snapshot::~Snapshot() = default;

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
  *dbptr = nullptr;

  DBImpl* impl = new DBImpl(options, dbname);
  impl->mutex_.lock();
  VersionDelta edit;
  bool save_manifest = false;
  Status s = impl->Recover(&edit, &save_manifest);
  if (s.ok() && impl->mem_ == nullptr) {
    uint64_t new_log_number = impl->catalog_->NewFileNumber();
    WritableFile* lfile;
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);
    if (s.ok()) {
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->Ref();
    }
  }
  if (s.ok() && save_manifest) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(impl->logfile_number_);
    s = impl->catalog_->CommitDelta(&edit, &impl->mutex_);
  }
  if (s.ok()) {
    impl->RemoveObsoleteFiles();
    impl->MaybeScheduleMerge();
  }
  impl->mutex_.unlock();
  if (s.ok()) {
    assert(impl->mem_ != nullptr);
    *dbptr = impl;
  } else {
    delete impl;
  }
  return s;
}

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  Status result = env->GetChildren(dbname, &filenames);
  if (!result.ok()) {
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {
        Status del = env->RemoveFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);
    env->RemoveFile(lockname);
    env->RemoveDir(dbname);
  }
  return result;
}

}  // namespace lsmdb
