#pragma once
// Minimal io_uring ring over the raw syscalls (no liburing): setup, the mmap'd SQ/CQ rings and
// SQE array, SQE allocation, submission and waiting with IORING_ENTER_EXT_ARG. Private to
// fastmm_net; the Reactor's io_uring backend is its only user.
//
// Single-threaded: every method must be called from the thread that owns the reactor.
#include <linux/io_uring.h>

#include <atomic>
#include <cstddef>
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

  [[nodiscard]] int fd() const noexcept { return ring_fd_; }
  [[nodiscard]] std::uint32_t features() const noexcept { return features_; }

  // A zeroed SQE, or nullptr when the SQ ring is full (submit() and retry).
  io_uring_sqe* get_sqe() noexcept;

  // SQEs queued but not yet consumed by the kernel.
  [[nodiscard]] unsigned sq_pending() const noexcept {
    return sqe_tail_ - sq_head_->load(std::memory_order_acquire);
  }
  // The kernel asks for an io_uring_enter: deferred task work (COOP_TASKRUN) or overflowed CQEs.
  [[nodiscard]] bool needs_enter() const noexcept {
    return (sq_flags_->load(std::memory_order_acquire) &
            (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) != 0;
  }

  // Submits pending SQEs without waiting. With `get_events`, also runs deferred task work and
  // flushes overflowed completions into the CQ ring. Returns the submitted count or -errno.
  int submit(bool get_events = false) noexcept;
  // Submits pending SQEs and waits for at least one completion or `timeout_ns` (< 0: forever).
  // Returns >= 0, or -errno (-ETIME on timeout, -EINTR on a signal).
  int submit_and_wait(std::int64_t timeout_ns) noexcept;

  // Completion queue access: entries in [cq_head(), cq_tail()) are ready.
  [[nodiscard]] unsigned cq_head() const noexcept {
    return cq_head_->load(std::memory_order_acquire);
  }
  [[nodiscard]] unsigned cq_tail() const noexcept {
    return cq_tail_->load(std::memory_order_acquire);
  }
  [[nodiscard]] const io_uring_cqe& cqe_at(unsigned index) const noexcept {
    return cqes_[index & cq_mask_];
  }
  void set_cq_head(unsigned head) noexcept { cq_head_->store(head, std::memory_order_release); }

 private:
  int enter(unsigned to_submit,
            unsigned min_complete,
            unsigned flags,
            void* arg,
            std::size_t arg_size) noexcept;
  void flush_sq_tail() noexcept { sq_tail_->store(sqe_tail_, std::memory_order_release); }
  void unmap() noexcept;

  int ring_fd_ = -1;
  std::uint32_t features_ = 0;

  void* sq_map_ = nullptr;
  std::size_t sq_map_len_ = 0;
  void* cq_map_ = nullptr;  // == sq_map_ with IORING_FEAT_SINGLE_MMAP
  std::size_t cq_map_len_ = 0;
  io_uring_sqe* sqes_ = nullptr;
  std::size_t sqes_len_ = 0;

  // Ring memory shared with the kernel; the kernel updates sq head, cq tail and the flags.
  std::atomic<std::uint32_t>* sq_head_ = nullptr;
  std::atomic<std::uint32_t>* sq_tail_ = nullptr;
  std::atomic<std::uint32_t>* sq_flags_ = nullptr;
  std::uint32_t sq_mask_ = 0;
  std::uint32_t sq_entries_ = 0;
  std::uint32_t sqe_tail_ = 0;  // local tail, published to sq_tail_ on submit

  std::atomic<std::uint32_t>* cq_head_ = nullptr;
  std::atomic<std::uint32_t>* cq_tail_ = nullptr;
  std::uint32_t cq_mask_ = 0;
  const io_uring_cqe* cqes_ = nullptr;
};

}  // namespace fastmm::net::detail
