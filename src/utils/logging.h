// 不得从任何 .h 文件中包含，以避免用宏污染命名空间。

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "db/slice.h"

namespace lsmdb {

class Slice;
class WritableFile;

// 将数字 "num" 转换为易读的文本格式并追加到 *str 末尾
void AppendNumberTo(std::string* str, uint64_t num);

// 将字符串 "value" 转换为易读的文本格式并追加到 *str 末尾。
// 会对 "value" 中包含的所有不可打印字符进行转义处理。
void AppendEscapedStringTo(std::string* str, const Slice& value);

// 返回数字 "num" 易读的文本格式字符串
std::string NumberToString(uint64_t num);

// 返回字符串 "value" 的易读文本版本。
// 会对 "value" 中包含的所有不可打印字符进行转义处理。
std::string EscapeString(const Slice& value);

// 从 "*in" 中解析出一个易读的十进制数字并存入 *val。
// 解析成功时，将 "*in" 向前推进以越过已消费的数字字符，并设置 *val 为对应的数值，然后返回 true。
// 解析失败时，返回 false，此时 *in 将处于未定义状态（不保证指针停留在何处）。
bool ConsumeDecimalNumber(Slice* in, uint64_t* val);

}  // namespace lsmdb
