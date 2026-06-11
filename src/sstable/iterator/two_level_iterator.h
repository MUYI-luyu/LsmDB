#pragma once

#include "db/iterator.h"

namespace lsmdb {

struct ReadOptions;

// 返回一个新的两级迭代器。两级迭代器包含一个索引迭代器，
// 其值指向一系列数据块，每个数据块本身又是一个键值对序列。
// 返回的两级迭代器按顺序拼接所有数据块中的键/值对。
// 接管 "index_iter" 的所有权，在不再需要时将其删除。
//
// 使用用户提供的函数将 index_iter 的值转换为对应数据块的迭代器。
Iterator* NewTwoLevelIterator(
    Iterator* index_iter,
    Iterator* (*block_function)(void* arg, const ReadOptions& options,
                                const Slice& index_value),
    void* arg, const ReadOptions& options);

}  // namespace lsmdb

