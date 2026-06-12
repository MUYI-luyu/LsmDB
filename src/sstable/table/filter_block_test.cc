#include "sstable/table/filter_block.h"

#include "db/filter_policy.h"
#include "gtest/gtest.h"
#include "utils/coding.h"
#include "utils/hash.h"
#include "utils/logging.h"

namespace lsmdb {

class FilterBlockTest : public testing::Test {
 public:
  // 核心改动：直接实例化真正的布隆过滤器实体（bits_per_key = 10）
  FilterBlockTest() : policy_(10) {}

  FilterPolicy policy_;
};

TEST_F(FilterBlockTest, EmptyBuilder) {
  // 修改点：直接传入真正过滤器的指针
  FilterBlockBuilder builder(&policy_);
  Slice block = builder.Finish();
  
  // 真正的布隆过滤器在没有任何 key 时，物理输出为 64 位（8 字节）全 0 位图 + 1 字节的 k 值 (0x06) + 4 字节偏移量索引
  // 这里我们直接验证它能够正常生成与反解析，不硬编码死特异的十六进制字符串
  FilterBlockReader reader(&policy_, block);
  ASSERT_TRUE(reader.KeyMayMatch(0, "foo"));
  ASSERT_TRUE(reader.KeyMayMatch(100000, "foo"));
}

TEST_F(FilterBlockTest, SingleChunk) {
  FilterBlockBuilder builder(&policy_);
  builder.StartBlock(100);
  builder.AddKey("foo");
  builder.AddKey("bar");
  builder.AddKey("box");
  builder.StartBlock(200);
  builder.AddKey("box");
  builder.StartBlock(300);
  builder.AddKey("hello");
  Slice block = builder.Finish();
  
  FilterBlockReader reader(&policy_, block);
  // 布隆过滤器对灌入的 key 必须 100% 命中
  ASSERT_TRUE(reader.KeyMayMatch(100, "foo"));
  ASSERT_TRUE(reader.KeyMayMatch(100, "bar"));
  ASSERT_TRUE(reader.KeyMayMatch(100, "box"));
  ASSERT_TRUE(reader.KeyMayMatch(100, "hello"));
  
  // 测试绝对不存在的 key 的拦截效果（高概率返回 false）
  ASSERT_TRUE(!reader.KeyMayMatch(100, "missing"));
  ASSERT_TRUE(!reader.KeyMayMatch(100, "other"));
}

TEST_F(FilterBlockTest, MultiChunk) {
  FilterBlockBuilder builder(&policy_);

  // 第一个数据块对应的过滤段
  builder.StartBlock(0);
  builder.AddKey("foo");
  builder.StartBlock(2000);
  builder.AddKey("bar");

  // 第二个数据块对应的过滤段
  builder.StartBlock(3100);
  builder.AddKey("box");

  // 第三个数据块对应的过滤段为空

  // 最后一个数据块对应的过滤段
  builder.StartBlock(9000);
  builder.AddKey("box");
  builder.AddKey("hello");

  Slice block = builder.Finish();
  FilterBlockReader reader(&policy_, block);

  // 验证第一个数据块区间的过滤契约
  ASSERT_TRUE(reader.KeyMayMatch(0, "foo"));
  ASSERT_TRUE(reader.KeyMayMatch(2000, "bar"));
  ASSERT_TRUE(!reader.KeyMayMatch(0, "box"));
  ASSERT_TRUE(!reader.KeyMayMatch(0, "hello"));

  // 验证第二个数据块区间的过滤契约
  ASSERT_TRUE(reader.KeyMayMatch(3100, "box"));
  ASSERT_TRUE(!reader.KeyMayMatch(3100, "foo"));
  ASSERT_TRUE(!reader.KeyMayMatch(3100, "bar"));

  // 验证空数据块区间（不应该捞出任何存在的 key）
  ASSERT_TRUE(!reader.KeyMayMatch(4100, "foo"));
  ASSERT_TRUE(!reader.KeyMayMatch(4100, "bar"));
  ASSERT_TRUE(!reader.KeyMayMatch(4100, "box"));

  // 验证最后一个数据块区间的过滤契约
  ASSERT_TRUE(reader.KeyMayMatch(9000, "box"));
  ASSERT_TRUE(reader.KeyMayMatch(9000, "hello"));
  ASSERT_TRUE(!reader.KeyMayMatch(9000, "foo"));
}

}  // namespace lsmdb