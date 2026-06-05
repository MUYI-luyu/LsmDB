cmake_minimum_required(VERSION 3.14)
project(LsmDB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lsmdb_core
  src/utils/coding.cc
  src/utils/hash.cc
  src/utils/crc32c.cc
  src/utils/logging.cc
  src/memtable/arena.cc
  src/sstable/iterator/iterator.cc
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
  src/db/env.cc
)

target_link_libraries(lsmdb_memtable PUBLIC lsmdb_core)
target_include_directories(lsmdb_memtable
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

add_library(lsmdb_sstable
  src/sstable/table/block.cc
  src/sstable/table/block_builder.cc
  src/sstable/table/format.cc
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