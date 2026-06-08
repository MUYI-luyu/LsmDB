#include "db/options.h"

#include "db/comparator.h"
#include "db/env.h"

namespace lsmdb {

Options::Options()
    : comparator(BytewiseComparator()),
      env(Env::Default()) {}

}  // namespace lsmdb
