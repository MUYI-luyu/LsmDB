// LsmDB 性能压测工具
//
// Phase 1 (N=500K): 全场景吞吐摸底（布隆 ON）
//   FillSeq → FillRandom → ReadSeq → ReadRandomHit(独立DB) → ReadMiss → Overwrite → ReadWriteMix
//
// Phase 2 (N=1M):  布隆过滤器对照实验
//   每个 DB 写入偶数 key (0,2,4,...2N-2) 的 FillSeq + FillRandom，Miss 查询奇数 key (1,3,5,...2N-1)。
//   奇数 key 均匀交错在数据 key 的字典序之间，确保必须走完整的 index→data block 搜索路径，
//   不会被 SSTable 的 smallest/largest boundary check 快速跳过。
//   布隆过滤器在 filter block 层拦截 NotFound，避免无效 data block I/O。
//
// Phase 3 (N=1M):  冷块缓存场景（关闭再打开）
//   注意：仅清空 LsmDB 内部 8MB 块缓存，OS page cache 不受影响。
//   真实冷磁盘读效果需要在有 root 权限时 drop_caches。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include "db/db.h"
#include "db/filter_policy.h"

namespace lsmdb {

// ---------- 工具函数 ----------

static std::string Uint64Key(uint64_t k) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%016lx", k);
  return std::string(buf, 16);
}

static double NowSec() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------- 预生成随机 value 池：避免每次 Put 都 malloc+rand ----------
class ValuePool {
 public:
  ValuePool(size_t val_len, size_t pool_size)
      : val_len_(val_len), pool_(pool_size) {
    std::mt19937_64 rng(42);
    for (size_t i = 0; i < pool_size; i++) {
      std::string v(val_len, '\0');
      for (size_t j = 0; j < val_len; j++)
        v[j] = 'a' + (rng() % 26);
      pool_[i] = std::move(v);
    }
  }
  Slice Get(size_t i) const { return Slice(pool_[i % pool_.size()]); }

 private:
  size_t val_len_;
  std::vector<std::string> pool_;
};

// ---------- 临时数据库 ----------

struct BenchDB {
  std::string name;
  DB* db;
  Options opts;

  ~BenchDB() {
    delete db;
    delete opts.filter_policy;
    DestroyDB(name, Options());
  }
};

static BenchDB* OpenBenchDB(const char* suffix, bool bloom) {
  Options opts;
  opts.create_if_missing = true;
  opts.error_if_exists = true;
  opts.write_buffer_size = 4 * 1024 * 1024;
  opts.max_file_size = 2 * 1024 * 1024;
  if (bloom) opts.filter_policy = new FilterPolicy(10);

  BenchDB* b = new BenchDB;
  b->name =
      std::string("/tmp/lsmdb_bench_") + std::to_string(getpid()) + "_" + suffix;
  b->opts = opts;
  Status s = DB::Open(opts, b->name, &b->db);
  if (!s.ok()) {
    fprintf(stderr, "Open(%s) failed: %s\n", b->name.c_str(),
            s.ToString().c_str());
    delete b;
    return nullptr;
  }
  return b;
}

static void ReopenCold(BenchDB* b) {
  delete b->db;
  b->db = nullptr;
  Options reopen_opts = b->opts;
  reopen_opts.create_if_missing = false;
  reopen_opts.error_if_exists = false;
  Status s = DB::Open(reopen_opts, b->name, &b->db);
  if (!s.ok()) {
    fprintf(stderr, "Reopen failed: %s\n", s.ToString().c_str());
  }
}

// ---------- 结果格式 ----------

struct Result {
  const char* label;
  double elapsed;
  uint64_t ops;
  double mb;
};

static std::string FormatOps(double ops) {
  if (ops >= 1e6) {
    char b[32];
    snprintf(b, sizeof(b), "%.2fM", ops / 1e6);
    return b;
  } else if (ops >= 1e3) {
    char b[32];
    snprintf(b, sizeof(b), "%.1fK", ops / 1e3);
    return b;
  }
  return std::to_string(static_cast<uint64_t>(ops));
}

static void PrintHeader() {
  printf("%-38s %10s %12s %10s\n", "场景", "耗时(s)", "吞吐(ops/s)", "带宽(MB/s)");
  printf("--------------------------------------------------------------\n");
}

static void Print(const Result& r) {
  printf("%-38s %10.3f %12s %10.1f\n", r.label, r.elapsed,
         FormatOps(r.ops / r.elapsed).c_str(),
         r.mb > 0 ? r.mb / r.elapsed : 0.0);
}

// ---------- 共享常量 ----------

static constexpr size_t kKeyLen = 16;
static constexpr size_t kValLen = 100;

// ---------- 写操作 ----------

static void DoFillSeq(BenchDB* b, size_t n, const ValuePool& vpool,
                      uint64_t key_multiplier = 1) {
  WriteOptions wopt;
  for (size_t i = 0; i < n; i++)
    b->db->Put(wopt, Uint64Key(i * key_multiplier), vpool.Get(i));
}

// 生成 [0, n) * multiplier 范围内的随机排列 key，与 FillSeq 共享同一 key 空间
// Phase1: multiplier=1 → key 在 [0, N)
// Phase2: multiplier=2 → key 在 {0,2,4,...2N-2}（偶数 key 空间）
static void DoFillRandom(BenchDB* b, size_t n, const ValuePool& vpool,
                         uint64_t key_multiplier, std::mt19937_64& rng) {
  std::vector<uint64_t> keys(n);
  for (size_t i = 0; i < n; i++) keys[i] = (rng() % n) * key_multiplier;
  WriteOptions wopt;
  for (size_t i = 0; i < n; i++)
    b->db->Put(wopt, Uint64Key(keys[i]), vpool.Get(i));
}

// ---------- 读操作 ----------

static Result BenchReadSeq(BenchDB* b) {
  ReadOptions ropt;
  Iterator* it = b->db->NewIterator(ropt);
  double t0 = NowSec();
  size_t cnt = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) cnt++;
  double t1 = NowSec();
  delete it;
  return {"ReadSeq(顺序读)", t1 - t0, cnt, 0};
}

// ReadRandomHit: 在只做了 FillSeq 的独立 DB 上测试，预 shuffle 确保真随机访问
static Result BenchReadRandomHit(BenchDB* b, size_t n, uint64_t key_multiplier,
                                 std::mt19937_64& rng) {
  ReadOptions ropt;
  std::string val;
  std::vector<uint64_t> keys(n);
  for (size_t i = 0; i < n; i++) keys[i] = i * key_multiplier;
  std::shuffle(keys.begin(), keys.end(), rng);
  double t0 = NowSec();
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++)
    if (b->db->Get(ropt, Uint64Key(keys[i]), &val).ok()) cnt++;
  double t1 = NowSec();
  return {"ReadRandomHit(随机读命中)", t1 - t0, cnt, 0};
}

// ReadMiss: 查询奇数 key (1,3,5,...2N-1)，均匀交错在数据 key (0,2,4,...2N-2) 之间。
// 每个 miss key 严格落入 SSTable 的 key range 内部，确保必须走 index→data block 搜索路径。
// 布隆过滤器在 filter block 层拦截 NotFound，避免穿透到 data block。
static Result BenchReadMiss(BenchDB* b, size_t n, const char* label) {
  ReadOptions ropt;
  std::string val;
  double t0 = NowSec();
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++) {
    // odd keys: 1, 3, 5, ... 2n-1   （data keys: 0, 2, 4, ... 2n-2）
    if (b->db->Get(ropt, Uint64Key(i * 2 + 1), &val).IsNotFound()) cnt++;
  }
  double t1 = NowSec();
  return {label, t1 - t0, cnt, 0};
}

// ---------- Phase 1: 全场景摸底 (N=500K, 布隆 ON) ----------

static void RunPhase1(ValuePool& vpool, std::mt19937_64& rng) {
  constexpr size_t N = 500000;
  constexpr uint64_t kM = 1;  // key multiplier = 1, 连续 key

  // 1) FillSeq + FillRandom on main DB
  BenchDB* b = OpenBenchDB("p1", true);
  if (!b) return;

  double t0, t1;

  t0 = NowSec(); DoFillSeq(b, N, vpool, kM); t1 = NowSec();
  Print({"FillSeq(顺序写)", t1 - t0, N,
         double(N * (kKeyLen + kValLen)) / (1 << 20)});

  t0 = NowSec(); DoFillRandom(b, N, vpool, kM, rng); t1 = NowSec();
  Print({"FillRandom(随机写)", t1 - t0, N,
         double(N * (kKeyLen + kValLen)) / (1 << 20)});

  Print(BenchReadSeq(b));
  Print(BenchReadMiss(b, N, "ReadMiss_warm(随机读不命中)"));

  // 2) ReadRandomHit — 用独立的只做了 FillSeq 的 DB，避免 FillRandom 污染
  {
    BenchDB* b_hit = OpenBenchDB("p1_hit", true);
    if (b_hit) {
      DoFillSeq(b_hit, N, vpool, kM);
      Print(BenchReadRandomHit(b_hit, N, kM, rng));
      delete b_hit;
    }
  }

  // 3) Overwrite 前半段 key
  WriteOptions wopt;
  t0 = NowSec();
  size_t no = N / 2;
  for (size_t i = 0; i < no; i++)
    b->db->Put(wopt, Uint64Key(i * kM), vpool.Get(i + N));
  t1 = NowSec();
  Print({"Overwrite(更新覆盖)", t1 - t0, no,
         double(no * (kKeyLen + kValLen)) / (1 << 20)});

  // 4) 7:3 混合负载
  {
    ReadOptions ropt;
    std::string val;
    std::vector<uint64_t> keys(N / 2);
    for (size_t i = 0; i < N / 2; i++)
      keys[i] = rng() % N;
    t0 = NowSec();
    size_t reads = 0, writes = 0;
    for (size_t i = 0; i < N; i++) {
      if (rng() % 100 < 70) {
        if (b->db->Get(ropt, Uint64Key(keys[i % keys.size()] * kM), &val).ok())
          reads++;
      } else {
        b->db->Put(wopt, Uint64Key(keys[writes % keys.size()] * kM),
                   vpool.Get(writes));
        writes++;
      }
    }
    t1 = NowSec();
    Print({"ReadWriteMix(读7写3)", t1 - t0, reads + writes,
           double(writes * (kKeyLen + kValLen)) / (1 << 20)});
  }

  delete b;
}

// ---------- Phase 2: 布隆对照实验 ----------
//
// 核心设计：
//   写入偶数 key: 0, 2, 4, ... 2N-2
//   不命中查询奇数 key: 1, 3, 5, ... 2N-1
//
// 奇数 key 严格落在相邻偶数 key 之间（如 key=3 在 2 和 4 之间），
// 属于 SSTable data block 的有效 key range 内部，boundary check 无法跳过。
// 无布隆时必须搜索 index block → 定位 data block → 在 block 内二分确认不存在。
// 有布隆时 filter block 直接拒绝，避免 index+data block 的两次 I/O。

static Result RunBloomCmp(size_t N, bool bloom, const char* label,
                          ValuePool& vpool, std::mt19937_64& rng) {
  // warm: FillSeq + FillRandom → 强制 Compact → ReadMiss
  BenchDB* b = OpenBenchDB(bloom ? "bloom" : "nobloom", bloom);
  if (!b) return {label, 0, 0, 0};
  DoFillSeq(b, N, vpool, /*key_multiplier=*/2);
  DoFillRandom(b, N, vpool, /*key_multiplier=*/2, rng);
  b->db->CompactRange(nullptr, nullptr);  // 全部数据进入 SSTable，确保布隆参与
  Result r = BenchReadMiss(b, N, label);   // 查奇数 key
  delete b;
  return r;
}

static Result RunBloomCmpCold(size_t N, bool bloom, const char* label,
                              ValuePool& vpool, std::mt19937_64& rng) {
  // cold: FillSeq + FillRandom → Compact → 关闭再打开 → ReadMiss
  BenchDB* b = OpenBenchDB(bloom ? "bloom_cold" : "nobloom_cold", bloom);
  if (!b) return {label, 0, 0, 0};
  DoFillSeq(b, N, vpool, /*key_multiplier=*/2);
  DoFillRandom(b, N, vpool, /*key_multiplier=*/2, rng);
  b->db->CompactRange(nullptr, nullptr);
  ReopenCold(b);
  Result r = BenchReadMiss(b, N, label);
  delete b;
  return r;
}

static void RunPhase2(ValuePool& vpool, std::mt19937_64& rng) {
  constexpr size_t N = 1000000;

  printf("  数据量: N=%zu (~%.0fMB 未压缩)\n", N,
         N * (kKeyLen + kValLen) / 1e6);
  printf("  写入: 偶数 key (0,2,4,...2N-2) 的 FillSeq + FillRandom\n");
  printf("  不命中: 奇数 key (1,3,5,...2N-1) 均匀交错在数据 key 之间\n");
  printf("  效果: 每个 miss key 都在 SSTable key range 内，boundary check 无法跳过\n\n");

  Result ra = RunBloomCmp(N, true, "ReadMiss(布隆,warm)", vpool, rng);
  Result rb = RunBloomCmp(N, false, "ReadMiss(无布隆,warm)", vpool, rng);
  Result rc = RunBloomCmpCold(N, true, "ReadMiss(布隆,cold)", vpool, rng);
  Result rd = RunBloomCmpCold(N, false, "ReadMiss(无布隆,cold)", vpool, rng);

  PrintHeader();
  Print(ra);
  Print(rb);
  Print(rc);
  Print(rd);

  printf("\n┌─ 布隆过滤器效果 ──────────────────────────────────┐\n");
  printf("│  Warm 加速比: %.2fx  (布隆耗时/无布隆耗时)        │\n",
         rb.elapsed / ra.elapsed);
  printf("│  Cold 加速比: %.2fx  (仅清空块缓存, OS cache 仍热) │\n",
         rd.elapsed / rc.elapsed);
  printf("│                                                   │\n");
  printf("│  >1.0 = 布隆更快，<1.0 = 无布隆更快              │\n");
  printf("│  Miss key 落入 SSTable key range, 必须搜索确认    │\n");
  printf("│  布隆在 filter block 拦截, 避免 data block I/O    │\n");
  printf("└───────────────────────────────────────────────────┘\n");
}

// ---------- 入口 ----------

int RunBenchmark() {
  // 预生成 value pool + 64 位随机数生成器
  ValuePool vpool(kValLen, /*pool_size=*/1 << 20);  // ~100MB
  std::mt19937_64 rng(42);

  printf("\n");
  printf("╔════════════════════════════════════════════════════════════════╗\n");
  printf("║              LsmDB  Benchmark  (key=%zuB, val=%zuB)               ║\n",
         kKeyLen, kValLen);
  printf("╚════════════════════════════════════════════════════════════════╝\n");

  printf("\n─── Phase 1: 全场景吞吐摸底 (N=500K, 布隆 ON) ───\n\n");
  PrintHeader();
  RunPhase1(vpool, rng);

  printf("\n─── Phase 2: 布隆过滤器对照 (N=1M) ───\n\n");
  RunPhase2(vpool, rng);

  printf("\n");
  return 0;
}

}  // namespace lsmdb

int main() { return lsmdb::RunBenchmark(); }
