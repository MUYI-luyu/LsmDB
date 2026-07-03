#include "db/db.h"

// Snapshot::~Snapshot() 和 DB::~DB() 的定义已迁移至 db_impl.cc，
// 与其他 DB 基类接口实现（DB::Open、DB::Put、DB::Delete、DestroyDB）集中管理。
namespace lsmdb {
}  // namespace lsmdb
