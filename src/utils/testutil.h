#pragma once

#include <string>

#include "db/slice.h"
#include "gtest/gtest.h"
#include "utils/random.h"

namespace lsmdb {
namespace test {

// 用于测试返回 lsmdb::Status 的函数结果的宏。
#define EXPECT_LEVELDB_OK(expression) \
  EXPECT_TRUE((expression).ok())
#define ASSERT_LEVELDB_OK(expression) \
  ASSERT_TRUE((expression).ok())

// 返回当前测试运行开始时使用的随机种子。
inline int RandomSeed() {
  return testing::UnitTest::GetInstance()->random_seed();
}

// 在 *dst 中存储长度为 "len" 的随机字符串，并返回引用生成数据的 Slice。
Slice RandomString(Random* rnd, int len, std::string* dst);

// 返回指定长度的随机键，可能包含特殊字符（如 \x00、\xff 等）。
std::string RandomKey(Random* rnd, int len);

// 在 *dst 中存储长度为 "len" 的字符串，压缩后约为
// "N*compressed_fraction" 字节，并返回引用生成数据的 Slice。
Slice CompressibleString(Random* rnd, double compressed_fraction, size_t len,
                         std::string* dst);

}  // namespace test
}  // namespace lsmdb
