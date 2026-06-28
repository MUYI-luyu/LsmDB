cmake_minimum_required(VERSION 3.14)
project(LsmDB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lsmdb_core
  src/utils/coding.cc
  src/utils/hash.cc
  src/utils/crc32c.cc
  src/utils/cache.cc
  src/utils/logging.cc
  src/utils/env_posix.cc
  src/utils/testutil.cc
  src/memtable/arena.cc
  src/sstable/iterator/iterator.cc
  src/db/env.cc
  src/db/filename.cc
)

target_include_directories(lsmdb_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

add_library(lsmdb_memtable
  src/memtable/memtable.cc
  src/db/dbformat.cc
  src/db/comparator.cc
  src/db/options.cc
)

target_link_libraries(lsmdb_memtable PUBLIC lsmdb_core)
target_include_directories(lsmdb_memtable
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── WriteBatch 批量写入 ────────────────────

add_library(lsmdb_writebatch
  src/db/write_batch.cc
)

target_link_libraries(lsmdb_writebatch
  PUBLIC
    lsmdb_memtable
)

target_include_directories(lsmdb_writebatch
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

add_library(lsmdb_sstable
  src/sstable/table/block.cc
  src/sstable/table/block_builder.cc
  src/sstable/table/filter_block.cc
  src/sstable/table/format.cc
  src/sstable/table/table.cc
  src/sstable/table/table_builder.cc
  src/sstable/iterator/merger.cc
  src/sstable/iterator/two_level_iterator.cc
  src/db/table_cache.cc
)

target_link_libraries(lsmdb_sstable
  PUBLIC
    lsmdb_core
  PRIVATE
    lsmdb_memtable
)

target_include_directories(lsmdb_sstable
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── WAL 预写日志 ────────────────────

add_library(lsmdb_wal
  src/wal/log_writer.cc
  src/wal/log_reader.cc
)

target_link_libraries(lsmdb_wal
  PUBLIC
    lsmdb_core
)

target_include_directories(lsmdb_wal
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── MVCC 版本管理 ────────────────────

add_library(lsmdb_mvcc
  src/mvcc/version_delta.cc
  src/mvcc/version_catalog.cc
)

target_link_libraries(lsmdb_mvcc
  PUBLIC
    lsmdb_core
  PRIVATE
    lsmdb_wal
    lsmdb_sstable
)

target_include_directories(lsmdb_mvcc
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── DB 数据库主逻辑 ────────────────────
#   db.cc        公共接口基类实现（Snapshot/DB 析构、DB::Open、DestroyDB）
#   builder.cc   构建器：MemTable → SSTable 持久化
#   snapshot.h   快照隔离：双向链表管理 MVCC 读视图
#   db_impl.cc   数据库引擎：初始化恢复、读写路径、压实调度、属性统计

add_library(lsmdb_db
  src/db/db.cc
  src/db/builder.cc
  src/db/db_iter.cc
  src/db/db_iter.h
  src/db/snapshot.h
  src/db/db_impl.cc
  src/db/db_impl.h
)

target_link_libraries(lsmdb_db
  PUBLIC
    lsmdb_core
  PRIVATE
    lsmdb_sstable
    lsmdb_mvcc
)

target_include_directories(lsmdb_db
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

find_package(GTest REQUIRED)

add_executable(skiplist_test
  src/memtable/skiplist_test.cc
)
target_link_libraries(skiplist_test
  PRIVATE
    lsmdb_memtable
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(skiplist_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

add_executable(crc32c_test
  src/utils/crc32c_test.cc
)
target_link_libraries(crc32c_test
  PRIVATE
    lsmdb_core
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(crc32c_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

add_executable(logging_test
  src/utils/logging_test.cc
)
target_link_libraries(logging_test
  PRIVATE
    lsmdb_core
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(logging_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── WriteBatch 测试 ────────────────────

add_executable(write_batch_test
  src/db/write_batch_test.cc
)
target_link_libraries(write_batch_test
  PRIVATE
    lsmdb_writebatch
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(write_batch_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── SSTable 过滤器块测试 ────────────────────

add_executable(filter_block_test
  src/sstable/table/filter_block_test.cc
)
target_link_libraries(filter_block_test
  PRIVATE
    lsmdb_sstable
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(filter_block_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── WAL 日志测试 ────────────────────

add_executable(log_test
  src/wal/log_test.cc
)
target_link_libraries(log_test
  PRIVATE
    lsmdb_wal
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(log_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# ──────────────────── MVCC 版本管理测试 ────────────────────

add_executable(version_delta_test
  src/mvcc/version_delta_test.cc
)
target_link_libraries(version_delta_test
  PRIVATE
    lsmdb_mvcc
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(version_delta_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

add_executable(version_catalog_test
  src/mvcc/version_catalog_test.cc
)
target_link_libraries(version_catalog_test
  PRIVATE
    lsmdb_mvcc
    GTest::GTest
    GTest::Main
    pthread
)
target_include_directories(version_catalog_test
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)