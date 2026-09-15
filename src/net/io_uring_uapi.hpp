#pragma once
// <linux/io_uring.h> plus the parts of the kernel ABI that older UAPI headers lack. The
// manylinux_2_28 image that builds the fastmm-live wheels ships RHEL 8 kernel headers (io_uring as
// of Linux 5.7): no EXT_ARG, COOP_TASKRUN, multishot poll or 32-bit poll events. The values below
// are the kernel's (include/uapi/linux/io_uring.h) and never change; Reactor::io_uring_supported()
// still decides at run time whether the running kernel has them. Private to fastmm_net.
#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <linux/types.h>

#include <cstdint>

// clang-format off
#ifndef IORING_SETUP_SUBMIT_ALL
#define IORING_SETUP_SUBMIT_ALL (1U << 7)  // NOLINT(readability-identifier-naming): Linux 5.18
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)  // NOLINT(readability-identifier-naming): Linux 5.19
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)  // NOLINT(readability-identifier-naming): Linux 5.19
#endif
#ifndef IORING_SQ_CQ_OVERFLOW
#define IORING_SQ_CQ_OVERFLOW (1U << 1)  // NOLINT(readability-identifier-naming): Linux 5.8
#endif
#ifndef IORING_SQ_TASKRUN
#define IORING_SQ_TASKRUN (1U << 2)  // NOLINT(readability-identifier-naming): Linux 5.19
#endif
#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE (1U << 1)  // NOLINT(readability-identifier-naming): Linux 5.13
#endif
#ifndef IORING_POLL_ADD_MULTI
#define IORING_POLL_ADD_MULTI (1U << 0)  // NOLINT(readability-identifier-naming): Linux 5.13
#endif
#ifndef IORING_POLL_UPDATE_EVENTS
#define IORING_POLL_UPDATE_EVENTS (1U << 1)  // NOLINT(readability-identifier-naming): Linux 5.13
#endif
// IORING_ENTER_EXT_ARG and struct io_uring_getevents_arg arrived together (Linux 5.11).
#ifndef IORING_FEAT_EXT_ARG
#define IORING_FEAT_EXT_ARG (1U << 8)  // NOLINT(readability-identifier-naming): Linux 5.11
#define IORING_ENTER_EXT_ARG (1U << 3)  // NOLINT(readability-identifier-naming): Linux 5.11
struct io_uring_getevents_arg {  // NOLINT(readability-identifier-naming)
  __u64 sigmask;
  __u32 sigmask_sz;
  __u32 pad;
  __u64 ts;
};
#endif
// clang-format on

namespace fastmm::net::detail {

// sqe.poll32_events (Linux 5.9) is the __u32 member of the op-flags union, where older headers
// declare only the 16-bit poll_events; fsync_flags is the same 32 bits in every header version.
inline void set_poll32_events(io_uring_sqe& sqe, std::uint32_t events) noexcept {
  sqe.fsync_flags = events;
}

}  // namespace fastmm::net::detail
