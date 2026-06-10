#pragma once

namespace lsmdb {

class Comparator;
class Iterator;

// 返回一个合并迭代器，提供 children[0..n-1] 中所有数据的并集。
// 接管所有子迭代器的所有权，并在结果迭代器销毁时一并删除它们。
// 结果不做去重。即，如果某个键在 K 个子迭代器中同时出现，
// 那么该键将在结果中被产出 K 次。
//
// 前置条件: n >= 0
Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children,
                             int n);

}

