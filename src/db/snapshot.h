#pragma once

#include <cstdint>

#include "db/db.h"
#include "db/dbformat.h"

namespace lsmdb {

class SnapshotImpl : public Snapshot {
 public:
  SnapshotImpl() : sequence_number_(0) {}
  explicit SnapshotImpl(SequenceNumber sequence_number)
      : sequence_number_(sequence_number) {}

  SequenceNumber sequence_number() const { return sequence_number_; }

 private:
  friend class SnapshotList;
  SequenceNumber sequence_number_;
  SnapshotImpl* prev_ = nullptr;
  SnapshotImpl* next_ = nullptr;
};

class SnapshotList {
 public:
  SnapshotList() {
    head_.prev_ = &head_;
    head_.next_ = &head_;
  }

  bool empty() const { return head_.next_ == &head_; }

  SnapshotImpl* oldest() const {
    return empty() ? nullptr : head_.next_;
  }

  SnapshotImpl* newest() const {
    return empty() ? nullptr : head_.prev_;
  }

  SnapshotImpl* New(SequenceNumber sequence_number) {
    auto* s = new SnapshotImpl(sequence_number);
    s->prev_ = head_.prev_;
    s->next_ = &head_;
    head_.prev_->next_ = s;
    head_.prev_ = s;
    return s;
  }

  void Delete(const SnapshotImpl* s) {
    s->prev_->next_ = s->next_;
    s->next_->prev_ = s->prev_;
    delete s;
  }

 private:
  SnapshotImpl head_;
};

}  // namespace lsmdb
