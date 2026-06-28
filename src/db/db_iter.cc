#include "db/db_iter.h"

#include <cassert>
#include <cstring>
#include <algorithm>

#include "db/db_impl.h"
#include "utils/logging.h"
#include "utils/random.h"

namespace lsmdb {

class DBIter : public Iterator {
 public:
  DBIter(DBImpl* db, const Comparator* comparator, Iterator* iter,
         SequenceNumber s, uint32_t seed)
      : db_(db),
        user_comparator_(comparator),
        iter_(iter),
        sequence_(s),
        direction_(kForward),
        valid_(false),
        rnd_(seed) {
  }

  DBIter(const DBIter&) = delete;
  DBIter& operator=(const DBIter&) = delete;

  ~DBIter() override {
    delete iter_;
  }

  bool Valid() const override { return valid_; }

  Slice key() const override {
    assert(valid_);
    return (direction_ == kForward) ? ExtractUserKey(iter_->key())
                                    : saved_key_;
  }

  Slice value() const override {
    assert(valid_);
    return (direction_ == kForward) ? iter_->value() : saved_value_;
  }

  Status status() const override {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }

  void Next() override;
  void Prev() override;
  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;

 private:
  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* ikey);

  inline void SaveKey(const Slice& k, std::string* dst) {
    dst->assign(k.data(), k.size());
  }

  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      std::swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }

  DBImpl* db_;
  const Comparator* const user_comparator_;
  Iterator* const iter_;
  SequenceNumber const sequence_;

  enum Direction { kForward, kReverse };

  Status status_;
  std::string saved_key_;
  std::string saved_value_;
  Direction direction_;
  bool valid_;
  Random rnd_;
};

bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  Slice k = iter_->key();
  if (!ParseInternalKey(k, ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    return false;
  }
  return true;
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {
    direction_ = kForward;
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
  } else {
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    iter_->Next();
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
  }

  FindNextUserEntry(true, &saved_key_);
}

void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
  assert(iter_->Valid());
  assert(direction_ == kForward);

  do {
    ParsedInternalKey ikey;
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
      switch (ikey.type) {
        case kTypeDeletion:
          // 遇到删除标记（墓碑值）：记录当前的 user_key 到 skip 中，并开启跳过（skipping）模式
          SaveKey(ikey.user_key, skip);
          skipping = true;
          break;
        case kTypeValue:
          // 遇到有效的 Value 记录：
          // 如果当前处于跳过模式，且当前键的 user_key 小于或等于之前记录的 skip 键，
          // 说明该记录已被更新的版本删除（或被上层逻辑显式要求跳过），因此不予暴露。
          if (skipping && user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
            // 此数据项已被删除标记掩盖，或者需要直接将其跳过。
          } else {
            // 成功定位到当前视图下第一个有效且未被删除的用户可见数据项
            valid_ = true;
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next(); // 继续推进底层迭代器
  } while (iter_->Valid());

  saved_key_.clear();
  valid_ = false;
}

void DBIter::Prev() {
  assert(valid_);

  if (direction_ == kForward) {
    assert(iter_->Valid());
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()),
                                     saved_key_) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);
  ValueType value_type = kTypeDeletion;

  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
        if ((value_type != kTypeDeletion) &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
          break;
        }
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.clear();
          ClearSavedValue();
        } else {
          SaveKey(ikey.user_key, &saved_key_);
          Slice raw_value = iter_->value();
          if (saved_value_.capacity() > raw_value.size() + 1048576) {
            std::string empty;
            std::swap(empty, saved_value_);
          }
          saved_value_.assign(raw_value.data(), raw_value.size());
        }
      }
      iter_->Prev();
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}

void DBIter::Seek(const Slice& target) {
  direction_ = kForward;
  ClearSavedValue();
  saved_key_.clear();
  AppendInternalKey(&saved_key_,
                    ParsedInternalKey(target, sequence_, kValueTypeForSeek));
  iter_->Seek(saved_key_);
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  iter_->SeekToFirst();
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToLast() {
  direction_ = kReverse;
  ClearSavedValue();
  iter_->SeekToLast();
  FindPrevUserEntry();
}

Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator,
                        Iterator* internal_iter, SequenceNumber sequence,
                        uint32_t seed) {
  return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}  // namespace lsmdb
