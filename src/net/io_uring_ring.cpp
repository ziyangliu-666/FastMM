#include "io_uring_ring.hpp"

#include <cerrno>

namespace fastmm::net::detail {

IoUring::~IoUring() {
  if (live_) io_uring_queue_exit(&ring_);
}

int IoUring::init(unsigned sq_entries, unsigned cq_entries) noexcept {
  if (live_) return -EBUSY;
  // SUBMIT_ALL: one bad SQE (a closed fd) must not stop the rest of the batch.
  // COOP_TASKRUN + TASKRUN_FLAG: no IPI per completion while the loop runs in userspace; the
  // kernel raises IORING_SQ_TASKRUN instead and liburing enters when it sees the flag.
  // All three need 5.19; older kernels retry without them.
  const unsigned base = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;
  const unsigned modern =
      IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
  io_uring_params p{};
  p.flags = base | modern;
  p.cq_entries = cq_entries;
  int rc = io_uring_queue_init_params(sq_entries, &ring_, &p);
  if (rc == -EINVAL) {
    p = io_uring_params{};
    p.flags = base;
    p.cq_entries = cq_entries;
    rc = io_uring_queue_init_params(sq_entries, &ring_, &p);
  }
  if (rc < 0) return rc;
  live_ = true;
  // EXT_ARG (5.11) carries the wait timeout; NODROP (5.5) keeps completions when the CQ is full.
  if ((p.features & IORING_FEAT_EXT_ARG) == 0 || (p.features & IORING_FEAT_NODROP) == 0) {
    io_uring_queue_exit(&ring_);
    live_ = false;
    return -EINVAL;
  }
  return 0;
}

int IoUring::submit_and_wait(std::int64_t timeout_ns) noexcept {
  io_uring_cqe* cqe = nullptr;
  if (timeout_ns < 0) return io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, nullptr, nullptr);
  __kernel_timespec ts{};
  ts.tv_sec = timeout_ns / 1'000'000'000;
  ts.tv_nsec = timeout_ns % 1'000'000'000;
  return io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &ts, nullptr);
}

}  // namespace fastmm::net::detail
