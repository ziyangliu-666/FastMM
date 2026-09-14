#include "io_uring_ring.hpp"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>

namespace fastmm::net::detail {

namespace {

int sys_setup(unsigned entries, io_uring_params* p) noexcept {
  const long r = ::syscall(__NR_io_uring_setup, entries, p);
  return r < 0 ? -errno : static_cast<int>(r);
}

// Typed pointer `off` bytes into a ring mapping.
template <class T>
T* ring_field(void* base, std::uint32_t off) noexcept {
  return reinterpret_cast<T*>(static_cast<char*>(base) + off);
}

void* map_ring(int fd, std::size_t len, std::uint64_t offset) noexcept {
  void* p = ::mmap(nullptr,
                   len,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE,
                   fd,
                   static_cast<off_t>(offset));
  return p == MAP_FAILED ? nullptr : p;
}

}  // namespace

IoUring::~IoUring() {
  unmap();
  if (ring_fd_ >= 0) ::close(ring_fd_);
}

int IoUring::init(unsigned sq_entries, unsigned cq_entries) noexcept {
  if (ring_fd_ >= 0) return -EBUSY;
  // SUBMIT_ALL: one bad SQE (a closed fd) must not stop the rest of the batch.
  // COOP_TASKRUN + TASKRUN_FLAG: no IPI per completion while the loop runs in userspace; the
  // kernel raises IORING_SQ_TASKRUN instead and the loop enters when it sees the flag.
  // All three need 5.19; older kernels retry without them.
  const std::uint32_t base = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;
  const std::uint32_t modern =
      IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
  io_uring_params p{};
  p.flags = base | modern;
  p.cq_entries = cq_entries;
  int fd = sys_setup(sq_entries, &p);
  if (fd == -EINVAL) {
    p = io_uring_params{};
    p.flags = base;
    p.cq_entries = cq_entries;
    fd = sys_setup(sq_entries, &p);
  }
  if (fd < 0) return fd;
  ring_fd_ = fd;
  features_ = p.features;

  auto fail = [this](int err) noexcept {
    unmap();
    ::close(ring_fd_);
    ring_fd_ = -1;
    return err;
  };
  // EXT_ARG (5.11) carries the wait timeout; NODROP (5.5) keeps completions when the CQ is full.
  if ((p.features & IORING_FEAT_EXT_ARG) == 0 || (p.features & IORING_FEAT_NODROP) == 0)
    return fail(-EINVAL);

  sq_map_len_ = p.sq_off.array + p.sq_entries * sizeof(std::uint32_t);
  cq_map_len_ = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
  const bool single_mmap = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
  if (single_mmap) sq_map_len_ = cq_map_len_ = std::max(sq_map_len_, cq_map_len_);
  sq_map_ = map_ring(ring_fd_, sq_map_len_, IORING_OFF_SQ_RING);
  if (sq_map_ == nullptr) return fail(-errno);
  if (single_mmap) {
    cq_map_ = sq_map_;
  } else {
    cq_map_ = map_ring(ring_fd_, cq_map_len_, IORING_OFF_CQ_RING);
    if (cq_map_ == nullptr) return fail(-errno);
  }
  sqes_len_ = p.sq_entries * sizeof(io_uring_sqe);
  void* sqes = map_ring(ring_fd_, sqes_len_, IORING_OFF_SQES);
  if (sqes == nullptr) return fail(-errno);
  sqes_ = static_cast<io_uring_sqe*>(sqes);

  sq_head_ = ring_field<std::atomic<std::uint32_t>>(sq_map_, p.sq_off.head);
  sq_tail_ = ring_field<std::atomic<std::uint32_t>>(sq_map_, p.sq_off.tail);
  sq_flags_ = ring_field<std::atomic<std::uint32_t>>(sq_map_, p.sq_off.flags);
  sq_mask_ = *ring_field<std::uint32_t>(sq_map_, p.sq_off.ring_mask);
  sq_entries_ = *ring_field<std::uint32_t>(sq_map_, p.sq_off.ring_entries);
  // Identity indirection: SQ ring slot i always refers to SQE i.
  auto* array = ring_field<std::uint32_t>(sq_map_, p.sq_off.array);
  for (std::uint32_t i = 0; i < sq_entries_; ++i) array[i] = i;
  sqe_tail_ = sq_tail_->load(std::memory_order_acquire);

  cq_head_ = ring_field<std::atomic<std::uint32_t>>(cq_map_, p.cq_off.head);
  cq_tail_ = ring_field<std::atomic<std::uint32_t>>(cq_map_, p.cq_off.tail);
  cq_mask_ = *ring_field<std::uint32_t>(cq_map_, p.cq_off.ring_mask);
  cqes_ = ring_field<const io_uring_cqe>(cq_map_, p.cq_off.cqes);
  return 0;
}

io_uring_sqe* IoUring::get_sqe() noexcept {
  if (sqe_tail_ - sq_head_->load(std::memory_order_acquire) >= sq_entries_) return nullptr;
  io_uring_sqe* sqe = &sqes_[sqe_tail_ & sq_mask_];
  ++sqe_tail_;
  std::memset(sqe, 0, sizeof(*sqe));
  return sqe;
}

int IoUring::submit(bool get_events) noexcept {
  flush_sq_tail();
  const unsigned pending = sq_pending();
  if (pending == 0 && !get_events) return 0;
  return enter(pending, 0, get_events ? IORING_ENTER_GETEVENTS : 0U, nullptr, 0);
}

int IoUring::submit_and_wait(std::int64_t timeout_ns) noexcept {
  flush_sq_tail();
  __kernel_timespec ts{};
  io_uring_getevents_arg arg{};
  arg.sigmask = 0;
  arg.sigmask_sz = _NSIG / 8;
  if (timeout_ns >= 0) {
    ts.tv_sec = timeout_ns / 1'000'000'000;
    ts.tv_nsec = timeout_ns % 1'000'000'000;
    arg.ts = reinterpret_cast<std::uintptr_t>(&ts);
  }
  return enter(sq_pending(), 1, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
}

int IoUring::enter(unsigned to_submit,
                   unsigned min_complete,
                   unsigned flags,
                   void* arg,
                   std::size_t arg_size) noexcept {
  const long r =
      ::syscall(__NR_io_uring_enter, ring_fd_, to_submit, min_complete, flags, arg, arg_size);
  return r < 0 ? -errno : static_cast<int>(r);
}

void IoUring::unmap() noexcept {
  if (sqes_ != nullptr) ::munmap(sqes_, sqes_len_);
  if (cq_map_ != nullptr && cq_map_ != sq_map_) ::munmap(cq_map_, cq_map_len_);
  if (sq_map_ != nullptr) ::munmap(sq_map_, sq_map_len_);
  sqes_ = nullptr;
  cq_map_ = nullptr;
  sq_map_ = nullptr;
}

}  // namespace fastmm::net::detail
