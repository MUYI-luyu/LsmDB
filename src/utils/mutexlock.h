#pragma once

#include <condition_variable>
#include <mutex>

namespace lsmdb {

class MutexLock {
 public:
  explicit MutexLock(std::mutex* mu) : mu_(mu) { mu_->lock(); }
  ~MutexLock() { mu_->unlock(); }

  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

 private:
  std::mutex* const mu_;
};

// A simple wrapper around std::condition_variable that works with
// MutexLock / std::mutex*, matching the original LevelDB port::CondVar API.
class CondVar {
 public:
  explicit CondVar(std::mutex* mu) : mu_(mu) {}
  ~CondVar() = default;

  // Wait on the condition variable.  The associated mutex must be
  // locked by the calling thread before calling Wait().
  void Wait() {
    std::unique_lock<std::mutex> lock(*mu_, std::adopt_lock);
    cv_.wait(lock);
    lock.release();
  }

  void Signal() { cv_.notify_one(); }
  void SignalAll() { cv_.notify_all(); }

 private:
  std::condition_variable cv_;
  std::mutex* mu_;
};

}  // namespace lsmdb
