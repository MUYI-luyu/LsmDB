#pragma once

#include <cstdint>

#include "db/slice.h"
#include "db/status.h"
#include "wal/log_format.h"

namespace lsmdb {

class SequentialFile;

namespace log {

class Reader {
 public:
  // 数据损坏与异常处理的抽象回调接口
  class Reporter {
   public:
    // 虚析构函数，确保派生类资源能够被正确释放，防止内存泄漏。
    virtual ~Reporter();

    // 当 Reader 在解析 32KB 物理块时，因 CRC 校验失败、记录长度越界或读到断层分片而触发数据擦除时回调此函数。
    // 【参数 bytes】：本次因数据损坏而不得不丢弃/跳过的近似物理字节数（上层可据此度量数据丢失的数据量）。
    // 【参数 status】：封装了具体损坏根因的非 OK 状态（如 Status::Corruption("bad crc")）。
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // 构造函数：初始化 WAL 日志读取器状态机。
  // 【生命周期约束】：入参指针 *file 和 *reporter（若非空）的生命周期必须严格 大于或等于 当前 Reader 实例。
  //                在 Reader 运行期间，若单方面析构外部文件或 reporter，将直接引发悬空指针引发 UAF 崩溃。
  // 【参数 checksum】：防磁盘静默损坏开关。设为 true 时，每读一条物理记录都会强行进行一次 Mask 逆向 CRC32c 验证。
  // 【参数 initial_offset】：增量恢复的绝对物理锚点。Reader 启动时会强制跳过文件位置小于该 offset 的所有 32KB 数据块，
  //                并自动进入 resyncing_ 重新同步模式，直至在新块中捕捉到第一个确定性物理记录起点。
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         uint64_t initial_offset);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  ~Reader();

  // 将下一条逻辑记录读入 *record。
  // 【返回值】：成功拼接并解析出一条完整记录返回 true；到达文件末尾（EOF）或底层发生不可恢复的损坏返回 false。
  // 【内存约束】：若记录未跨块（kFullType），*record 将直接指向内部的静态内存缓冲区（Zero-Copy）；
  //             若记录跨块，则使用 *scratch 作为缓冲区拼接物理分片。
  // 【生命周期】：*record 的内容在当前 Reader 调用下一次 ReadRecord()、或外部修改 *scratch 之前有效。
  bool ReadRecord(Slice* record, std::string* scratch);

  // 返回上一次成功被 ReadRecord() 解析并返回给用户的逻辑记录在文件中的绝对起始物理偏移量。
  // 在首次成功调用 ReadRecord() 并在其返回 true 之前，此返回值未定义（通常为 0）。
  uint64_t LastRecordOffset();

 private:
  // 供内部状态机使用的扩展物理记录类型，用作 ReadPhysicalRecord 的特殊状态返回值。
  enum {
    // 物理文件已到达末尾（EOF）
    kEof = kMaxRecordType + 1,
    
    // 遇到无效、损坏或需要被静默忽略的物理记录。
    // 状态机在以下三种物理情景下会触发此返回值：
    // * 记录的 CRC 无效（ReadPhysicalRecord 报告丢弃）
    // * 记录是零长度记录（不报告丢弃）
    // * 记录位于构造函数的 initial_offset 之下（不报告丢弃） 
    kBadRecord = kMaxRecordType + 2
  };

  // 直接定位并跳过所有完全位于 "initial_offset_" 之前的 32KB 物理块。
  bool SkipToInitialBlock();

  // 从当前的 32KB 物理块缓冲区中切分并读取下一个基础的物理分片。
  // 【参数 result】：用于承载当前物理分片的 Payload 数据指针与长度。
  // 【返回值】：若读取成功，返回其物理 Header 里的 RecordType（kFull/First/Middle/Last）；
  //           若触发特殊边界或损坏，返回上述扩展枚举值（kEof 或 kBadRecord）。
  unsigned int ReadPhysicalRecord(Slice* result);

  // 向 reporter 报告丢弃的字节数。
  // 调用前必须更新 buffer_ 以移除已丢弃的字节。
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason); 

  SequentialFile* const file_;  // 底层顺序读取的物理文件句柄
  Reporter* const reporter_;    // 错误报告回调接口，用于向上层通知数据损坏（Corruption）
  bool const checksum_;         // 是否开启 CRC 校验和验证（防磁盘静默损坏开关）
  char* const backing_store_;   // 严格固定为 32KB 的硬件级物理块（Block）内存缓冲区
  Slice buffer_;                // 动态游标：指向 backing_store_ 中当前还未被解析的物理数据片段
  bool eof_;                    // 停机标志：上次单次读盘返回数据小于 32KB，代表文件已到底

  // 最近成功返回给用户的完整记录在文件中的绝对起始物理偏移量
  uint64_t last_record_offset_;       

  // 当前 32KB 内存块尾部在物理文件中的绝对截止偏移量
  uint64_t end_of_buffer_offset_;

  // 启动或 Seek 时，要求跳过在此之前的全部历史物理数据
  uint64_t const initial_offset_;

  // 空降到物理块内部时，用于静默过滤掉断层的残缺分片
  bool resyncing_;
};

}  // namespace log
}  // namespace lsmdb

