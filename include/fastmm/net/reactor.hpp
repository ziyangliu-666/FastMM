#pragma once
// Single-threaded epoll (edge-triggered) event loop with a timer min-heap and a thread-safe
// mailbox (post/wake via eventfd). All handler callbacks and timers run on the thread that
// calls run()/run_once(); only wake() and post() may be called from other threads.
//
// Edge-triggered contract: a handler receiving on_readable()/on_writable() must drain the fd
// until it returns want_read/want_write, otherwise no further event is delivered.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fastmm::net {

// NOLINTNEXTLINE(performance-enum-size): same width as the epoll event mask it is converted to
enum class IoEvent : std::uint32_t { None = 0, Read = 1, Write = 2, ReadWrite = 3 };

constexpr IoEvent operator|(IoEvent a, IoEvent b) noexcept {
  return static_cast<IoEvent>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool has(IoEvent set, IoEvent flag) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;
}

class IoHandler {
 public:
  virtual ~IoHandler() = default;
  virtual void on_readable() = 0;
  virtual void on_writable() = 0;
  // EPOLLERR / EPOLLHUP. `err` is the socket's SO_ERROR when retrievable, else 0.
  virtual void on_error(int err) = 0;
};

using TimerId = std::uint64_t;
inline constexpr TimerId kInvalidTimer = 0;

class Reactor {
 public:
  using TimerCallback = std::function<void()>;
  using Task = std::function<void()>;

  Reactor();  // throws std::runtime_error if epoll_create1/eventfd fail
  ~Reactor();
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  // fd registration (reactor thread only). Events are always edge-triggered and include
  // EPOLLRDHUP so half-closes surface as on_readable() (read returns EOF).
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
  // Thread-safe: interrupt a blocking epoll_wait.
  void wake() noexcept;

  // One iteration: epoll_wait(min(next timer, max_wait_ms)), dispatch I/O, posted tasks,
  // expired timers. max_wait_ms < 0 blocks until something happens. Returns the number of
  // I/O events dispatched, or -1 on an epoll_wait error other than EINTR.
  int run_once(int max_wait_ms);
  // Loop until `stop` becomes true (checked once per iteration; call wake() after setting it).
  void run(const std::atomic<bool>& stop);

  // Busy polling: epoll_wait with a zero timeout and a pause instruction between empty polls.
  void set_busy_poll(bool enabled) noexcept { busy_poll_ = enabled; }
  bool busy_poll() const noexcept { return busy_poll_; }

  static std::int64_t now_ns() noexcept;  // CLOCK_MONOTONIC
  int epoll_fd() const noexcept { return epoll_fd_; }

 private:
  struct TimerEntry {
    std::int64_t deadline_ns;
    TimerId id;
    bool operator>(const TimerEntry& o) const noexcept {
      return deadline_ns != o.deadline_ns ? deadline_ns > o.deadline_ns : id > o.id;
    }
  };

  int compute_timeout_ms(int max_wait_ms) const noexcept;
  void dispatch_io(int nfds);
  void run_posted();
  void run_expired_timers();
  void drain_wake_fd() noexcept;
  static int socket_error(int fd) noexcept;

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
};

}  // namespace fastmm::net
