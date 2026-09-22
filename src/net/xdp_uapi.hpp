#pragma once
// Kernel ABI for the AF_XDP backend: <linux/if_xdp.h> plus the parts older UAPI headers lack, and
// the few bpf(2) attribute layouts the backend uses. The manylinux_2_28 image that builds the
// fastmm-live wheels ships RHEL 8 kernel headers, where struct members added after Linux 4.18
// (ring flags, extended statistics) and the enums of <linux/bpf.h> (BPF_LINK_CREATE, BPF_XDP) may
// be missing; enums and struct members cannot be backfilled with #ifndef, so the layouts below
// are declared here with the kernel's values, which never change. Where the build headers are
// new enough, static_asserts pin the layouts to theirs. Private to fastmm_net.
#include <linux/if_xdp.h>
#include <linux/types.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>

// clang-format off
// Layout checks against the build machine's headers, before any backfill below defines the
// macros they key on.
#ifdef XDP_USE_NEED_WAKEUP
#define FASTMM_XDP_HEADERS_HAVE_RING_FLAGS 1
#endif
#ifndef AF_XDP
#define AF_XDP 44  // NOLINT(readability-identifier-naming): Linux 4.18
#endif
#ifndef SOL_XDP
#define SOL_XDP 283  // NOLINT(readability-identifier-naming): Linux 4.18
#endif
#ifndef XDP_USE_NEED_WAKEUP
#define XDP_USE_NEED_WAKEUP (1 << 3)  // NOLINT(readability-identifier-naming): Linux 5.4
#endif
#ifndef XDP_RING_NEED_WAKEUP
#define XDP_RING_NEED_WAKEUP (1 << 0)  // NOLINT(readability-identifier-naming): Linux 5.4
#endif
#ifndef XDP_OPTIONS
#define XDP_OPTIONS 8  // NOLINT(readability-identifier-naming): Linux 5.3
#endif
#ifndef XDP_OPTIONS_ZEROCOPY
#define XDP_OPTIONS_ZEROCOPY (1 << 0)  // NOLINT(readability-identifier-naming): Linux 5.3
#endif
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46  // NOLINT(readability-identifier-naming): Linux 3.11
#endif
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69  // NOLINT(readability-identifier-naming): Linux 5.11
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70  // NOLINT(readability-identifier-naming): Linux 5.11
#endif
// clang-format on

namespace fastmm::net::detail {

// ---- AF_XDP (include/uapi/linux/if_xdp.h) ------------------------------------------------------

// struct xdp_ring_offset with `flags` (Linux 5.4).
struct XdpRingOffset {
  std::uint64_t producer;
  std::uint64_t consumer;
  std::uint64_t desc;
  std::uint64_t flags;
};
struct XdpMmapOffsets {
  XdpRingOffset rx;
  XdpRingOffset tx;
  XdpRingOffset fr;  // fill
  XdpRingOffset cr;  // completion
};
// struct xdp_umem_reg with `flags` (5.4) and `tx_metadata_len` (6.8); 0 in both is the 5.4 ABI.
struct XdpUmemReg {
  std::uint64_t addr;
  std::uint64_t len;
  std::uint32_t chunk_size;
  std::uint32_t headroom;
  std::uint32_t flags;
  std::uint32_t tx_metadata_len;
};
// struct xdp_statistics with the ring counters (Linux 5.9).
struct XdpStatistics {
  std::uint64_t rx_dropped;
  std::uint64_t rx_invalid_descs;
  std::uint64_t tx_invalid_descs;
  std::uint64_t rx_ring_full;
  std::uint64_t rx_fill_ring_empty_descs;
  std::uint64_t tx_ring_empty_descs;
};
struct XdpDesc {
  std::uint64_t addr;
  std::uint32_t len;
  std::uint32_t options;
};
static_assert(sizeof(XdpMmapOffsets) == 128);
static_assert(sizeof(XdpUmemReg) == 32);
static_assert(sizeof(XdpStatistics) == 48);
static_assert(sizeof(XdpDesc) == 16);
static_assert(sizeof(sockaddr_xdp) == 16);
#ifdef FASTMM_XDP_HEADERS_HAVE_RING_FLAGS
static_assert(sizeof(xdp_mmap_offsets) == sizeof(XdpMmapOffsets));
static_assert(offsetof(xdp_ring_offset, flags) == offsetof(XdpRingOffset, flags));
static_assert(sizeof(xdp_desc) == sizeof(XdpDesc));
static_assert(offsetof(xdp_umem_reg, headroom) == offsetof(XdpUmemReg, headroom));
#endif

// UMEM chunks start with XDP_PACKET_HEADROOM (256) bytes the kernel reserves in copy mode.
inline constexpr std::uint32_t kXdpPacketHeadroom = 256;

// ---- bpf(2) (include/uapi/linux/bpf.h) ---------------------------------------------------------

// enum bpf_cmd
inline constexpr int kBpfMapCreate = 0;
inline constexpr int kBpfMapLookupElem = 1;
inline constexpr int kBpfMapUpdateElem = 2;
inline constexpr int kBpfProgLoad = 5;
inline constexpr int kBpfProgTestRun = 10;
inline constexpr int kBpfLinkCreate = 28;  // Linux 5.7
// enum bpf_map_type
inline constexpr std::uint32_t kBpfMapTypeHash = 1;
inline constexpr std::uint32_t kBpfMapTypePercpuArray = 6;
inline constexpr std::uint32_t kBpfMapTypeXskmap = 17;  // Linux 4.18
// enum bpf_prog_type
inline constexpr std::uint32_t kBpfProgTypeXdp = 6;
// enum bpf_attach_type
inline constexpr std::uint32_t kBpfXdp = 37;  // Linux 5.9 (XDP through BPF_LINK_CREATE)
// BPF_ANY for BPF_MAP_UPDATE_ELEM
inline constexpr std::uint64_t kBpfAny = 0;
// XDP_FLAGS_* (include/uapi/linux/if_link.h)
inline constexpr std::uint32_t kXdpFlagsSkbMode = 1U << 1U;
inline constexpr std::uint32_t kXdpFlagsDrvMode = 1U << 2U;

// Prefixes of union bpf_attr, one per command. bpf(2) takes the size actually passed and treats
// the rest of the kernel's union as zero.
struct BpfMapCreateAttr {
  std::uint32_t map_type;
  std::uint32_t key_size;
  std::uint32_t value_size;
  std::uint32_t max_entries;
  std::uint32_t map_flags;
  std::uint32_t inner_map_fd;
  std::uint32_t numa_node;
  char map_name[16];
  std::uint32_t map_ifindex;
};
static_assert(offsetof(BpfMapCreateAttr, map_name) == 28);
static_assert(sizeof(BpfMapCreateAttr) == 48);

struct BpfMapElemAttr {
  std::uint32_t map_fd;
  std::uint32_t pad0;
  std::uint64_t key;
  std::uint64_t value;
  std::uint64_t flags;
};
static_assert(offsetof(BpfMapElemAttr, key) == 8);
static_assert(sizeof(BpfMapElemAttr) == 32);

struct BpfProgLoadAttr {
  std::uint32_t prog_type;
  std::uint32_t insn_cnt;
  std::uint64_t insns;
  std::uint64_t license;
  std::uint32_t log_level;
  std::uint32_t log_size;
  std::uint64_t log_buf;
  std::uint32_t kern_version;
  std::uint32_t prog_flags;
  char prog_name[16];
  std::uint32_t prog_ifindex;
  std::uint32_t expected_attach_type;
};
static_assert(offsetof(BpfProgLoadAttr, log_buf) == 32);
static_assert(offsetof(BpfProgLoadAttr, prog_name) == 48);
static_assert(offsetof(BpfProgLoadAttr, expected_attach_type) == 68);
static_assert(sizeof(BpfProgLoadAttr) == 72);

struct BpfTestRunAttr {
  std::uint32_t prog_fd;
  std::uint32_t retval;
  std::uint32_t data_size_in;
  std::uint32_t data_size_out;
  std::uint64_t data_in;
  std::uint64_t data_out;
  std::uint32_t repeat;
  std::uint32_t duration;
};
static_assert(offsetof(BpfTestRunAttr, data_in) == 16);
static_assert(sizeof(BpfTestRunAttr) == 40);

struct BpfLinkCreateAttr {
  std::uint32_t prog_fd;
  std::uint32_t target_ifindex;
  std::uint32_t attach_type;
  std::uint32_t flags;
};
static_assert(sizeof(BpfLinkCreateAttr) == 16);

// ---- capabilities (include/uapi/linux/capability.h) ---------------------------------------------

inline constexpr unsigned kCapNetAdmin = 12;
inline constexpr unsigned kCapNetRaw = 13;
inline constexpr unsigned kCapIpcLock = 14;
inline constexpr unsigned kCapSysAdmin = 21;
inline constexpr unsigned kCapBpf = 39;  // Linux 5.8
inline constexpr std::uint32_t kCapabilityVersion3 = 0x20080522;

struct CapHeader {
  std::uint32_t version;
  int pid;
};
struct CapData {
  std::uint32_t effective;
  std::uint32_t permitted;
  std::uint32_t inheritable;
};

}  // namespace fastmm::net::detail
