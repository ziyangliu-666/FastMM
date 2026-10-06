#pragma once
// The instance lock of an engine name: an exclusive flock(2) on <journal_dir>/<engine name>.lock
// ([engine] lock_file). The process holding it is the one that may trade that engine's account;
// fastmm-live takes it before it reads the kill state, the store or the epoch file and before any
// venue is told anything, and drops it after its shutdown has cancelled, flushed and closed all of
// them (docs/how-to/operations/hand-over-a-session.md).
//
// The kernel drops the lock with the last descriptor of the open file, so a crashed, killed or
// OOM-killed process never leaves it held. The file stays on disk (removing a lock file races a
// process about to lock it) and holds the holder's pid as text, for the messages of whoever waits.
//
// The descriptor is close-on-exec. A child forked without exec keeps the lock while it lives.
#include <cstdint>
#include <string>

namespace fastmm::live {

class InstanceLock {
 public:
  enum class Status : std::uint8_t {
    Acquired,  // this object holds the lock
    Held,      // another open file description holds it
    Error,     // the file could not be opened or locked; see `error`
  };

  InstanceLock() noexcept = default;
  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;
  ~InstanceLock() { release(); }

  // Never blocks. Creates the file and its directory when missing; on Acquired writes this
  // process's pid into it. Calling it again while held returns Acquired.
  [[nodiscard]] Status try_acquire(const std::string& path, std::string* error = nullptr);
  // Unlocks and closes; a no-op when not held.
  void release() noexcept;

  [[nodiscard]] bool held() const noexcept { return fd_ >= 0; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  // The pid the holder of `path` wrote, 0 when the file is missing or holds no number. The writer
  // may have exited since: only a failed try_acquire says the lock is held.
  [[nodiscard]] static std::uint32_t holder_pid(const std::string& path) noexcept;

 private:
  int fd_ = -1;
  std::string path_;
};

// <journal_dir>/<engine>.lock unless `lock_file` names another file.
[[nodiscard]] std::string instance_lock_path(const std::string& lock_file,
                                             const std::string& journal_dir,
                                             const std::string& engine_name);

}  // namespace fastmm::live
