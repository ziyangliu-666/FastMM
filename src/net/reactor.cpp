#include "fastmm/net/reactor.hpp"

#include "io_uring_ring.hpp"

#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

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

// ---- io_uring ----------------------------------------------------------------------------

constexpr unsigned kUringSqEntries = 512;
constexpr unsigned kUringCqEntries = 4096;

// user_data layout: | op (4 bits) | registration generation (28 bits) | fd (32 bits) |
enum class UringOp : std::uint8_t { Poll = 0, Update = 1, Remove = 2 };
constexpr std::uint32_t kGenMask = (1U << 28) - 1;

constexpr std::uint64_t encode(UringOp op, int fd, std::uint32_t gen) noexcept {
  return (static_cast<std::uint64_t>(op) << 60) |
         (static_cast<std::uint64_t>(gen & kGenMask) << 32) | static_cast<std::uint32_t>(fd);
}
constexpr UringOp op_of(std::uint64_t ud) noexcept {
  return static_cast<UringOp>(ud >> 60);
}
constexpr int fd_of(std::uint64_t ud) noexcept {
  return static_cast<int>(static_cast<std::uint32_t>(ud & 0xffff'ffffULL));
}
constexpr std::uint32_t gen_of(std::uint64_t ud) noexcept {
  return static_cast<std::uint32_t>(ud >> 32) & kGenMask;
}

std::uint32_t to_poll(IoEvent ev) noexcept {
  std::uint32_t m = POLLRDHUP;
  if (has(ev, IoEvent::Read)) m |= POLLIN;
  if (has(ev, IoEvent::Write)) m |= POLLOUT;
  return m;
}

// Functional probe: a multishot poll on an eventfd, updated to POLLOUT, must report the update
// and then a POLLOUT completion flagged IORING_CQE_F_MORE.
bool probe_io_uring() noexcept {
  detail::IoUring uring;
  if (uring.init(8, 16) != 0) return false;
  io_uring& ring = uring.ring();
  const int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (efd < 0) return false;
  io_uring_sqe* add = io_uring_get_sqe(&ring);
  io_uring_sqe* upd = io_uring_get_sqe(&ring);
  if (add == nullptr || upd == nullptr) {
    ::close(efd);
    return false;
  }
  io_uring_prep_poll_multishot(add, efd, POLLIN);
  io_uring_sqe_set_data64(add, 1);
  io_uring_prep_poll_update(
      upd, 1, 0, POLLIN | POLLOUT, IORING_POLL_UPDATE_EVENTS | IORING_POLL_ADD_MULTI);
  io_uring_sqe_set_data64(upd, 2);
  bool updated = false;
  bool multishot = false;
  bool failed = false;
  const std::int64_t deadline = Reactor::now_ns() + 1'000'000'000;
  while (!failed && !(updated && multishot) && Reactor::now_ns() < deadline) {
    const int rc = uring.submit_and_wait(100'000'000);
    if (rc < 0 && rc != -ETIME && rc != -EINTR) {
      failed = true;
      break;
    }
    io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&ring, &cqe) == 0 && cqe != nullptr) {
      if (cqe->user_data == 2) {
        updated = cqe->res == 0;
        failed = failed || cqe->res != 0;
      } else if (cqe->user_data == 1) {
        if (cqe->res < 0) failed = true;
        if (cqe->res > 0 && (static_cast<std::uint32_t>(cqe->res) & POLLOUT) != 0 &&
            (cqe->flags & IORING_CQE_F_MORE) != 0)
          multishot = true;
      }
      io_uring_cqe_seen(&ring, cqe);
    }
  }
  ::close(efd);
  return !failed && updated && multishot;
}

}  // namespace

std::string_view to_string(ReactorBackend backend) noexcept {
  return backend == ReactorBackend::IoUring ? "io_uring" : "epoll";
}

bool parse_reactor_backend(std::string_view text, ReactorBackend& out) noexcept {
  if (text == "epoll") {
    out = ReactorBackend::Epoll;
    return true;
  }
  if (text == "io_uring") {
    out = ReactorBackend::IoUring;
    return true;
  }
  return false;
}

bool Reactor::io_uring_supported() noexcept {
  static const bool supported = probe_io_uring();
  return supported;
}

ReactorBackend Reactor::resolve_backend(ReactorBackend requested) noexcept {
  if (requested == ReactorBackend::IoUring && !io_uring_supported()) return ReactorBackend::Epoll;
  return requested;
}

Reactor::Reactor(ReactorBackend backend) : backend_(backend) {
  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) throw std::runtime_error("eventfd failed");
  handlers_.reserve(256);
  if (backend_ == ReactorBackend::Epoll) {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
      release();
      throw std::runtime_error("epoll_create1 failed");
    }
    events_ = new epoll_event[kMaxEvents];
  } else {
    uring_ = std::make_unique<detail::IoUring>();
    if (const int err = uring_->init(kUringSqEntries, kUringCqEntries); err != 0) {
      release();
      throw std::runtime_error("io_uring_setup failed: errno " + std::to_string(-err));
    }
    fd_state_.reserve(256);
  }
  if (!add(wake_fd_, g_wake_handler, IoEvent::Read)) {
    release();
    throw std::runtime_error("cannot register the reactor wake eventfd");
  }
}

Reactor::~Reactor() {
  release();
}

void Reactor::release() noexcept {
  // Closing the ring cancels every poll request and drops their file references.
  uring_.reset();
  if (wake_fd_ >= 0) ::close(wake_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  wake_fd_ = -1;
  epoll_fd_ = -1;
  delete[] static_cast<epoll_event*>(events_);
  events_ = nullptr;
}

bool Reactor::add(int fd, IoHandler& handler, IoEvent events) noexcept {
  if (fd < 0) return false;
  if (backend_ == ReactorBackend::IoUring) return uring_add(fd, handler, events);
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
  const auto idx = static_cast<std::size_t>(fd);
  if (backend_ == ReactorBackend::IoUring) {
    const std::uint32_t mask = to_poll(events);
    if (mask != fd_state_[idx].mask) {
      if (!uring_queue_update(fd, mask)) return false;
      fd_state_[idx].mask = mask;
    }
    handlers_[idx] = &handler;
    return true;
  }
  epoll_event ev{};
  ev.events = to_epoll(events);
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) != 0) return false;
  handlers_[idx] = &handler;
  return true;
}

bool Reactor::remove(int fd) noexcept {
  if (!is_registered(fd)) return false;
  const auto idx = static_cast<std::size_t>(fd);
  handlers_[idx] = nullptr;
  if (backend_ == ReactorBackend::IoUring) {
    if (!uring_queue_remove(fd, fd_state_[idx].gen)) return false;
    // Submit now: the poll request pins the file, and callers close the fd right after this.
    static_cast<void>(io_uring_submit(&uring_->ring()));
    return true;
  }
  // The fd may already be closed by the caller (closing removes it from epoll implicitly).
  return ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0 || errno == EBADF ||
         errno == ENOENT;
}

bool Reactor::is_registered(int fd) const noexcept {
  return fd >= 0 && static_cast<std::size_t>(fd) < handlers_.size() &&
         handlers_[static_cast<std::size_t>(fd)] != nullptr;
}

TimerId Reactor::add_timer(std::int64_t deadline_ns, TimerCallback cb) {
  std::uint32_t slot = 0;
  if (!free_timer_slots_.empty()) {
    slot = free_timer_slots_.back();
    free_timer_slots_.pop_back();
  } else {
    if (timer_slots_.size() > kTimerSlotMask) throw std::length_error("too many reactor timers");
    slot = static_cast<std::uint32_t>(timer_slots_.size());
    timer_slots_.emplace_back();
    // release_timer_slot() then pushes without allocating.
    if (free_timer_slots_.capacity() < timer_slots_.capacity())
      free_timer_slots_.reserve(timer_slots_.capacity());
  }
  const TimerId id = (next_timer_seq_++ << kTimerSlotBits) | slot;
  TimerSlot& s = timer_slots_[slot];
  s.id = id;
  s.cb = std::move(cb);
  ++active_timers_;
  // Cancelled timers leave their heap entries until the deadline passes. Before the heap would
  // grow, drop them when they are at least half of it, so cancel-and-re-arm does not allocate.
  if (heap_.size() == heap_.capacity() && heap_.size() >= 2 * active_timers_) {
    std::erase_if(heap_, [this](const TimerEntry& e) { return !timer_live(e.id); });
    std::make_heap(heap_.begin(), heap_.end(), std::greater<>{});
  }
  heap_.push_back(TimerEntry{deadline_ns, id});
  std::push_heap(heap_.begin(), heap_.end(), std::greater<>{});
  return id;
}

bool Reactor::cancel_timer(TimerId id) noexcept {
  const auto slot = id & kTimerSlotMask;
  if (id == kInvalidTimer || slot >= timer_slots_.size() || timer_slots_[slot].id != id)
    return false;
  // Lazy cancellation: the heap entry stays and is skipped when it surfaces.
  release_timer_slot(slot);
  return true;
}

void Reactor::release_timer_slot(std::size_t slot) noexcept {
  TimerSlot& s = timer_slots_[slot];
  s.id = kInvalidTimer;
  s.cb.reset();
  --active_timers_;
  free_timer_slots_.push_back(static_cast<std::uint32_t>(slot));
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

std::int64_t Reactor::compute_timeout_ns(int max_wait_ms) const noexcept {
  if (busy_poll_) return 0;
  std::int64_t timeout = max_wait_ms < 0 ? -1 : std::int64_t{max_wait_ms} * 1'000'000;
  if (!heap_.empty()) {
    const std::int64_t delta = std::max<std::int64_t>(heap_.front().deadline_ns - now_ns(), 0);
    timeout = timeout < 0 ? delta : std::min(timeout, delta);
  }
  return timeout;
}

int Reactor::run_once(int max_wait_ms) {
  if (backend_ == ReactorBackend::IoUring) return uring_run_once(max_wait_ms);
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
    deliver(fd,
            h,
            (ev & EPOLLERR) != 0,
            (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0,
            (ev & EPOLLOUT) != 0);
  }
}

void Reactor::deliver(int fd, IoHandler* h, bool error, bool readable, bool writable) {
  if (error) {
    h->on_error(socket_error(fd));
    return;
  }
  if (readable) {
    h->on_readable();
    if (!is_registered(fd) || handlers_[static_cast<std::size_t>(fd)] != h) return;
  }
  if (writable) h->on_writable();
}

// ---- io_uring backend ------------------------------------------------------------------------

int Reactor::uring_run_once(int max_wait_ms) {
  io_uring& ring = uring_->ring();
  const std::int64_t timeout_ns = compute_timeout_ns(max_wait_ms);
  int rc = 0;
  if (timeout_ns == 0 || io_uring_cq_ready(&ring) != 0) {
    // Nothing to wait for: io_uring_submit enters the kernel only for queued SQEs, or when the
    // kernel flags deferred task work (COOP_TASKRUN) or overflowed completions.
    rc = io_uring_submit(&ring);
  } else {
    rc = uring_->submit_and_wait(timeout_ns);
  }
  if (rc < 0 && rc != -EINTR && rc != -ETIME && rc != -EBUSY && rc != -EAGAIN) return -1;
  const int n = uring_dispatch();
  if (n == 0 && busy_poll_) FASTMM_CPU_PAUSE();
  run_posted();
  run_expired_timers();
  return n;
}

int Reactor::uring_dispatch() {
  io_uring& ring = uring_->ring();
  // Only what is ready now: completions produced by the callbacks wait for the next run_once.
  const unsigned ready = io_uring_cq_ready(&ring);
  int n = 0;
  for (unsigned i = 0; i < ready; ++i) {
    io_uring_cqe* cqe = nullptr;
    if (io_uring_peek_cqe(&ring, &cqe) != 0 || cqe == nullptr) break;
    const std::uint64_t user_data = cqe->user_data;
    const std::int32_t res = cqe->res;
    const std::uint32_t flags = cqe->flags;
    io_uring_cqe_seen(&ring, cqe);  // consumed before the callback runs
    ++n;
    uring_complete(user_data, res, flags);
  }
  return n;
}

void Reactor::uring_complete(std::uint64_t user_data, std::int32_t res, std::uint32_t flags) {
  const int fd = fd_of(user_data);
  const std::uint32_t gen = gen_of(user_data);
  const auto idx = static_cast<std::size_t>(fd);
  // Looked up per completion: an earlier handler in this batch may have removed or replaced it,
  // and a completion may belong to an earlier registration of the same fd number.
  const bool current =
      idx < fd_state_.size() && fd_state_[idx].gen == gen && handlers_[idx] != nullptr;
  switch (op_of(user_data)) {
    case UringOp::Poll:
      break;
    case UringOp::Update:
      // -EALREADY: the poll was completing; retry. -ENOENT: it had terminated, and re-arming
      // on its final completion already uses the new mask.
      if (res == -EALREADY && current)
        static_cast<void>(uring_queue_update(fd, fd_state_[idx].mask));
      return;
    case UringOp::Remove:
      if (res == -EALREADY) static_cast<void>(uring_queue_remove(fd, gen));
      return;
    default:
      return;
  }
  if (!current) return;
  IoHandler* h = handlers_[idx];
  const bool more = (flags & IORING_CQE_F_MORE) != 0;
  if (res < 0) {
    if (more) return;
    if (res == -ECANCELED) {
      if (!uring_arm(fd)) h->on_error(ENOBUFS);
    } else {
      h->on_error(-res);  // the kernel refused to poll this descriptor; not re-armed
    }
    return;
  }
  const auto ev = static_cast<std::uint32_t>(res);
  if (fd == wake_fd_) {
    drain_wake_fd();
  } else {
    deliver(fd,
            h,
            (ev & POLLERR) != 0,
            (ev & (POLLIN | POLLRDHUP | POLLHUP)) != 0,
            (ev & POLLOUT) != 0);
  }
  // A multishot poll ends without IORING_CQE_F_MORE (e.g. the CQ ring overflowed): re-arm it if
  // the registration survived the callbacks.
  if (!more && fd_state_[idx].gen == gen && handlers_[idx] != nullptr && !uring_arm(fd))
    handlers_[idx]->on_error(ENOBUFS);
}

bool Reactor::uring_add(int fd, IoHandler& handler, IoEvent events) noexcept {
  struct stat st {};
  if (::fstat(fd, &st) != 0) return false;
  // epoll refuses regular files and directories (EPERM); a poll on them is always ready.
  if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return false;
  const auto idx = static_cast<std::size_t>(fd);
  if (idx >= handlers_.size()) handlers_.resize(idx + 1, nullptr);
  if (idx >= fd_state_.size()) fd_state_.resize(idx + 1);
  FdState& s = fd_state_[idx];
  const auto dev = static_cast<std::uint64_t>(st.st_dev);
  const auto ino = static_cast<std::uint64_t>(st.st_ino);
  if (handlers_[idx] != nullptr) {
    if (s.dev == dev && s.ino == ino) return false;  // already registered (epoll: EEXIST)
    // The registered file was closed without remove() and its number reused: cancel the stale
    // poll so it releases the old file.
    if (!uring_queue_remove(fd, s.gen)) return false;
    handlers_[idx] = nullptr;
  }
  const FdState previous = s;
  s.gen = (s.gen + 1) & kGenMask;
  s.mask = to_poll(events);
  s.dev = dev;
  s.ino = ino;
  if (!uring_arm(fd)) {
    s = previous;
    return false;
  }
  handlers_[idx] = &handler;
  return true;
}

io_uring_sqe* Reactor::uring_sqe() noexcept {
  io_uring& ring = uring_->ring();
  io_uring_sqe* sqe = io_uring_get_sqe(&ring);
  if (sqe == nullptr) {
    static_cast<void>(io_uring_submit(&ring));
    sqe = io_uring_get_sqe(&ring);
  }
  return sqe;
}

bool Reactor::uring_arm(int fd) noexcept {
  io_uring_sqe* sqe = uring_sqe();
  if (sqe == nullptr) return false;
  const FdState& s = fd_state_[static_cast<std::size_t>(fd)];
  io_uring_prep_poll_multishot(sqe, fd, s.mask);
  io_uring_sqe_set_data64(sqe, encode(UringOp::Poll, fd, s.gen));
  return true;
}

bool Reactor::uring_queue_update(int fd, std::uint32_t mask) noexcept {
  io_uring_sqe* sqe = uring_sqe();
  if (sqe == nullptr) return false;
  const std::uint32_t gen = fd_state_[static_cast<std::size_t>(fd)].gen;
  // ADD_MULTI keeps the updated request multishot; its user_data is left as it was.
  io_uring_prep_poll_update(sqe,
                            encode(UringOp::Poll, fd, gen),
                            0,
                            mask,
                            IORING_POLL_UPDATE_EVENTS | IORING_POLL_ADD_MULTI);
  io_uring_sqe_set_data64(sqe, encode(UringOp::Update, fd, gen));
  return true;
}

bool Reactor::uring_queue_remove(int fd, std::uint32_t gen) noexcept {
  io_uring_sqe* sqe = uring_sqe();
  if (sqe == nullptr) return false;
  io_uring_prep_poll_remove(sqe, encode(UringOp::Poll, fd, gen));
  io_uring_sqe_set_data64(sqe, encode(UringOp::Remove, fd, gen));
  return true;
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
    if (!timer_live(entry.id)) continue;  // cancelled (its slot may be in use again)
    const auto slot = entry.id & kTimerSlotMask;
    TimerCallback cb = std::move(timer_slots_[slot].cb);
    release_timer_slot(slot);  // before the call, so the callback may re-arm itself
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
