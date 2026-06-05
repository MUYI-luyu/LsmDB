#include "db/options.h"

#include "db/comparator.h"

namespace lsmdb {

Options::Options()
    : comparator(BytewiseComparator()),
      env(nullptr) {}

}  // namespace lsmdb
