#include "fastmm/net/reactor.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define FASTMM_CPU_PAUSE() _mm_pause()
#else
#define FASTMM_CPU_PAUSE() \
  do {                     \
  } while (0)
#endif

namespace fastmm::net {

namespace {

// Wake-fd handler is a singleton per reactor: draining happens inline in run_once, so the
// handler only needs to exist to have a slot in the fd table.
class WakeHandler final : public IoHandler {
 public:
  void on_readable() override {}
  void on_writable() override {}
  void on_error(int) override {}
};
WakeHandler g_wake_handler;

std::uint32_t to_epoll(IoEvent ev) noexcept {
  std::uint32_t e = EPOLLET | EPOLLRDHUP;
  if (has(ev, IoEvent::Read)) e |= EPOLLIN;
  if (has(ev, IoEvent::Write)) e |= EPOLLOUT;
  return e;
}

}  // namespace

Reactor::Reactor() {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) throw std::runtime_error("epoll_create1 failed");
  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    ::close(epoll_fd_);
    throw std::runtime_error("eventfd failed");
  }
  events_ = new epoll_event[kMaxEvents];
  handlers_.reserve(256);
  if (!add(wake_fd_, g_wake_handler, IoEvent::Read)) {
    ::close(wake_fd_);
    ::close(epoll_fd_);
    delete[] static_cast<epoll_event*>(events_);
    throw std::runtime_error("epoll_ctl(wake_fd) failed");
  }
}

Reactor::~Reactor() {
  ::close(wake_fd_);
  ::close(epoll_fd_);
  delete[] static_cast<epoll_event*>(events_);
}

bool Reactor::add(int fd, IoHandler& handler, IoEvent events) noexcept {
  if (fd < 0) return false;
  epoll_event ev{};
  ev.events = to_epoll(events);
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
  const auto idx = static_cast<std::size_t>(fd);
  if (idx >= handlers_.size()) handlers_.resize(idx + 1, nullptr);
  handlers_[idx] = &handler;
  return true;
}

bool Reactor::modify(int fd, IoHandler& handler, IoEvent events) noexcept {
  if (!is_registered(fd)) return false;
  epoll_event ev{};
  ev.events = to_epoll(events);
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) != 0) return false;
  handlers_[static_cast<std::size_t>(fd)] = &handler;
  return true;
}

bool Reactor::remove(int fd) noexcept {
  if (!is_registered(fd)) return false;
  handlers_[static_cast<std::size_t>(fd)] = nullptr;
  // The fd may already be closed by the caller (closing removes it from epoll implicitly).
  return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0 || errno == EBADF ||
         errno == ENOENT;
}

bool Reactor::is_registered(int fd) const noexcept {
  return fd >= 0 && static_cast<std::size_t>(fd) < handlers_.size() &&
         handlers_[static_cast<std::size_t>(fd)] != nullptr;
}

TimerId Reactor::add_timer(std::int64_t deadline_ns, TimerCallback cb) {
  const TimerId id = next_timer_id_++;
  timers_.emplace(id, std::move(cb));
  heap_.push_back(TimerEntry{deadline_ns, id});
  std::push_heap(heap_.begin(), heap_.end(), std::greater<>{});
  return id;
}

bool Reactor::cancel_timer(TimerId id) noexcept {
  // Lazy cancellation: the heap entry stays and is skipped when it surfaces.
  return timers_.erase(id) > 0;
}

void Reactor::post(Task task) {
  {
    std::lock_guard lock(post_mutex_);
    posted_.push_back(std::move(task));
  }
  wake();
}

void Reactor::wake() noexcept {
  const std::uint64_t one = 1;
  // EAGAIN means the counter is already saturated, which still wakes the loop.
  [[maybe_unused]] const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
}

std::int64_t Reactor::now_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL +
         static_cast<std::int64_t>(ts.tv_nsec);
}

int Reactor::compute_timeout_ms(int max_wait_ms) const noexcept {
  if (busy_poll_) return 0;
  int timeout = max_wait_ms;  // may be -1 (infinite)
  // Lazily-cancelled entries may sit at the top of the heap; they only make us wake early.
  if (!heap_.empty()) {
    const std::int64_t delta = heap_.front().deadline_ns - now_ns();
    // Round up so we never wake before the deadline and spin.
    const std::int64_t ms = delta <= 0 ? 0 : (delta + 999'999) / 1'000'000;
    const int timer_ms = ms > 1'000'000 ? 1'000'000 : static_cast<int>(ms);
    timeout = timeout < 0 ? timer_ms : std::min(timeout, timer_ms);
  }
  return timeout;
}

int Reactor::run_once(int max_wait_ms) {
  auto* events = static_cast<epoll_event*>(events_);
  const int timeout = compute_timeout_ms(max_wait_ms);
  int nfds = ::epoll_wait(epoll_fd_, events, kMaxEvents, timeout);
  if (nfds < 0) {
    if (errno != EINTR) return -1;
    nfds = 0;
  }
  if (nfds == 0 && busy_poll_) FASTMM_CPU_PAUSE();
  dispatch_io(nfds);
  run_posted();
  run_expired_timers();
  return nfds;
}

void Reactor::run(const std::atomic<bool>& stop) {
  while (!stop.load(std::memory_order_relaxed)) {
    if (run_once(100) < 0) break;
  }
}

void Reactor::dispatch_io(int nfds) {
  auto* events = static_cast<epoll_event*>(events_);
  for (int i = 0; i < nfds; ++i) {
    const int fd = events[i].data.fd;
    const std::uint32_t ev = events[i].events;
    if (fd == wake_fd_) {
      drain_wake_fd();
      continue;
    }
    // Look the handler up per event: an earlier handler in this batch may have removed it.
    IoHandler* h = is_registered(fd) ? handlers_[static_cast<std::size_t>(fd)] : nullptr;
    if (h == nullptr) continue;
    if ((ev & EPOLLERR) != 0) {
      h->on_error(socket_error(fd));
      continue;
    }
    if ((ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
      h->on_readable();
      if (!is_registered(fd) || handlers_[static_cast<std::size_t>(fd)] != h) continue;
    }
    if ((ev & EPOLLOUT) != 0) h->on_writable();
  }
}

void Reactor::run_posted() {
  {
    std::lock_guard lock(post_mutex_);
    if (posted_.empty()) return;
    running_.swap(posted_);
  }
  for (auto& task : running_) task();
  running_.clear();
}

void Reactor::run_expired_timers() {
  if (heap_.empty()) return;
  const std::int64_t now = now_ns();
  while (!heap_.empty() && heap_.front().deadline_ns <= now) {
    std::pop_heap(heap_.begin(), heap_.end(), std::greater<>{});
    const TimerEntry entry = heap_.back();
    heap_.pop_back();
    auto it = timers_.find(entry.id);
    if (it == timers_.end()) continue;  // cancelled
    TimerCallback cb = std::move(it->second);
    timers_.erase(it);  // erased before the call so the callback may re-arm itself
    cb();
  }
}

void Reactor::drain_wake_fd() noexcept {
  std::uint64_t v = 0;
  [[maybe_unused]] const ssize_t n = ::read(wake_fd_, &v, sizeof(v));
}

int Reactor::socket_error(int fd) noexcept {
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return 0;
  return err;
}

}  // namespace fastmm::net
