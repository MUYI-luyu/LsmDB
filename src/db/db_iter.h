#pragma once

#include "db/dbformat.h"
#include "db/iterator.h"
#include "db/options.h"

namespace lsmdb {

class DBImpl;

// 创建并返回一个面向用户的 DBIterator。它接收一个底层的内部迭代器（*internal_iter），
// 并将其中在指定 sequence（版本号）之前处于活跃状态的内部键，过滤并提炼为对应的一致性用户键。
Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator,
                        Iterator* internal_iter, SequenceNumber sequence,
                        uint32_t seed);

}  // namespace lsmdb
