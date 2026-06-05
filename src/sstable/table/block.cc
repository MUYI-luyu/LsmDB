// 解码 block_builder.cc 生成的块。

#include "sstable/table/block.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "db/comparator.h"
#include "sstable/table/format.h"
#include "utils/coding.h"
#include "utils/logging.h"

namespace lsmdb {

uint32_t Block::NumRestarts() const {
  assert(size_ >= sizeof(uint32_t));
  return coding::DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents)
    : data_(contents.data.data()),
      size_(contents.data.size()),
      owned_(contents.heap_allocated) {
  // 严格对接 format.h 中定义的 BlockContents::heap_allocated 状态机
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // 无效标记：数据块头部损坏
  } else {
    size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
    if (NumRestarts() > max_restarts_allowed) {
      // 声称的重启点数量超出缓冲区可容纳的范围
      size_ = 0;
    } else {
      restart_offset_ = size_ - (1 + NumRestarts()) * sizeof(uint32_t);
    }
  }
}

Block::~Block() {
  // 当 heap_allocated == true（即 owned_ == true）时，
  // data_ 指向由 new char[] 分配的堆内存，必须安全释放。
  // 当 heap_allocated == false 时，data_ 由外部（文件缓冲或 mmap）管理，
  // Block 不拥有其所有权，不可释放。
  if (owned_) {
    delete[] data_;
  }
}

// 从字节流 p 中解析出条目的三个变长长度字段（shared、non_shared、value_length），
// 成功则返回指向实际键后缀（key_delta）起始位置的指针。
static inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared, uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = reinterpret_cast<const uint8_t*>(p)[0];
  *non_shared = reinterpret_cast<const uint8_t*>(p)[1];
  *value_length = reinterpret_cast<const uint8_t*>(p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // 快速路径：三个字段均编码为单字节（varint 无延续位）
    p += 3;
  } else {
    if ((p = coding::GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = coding::GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = coding::GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return nullptr;
  }
  return p;
}

// ── Block::Iter ──────────────────────────────────────────────
// 块内迭代器：通过重启点二分查找 + 区间内线性扫描实现高效随机访问。

class Block::Iter : public Iterator {
 private:
  const Comparator* const comparator_;
  const char* const data_;           // 指向块内存起点的指针
  uint32_t const restarts_;          // 重启数组在 data_ 中的偏移量
  uint32_t const num_restarts_;      // 重启点总数

  uint32_t current_;                 // 当前条目在 data_ 中的字节偏移量
  uint32_t restart_index_;           // 当前条目所属的重启区间索引
  std::string key_;                  // 重建后的完整当前键
  Slice value_;                      // 当前值
  Status status_;

  inline int Compare(const Slice& a, const Slice& b) const {
    return comparator_->Compare(a, b);
  }

  // 返回当前条目结束位置（即下一条条目起始位置）在 data_ 中的偏移量
  inline uint32_t NextEntryOffset() const {
    return (value_.data() + value_.size()) - data_;
  }

  // 返回指定重启点对应条目在数据区中的绝对偏移量（随机访问的基石）
  uint32_t GetRestartPoint(uint32_t index) {
    assert(index < num_restarts_);
    return coding::DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  // 将迭代器定位到指定重启点，为 ParseNextKey 做状态准备
  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    // current_ 稍后由 ParseNextKey() 修正
    // 将 value_ 设为该重启点的零长度切片，ParseNextKey 将从该位置继续解析
    uint32_t offset = GetRestartPoint(index);
    value_ = Slice(data_ + offset, 0);
  }

 public:
  Iter(const Comparator* comparator, const char* data, uint32_t restarts,
       uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts_),
        restart_index_(num_restarts_) {
    assert(num_restarts_ > 0);
  }

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }
  Slice key() const override {
    assert(Valid());
    return key_;
  }
  Slice value() const override {
    assert(Valid());
    return value_;
  }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  // 后退到前一个条目。通过回溯重启区间 + 前向扫描实现。
  void Prev() override {
    assert(Valid());

    // 从当前所在重启点向上回溯，找到位于 current_ 之前的最近重启区间
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        // 已到第一个重启点之前，没有更多条目了
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);
    // 前向扫描，直到下一个条目的起始位置 >= original
    do {
      // 循环体为空；ParseNextKey() 和条件判断完成所有工作
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  // 通过对尾部重启点进行二分查找定位目标区间，再通过区间内线性扫描，
  // 使迭代器对齐到块内第一个 >= target 的条目。
  void Seek(const Slice& target) override {
    // 在重启数组上进行二分查找，定位 "最后一个 key < target" 的重启点区间
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    int current_key_compare = 0;

    if (Valid()) {
      // 如果已处于有效位置，将其作为搜索起点以加速邻近查找
      current_key_compare = Compare(key_, target);
      if (current_key_compare < 0) {
        // 当前 key < target，目标一定在当前键之后
        left = restart_index_;
      } else if (current_key_compare > 0) {
        // 当前 key > target，目标在当前键之前
        right = restart_index_;
      } else {
        // 当前 key == target，已命中，直接返回
        return;
      }
    }

    // 二分查找核心：(left + right + 1) / 2 确保取上中位数，
    // 避免当 left == right 时死循环
    while (left < right) {
      uint32_t mid = (left + right + 1) / 2;
      uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr =
          DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                      &non_shared, &value_length);
      if (key_ptr == nullptr || (shared != 0)) {
        CorruptionError();
        return;
      }
      Slice mid_key(key_ptr, non_shared);
      if (Compare(mid_key, target) < 0) {
        // mid 处重启点的完整 key < target，目标在 mid 之后，保留 mid
        left = mid;
      } else {
        // mid 处重启点的完整 key >= target，目标在 mid 之前，排除 mid
        right = mid - 1;
      }
    }

    // 可以安全地重用当前块内的当前位置。
    // 当 key_ < target 且 left 恰好等于重启区间索引时，
    // 当前迭代位置已经处于正确的搜索区域，可跳过 SeekToRestartPoint。
    assert(current_key_compare == 0 || Valid());
    bool skip_seek = (left == restart_index_ && current_key_compare < 0);
    if (!skip_seek) {
      SeekToRestartPoint(left);
    }
    // 在定位到的重启区间内线性扫描，找到第一个 key >= target 的条目
    while (true) {
      if (!ParseNextKey()) {
        return;
      }
      if (Compare(key_, target) >= 0) {
        return;
      }
    }
  }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
      // 持续跳过，直到最后一个有效条目
    }
  }

 private:
  // 标记数据损坏：将迭代器设为无效并记录错误状态
  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = Slice();
  }

  // 解析并前进到下一条条目。返回 false 表示已无更多条目。
  // 核心逻辑：读取当前条目编码，解码 shared/non_shared/value_length，
  // 根据 shared 长度从 last_key 前缀恢复完整 key。
  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;  // 重启点数组紧跟在数据之后
    if (p >= limit) {
      // 已耗尽所有条目，标记为无效
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }

    // 解码下一条条目
    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }
    // 前缀恢复：保留 shared 长度的前缀，追加 non_shared 差分
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = Slice(p + non_shared, value_length);
    // 更新 restart_index_，使其始终指向包含 current_ 的重启区间
    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }
};

Iterator* Block::NewIterator(const Comparator* comparator) {
  if (size_ < sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    return NewEmptyIterator();
  }
  return new Iter(comparator, data_, restart_offset_, num_restarts);
}

}  // namespace lsmdb
