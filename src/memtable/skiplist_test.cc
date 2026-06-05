#include "memtable/skiplist.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "gtest/gtest.h"
#include "utils/hash.h"
#include "utils/random.h"

namespace lsmdb {

// ---------- 测试所用的键类型 ----------
typedef uint64_t Key;

// ---------- 测试比较器 ----------
struct Comparator {
  int operator()(const Key& a, const Key& b) const {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
};

// ---------- 单元测试：空跳表 ----------

TEST(SkipTest, Empty) {
  Arena arena;
  Comparator cmp;
  SkipList<Key, Comparator> list(cmp, &arena);
  ASSERT_TRUE(!list.Contains(10));

  SkipList<Key, Comparator>::Iterator iter(&list);
  ASSERT_TRUE(!iter.Valid());
  iter.SeekToFirst();
  ASSERT_TRUE(!iter.Valid());
  iter.Seek(100);
  ASSERT_TRUE(!iter.Valid());
  iter.SeekToLast();
  ASSERT_TRUE(!iter.Valid());
}

// ---------- 单元测试：插入与查找 ----------

TEST(SkipTest, InsertAndLookup) {
  const int N = 2000;
  const int R = 5000;
  Random rnd(1000);
  std::set<Key> keys;
  Arena arena;
  Comparator cmp;
  SkipList<Key, Comparator> list(cmp, &arena);
  for (int i = 0; i < N; i++) {
    Key key = rnd.Next() % R;
    if (keys.insert(key).second) {
      list.Insert(key);
    }
  }

  for (int i = 0; i < R; i++) {
    if (list.Contains(i)) {
      ASSERT_EQ(keys.count(i), 1);
    } else {
      ASSERT_EQ(keys.count(i), 0);
    }
  }

  // 简易迭代器基础测试
  {
    SkipList<Key, Comparator>::Iterator iter(&list);
    ASSERT_TRUE(!iter.Valid());

    iter.Seek(0);
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(*(keys.begin()), iter.key());

    iter.SeekToFirst();
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(*(keys.begin()), iter.key());

    iter.SeekToLast();
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(*(keys.rbegin()), iter.key());
  }

  // 正向迭代测试
  for (int i = 0; i < R; i++) {
    SkipList<Key, Comparator>::Iterator iter(&list);
    iter.Seek(i);

    std::set<Key>::iterator model_iter = keys.lower_bound(i);
    for (int j = 0; j < 3; j++) {
      if (model_iter == keys.end()) {
        ASSERT_TRUE(!iter.Valid());
        break;
      } else {
        ASSERT_TRUE(iter.Valid());
        ASSERT_EQ(*model_iter, iter.key());
        ++model_iter;
        iter.Next();
      }
    }
  }

  // 反向迭代测试
  {
    SkipList<Key, Comparator>::Iterator iter(&list);
    iter.SeekToLast();

    for (std::set<Key>::reverse_iterator model_iter = keys.rbegin();
         model_iter != keys.rend(); ++model_iter) {
      ASSERT_TRUE(iter.Valid());
      ASSERT_EQ(*model_iter, iter.key());
      iter.Prev();
    }
    ASSERT_TRUE(!iter.Valid());
  }
}

// ============================================================
// 无锁并发读写测试骨架
//
// 验证：单写 + 多读并发下，迭代器必须至少包含创建时已存在的所有元素，
// 不得遗漏；允许观察到迭代器创建之后插入的新元素，但不能出现缺失或损坏。
//
// key 格式：<key, gen, hash>
// key: [0..K-1]
// gen: 版本号
// hash = Hash(key, gen)
// ============================================================

class ConcurrentTest {
 private:
  static constexpr uint32_t K = 4;

  // ----- 键的字段编解码 -----
  static uint64_t key_field(Key k) { return (k >> 40); }
  static uint64_t gen_field(Key k) { return (k >> 8) & 0xffffffffu; }
  static uint64_t hash_field(Key k) { return k & 0xff; }

  static uint64_t HashNumbers(uint64_t k, uint64_t g) {
    uint64_t data[2] = {k, g};
    return Hash(reinterpret_cast<char*>(data), sizeof(data), 0);
  }

  static Key MakeKey(uint64_t k, uint64_t g) {
    static_assert(sizeof(Key) == sizeof(uint64_t), "");
    assert(k <= K);  // 有时传 K 用于 seek 到末尾
    assert(g <= 0xffffffffu);
    return ((k << 40) | (g << 8) | (HashNumbers(k, g) & 0xff));
  }

  static bool IsValidKey(Key k) {
    return hash_field(k) == (HashNumbers(key_field(k), gen_field(k)) & 0xff);
  }

  static Key RandomTarget(Random* rnd) {
    switch (rnd->Next() % 10) {
      case 0:
        return MakeKey(0, 0);
      case 1:
        return MakeKey(K, 0);
      default:
        return MakeKey(rnd->Next() % K, 0);
    }
  }

  // ----- 每键代数状态（原子） -----
  struct State {
    std::atomic<int> generation[K];
    void Set(int k, int v) {
      generation[k].store(v, std::memory_order_release);
    }
    int Get(int k) { return generation[k].load(std::memory_order_acquire); }

    State() {
      for (int k = 0; k < K; k++) {
        Set(k, 0);
      }
    }
  };

  // ----- 测试状态成员 -----
  State current_;
  Arena arena_;
  SkipList<Key, Comparator> list_;

 public:
  ConcurrentTest() : list_(Comparator(), &arena_) {}

  // 写入步骤（要求外部同步）
  void WriteStep(Random* rnd) {
    const uint32_t k = rnd->Next() % K;
    const intptr_t g = current_.Get(k) + 1;
    const Key key = MakeKey(k, g);
    list_.Insert(key);
    current_.Set(k, g);
  }

  // 读取步骤（无锁并发安全）
  void ReadStep(Random* rnd) {
    // 快照迭代器构造时的提交状态
    State initial_state;
    for (int k = 0; k < K; k++) {
      initial_state.Set(k, current_.Get(k));
    }

    Key pos = RandomTarget(rnd);
    SkipList<Key, Comparator>::Iterator iter(&list_);
    iter.Seek(pos);
    while (true) {
      Key current;
      if (!iter.Valid()) {
        current = MakeKey(K, 0);
      } else {
        current = iter.key();
        ASSERT_TRUE(IsValidKey(current)) << current;
      }
      ASSERT_LE(pos, current) << "should not go backwards";

      // 验证 [pos, current) 范围内所有键在 initial_state 中不存在
      while (pos < current) {
        ASSERT_LT(key_field(pos), K) << pos;

        // 第 0 代从不被插入，所以缺失是允许的
        ASSERT_TRUE((gen_field(pos) == 0) ||
                    (gen_field(pos) > static_cast<Key>(initial_state.Get(key_field(pos)))))
            << "key: " << key_field(pos) << "; gen: " << gen_field(pos)
            << "; initgen: " << initial_state.Get(key_field(pos));

        // 前进到下一个有效键空间中的键
        if (key_field(pos) < key_field(current)) {
          pos = MakeKey(key_field(pos) + 1, 0);
        } else {
          pos = MakeKey(key_field(pos), gen_field(pos) + 1);
        }
      }

      if (!iter.Valid()) {
        break;
      }

      if (rnd->Next() % 2) {
        iter.Next();
        pos = MakeKey(key_field(pos), gen_field(pos) + 1);
      } else {
        Key new_target = RandomTarget(rnd);
        if (new_target > pos) {
          pos = new_target;
          iter.Seek(new_target);
        }
      }
    }
  }
};

// 显式实例化静态常量（C++17 中可省略，这里为兼容性保留）
constexpr uint32_t ConcurrentTest::K;

// ---------- 单线程并发骨架测试 ----------
TEST(SkipTest, ConcurrentWithoutThreads) {
  ConcurrentTest test;
  Random rnd(0xdeadbeef);
  for (int i = 0; i < 10000; i++) {
    test.ReadStep(&rnd);
    test.WriteStep(&rnd);
  }
}

// ============================================================
// 纯标准库实现的多线程并发读写测试基础设施
// ============================================================

class TestState {
 public:
  ConcurrentTest t_;
  int seed_;
  std::atomic<bool> quit_flag_;

  enum ReaderState { STARTING, RUNNING, DONE };

  explicit TestState(int s)
      : seed_(s), quit_flag_(false), state_(STARTING) {}

  void Wait(ReaderState s) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this, s] { return state_ == s; });
  }

  void Change(ReaderState s) {
    std::lock_guard<std::mutex> lock(mu_);
    state_ = s;
    cv_.notify_all();
  }

 private:
  std::mutex mu_;
  ReaderState state_;
  std::condition_variable cv_;
};

// 并发读取器线程入口函数
static void ConcurrentReader(TestState* state) {
  Random rnd(state->seed_);
  int64_t reads = 0;
  state->Change(TestState::RUNNING);
  while (!state->quit_flag_.load(std::memory_order_acquire)) {
    state->t_.ReadStep(&rnd);
    ++reads;
  }
  state->Change(TestState::DONE);
}

// 并发测试运行器
static void RunConcurrent(int run) {
  const int seed = 0xdeadbeef + (run * 100);
  Random rnd(seed);
  const int N = 1000;
  const int kSize = 1000;
  for (int i = 0; i < N; i++) {
    if ((i % 100) == 0) {
      std::fprintf(stderr, "Run %d of %d\n", i, N);
    }
    TestState state(seed + 1);
    std::thread reader_thread(ConcurrentReader, &state);
    state.Wait(TestState::RUNNING);
    for (int i = 0; i < kSize; i++) {
      state.t_.WriteStep(&rnd);
    }
    state.quit_flag_.store(true, std::memory_order_release);
    state.Wait(TestState::DONE);
    reader_thread.join();
  }
}

TEST(SkipTest, Concurrent1) { RunConcurrent(1); }
TEST(SkipTest, Concurrent2) { RunConcurrent(2); }
TEST(SkipTest, Concurrent3) { RunConcurrent(3); }
TEST(SkipTest, Concurrent4) { RunConcurrent(4); }
TEST(SkipTest, Concurrent5) { RunConcurrent(5); }

}  // namespace lsmdb
