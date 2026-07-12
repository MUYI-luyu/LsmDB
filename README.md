# LsmDB

> 基于 LSM-Tree 架构的嵌入式键值存储引擎 —— 从零构建，适用于学习存储系统内部原理。

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.14%2B-green)](https://cmake.org/)
[![Tests](https://img.shields.io/badge/tests-150%20passing-brightgreen)]()
[![Lines](https://img.shields.io/badge/code-16K%20lines-orange)]()

---

## 核心特性

- **LSM-Tree 全链路实现** — MemTable（跳表）→ WAL → SSTable（Block/Filter/Index）→ Compaction
- **MVCC 快照隔离** — 基于 VersionCatalog / VersionDelta 的独立版本管理层，支持多版本并发读
- **Group Commit 批量写入** — Writer 队列合并 + 动态容量上限（Leader ≤ 128KB 时软限制 +128KB），sync/non-sync 隔离
- **布隆过滤器加速** — 默认 10 bits/key（~1% 误判率），过滤 ~90% 的不存在键磁盘 I/O
- **崩溃恢复** — WAL 日志回放 + MANIFEST 文件恢复，保证持久性（Durability）
- **背压流控** — L0 慢写限速 / 硬停止 / MemTable 满自动切换，防止写入突发击穿
- **双向迭代器** — DBIter 支持正向/反向遍历，方向切换自动重定位，大 Value 主动回收

---

## 快速开始

### 环境要求

- **CMake** ≥ 3.14
- **GCC** ≥ 9 或 **Clang** ≥ 10（需支持 C++17）
- **Google Test**（自动拉取，或系统安装）

### 构建

```bash
git clone <repo-url> && cd LsmDB
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行测试

```bash
# 全量测试（150 个用例）
ctest --output-on-failure

# 或单独运行
./db_test           # DB 集成测试（57 用例）
./sstable_test      # SSTable 读写测试（10 用例）
./log_test          # WAL 日志测试（38 用例）
```

### 最小示例

```cpp
#include "db/db.h"

int main() {
  lsmdb::DB* db;
  lsmdb::Options opts;
  opts.create_if_missing = true;

  // 打开数据库
  lsmdb::Status s = lsmdb::DB::Open(opts, "./testdb", &db);
  if (!s.ok()) return 1;

  // 写入
  db->Put(lsmdb::WriteOptions(), "hello", "world");

  // 读取
  std::string value;
  s = db->Get(lsmdb::ReadOptions(), "hello", &value);
  // value == "world"

  // 批量原子写入
  lsmdb::WriteBatch batch;
  batch.Put("k1", "v1");
  batch.Delete("k2");
  db->Write(lsmdb::WriteOptions(), &batch);

  // 快照读
  const lsmdb::Snapshot* snap = db->GetSnapshot();
  lsmdb::ReadOptions read_opts;
  read_opts.snapshot = snap;
  db->Get(read_opts, "hello", &value);  // 读取 snap 时刻的数据
  db->ReleaseSnapshot(snap);

  // 迭代遍历
  lsmdb::Iterator* it = db->NewIterator(lsmdb::ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    // it->key(), it->value()
  }
  delete it;

  delete db;
  return 0;
}
```

---

## 架构概览
*（图片绘制中）*

### 写入路径

```
Put/Delete → WriteBatch → Group Commit 合并
     │
     ▼
 WAL (顺序写) ──────► 磁盘 .log 文件
     │
     ▼
 MemTable (跳表) ──► 内存有序缓冲
     │ 写满触发
     ▼
 Immutable MemTable ──► BuildTable ──► Level-0 SSTable
     │
     ▼ (后台 Compaction)
 Level-0 ──► Level-1 ──► ... ──► Level-N
```

### 读取路径

```
Get(key)
  │
  ├─► 1. MemTable 查找（最新数据）
  ├─► 2. Immutable MemTable 查找
  └─► 3. SSTable 逐层查找
        ├─► Bloom Filter 快速过滤不存在键
        └─► Block Index 二分定位 → Data Block 扫描
```

### 压缩策略（Compaction）

- **Minor Compaction**：MemTable → Level-0 SSTable
- **Major Compaction**：Level-N + Level-(N+1) 多路归并
- **TrivialMove 优化**：单文件无重叠时直接移动至下一层
- **安全边界**：基于 `oldest_snapshot` 丢弃过时版本和可清理墓碑

---

## 模块说明

| 模块 | 库名 | 行数 | 职责 |
|------|------|------|------|
| 基础工具 | `lsmdb_core` | 2,007 | CRC32 校验、Varint 编码、LRU Cache、POSIX 文件抽象 |
| 内存表 | `lsmdb_memtable` | 1,008 | 并发跳表 (SkipList)、Arena 内存池、MemTable |
| 批量写入 | `lsmdb_writebatch` | — | 原子批量 Put/Delete，Group Commit 合并 |
| 存储格式 | `lsmdb_sstable` | 3,011 | Block 编解码、FilterBlock 布隆过滤器、Table 读写、两级/归并迭代器 |
| 预写日志 | `lsmdb_wal` | 1,099 | 日志格式定义、LogWriter/LogReader、CRC 校验 |
| 版本控制 | `lsmdb_mvcc` | 2,118 | VersionCatalog 版本账本、VersionDelta 增量编码、快照链表 |
| 数据库引擎 | `lsmdb_db` | 5,840 | DBImpl 完整实现、DBIter 用户迭代器、Builder 构建器 |
| 公共接口 | `include/db/` | 1,281 | 12 个头文件，纯虚接口设计 |

### 测试覆盖（150 用例）

| 测试文件 | 用例数 | 覆盖范围 |
|---------|--------|---------|
| `db_test.cc` | 57 | 全链路读写、快照隔离、Compaction、崩溃恢复、故障注入 |
| `log_test.cc` | 38 | WAL 写入/读取/截断/损坏恢复 |
| `version_catalog_test.cc` | 13 | 文件查找、区间重叠、边界输入 |
| `sstable_test.cc` | 10 | Table/Block/MemTable/DB 四层迭代器测试 |
| `skiplist_test.cc` | 8 | 跳表并发插入/查找 |
| `dbformat_test.cc` | 6 | InternalKey 编解码、分隔符生成 |
| `write_batch_test.cc` | 5 | 批量写入序列化/反序列化 |
| `logging_test.cc` | 5 | 日志工具函数 |
| `crc32c_test.cc` | 4 | CRC32C 校验正确性 |
| `filter_block_test.cc` | 3 | 布隆过滤器构建/查询 |
| `version_delta_test.cc` | 1 | VersionDelta 编解码往返 |

---

## 关键设计亮点

### 1. SSTable 文件结构：四段式分层布局

每个 SSTable 文件由 **Data Blocks → Filter Block → Meta Index Block → Index Block → Footer** 五部分组成，自底向上逐步构建：

```
+-----------------------------------------------------------------------+
| Data Blocks (用户数据块区)                                              |
|   [ Data Block 0 ][ Data Block 1 ] ... [ Data Block M ]               |
+-----------------------------------------------------------------------+
| Filter Block (布隆过滤器块区)                                           |
|   [ Filter 0~N Data ] | [ Offsets Array ] | [ Array Offset ] | [BaseLg]|
+-----------------------------------------------------------------------+
| Meta Index Block (元索引块区)                                          |
|   [ 唯一 Entry: "filter.built_in_bloom_filter" -> Filter Block 门票 ]   |
+-----------------------------------------------------------------------+
| Index Block (核心数据索引块区)                                          |
|   [ Entry 0~M: 各 Data Block 的短隔离键 -> 各 Data Block 物理门票 ]      |
+-----------------------------------------------------------------------+
| Footer (固定 48 字节)                                                   |
|   [ Metaindex Handle ] [ Index Handle ] [ Padding ] [ Magic Number ]  |
+-----------------------------------------------------------------------+ <--- EOF
```

**单个 Data Block 内部结构** — 差分前缀压缩 + 重启点加速随机查找：

每条 KV Entry 由 5 个字段组成，键部分采用**前缀共享编码**，Varint 变长整数压缩元数据：

```
+-------------------+-------------------+-------------------+-------------------+-------------------+
| shared_bytes_len  | unshared_bytes_len|     value_len     |   key_unshared    |    value_data     |
| (Varint32 变长)   |  (Varint32 变长)   |  (Varint32 变长)   |  (变长裸字符串)    |   (变长裸二进制)   |
+-------------------+-------------------+-------------------+-------------------+-------------------+
```

**Varint32 编码** — 每字节高位置 1 表示后续字节仍属于当前整数，最高位为 0 表示终止。小整数（如 `shared_len=0`）只需 1 字节，大偏移量自动扩展到 5 字节，兼顾空间效率与通用性。

**差分前缀压缩示例**：
```
Entry 0: shared=0, unshared=4, key="abcd"           ← 重启点：写入完整 Key
Entry 1: shared=4, unshared=1, key="e"   (→ "abcde") ← 只存后缀 1 字节
Entry 2: shared=4, unshared=2, key="fg" (→ "abcfg")  ← 只存后缀 2 字节
...
Entry 16: shared=0, unshared=6, key="canvas"         ← 重启点：每 16 条强制截断
```

**重启点 (Restart Point)**：每 16 条 Entry 强制写入一个完整 Key，Block 末尾的 Restarts Array 记录每个重启点的块内偏移量（每项 uint32）。二分查找时先在 Restarts Array 中定位目标重启点，再在 16 条内线性扫描，实现 O(log N) 的 Seek 复杂度。

**Index Block** 的工作机制：TableBuilder 在每个 Data Block 满 4KB 刷盘时，取该 Block 的最后一个 Key 与下一个 Block 的第一个 Key 之间的**短分隔符 (ShortestSeparator)** 作为索引键。读取时通过 Index Block 二分定位到目标 Data Block，避免全表扫描。

**Footer** 设计：严格固定在文件末尾 48 字节。`metaindex_handle` 和 `index_handle` 各自 Varint 编码后不足 40 字节时用 `0x00` 填充，最后 8 字节写入魔数 `0xdb4775248b80fb57`（`echo LsmDB | sha1sum` 前 64 位），作为文件格式校验锚点。

### 2. 并发跳表 (SkipList) + Arena 内存池

MemTable 的核心数据结构是模板化的 `SkipList<Key, Comparator>`，最大高度 `kMaxHeight=12`，支持 ~4096 个节点的高效索引。

**无锁并发读**：写操作需要外部互斥锁保护（由 MemTable 调用方保证），读操作完全不需内部锁。`Node::Next(n)` 使用 `std::memory_order_acquire` 确保读到完全初始化的节点，`max_height_` 使用 `std::atomic<int>` 配合 `memory_order_relaxed` 读取（陈旧值不影响正确性，只影响查找效率）。

**Arena 内存池**：跳表所有节点通过 `Arena` 分配，而非逐节点 `new`。Arena 内部以 4KB 为单位向系统申请大块内存，节点分配仅在当前块内移动指针（O(1) 指针偏移），具有以下优势：

- **消除碎片**：所有节点内存在连续的 Arena Block 中紧密排列，不存在 `new/delete` 的堆碎片问题
- **批量释放**：MemTable 析构时一次性释放所有 Arena Block，无需逐个 delete 节点
- **缓存友好**：同一 Arena Block 内的节点在物理内存中连续，提高 CPU Cache 命中率

```cpp
// skiplist.h — 节点通过 Arena 分配，而非堆
Node* NewNode(const Key& key, int height) {
    char* mem = arena_->Allocate(sizeof(Node) + sizeof(std::atomic<Node*>) * height);
    return new (mem) Node(key);  // placement new，内存来自 Arena
}
```

随机高度生成使用 `Random::Uniform(2^kMaxHeight)` 模拟几何分布，期望高度 ~2，空间开销控制在 O(N)。

### 3. 多层迭代器组合架构

迭代器采用 **Wrapper → Merging → TwoLevel → DBIter** 的四层组合模式，每层职责单一：

```
DBIter                     ← 用户可见层：过滤删除标记 / 旧版本，统一用户键视图
  └─ MergingIterator       ← 多路归并层：合并 MemTable + Imm + SSTable 的多路有序流
       ├─ MemTable::Iterator
       ├─ Immutable MemTable::Iterator
       └─ TwoLevelIterator  ← 两级映射层：Index Block 迭代器 → Data Block 迭代器
            ├─ index_iter (遍历 Index Block 中的 [分隔键 → BlockHandle] 条目)
            └─ data_iter  (遍历单个 Data Block 中的 KV Entry)
```

**IteratorWrapper** — 消除虚函数调用开销：

```cpp
class IteratorWrapper {
    void Update() {
        valid_ = iter_->Valid();
        if (valid_) key_ = iter_->key();  // 缓存 valid 状态和 key 值
    }
};
```

子迭代器的 `Valid()` 和 `key()` 结果被缓存，归并排序时频繁的比较操作无需穿透虚函数调用。每次 `Next()/Prev()/Seek()` 后调用 `Update()` 刷新缓存。

**MergingIterator** — 最小堆多路归并：

N 个子迭代器封装为 `IteratorWrapper` 数组，前进方向 (kForward) 时通过线性扫描找最小键，后退方向 (kReverse) 时找最大键。所有子迭代器的数据为有序流（各自已排序），归并后产生全局有序流，不额外占用内存。

**TwoLevelIterator** — 两级指针惰性加载：

索引层 (`index_iter`) 的值是 Data Block 的 `BlockHandle`。`block_function` 回调将 BlockHandle 转换为该 Data Block 的迭代器。只有当前 Block 遍历完、移动到下一个索引条目时，才创建新的 data_iter_。这样避免了预加载所有 Data Block 的内存压力。

### 4. MVCC 版本控制与快照隔离

`lsmdb_mvcc` 库独立于引擎核心，`VersionCatalog` 管理全局版本账本（各层级 SSTable 文件清单 + 序列号范围），`VersionDelta` 编码原子增量变更（新增文件 / 删除文件 / 压缩指针）。一次 Compaction 完成后通过 `VersionDelta` 原子提交到 `VersionCatalog`，确保版本切换的一致性。

`SnapshotList` 基于循环双向链表管理活跃快照：

```
head_ (哨兵) ⇄ snap_1 (seq=100) ⇄ snap_2 (seq=200) ⇄ ...
     ↑ oldest()                                   ↑ newest()
```

- `New(seq)` 在链表尾部插入新节点 → O(1)
- `Delete(snap)` 从链表中摘除并释放 → O(1)
- `oldest()` 返回 `head_.next_` → O(1)
- Compaction 通过 `oldest()->sequence_number()` 确定安全回收边界：序列号 ≤ 最老快照的过时数据可以被安全丢弃

### 5. Group Commit 写入合并

写入线程在 `writers_` 双端队列中排队，Leader 线程通过 `CoalesceWriteBatch` 将连续请求合并为一个 Batch Group：

- **sync/non-sync 隔离**：如果 Follower 要求 sync 而 Leader 不要求，则停止合并，避免破坏持久性承诺
- **动态容量上限**：硬上限 1MB；Leader ≤ 128KB 时软上限为 `LeaderSize + 128KB`，防止大吞吐请求拖慢小写延迟
- **批量分配序列号**：合并后的 Batch 一次性分配连续序列号，WAL 写入 + MemTable 插入在释放锁的状态下执行，最大化并发

### 6. 布隆过滤器分级过滤

- **粒度**：每 2KB Data Block 对应一段独立的 Filter，`KeyMayMatch(offset, key)` 按 Data Block 偏移量精准定位对应 Filter 段
- **双重哈希**：`h ← Hash(key)`, `delta ← (h >> 17) | (h << 15)`，`h += delta` 循环模拟 k 个独立哈希函数，避免多次完整哈希计算
- **默认 10 bits/key**：k ≈ 7 个哈希函数，误判率 ~1%，空间开销 ~1.25 bytes/key，可过滤 ~90% 的不存在键查询
- **兼容性防护**：仅对 `BytewiseComparator` 自动启用；自定义比较器（如 `NumberComparator`）的键等价语义与字节哈希不一致，SanitizeOptions 自动跳过

### 7. DBIter 方向感知状态机

```
kForward ←────────→ kReverse
   │                    │
   └─ FindNextUserEntry  └─ FindPrevUserEntry
      (遇到删除标记则记入       (回溯至首个有效 Value，
       skip 跳过后续旧版本)      遇到删除标记则标记无效)
```

- 方向切换时自动重定位至正确的用户键位置
- `saved_key_` / `saved_value_` 缓存确保 `key()`/`value()` 在方向切换时一致性
- Value 容量超 1MB 时主动 `std::swap` 释放，避免迭代器持有大内存阻塞 Compaction

### 8. 崩溃恢复双重保证

```
DB::Open()
  ├─► 1. MANIFEST 恢复 → 重建 VersionCatalog（SSTable 文件清单 + 层级分布 + 序列号水位）
  ├─► 2. WAL 回放    → log::Reader 逐条解析，WriteBatchInternal::InsertInto 重放至 MemTable
  │                      MemTable 满时自动触发 WriteLevel0Table 生成 SSTable
  └─► 3. 日志复用    → 若最后一个 WAL 无需 Compaction 且 options_.reuse_logs 为真，直接追加复用
```

WAL 读取使用 `log::Reader` 状态机，支持块内记录跨边界、零填充头部、校验和不匹配自动跳过损坏记录。

### 9. 背压与流控

| 条件 | 行为 |
|------|------|
| L0 文件数 ≥ `kL0_SlowdownWritesTrigger` | 每次写入休眠 1ms，让出 CPU 给后台 Compaction |
| L0 文件数 ≥ `kL0_StopWritesTrigger` | 阻塞等待，直到 Compaction 将 L0 文件数降到安全线以下 |
| MemTable 满且 imm_ 非空 | 等待前一个 MemTable 的刷盘任务完成 |
| MemTable 满且 imm_ 空闲 | 原子切换：新 MemTable + 新 WAL，imm_ 移交后台 Minor Compaction |

写入路径 `ApplyBackpressure` 在持有锁的情况下循环检查以上条件，确保写入不会被无限阻塞（如 `bg_error_` 发生后立即释放并返回错误）。

---

## 目录结构

```
LsmDB/
├── CMakeLists.txt              # 构建系统
├── README.md
├── include/db/                 # 公共接口（12 头文件）
│   ├── db.h                    # DB / Snapshot 抽象类
│   ├── iterator.h              # 迭代器接口
│   ├── options.h               # 数据库选项
│   ├── status.h                # 错误状态码
│   ├── slice.h                 # 零拷贝字符串切片
│   ├── cache.h                 # 缓存抽象接口
│   ├── comparator.h            # 键比较器
│   ├── env.h                   # 操作系统抽象层
│   ├── filter_policy.h         # 布隆过滤器策略
│   ├── table.h / table_builder.h  # SSTable 读写
│   └── write_batch.h           # 批量写入
└── src/
    ├── db/                     # 数据库引擎（22 文件）
    │   ├── db_impl.cc/h        # DBImpl 完整实现
    │   ├── db_iter.cc/h        # DBIter 用户迭代器
    │   ├── builder.cc/h        # MemTable → SSTable 构建器
    │   ├── snapshot.h          # 快照双向链表
    │   ├── table_cache.cc/h    # SSTable 文件缓存
    │   ├── filename.cc/h       # 文件命名工具
    │   ├── dbformat.cc/h       # InternalKey 编解码
    │   ├── comparator.cc       # BytewiseComparator 实现
    │   ├── options.cc          # 选项默认值
    │   └── *_test.cc           # 测试文件
    ├── memtable/               # 内存表（6 文件）
    │   ├── memtable.cc/h       # MemTable 实现
    │   ├── skiplist.h          # 并发跳表
    │   └── arena.cc/h          # 内存池
    ├── mvcc/                   # MVCC 版本管理（6 文件）
    │   ├── version_catalog.cc/h  # VersionCatalog 版本账本
    │   ├── version_delta.cc/h    # VersionDelta 增量变更
    │   └── *_test.cc           # 测试文件
    ├── sstable/                # SSTable 存储（18 文件）
    │   ├── table/              # Block / Filter / Table 编解码
    │   ├── iterator/           # 合并/两级迭代器
    │   └── sstable_test.cc
    ├── wal/                    # 预写日志（6 文件）
    │   ├── log_format.h        # 日志记录格式
    │   ├── log_writer.cc/h     # 日志写入器
    │   ├── log_reader.cc/h     # 日志读取器（状态机）
    │   └── log_test.cc
    └── utils/                  # 基础工具（16 文件）
        ├── env_posix.cc        # POSIX 环境实现
        ├── cache.cc/h          # LRU Cache
        ├── coding.cc/h         # Varint 编解码
        ├── crc32c.cc/h         # CRC32C 校验
        ├── hash.cc/h           # 哈希函数
        └── *_test.cc
```

---

## 构建选项

```cmake
# 可选：启用 Snappy 压缩支持
# cmake .. -DLSMDB_HAVE_SNAPPY=ON

# 可选：启用 Zstd 压缩支持
# cmake .. -DLSMDB_HAVE_ZSTD=ON

# Debug 模式（含断言和详细日志）
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

---

## License

MIT
