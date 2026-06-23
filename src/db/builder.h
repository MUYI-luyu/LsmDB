#pragma once

#include <cstdint>

#include "db/dbformat.h"
#include "db/env.h"
#include "db/iterator.h"
#include "db/options.h"
#include "db/status.h"
#include "mvcc/version_delta.h"

namespace lsmdb {

class MemTable;
class TableCache;
class TableBuilder;
class WritableFile;

// 根据 *memtable 的内容构建一个 Table 文件。生成的文件将按照给定的文件编号在磁盘上命名。
// 调用方负责在 Finish() 返回后关闭 *file。
// 如果发生错误，调用方同样需要负责关闭 *file。
Status BuildTable(const std::string& dbname, Env* env,
                  const Options& options, TableCache* table_cache,
                  Iterator* iter, SSTableDescriptor* meta);

}