#pragma once
// Owner of the liburing ring behind the Reactor's io_uring backend: setup with the flags the
// reactor wants (and a fallback for kernels older than 5.19), and the wait with a nanosecond
// timeout. SQE allocation, submission and completion reaping use liburing directly on ring().
// Private to fastmm_net.
//
// Single-threaded: every method must be called from the thread that owns the reactor.
#include <liburing.h>

#include <cstdint>

namespace fastmm::net::detail {

class IoUring {
 public:
  IoUring() = default;
  ~IoUring();
  IoUring(const IoUring&) = delete;
  IoUring& operator=(const IoUring&) = delete;

  // Creates the ring. Returns 0 or a negative errno (ENOSYS, EPERM when io_uring is disabled,
  // ENOMEM, EINVAL on kernels without the features the reactor needs, ...).
  int init(unsigned sq_entries, unsigned cq_entries) noexcept;

  [[nodiscard]] io_uring& ring() noexcept { return ring_; }

  // Submits pending SQEs and waits for at least one completion or `timeout_ns` (< 0: forever).
  // Returns >= 0, or -errno (-ETIME on timeout, -EINTR on a signal).
  int submit_and_wait(std::int64_t timeout_ns) noexcept;

 private:
  io_uring ring_{};
  bool live_ = false;
};

}  // namespace fastmm::net::detail
