#pragma once
// Single-threaded event loop with a timer min-heap and a thread-safe mailbox (post/wake via
// eventfd), over one of two backends chosen at construction:
//
//   ReactorBackend::Epoll    edge-triggered epoll (the default)
//   ReactorBackend::IoUring  io_uring, one multishot IORING_OP_POLL_ADD per registered fd,
//                            timeouts through IORING_ENTER_EXT_ARG, over liburing
//
// All handler callbacks and timers run on the thread that calls run()/run_once(); only wake()
// and post() may be called from other threads.
//
// Handler contract (both backends): a handler receiving on_readable()/on_writable() must drain
// the fd until it returns want_read/want_write, otherwise no further event may be delivered. The
// io_uring backend may report readiness the handler has already consumed; draining until EAGAIN
// makes that harmless. POLLOUT is only requested while the registration asks for Write.
//
// Descriptor lifetime: call remove() before closing a registered fd. epoll forgets a closed
// descriptor on its own, but an io_uring poll request holds a reference to the file, so the
// socket stays open (no FIN, port still bound) until remove() cancels the request. add() of a
// new socket that reuses the number of a closed but still registered fd cancels the stale
// registration; this cannot be detected for anonymous-inode descriptors (eventfd, timerfd).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

struct io_uring_sqe;  // NOLINT(readability-identifier-naming): <liburing.h>

namespace fastmm::net {

namespace detail {
class IoUring;
}

// NOLINTNEXTLINE(performance-enum-size): same width as the epoll event mask it is converted to
enum class IoEvent : std::uint32_t { None = 0, Read = 1, Write = 2, ReadWrite = 3 };

constexpr IoEvent operator|(IoEvent a, IoEvent b) noexcept {
  return static_cast<IoEvent>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool has(IoEvent set, IoEvent flag) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;
}

enum class ReactorBackend : std::uint8_t { Epoll, IoUring };

// "epoll" / "io_uring", the spelling of [engine] net_backend.
[[nodiscard]] std::string_view to_string(ReactorBackend backend) noexcept;
// Parses "epoll" / "io_uring". Returns false (and leaves `out` alone) for anything else.
[[nodiscard]] bool parse_reactor_backend(std::string_view text, ReactorBackend& out) noexcept;

class IoHandler {
 public:
  virtual ~IoHandler() = default;
  virtual void on_readable() = 0;
  virtual void on_writable() = 0;
  // POLLERR (or a poll request the kernel refused). `err` is the socket's SO_ERROR when
  // retrievable (the refused request's errno for the latter), else 0.
  virtual void on_error(int err) = 0;
};

using TimerId = std::uint64_t;
inline constexpr TimerId kInvalidTimer = 0;

class Reactor {
 public:
  using TimerCallback = std::function<void()>;
  using Task = std::function<void()>;

  Reactor() : Reactor(ReactorBackend::Epoll) {}
  // Throws std::runtime_error if the backend (epoll_create1 / io_uring_setup) or the eventfd
  // cannot be created. Check io_uring_supported() before asking for IoUring.
  explicit Reactor(ReactorBackend backend);
  ~Reactor();
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  [[nodiscard]] ReactorBackend backend() const noexcept { return backend_; }
  // True when this kernel lets the process create an io_uring with everything the backend uses
  // (multishot poll, poll update, EXT_ARG timeouts). Probed once, then cached. False on ENOSYS,
  // EPERM (kernel.io_uring_disabled, seccomp), ENOMEM and kernels older than 5.13.
  [[nodiscard]] static bool io_uring_supported() noexcept;
  // `requested`, or Epoll when io_uring was requested but is not supported.
  [[nodiscard]] static ReactorBackend resolve_backend(ReactorBackend requested) noexcept;

  // fd registration (reactor thread only). Registrations always include RDHUP so half-closes
  // surface as on_readable() (read returns EOF). add() fails for an fd that is already
  // registered, a closed fd and (like epoll) regular files and directories.
  bool add(int fd, IoHandler& handler, IoEvent events) noexcept;
  bool modify(int fd, IoHandler& handler, IoEvent events) noexcept;
  bool remove(int fd) noexcept;
  bool is_registered(int fd) const noexcept;

  // Timers (reactor thread only). Deadlines are on the now_ns() clock.
  TimerId add_timer(std::int64_t deadline_ns, TimerCallback cb);
  TimerId add_timer_after(std::int64_t delay_ns, TimerCallback cb) {
    return add_timer(now_ns() + delay_ns, std::move(cb));
  }
  bool cancel_timer(TimerId id) noexcept;
  std::size_t active_timers() const noexcept { return timers_.size(); }

  // Thread-safe: queue a task for the reactor thread and wake it.
  void post(Task task);
  // Thread-safe: interrupt a blocking wait in run_once().
  void wake() noexcept;

  // One iteration: wait for I/O at most min(next timer, max_wait_ms), dispatch I/O, posted
  // tasks, expired timers. max_wait_ms < 0 blocks until something happens. Returns the number
  // of I/O events (epoll) or completions (io_uring) dispatched, or -1 on a wait error other than
  // EINTR. epoll rounds the timer bound up to whole milliseconds; io_uring waits in nanoseconds.
  int run_once(int max_wait_ms);
  // Loop until `stop` becomes true (checked once per iteration; call wake() after setting it).
  void run(const std::atomic<bool>& stop);

  // Busy polling: never block, pause instruction between empty polls. epoll calls epoll_wait
  // with a zero timeout; io_uring reads the completion ring without a system call unless the
  // kernel flags pending work.
  void set_busy_poll(bool enabled) noexcept { busy_poll_ = enabled; }
  bool busy_poll() const noexcept { return busy_poll_; }

  static std::int64_t now_ns() noexcept;  // CLOCK_MONOTONIC
  // The epoll descriptor, or -1 with the io_uring backend.
  int epoll_fd() const noexcept { return epoll_fd_; }

 private:
  struct TimerEntry {
    std::int64_t deadline_ns;
    TimerId id;
    bool operator>(const TimerEntry& o) const noexcept {
      return deadline_ns != o.deadline_ns ? deadline_ns > o.deadline_ns : id > o.id;
    }
  };
  // io_uring registration state, indexed by fd.
  struct FdState {
    std::uint32_t gen = 0;   // bumped by add(); stale completions carry an older value
    std::uint32_t mask = 0;  // poll events currently requested
    std::uint64_t dev = 0;   // identity of the registered file, to detect fd number reuse
    std::uint64_t ino = 0;
  };

  void release() noexcept;
  int compute_timeout_ms(int max_wait_ms) const noexcept;
  std::int64_t compute_timeout_ns(int max_wait_ms) const noexcept;
  void dispatch_io(int nfds);
  void deliver(int fd, IoHandler* h, bool error, bool readable, bool writable);
  void run_posted();
  void run_expired_timers();
  void drain_wake_fd() noexcept;
  static int socket_error(int fd) noexcept;

  // io_uring backend
  int uring_run_once(int max_wait_ms);
  int uring_dispatch();
  void uring_complete(std::uint64_t user_data, std::int32_t res, std::uint32_t flags);
  bool uring_add(int fd, IoHandler& handler, IoEvent events) noexcept;
  io_uring_sqe* uring_sqe() noexcept;
  bool uring_arm(int fd) noexcept;
  bool uring_queue_update(int fd, std::uint32_t mask) noexcept;
  bool uring_queue_remove(int fd, std::uint32_t gen) noexcept;

  ReactorBackend backend_;
  int epoll_fd_ = -1;
  int wake_fd_ = -1;
  bool busy_poll_ = false;
  std::vector<IoHandler*> handlers_;  // indexed by fd; nullptr when not registered

  std::vector<TimerEntry> heap_;                       // min-heap on deadline
  std::unordered_map<TimerId, TimerCallback> timers_;  // live timers; cancel = erase
  TimerId next_timer_id_ = 1;

  std::mutex post_mutex_;
  std::vector<Task> posted_;
  std::vector<Task> running_;  // swapped in under the lock, executed unlocked

  static constexpr int kMaxEvents = 64;
  void* events_ = nullptr;  // epoll_event[kMaxEvents]; kept opaque to avoid <sys/epoll.h> here

  std::unique_ptr<detail::IoUring> uring_;
  std::vector<FdState> fd_state_;
};

}  // namespace fastmm::net
