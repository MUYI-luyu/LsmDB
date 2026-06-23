#include "db/builder.h"

#include <cstdint>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_builder.h"
#include "db/table_cache.h"
#include "db/options.h"
#include "utils/logging.h"

namespace lsmdb {

Status BuildTable(const std::string& dbname, Env* env,
                  const Options& options, TableCache* table_cache,
                  Iterator* iter, SSTableDescriptor* meta) {
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    WritableFile* file;
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }

    TableBuilder builder(options, file);
    meta->smallest.DecodeFrom(iter->key());
    for (; iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      meta->largest.DecodeFrom(key);
      builder.Add(key, iter->value());
    }

    // 完成构建并检查 builder 是否发生错误
    s = builder.Finish();
    if (s.ok()) {
      meta->file_size = builder.FileSize();
      assert(meta->file_size > 0);
    }

    if (!iter->status().ok()) {
      s = iter->status();
    }

    if (s.ok()) {
      s = file->Sync();
    }

    if (s.ok()) {
      // 验证该 table 是否能够正常打开
      Iterator* it = table_cache->NewIterator(ReadOptions(),
                                                meta->number,
                                                meta->file_size);
      s = it->status();
      if (s.ok()) {
        it->SeekToFirst();
      }
      delete it;
    }

    delete file;  // 即使发生错误，文件也会被关闭
  }

  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // 保留该文件
  } else {
    env->RemoveFile(fname);
  }

  return s;
}

}  // namespace lsmdb