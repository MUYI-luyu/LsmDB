#pragma once

#include <cstddef>

namespace lsmdb {

class Cache;
class Comparator;
class Env;
class FilterPolicy;
class Logger;
class Snapshot;

// DB 内容存储在一组块中，每个块包含一系列键值对。
// 每个块在存储到文件前可能被压缩。以下枚举描述了
// 用于压缩块的压缩方法（如果有）。
enum CompressionType {
  // 注意：不要更改现有条目的值，因为它们是
  // 磁盘上持久格式的一部分。
  kNoCompression = 0x0,
  kSnappyCompression = 0x1,
  kZstdCompression = 0x2,
};

// 控制数据库行为的选项（传递给 DB::Open）
struct Options {
  // 创建具有所有字段默认值的 Options 对象。
  Options();

  // -------------------
  // 影响行为的参数
  // 用于定义表中键顺序的比较器。
  // 默认值：使用字典序字节比较的比较器
  //
  // 要求：调用者必须确保此处提供的比较器与
  // 先前打开同一 DB 时提供的比较器具有相同的名称，
  // 并且对键的排序方式*完全*相同。
  const Comparator* comparator;

  // 如果为 true，数据库缺失时将创建。
  bool create_if_missing = false;

  // 如果为 true，数据库已存在时将报错。
  bool error_if_exists = false;

  // 如果为 true，实现对正在处理的数据进行积极检查，
  // 并在检测到任何错误时提前停止。这可能会产生不可预见的后果：
  // 例如，一个 DB 条目的损坏可能导致大量条目
  // 变得不可读或整个 DB 无法打开。
  bool paranoid_checks = false;

  // 使用指定的对象与环境交互，
  // 例如读写文件、调度后台工作等。
  // 默认值：Env::Default()
  Env* env;

  // 数据库生成的任何内部进度/错误信息将写入 info_log
  // （如果非空），如果 info_log 为空，则写入
  // DB 内容所在目录中的文件。
  Logger* info_log = nullptr;

  // -------------------
  // 影响性能的参数

  // 在转换为排序磁盘文件之前，在内存中累积的数据量
  //（由磁盘上未排序的日志支持）。
  //
  // 较大的值可提高性能，尤其是在批量加载时。
  // 内存中最多可同时保留两个写缓冲区，
  // 因此可能需要调整此参数以控制内存使用量。
  // 此外，较大的写缓冲区将导致下次打开数据库时
  // 恢复时间更长。
  size_t write_buffer_size = 4 * 1024 * 1024;

  // DB 可以使用的打开文件数。如果数据库工作集较大，
  // 可能需要增加此值（每 2MB 工作集预算一个打开文件）。
  int max_open_files = 1000;

  // 块的控制（用户数据存储在一组块中，
  // 块是从磁盘读取的单位）。

  // 如果非空，为块使用指定的缓存。
  // 如果为 null，LsmDB 将自动创建并使用一个 8MB 的内部缓存。
  Cache* block_cache = nullptr;

  // 每个块打包的用户数据的近似大小。注意，
  // 此处指定的块大小对应未压缩数据。
  // 如果启用压缩，从磁盘读取的实际单元大小可能更小。
  // 此参数可动态更改。
  size_t block_size = 4 * 1024;

  // 键的增量编码的重启点之间的键数量。
  // 此参数可动态更改。大多数客户端应保持此参数不变。
  int block_restart_interval = 16;

  // 在切换文件之前，写入单个文件的最大字节数。
  // 大多数客户端应保持此参数不变。但如果文件系统
  // 对大文件更高效，可以考虑增加该值。
  // 缺点是压缩时间更长，从而带来更高的延迟/性能抖动。
  // 增加此参数的另一个原因可能是在初始加载大型数据库时。
  size_t max_file_size = 2 * 1024 * 1024;

  // 使用指定的压缩算法压缩块。此参数可动态更改。
  //
  // 默认值：kSnappyCompression，提供轻量但快速的压缩。
  //
  // kSnappyCompression 在 Intel(R) Core(TM)2 2.4GHz 上的典型速度：
  //   ~200-500MB/s 压缩
  //   ~400-800MB/s 解压缩
  // 注意，这些速度明显快于大多数持久存储的速度，
  // 因此通常没必要切换到 kNoCompression。
  // 即使输入数据是不可压缩的，kSnappyCompression 实现
  // 也能有效检测到并切换到未压缩模式。
  CompressionType compression = kSnappyCompression;

  // zstd 的压缩级别。
  // 目前仅支持 [-5,22] 范围。默认值为 1。
  int zstd_compression_level = 1;

  // 实验性：如果为 true，打开数据库时追加到现有
  // MANIFEST 和日志文件。这可以显著加快打开速度。
  //
  // 默认值：当前为 false，但将来可能变为 true。
  bool reuse_logs = false;

  // 如果非空，使用指定的过滤器策略减少磁盘读取。
  // 许多应用程序将通过在此传入 NewBloomFilterPolicy() 的结果受益。
  const FilterPolicy* filter_policy = nullptr;
};

// 控制读取操作的选项
struct ReadOptions {
  // 如果为 true，从底层存储读取的所有数据将
  // 根据相应校验和进行验证。
  bool verify_checksums = false;

  // 本次迭代读取的数据是否应缓存在内存中？
  // 调用者可能希望为批量扫描将此字段设为 false。
  bool fill_cache = true;

  // 如果 "snapshot" 非空，基于提供的快照读取
  //（必须属于正在读取的 DB，且必须尚未被释放）。
  // 如果 "snapshot" 为空，使用此读取操作开始时的
  // 状态的隐式快照。
  const Snapshot* snapshot = nullptr;
};

// 控制写入操作的选项
struct WriteOptions {
  WriteOptions() = default;

  // 如果为 true，写入将在认为完成之前从操作系统
  // 缓冲区缓存中刷新（通过调用 WritableFile::Sync()）。
  // 如果此标志为 true，写入将变慢。
  //
  // 如果此标志为 false，且机器崩溃，一些最近的写入
  // 可能会丢失。注意，如果只是进程崩溃
  //（即机器未重启），即使 sync==false 也不会丢失写入。
  //
  // 换句话说，sync==false 的 DB 写入与 "write()" 系统调用
  // 具有类似的崩溃语义。sync==true 的 DB 写入与 "write()"
  // 后跟 "fsync()" 具有类似的崩溃语义。
  bool sync = false;
};

}  // namespace lsmdb
