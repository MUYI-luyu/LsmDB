#include "mvcc/version_catalog.h"

#include <cstdio>
#include <algorithm>
#include <vector>

#include "db/dbformat.h"
#include "db/env.h"
#include "db/filename.h"
#include "db/status.h"
#include "db/table_cache.h"
#include "sstable/iterator/merger.h"
#include "sstable/iterator/two_level_iterator.h"
#include "utils/coding.h"
#include "utils/logging.h"
#include "wal/log_reader.h"
#include "wal/log_writer.h"

namespace lsmdb {

using namespace coding;

static const int kTargetFileSize = 2 * 1048576;
static const int64_t kMaxGrandParentOverlapBytes = 10 * kTargetFileSize;

static double MaxBytesForLevel(int level) {
  double result = 10. * 1048576.0;
  while (level > 1) { result *= 10; level--; }
  return result;
}

static uint64_t MaxFileSizeForLevel(const Options* options, int level) {
  return kTargetFileSize;
}

static int64_t TotalFileSize(const std::vector<SSTableDescriptor*>& files) {
  int64_t sum = 0;
  for (size_t i = 0; i < files.size(); i++) sum += files[i]->file_size;
  return sum;
}

int FindFile(const InternalKeyComparator& icmp,
             const std::vector<SSTableDescriptor*>& files, const Slice& key) {
  auto cmp = [&icmp](const SSTableDescriptor* f, const Slice& k) {
    return icmp.Compare(f->largest.Encode(), k) < 0;
  };
  return std::lower_bound(files.begin(), files.end(), key, cmp) - files.begin();
}

static bool AfterFile(const Comparator* ucmp, const Slice* user_key,
                      const SSTableDescriptor* f) {
  if (user_key == nullptr) return false;
  return ucmp->Compare(*user_key, f->largest.user_key()) > 0;
}

static bool BeforeFile(const Comparator* ucmp, const Slice* user_key,
                       const SSTableDescriptor* f) {
  if (user_key == nullptr) return false;
  return ucmp->Compare(*user_key, f->smallest.user_key()) < 0;
}

bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<SSTableDescriptor*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  if (!disjoint_sorted_files) {
    for (size_t i = 0; i < files.size(); i++) {
      const SSTableDescriptor* f = files[i];
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) continue;
      return true;
    }
    return false;
  }
  uint32_t index = 0;
  if (smallest_user_key != nullptr) {
    InternalKey boundary_key(*smallest_user_key, kMaxSequenceNumber,
                                kValueTypeForSeek);
    Slice boundary = boundary_key.Encode();
    index = FindFile(icmp, files, boundary);
    if (index >= files.size()) return false;
  }
  return index < files.size() &&
         !BeforeFile(ucmp, largest_user_key, files[index]);
}

void AddBoundaryInputs(const InternalKeyComparator& icmp,
                       const std::vector<SSTableDescriptor*>& level_files,
                       std::vector<SSTableDescriptor*>* compaction_files) {
  if (compaction_files->empty() || level_files.empty()) return;
  // Sort level_files by smallest key (matching original LsmDB FileSet order)
  std::vector<SSTableDescriptor*> sorted(level_files.begin(), level_files.end());
  std::sort(sorted.begin(), sorted.end(),
            [&icmp](SSTableDescriptor* a, SSTableDescriptor* b) {
              int r = icmp.Compare(a->smallest, b->smallest);
              if (r != 0) return r < 0;
              return a->number < b->number;
            });
  // Track by smallest.Encode().ToString() to avoid adding duplicate ranges
  std::set<std::string> added;
  for (auto* f : *compaction_files)
    added.insert(f->smallest.Encode().ToString());
  bool keep_looking = true;
  while (keep_looking) {
    keep_looking = false;
    for (auto* f : sorted) {
      std::string smallest_encoded = f->smallest.Encode().ToString();
      if (added.count(smallest_encoded)) continue;
      if (icmp.user_comparator()->Compare(
              compaction_files->back()->largest.user_key(),
              f->smallest.user_key()) == 0) {
        compaction_files->push_back(f);
        added.insert(smallest_encoded);
        keep_looking = true;
      }
    }
  }
}

static Iterator* GetFileIterator(void* arg, const ReadOptions& options,
                                  const Slice& file_value) {
  TableCache* cache = reinterpret_cast<TableCache*>(arg);
  if (file_value.size() != 16)
    return NewErrorIterator(Status::Corruption("FileReader invoked with unexpected value"));
  uint64_t number = DecodeFixed64(file_value.data());
  uint64_t size = DecodeFixed64(file_value.data() + 8);
  return cache->NewIterator(options, number, size);
}

// ── Saver Helper (for Version::Get) ────────────────────────

enum SaverState { kNotFound, kFound, kDeleted, kCorrupt };

struct Saver {
  SaverState state;
  const Comparator* ucmp;
  Slice user_key;
  std::string* value;
};

static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  Saver* s = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) {
    s->state = kCorrupt;
  } else {
    // Only accept the result the user_key matches exactly.
    // The internal key (which includes sequence number) may be different
    // from the search key since we used max_sequence in the lookup key.
    if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
      s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
      s->value->assign(v.data(), v.size());
    }
  }
}

// ── VersionCatalog::Builder ────────────────────────────────────

class VersionCatalog::Builder {
 public:
  struct FileComparator {
    bool operator()(const SSTableDescriptor* a, const SSTableDescriptor* b) const {
      return a->number < b->number;
    }
  };

  Builder(VersionCatalog* catalog, Version* base) : catalog_(catalog), base_(base) {
    base_->Ref();
    levels_ = new std::set<SSTableDescriptor*, FileComparator>[config::kNumLevels]();
    deleted_files_ = new std::set<uint64_t>[config::kNumLevels]();
  }

  ~Builder() {
    for (int level = 0; level < config::kNumLevels; level++) {
      for (SSTableDescriptor* f : levels_[level]) { f->refs--; if (f->refs <= 0) delete f; }
    }
    delete[] levels_;
    delete[] deleted_files_;
    base_->Unref();
  }

  void Apply(VersionDelta* edit) {
    for (auto& cp : edit->compact_pointers_)
      catalog_->compact_pointer_[cp.first] = cp.second.Encode().ToString();
    for (auto& d : edit->deleted_files_) {
      int level = d.first;
      uint64_t number = d.second;
      // Check this file was newly added in this batch
      bool found_in_delta = false;
      for (auto it = levels_[level].begin(); it != levels_[level].end(); ++it) {
        if ((*it)->number == number) {
          (*it)->refs--;
          if ((*it)->refs <= 0) delete *it;
          levels_[level].erase(it);
          found_in_delta = true;
          break;
        }
      }
      // If not found in the delta (i.e., it's a base file), record the deletion
      // so SaveTo will exclude it from the new version.
      if (!found_in_delta) {
        deleted_files_[level].insert(number);
      }
    }
    for (auto& nf : edit->new_files_) {
      int level = nf.first;
      SSTableDescriptor* f = new SSTableDescriptor(nf.second);
      f->refs = 1;
      // 如果同一个文件编号在本次 Edit 中
      // 既被删除又被添加（例如 TrivialMove），应当从 deleted_files_ 中
      // 擦除该记录，使最终的 SaveTo 正确保留该文件。
      deleted_files_[level].erase(f->number);
      levels_[level].insert(f);
    }
  }

  void SaveTo(Version* v) {
    for (int level = 0; level < config::kNumLevels; level++) {
      // Include base files that are NOT deleted and NOT already in the delta
      for (auto* f : base_->files_[level]) {
        if (deleted_files_[level].count(f->number) > 0) {
          // This base file was deleted; skip it
          continue;
        }
        bool found_in_delta = false;
        for (auto* added : levels_[level]) {
          if (added->number == f->number) { found_in_delta = true; break; }
        }
        if (!found_in_delta) { f->refs++; v->files_[level].push_back(f); }
      }
      // Include all newly added files
      for (auto* f : levels_[level]) { f->refs++; v->files_[level].push_back(f); }
      if (level > 0)
        std::sort(v->files_[level].begin(), v->files_[level].end(),
                  [this](SSTableDescriptor* a, SSTableDescriptor* b) {
                    return catalog_->icmp_.Compare(a->smallest, b->smallest) < 0; });
    }
  }

  void MaybeAddFile(Version* v, int level, SSTableDescriptor* f) {
    if (level > 0)
      assert(v->files_[level].empty() ||
             catalog_->icmp_.Compare(v->files_[level].back()->largest, f->smallest) < 0);
    f->refs++;
    v->files_[level].push_back(f);
  }

 private:
  VersionCatalog* catalog_;
  Version* base_;
  std::set<SSTableDescriptor*, FileComparator>* levels_;
  std::set<uint64_t>* deleted_files_;
};

// ── VersionCatalog ──────────────────────────────────────────────

VersionCatalog::VersionCatalog(const std::string& dbname, const Options* options,
                       TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env), dbname_(dbname), options_(options),
      table_cache_(table_cache), icmp_(*cmp),
      next_file_number_(2), catalog_file_number_(0),
      last_sequence_(0), log_number_(0), prev_log_number_(0),
      catalog_file_(nullptr), catalog_writer_(nullptr),
      dummy_versions_(this), current_(nullptr) {
  AppendVersion(new Version(this));
}

VersionCatalog::~VersionCatalog() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);
  delete catalog_writer_;
  delete catalog_file_;
}

void VersionCatalog::AppendVersion(Version* v) {
  assert(v->refs_ == 0 && v != current_);
  if (current_) current_->Unref();
  current_ = v;
  v->Ref();
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

const char* VersionCatalog::LevelSummary(LevelSummaryStorage* scratch) const {
  std::snprintf(scratch->buffer, sizeof(scratch->buffer),
                "files[ %zu %zu %zu %zu %zu %zu %zu ]",
                current_->files_[0].size(), current_->files_[1].size(),
                current_->files_[2].size(), current_->files_[3].size(),
                current_->files_[4].size(), current_->files_[5].size(),
                current_->files_[6].size());
  return scratch->buffer;
}

Status VersionCatalog::CommitDelta(VersionDelta* edit, std::mutex* mu) {
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else { edit->SetLogNumber(log_number_); }
  if (!edit->has_prev_log_number_) edit->SetPrevLogNumber(prev_log_number_);
  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  Version* v = new Version(this);
  { Builder builder(this, current_); builder.Apply(edit); builder.SaveTo(v); }
  Finalize(v);

  std::string new_manifest_file;
  if (catalog_writer_ == nullptr) {
    edit->SetNextFile(next_file_number_);
    new_manifest_file = DescriptorFileName(dbname_, catalog_file_number_);
    WritableFile* file;
    Status s = env_->NewWritableFile(new_manifest_file, &file);
    if (!s.ok()) { delete v; return s; }
    catalog_file_ = file;
    catalog_writer_ = new log::Writer(file);
  }

  // 如果是新创建的 MANIFEST 文件（catalog_writer_ 刚被初始化），
  // 需要先写入完整的版本快照，确保 manifest 自包含所有文件信息。
  // 这与原始 LsmDB 行为一致，防止因 manifest 不完整导致的数据追踪丢失。
  Status s;
  if (!new_manifest_file.empty()) {
    s = InstallFullSnapshot(catalog_writer_);
    if (!s.ok()) { delete v; return s; }
  }

  std::string record;
  edit->EncodeTo(&record);
  s = catalog_writer_->AddRecord(record);
  if (s.ok()) s = catalog_file_->Sync();
  if (!s.ok()) { delete v; return s; }

  if (s.ok() && !new_manifest_file.empty()) {
    s = SetCurrentFile(env_, dbname_, catalog_file_number_);
    if (!s.ok()) { delete v; return s; }
  }

  AppendVersion(v);
  log_number_ = edit->log_number_;
  prev_log_number_ = edit->prev_log_number_;
  return Status::OK();
}

Status VersionCatalog::Recover(bool* save_manifest) {
  struct LogReporter : public log::Reader::Reporter {
    Status* status;
    void Corruption(size_t bytes, const Status& s) override {
      if (this->status->ok()) *this->status = s;
    }
  };

  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) return s;

  if (current.empty() || current.back() != '\n')
    return Status::Corruption("CURRENT file does not end with newline");
  current.pop_back();

  SequentialFile* file;
  s = env_->NewSequentialFile(dbname_ + "/" + current, &file);
  if (!s.ok()) return s;

  bool have_log_number = false, have_prev_log_number = false;
  bool have_next_file = false, have_last_sequence = false;
  uint64_t next_file = 0, last_sequence = 0;
  uint64_t log_number = 0, prev_log_number = 0;
  Builder builder(this, current_);

  {
    LogReporter reporter;
    reporter.status = &s;
    log::Reader reader(file, &reporter, true, 0);
    Slice record;
    std::string scratch;
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      VersionDelta edit;
      s = edit.DecodeFrom(record);
      if (s.ok() && edit.has_comparator_ &&
          edit.comparator_ != icmp_.user_comparator()->Name())
        s = Status::InvalidArgument(edit.comparator_ + " does not match existing comparator ",
                                    icmp_.user_comparator()->Name());
      if (s.ok()) builder.Apply(&edit);
      if (edit.has_log_number_) { log_number = edit.log_number_; have_log_number = true; }
      if (edit.has_prev_log_number_) { prev_log_number = edit.prev_log_number_; have_prev_log_number = true; }
      if (edit.has_next_file_number_) { next_file = edit.next_file_number_; have_next_file = true; }
      if (edit.has_last_sequence_) { last_sequence = edit.last_sequence_; have_last_sequence = true; }
    }
  }
  delete file;

  if (s.ok()) {
    if (!have_next_file)
      s = Status::Corruption("no meta-nextfile entry in descriptor");
    else if (!have_log_number)
      s = Status::Corruption("no meta-lognumber entry in descriptor");
    else if (!have_last_sequence)
      s = Status::Corruption("no meta-lastsequence entry in descriptor");
    if (!have_prev_log_number) prev_log_number = 0;
    MarkFileNumberUsed(prev_log_number);
    MarkFileNumberUsed(log_number);
  }

  if (s.ok()) {
    Version* v = new Version(this);
    builder.SaveTo(v);
    Finalize(v);
    AppendVersion(v);

    // 从 CURRENT 文件名解析出实际的 MANIFEST 文件编号。
    // current 此时形如 "MANIFEST-000001"，需要提取出数字部分。
    {
      uint64_t parsed;
      FileType ft;
      if (ParseFileName(current, &parsed, &ft) && ft == kDescriptorFile) {
        catalog_file_number_ = parsed;
      } else {
        catalog_file_number_ = next_file;
      }
    }
    MarkFileNumberUsed(next_file);
    last_sequence_ = last_sequence;
    log_number_ = log_number;
    prev_log_number_ = prev_log_number;
    *save_manifest = true;
  }
  return s;
}

bool VersionCatalog::ReuseManifest(const std::string& dscname,
                                const std::string& dscbase) {
  return false;
}

void VersionCatalog::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) next_file_number_ = number + 1;
}

void VersionCatalog::Finalize(Version* v) {
  int best_level = 0;
  double best_score = 0;
  for (int level = 0; level < config::kNumLevels - 1; level++) {
    double score = (level == 0)
        ? v->files_[0].size() / (double)config::kL0_CompactionTrigger
        : TotalFileSize(v->files_[level]) / MaxBytesForLevel(level);
    if (score > best_score) { best_level = level; best_score = score; }
  }
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

int VersionCatalog::NumLevelFiles(int level) const {
  assert(level >= 0 && level < config::kNumLevels);
  return current_->files_[level].size();
}

int64_t VersionCatalog::NumLevelBytes(int level) const {
  assert(level >= 0 && level < config::kNumLevels);
  return TotalFileSize(current_->files_[level]);
}

int64_t VersionCatalog::MaxNextLevelOverlappingBytes() {
  int64_t result = 0;
  std::vector<SSTableDescriptor*> overlaps;
  for (int level = 1; level < config::kNumLevels - 1; level++)
    for (auto* f : current_->files_[level]) {
      current_->GetOverlappingInputs(level + 1, &f->smallest, &f->largest, &overlaps);
      result = std::max(result, TotalFileSize(overlaps));
    }
  return result;
}

void VersionCatalog::AddLiveFiles(std::set<uint64_t>* live) {
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_)
    for (int level = 0; level < config::kNumLevels; level++)
      for (auto* f : v->files_[level]) live->insert(f->number);
}

uint64_t VersionCatalog::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
  uint64_t result = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    for (auto* f : v->files_[level]) {
      if (icmp_.Compare(f->largest, ikey) <= 0) {
        result += f->file_size;
      } else if (icmp_.Compare(f->smallest, ikey) > 0) {
        if (level > 0) break;
      } else {
        ReadOptions options;
        result += table_cache_->ApproximateOffsetOf(options, f->number,
                                                     f->file_size,
                                                     ikey.Encode());
      }
    }
  }
  return result;
}

void VersionCatalog::GetRange(const std::vector<SSTableDescriptor*>& inputs,
                           InternalKey* smallest, InternalKey* largest) {
  assert(!inputs.empty());
  *smallest = inputs[0]->smallest;
  *largest = inputs[0]->largest;
  for (size_t i = 1; i < inputs.size(); i++) {
    if (icmp_.Compare(inputs[i]->smallest, *smallest) < 0) *smallest = inputs[i]->smallest;
    if (icmp_.Compare(inputs[i]->largest, *largest) > 0) *largest = inputs[i]->largest;
  }
}

void VersionCatalog::GetRange2(const std::vector<SSTableDescriptor*>& inputs1,
                            const std::vector<SSTableDescriptor*>& inputs2,
                            InternalKey* smallest, InternalKey* largest) {
  std::vector<SSTableDescriptor*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

MergeTask* VersionCatalog::SelectMergeCandidate() {
  bool size_compaction = (current_->compaction_score_ >= 1);
  bool seek_compaction = (current_->file_to_compact_ != nullptr);
  if (!size_compaction && !seek_compaction) return nullptr;

  int level;
  MergeTask* c;
  if (size_compaction) {
    level = current_->compaction_level_;
    assert(level >= 0 && level + 1 < config::kNumLevels);
    c = new MergeTask(options_, level);
    for (auto* f : current_->files_[level]) {
      if (compact_pointer_[level].empty() ||
          icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
        c->inputs_[0].push_back(f); break;
      }
    }
    if (c->inputs_[0].empty()) c->inputs_[0].push_back(current_->files_[level][0]);
  } else {
    level = current_->file_to_compact_level_;
    // 不能对最后一层（kNumLevels-1）发起 compaction，
    // 因为需要 compact 到 level+1 层。
    if (level + 1 >= config::kNumLevels) {
      return nullptr;
    }
    c = new MergeTask(options_, level);
    c->inputs_[0].push_back(current_->file_to_compact_);
  }

  c->input_version_ = current_;
  c->input_version_->Ref();

  if (level == 0) {
    InternalKey smallest, largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    c->inputs_[0].clear();
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
    assert(!c->inputs_[0].empty());
  }
  SetupOtherInputs(c);
  return c;
}

void VersionCatalog::SetupOtherInputs(MergeTask* c) {
  const int level = c->level();
  InternalKey smallest, largest;
  GetRange(c->inputs_[0], &smallest, &largest);
  if (level + 1 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);
  }
  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
  if (level == 0) {
    current_->GetOverlappingInputs(0, &all_start, &all_limit, &c->inputs_[0]);
    assert(!c->inputs_[0].empty());
  }
  c->grandparents_.clear();
  if (level + 2 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                    &c->grandparents_);
  }
  compact_pointer_[level] = largest.Encode().ToString();
  c->edit_.SetCompactPointer(level, largest);
}

MergeTask* VersionCatalog::CompactRange(int level, const InternalKey* begin,
                                     const InternalKey* end) {
  std::vector<SSTableDescriptor*> inputs;
  current_->GetOverlappingInputs(level, begin, end, &inputs);
  if (inputs.empty()) return nullptr;
  if (level > 0 && inputs.size() > 1) inputs.resize(1);
  MergeTask* c = new MergeTask(options_, level);
  c->input_version_ = current_;
  c->input_version_->Ref();
  c->inputs_[0] = inputs;
  SetupOtherInputs(c);
  return c;
}

Status VersionCatalog::InstallFullSnapshot(log::Writer* log) {
  VersionDelta edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());
  for (int level = 0; level < config::kNumLevels; level++)
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  for (int level = 0; level < config::kNumLevels; level++)
    for (auto* f : current_->files_[level])
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}

// ── Version::LevelFileNumIterator ──────────────────────────

class Version::LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<SSTableDescriptor*>* files)
      : icmp_(icmp), files_(files), index_(files->size()) {}
  bool Valid() const override { return index_ < files_->size(); }
  void Seek(const Slice& target) override {
    auto cmp = [this](const SSTableDescriptor* f, const Slice& k) {
      return icmp_.Compare(f->largest.Encode(), k) < 0;
    };
    index_ = std::lower_bound(files_->begin(), files_->end(), target, cmp) - files_->begin();
  }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override { index_ = files_->size() - 1; }
  void Next() override { assert(Valid()); index_++; }
  void Prev() override { assert(Valid()); index_--; }
  Slice key() const override { assert(Valid()); return (*files_)[index_]->largest.Encode(); }
  Slice value() const override {
    assert(Valid());
    value_str_.resize(16);
    EncodeFixed64(&value_str_[0], (*files_)[index_]->number);
    EncodeFixed64(&value_str_[8], (*files_)[index_]->file_size);
    return value_str_;
  }
  Status status() const override { return Status::OK(); }
 private:
  const InternalKeyComparator& icmp_;
  const std::vector<SSTableDescriptor*>* files_;
  size_t index_;
  mutable std::string value_str_;
};

Iterator* VersionCatalog::MakeInputIterator(MergeTask* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;
  const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
  Iterator** list = new Iterator*[space];
  int num = 0;
  for (int which = 0; which < 2; which++) {
    if (c->inputs_[which].empty()) continue;
    if (c->level() + which == 0) {
      for (auto* f : c->inputs_[which])
        list[num++] = table_cache_->NewIterator(options, f->number, f->file_size);
    } else {
      list[num++] = NewTwoLevelIterator(
          new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]),
          &GetFileIterator, table_cache_, options);
    }
  }
  assert(num <= space);
  Iterator* result = NewMergingIterator(&icmp_, list, num);
  delete[] list;
  return result;
}

// ── Version ────────────────────────────────────────────────

Version::~Version() {
  assert(refs_ == 0);
  prev_->next_ = next_;
  next_->prev_ = prev_;
  for (int level = 0; level < config::kNumLevels; level++)
    for (auto* f : files_[level]) { f->refs--; if (f->refs <= 0) delete f; }
}

void Version::Ref() {
  ++refs_;
}

void Version::Unref() {
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) delete this;
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin,
                                    const InternalKey* end,
                                    std::vector<SSTableDescriptor*>* inputs) {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  inputs->clear();
  Slice user_begin, user_end;
  if (begin != nullptr) user_begin = begin->user_key();
  if (end != nullptr) user_end = end->user_key();
  const Comparator* user_cmp = catalog_->icmp_.user_comparator();
  for (size_t i = 0; i < files_[level].size();) {
    SSTableDescriptor* f = files_[level][i++];
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {
      // "f" is completely before specified range; skip it
    } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
      // "f" is completely after specified range; skip it
    } else {
      inputs->push_back(f);
      if (level == 0) {
        // Level-0 文件可能互相重叠。如果新添加的文件扩大了搜索范围，
        // 则必须清空 inputs 并从 i=0 重新扫描所有文件，因为扩张
        // 后的范围可能使之前被跳过（完全在旧范围之前/之后）的文件
        // 现在落入新的范围而需要被包含。
        if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs->clear();
          i = 0;
        } else if (end != nullptr && user_cmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;
          inputs->clear();
          i = 0;
        }
      }
    }
  }
}

bool Version::OverlapInLevel(int level, const Slice* smallest_user_key,
                              const Slice* largest_user_key) {
  return SomeFileOverlapsRange(catalog_->icmp_, level > 0, files_[level],
                                smallest_user_key, largest_user_key);
}

int Version::PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                         const Slice& largest_user_key) {
  int level = 0;
  if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key))
    while (level < config::kMaxMemCompactLevel) {
      if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) break;
      level++;
    }
  return level;
}

bool Version::UpdateStats(const GetStats& stats) {
  if (stats.seek_file) {
    stats.seek_file->allowed_seeks--;
    if (stats.seek_file->allowed_seeks <= 0 && !file_to_compact_) {
      file_to_compact_ = stats.seek_file;
      file_to_compact_level_ = stats.seek_file_level;
      return true;
    }
  }
  return false;
}

bool Version::RecordReadSample(Slice key) {
  if (file_to_compact_) return false;
  for (int level = 0; level < config::kNumLevels; level++)
    for (auto* f : files_[level]) {
      if (catalog_->icmp_.Compare(f->largest.Encode(), key) >= 0 &&
          catalog_->icmp_.Compare(f->smallest.Encode(), key) <= 0) {
        f->allowed_seeks--;
        if (f->allowed_seeks <= 0 && !file_to_compact_) {
          file_to_compact_ = f;
          file_to_compact_level_ = level;
          return true;
        }
        return false;
      }
    }
  return false;
}

std::string Version::DebugString() const {
  std::string result;
  for (int level = 0; level < config::kNumLevels; level++) {
    if (files_[level].empty()) continue;
    char buf[100];
    std::snprintf(buf, sizeof(buf), "--- level %d ---\n", level);
    result.append(buf);
    for (auto* f : files_[level]) {
      std::snprintf(buf, sizeof(buf), "  %llu %s %s\n",
                    (unsigned long long)f->number,
                    f->smallest.DebugString().c_str(),
                    f->largest.DebugString().c_str());
      result.append(buf);
    }
  }
  return result;
}

Status Version::Get(const ReadOptions& options, const LookupKey& k,
                     std::string* value, GetStats* stats) {
  Slice ikey = k.internal_key();
  Slice user_key = k.user_key();
  const Comparator* ucmp = catalog_->icmp_.user_comparator();

  // Level-0 文件可能有重叠（overlap），并且 files_[0] 中按从旧到新的顺序排列
  // （SaveTo 先添加 base_ 的旧文件，后添加新文件）。
  // 点查时必须从新到旧反向遍历，确保查到的总是最新的值。
  for (int i = static_cast<int>(files_[0].size()) - 1; i >= 0; i--) {
    SSTableDescriptor* f = files_[0][i];
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      Saver saver;
      saver.state = kNotFound;
      saver.ucmp = ucmp;
      saver.user_key = user_key;
      saver.value = value;
      Status s = catalog_->table_cache_->Get(options, f->number, f->file_size,
                                           ikey, &saver, &SaveValue);
      if (!s.ok()) return s;
      if (saver.state == kFound) {
        if (stats && !stats->seek_file) {
          stats->seek_file = f;
          stats->seek_file_level = 0;
        }
        return Status::OK();
      }
      if (saver.state == kDeleted) {
        // 找到删除标记：键已被删除，视为不存在
        if (stats && !stats->seek_file) {
          stats->seek_file = f;
          stats->seek_file_level = 0;
        }
        return Status::NotFound(Slice());
      }
      // key not found in this L0 file, but the file was accessed
      // Record it as a seek so read-triggered compaction can fire
      if (stats && !stats->seek_file) {
        stats->seek_file = f;
        stats->seek_file_level = 0;
      }
    }
  }

  for (int level = 1; level < config::kNumLevels; level++) {
    if (files_[level].empty()) continue;
    uint32_t index = FindFile(catalog_->icmp_, files_[level], ikey);
    if (index < files_[level].size()) {
      SSTableDescriptor* f = files_[level][index];
      if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
          ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
        Saver saver;
        saver.state = kNotFound;
        saver.ucmp = ucmp;
        saver.user_key = user_key;
        saver.value = value;
        Status s = catalog_->table_cache_->Get(options, f->number, f->file_size,
                                             ikey, &saver, &SaveValue);
        if (!s.ok()) return s;
        if (saver.state == kFound) {
          if (stats && !stats->seek_file) {
            stats->seek_file = f;
            stats->seek_file_level = level;
          }
          return Status::OK();
        }
        if (saver.state == kDeleted) {
          // 找到删除标记：键已被删除，视为不存在
          return Status::NotFound(Slice());
        }
      }
    }
  }
  return Status::NotFound(Slice());
}

void Version::AddIterators(const ReadOptions& options,
                            std::vector<Iterator*>* iters) {
  for (auto* f : files_[0])
    iters->push_back(catalog_->table_cache_->NewIterator(options, f->number, f->file_size));
  for (int level = 1; level < config::kNumLevels; level++)
    if (!files_[level].empty())
      iters->push_back(NewConcatenatingIterator(options, level));
}

static bool NewestFirst(SSTableDescriptor* a, SSTableDescriptor* b) {
  return a->number > b->number;
}

void Version::ForEachOverlapping(Slice user_key, Slice internal_key,
                                  void* arg,
                                  bool (*func)(void*, int, SSTableDescriptor*)) {
  const Comparator* ucmp = catalog_->icmp_.user_comparator();

  // Level-0 文件可能互相重叠，按文件编号从大到小（最新到最旧）排序后遍历
  if (!files_[0].empty()) {
    std::vector<SSTableDescriptor*> tmp;
    tmp.reserve(files_[0].size());
    for (auto* f : files_[0]) {
      if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
          ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
        tmp.push_back(f);
      }
    }
    if (!tmp.empty()) {
      std::sort(tmp.begin(), tmp.end(), NewestFirst);
      for (auto* f : tmp) {
        if (!func(arg, 0, f)) return;
      }
    }
  }

  // 搜索非 Level-0 级别（文件不重叠，可用二分查找）
  for (int level = 1; level < config::kNumLevels; level++) {
    if (files_[level].empty()) continue;
    uint32_t index = FindFile(catalog_->icmp_, files_[level], internal_key);
    if (index < files_[level].size()) {
      SSTableDescriptor* f = files_[level][index];
      if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0)
        if (!func(arg, level, f)) return;
    }
  }
}

Iterator* Version::NewConcatenatingIterator(const ReadOptions& options,
                                             int level) const {
  return NewTwoLevelIterator(
      new LevelFileNumIterator(catalog_->icmp_, &files_[level]),
      &GetFileIterator, catalog_->table_cache_, options);
}

// ── Compaction ─────────────────────────────────────────────

MergeTask::MergeTask(const Options* options, int level)
    : level_(level),
      max_output_file_size_(MaxFileSizeForLevel(options, level)),
      input_version_(nullptr), grandparent_index_(0),
      seen_key_(false), overlapped_bytes_(0) {
  for (int i = 0; i < config::kNumLevels; i++) level_ptrs_[i] = 0;
}

MergeTask::~MergeTask() {
  if (input_version_) input_version_->Unref();
}

bool MergeTask::IsTrivialMove() const {
  return num_input_files(0) == 1 && num_input_files(1) == 0;
}

void MergeTask::AddInputDeletions(VersionDelta* edit) {
  for (int which = 0; which < 2; which++)
    for (auto* f : inputs_[which])
      edit->RemoveFile(level_ + which, f->number);
}

bool MergeTask::IsBaseLevelForKey(const Slice& user_key) {
  const Comparator* ucmp = input_version_->catalog_->icmp_.user_comparator();
  int start_level = level_ + 2;
  if (start_level >= config::kNumLevels) return true;
  for (int level = start_level; level < config::kNumLevels; level++) {
    while (level_ptrs_[level] < input_version_->files_[level].size()) {
      SSTableDescriptor* f = input_version_->files_[level][level_ptrs_[level]];
      if (ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0) return false;
        break;
      }
      level_ptrs_[level]++;
    }
  }
  return true;
}

bool MergeTask::ShouldStopBefore(const Slice& internal_key) {
  const InternalKeyComparator& icmp = input_version_->catalog_->icmp_;
  while (grandparent_index_ < grandparents_.size() &&
         icmp.Compare(grandparents_[grandparent_index_]->largest.Encode(),
                       internal_key) < 0) {
    if (seen_key_) overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    grandparent_index_++;
  }
  seen_key_ = true;
  if (overlapped_bytes_ > kMaxGrandParentOverlapBytes) {
    overlapped_bytes_ = 0;
    seen_key_ = false;
    return true;
  }
  return false;
}

void MergeTask::ReleaseInputs() {
  if (input_version_) { input_version_->Unref(); input_version_ = nullptr; }
}

}  // namespace lsmdb
