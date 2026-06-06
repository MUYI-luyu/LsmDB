#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include "db/comparator.h"
#include "db/slice.h"
#include "utils/coding.h"

namespace lsmdb {

// 将 coding 子命名空间的编解码工具引入当前作用域
using namespace coding;

// 常量分组。将来可能希望将这些参数配置化。
namespace config {
static const int kNumLevels = 7;

// 当达到此文件数时触发 Level-0 压缩。
static const int kL0_CompactionTrigger = 4;

// Level-0 文件数的软限制。达到此值时开始减速写入。
static const int kL0_SlowdownWritesTrigger = 8;

// Level-0 文件数的硬限制。达到此值时停止写入。
static const int kL0_StopWritesTrigger = 12;

// 新压缩的 memtable 在不产生重叠的情况下可推送到的最大层级。
// 尝试推送到第 2 层以避免相对昂贵的 0=>1 层压缩和昂贵的清单操作。
// 不一路推送到最大层级，因为若同一键空间反复覆盖会浪费大量磁盘空间。
static const int kMaxMemCompactLevel = 2;

// 迭代期间读取的数据样本之间的近似间隔（字节数）。
static const int kReadBytesPeriod = 1048576;

}  // namespace config

class InternalKey;

// 值类型编码为内部键的最后一个组成部分。
// 不要更改这些枚举值：它们嵌入在磁盘数据结构中。
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };

// kValueTypeForSeek 定义了在构建用于查找（Seek）特定序列号的 ParsedInternalKey 对象时，
// 应该传入的 ValueType。
// （鉴于内部键的排序规则是：序列号按降序排列，且操作类型被嵌入在序列号的低 8 位中；
// 为了确保查找时能最先定位到相同序列号中最新鲜/最有效的数据，
// 我们必须使用数值最大的那个 ValueType，而不是最小的）。
static const ValueType kValueTypeForSeek = kTypeValue;

typedef uint64_t SequenceNumber;

// 我们在 64 位整数的底部预留了 8 位空闲空间，
// 以便将操作类型（Type）和序列号（Sequence Number）完美打包进一个 64 位整数中。
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() {}  // Intentionally left uninitialized (for speed)
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
  std::string DebugString() const;
};

// 返回内部键（key）序列化之后的字节编码长度。
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

// 将内部键（key）序列化后的二进制数据追加到 *result 末尾。
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// 尝试从原始切片 "internal_key" 中反序列化解析出内部键。
// 解析成功时，将解析后的数据存入 "*result" 并返回 true。
// 解析失败时，返回 false，此时 "*result" 将处于未定义状态。
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// 从一个完整的内部键（internal_key）中截取并返回用户键（User Key）部分。
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

// 内部键专用的比较器。
// 它使用指定的定制比较器来对用户键（User Key）部分进行主排序；
// 当用户键相同时，通过全局序列号（Sequence Number）的降序排列来打破平局。
class InternalKeyComparator : public Comparator {
 private:
  const Comparator* user_comparator_;

 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}
  const char* Name() const override;
  int Compare(const Slice& a, const Slice& b) const override;
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override;
  void FindShortSuccessor(std::string* key) const override;

  const Comparator* user_comparator() const { return user_comparator_; }

  int Compare(const InternalKey& a, const InternalKey& b) const;
};


// 该目录下的模块应当将内部键（Internal Key）强类型封装在如下类中，
// 而不是直接使用原始的普通字符串（std::string）。
// 这样做的目的是为了提供编译期保护，防止开发人员错误地使用了默认的字符串字典序比较，
// 从而规避未通过 InternalKeyComparator 专用比较器进行比对的潜在风险。
class InternalKey {
 private:
  std::string rep_;

 public:
  InternalKey() {}  // 保持 rep_ 为空，用以表示当前内部键处于无效状态
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }

  bool DecodeFrom(const Slice& s) {
    rep_.assign(s.data(), s.size());
    return !rep_.empty();
  }

  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  Slice user_key() const { return ExtractUserKey(rep_); }

  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;
};

inline int InternalKeyComparator::Compare(const InternalKey& a,
                                          const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}

inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  uint8_t c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<uint8_t>(kTypeValue));
}

// 专用于 DBImpl::Get() 读路径的辅助看门人类
class LookupKey {
 public:
  // 初始化当前对象，用于在指定序列号（快照版本）下查找对应的用户键（user_key）。
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  ~LookupKey();

  // 返回适合在内存表（MemTable）中进行迭代查找的键（包含 Varint32 长度前缀）。
  Slice memtable_key() const { return Slice(start_, end_ - start_); }

  // 返回适合传递给内部迭代器（Internal Iterator）或磁盘 SSTable 查找的内部键视图。
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

  // 返回纯粹的用户键（User Key）视图。
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  // 我们在内部紧凑地构造了一个连续的字符数组，其物理内存布局如下：
  //    klength  varint32               <-- start_ （整个数组的起点）
  //    userkey  char[klength]          <-- kstart_（用户键数据的起点）
  //    tag      uint64（Seq + Type）
  //                                    <-- end_   （数组的终点）
  // 
  // 整个连续数组（从 start_ 到 end_）是一个完美的、符合 MemTable 规范的键。
  // 而从 "userkey" 开始的后缀部分（从 kstart_ 到 end_）则可以直接视作一个 InternalKey。
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];  // 预分配的栈缓冲区，用于规避短键带来的单次堆内存分配开销（优化性能）
};

inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace lsmdb


