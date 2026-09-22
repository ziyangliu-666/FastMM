#include "fastmm/net/xdp_datagram_source.hpp"

#include "xdp_uapi.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace fastmm::net {

namespace {

using detail::BpfLinkCreateAttr;
using detail::BpfMapCreateAttr;
using detail::BpfMapElemAttr;
using detail::BpfProgLoadAttr;
using detail::BpfTestRunAttr;

template <class Attr>
int sys_bpf(int cmd, Attr& attr) noexcept {
  const long r = ::syscall(__NR_bpf, cmd, &attr, sizeof(attr));
  return r < 0 ? -errno : static_cast<int>(r);
}

std::uint64_t ptr_u64(const void* p) noexcept {
  return reinterpret_cast<std::uintptr_t>(p);
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

std::string errno_text(int neg_errno) {
  return {std::strerror(-neg_errno)};
}

int create_map(std::uint32_t type,
               std::uint32_t key_size,
               std::uint32_t value_size,
               std::uint32_t max_entries,
               const char* name) noexcept {
  BpfMapCreateAttr a{};
  a.map_type = type;
  a.key_size = key_size;
  a.value_size = value_size;
  a.max_entries = max_entries;
  std::strncpy(a.map_name, name, sizeof(a.map_name) - 1);
  int fd = sys_bpf(detail::kBpfMapCreate, a);
  if (fd == -EINVAL) {  // kernels that reject the name (not expected on 5.11+)
    std::memset(a.map_name, 0, sizeof(a.map_name));
    fd = sys_bpf(detail::kBpfMapCreate, a);
  }
  return fd;
}

int update_elem(int map_fd, const void* key, const void* value) noexcept {
  BpfMapElemAttr a{};
  a.map_fd = static_cast<std::uint32_t>(map_fd);
  a.key = ptr_u64(key);
  a.value = ptr_u64(value);
  a.flags = detail::kBpfAny;
  return sys_bpf(detail::kBpfMapUpdateElem, a);
}

int lookup_elem(int map_fd, const void* key, void* value) noexcept {
  BpfMapElemAttr a{};
  a.map_fd = static_cast<std::uint32_t>(map_fd);
  a.key = ptr_u64(key);
  a.value = ptr_u64(value);
  return sys_bpf(detail::kBpfMapLookupElem, a);
}

// Number of possible CPUs: the per-CPU map value count (/sys/devices/system/cpu/possible, e.g.
// "0-15" or "0,2-5").
std::size_t possible_cpus() noexcept {
  std::size_t count = 0;
  if (FILE* f = std::fopen("/sys/devices/system/cpu/possible", "re")) {
    char buf[256] = {};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    const char* p = buf;
    const char* end = buf + n;
    while (p < end && *p != '\n' && *p != '\0') {
      unsigned lo = 0;
      auto r = std::from_chars(p, end, lo);
      if (r.ec != std::errc{}) break;
      unsigned hi = lo;
      p = r.ptr;
      if (p < end && *p == '-') {
        r = std::from_chars(p + 1, end, hi);
        if (r.ec != std::errc{}) break;
        p = r.ptr;
      }
      if (hi >= lo) count += hi - lo + 1;
      if (p < end && *p == ',') ++p;
    }
  }
  if (count == 0) {
    const long n = ::sysconf(_SC_NPROCESSORS_CONF);
    count = n > 0 ? static_cast<std::size_t>(n) : 1;
  }
  return count;
}

// XSKMAP entries when the queues are not listed: RX queue indexes the program can redirect.
constexpr std::uint32_t kXskMapSlots = 256;

// RX queues the interface has now: ETHTOOL_GCHANNELS (rx + combined channels) in this thread's
// network namespace, else /sys/class/net/<if>/queues/rx-* (real_num_rx_queues, but of the
// namespace sysfs was mounted in), else 1.
std::uint32_t rx_queue_count(const std::string& ifname) noexcept {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd >= 0) {
    ethtool_channels ch{};
    ch.cmd = ETHTOOL_GCHANNELS;
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&ch);
    const bool ok = ::ioctl(fd, SIOCETHTOOL, &ifr) == 0;
    ::close(fd);
    if (ok && ch.rx_count + ch.combined_count != 0) return ch.rx_count + ch.combined_count;
  }
  const std::string path = "/sys/class/net/" + ifname + "/queues";
  DIR* d = ::opendir(path.c_str());
  if (d == nullptr) return 1;
  std::uint32_t n = 0;
  while (const dirent* e = ::readdir(d))
    if (std::strncmp(e->d_name, "rx-", 3) == 0) ++n;
  ::closedir(d);
  return n != 0 ? n : 1;
}

bool is_power_of_two(std::uint32_t v) noexcept {
  return v != 0 && (v & (v - 1)) == 0;
}

std::string ip_text(std::uint32_t be) {
  char buf[INET_ADDRSTRLEN] = {};
  in_addr a{};
  a.s_addr = be;
  ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

// Kernel UDP socket that joins `group` on `ifindex` so the IGMP report goes out. Never bound, so
// it receives nothing. Returns the fd or -errno.
int join_group(unsigned ifindex, std::uint32_t group_be, std::uint32_t source_be) noexcept {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  int r = 0;
  if (source_be == 0) {
    ip_mreqn m{};
    m.imr_multiaddr.s_addr = group_be;
    m.imr_ifindex = static_cast<int>(ifindex);
    r = ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));
  } else {
    group_source_req g{};
    g.gsr_interface = ifindex;
    auto* grp = reinterpret_cast<sockaddr_in*>(&g.gsr_group);
    grp->sin_family = AF_INET;
    grp->sin_addr.s_addr = group_be;
    auto* src = reinterpret_cast<sockaddr_in*>(&g.gsr_source);
    src->sin_family = AF_INET;
    src->sin_addr.s_addr = source_be;
    r = ::setsockopt(fd, IPPROTO_IP, MCAST_JOIN_SOURCE_GROUP, &g, sizeof(g));
  }
  if (r != 0) {
    const int err = -errno;
    ::close(fd);
    return err;
  }
  return fd;
}

std::string self_exe() {
  char buf[4096] = {};
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string("<binary>");
}

}  // namespace

// ---- checks -------------------------------------------------------------------------------------

std::string xdp_capability_error() {
  detail::CapHeader hdr{detail::kCapabilityVersion3, 0};
  std::array<detail::CapData, 2> data{};
  if (::syscall(SYS_capget, &hdr, data.data()) != 0)
    return std::string("capget failed: ") + std::strerror(errno);
  const auto has = [&data](unsigned cap) {
    return (data[cap / 32].effective & (1U << (cap % 32))) != 0;
  };
  std::string missing;
  const auto need = [&](bool ok, const char* name) {
    if (ok) return;
    if (!missing.empty()) missing += ", ";
    missing += name;
  };
  need(has(detail::kCapNetAdmin), "CAP_NET_ADMIN");
  need(has(detail::kCapNetRaw), "CAP_NET_RAW");
  need(has(detail::kCapBpf) || has(detail::kCapSysAdmin), "CAP_BPF");
  need(has(detail::kCapIpcLock), "CAP_IPC_LOCK");
  if (missing.empty()) return {};
  return "af_xdp needs CAP_NET_ADMIN, CAP_NET_RAW, CAP_BPF and CAP_IPC_LOCK; missing " + missing +
         ". Run as root or grant them: sudo setcap "
         "cap_net_admin,cap_net_raw,cap_bpf,cap_ipc_lock+ep " +
         self_exe();
}

std::string xdp_kernel_error() {
  utsname u{};
  if (::uname(&u) != 0) return std::string("uname failed: ") + std::strerror(errno);
  const char* p = u.release;
  const char* end = p + std::strlen(p);
  unsigned major = 0;
  unsigned minor = 0;
  auto r = std::from_chars(p, end, major);
  if (r.ec == std::errc{} && r.ptr < end && *r.ptr == '.')
    r = std::from_chars(r.ptr + 1, end, minor);
  if (r.ec != std::errc{}) return std::string("cannot parse kernel release ") + u.release;
  if (major > 5 || (major == 5 && minor >= 11)) return {};
  return std::string("af_xdp needs Linux 5.11 or later; this kernel is ") + u.release;
}

// ---- XdpFilter ----------------------------------------------------------------------------------

int XdpFilter::create(std::span<const xdp::Key> keys,
                      std::uint32_t xsk_slots,
                      std::string& err,
                      xdp::TcpMatch tcp) {
  close();
  const auto bail = [&](int rc, const char* step) {
    err = std::string(step) + ": " + errno_text(rc);
    close();
    return rc;
  };
  const auto key_count = static_cast<std::uint32_t>(std::max<std::size_t>(keys.size(), 1));
  subs_fd_ = create_map(detail::kBpfMapTypeHash, sizeof(xdp::Key), 4, key_count, "fastmm_subs");
  if (subs_fd_ < 0) return bail(subs_fd_, "BPF_MAP_CREATE (hash)");
  xsks_fd_ = create_map(
      detail::kBpfMapTypeXskmap, 4, 4, std::max<std::uint32_t>(xsk_slots, 1), "fastmm_xsks");
  if (xsks_fd_ < 0) return bail(xsks_fd_, "BPF_MAP_CREATE (xskmap)");
  fallback_fd_ = create_map(detail::kBpfMapTypePercpuArray, 4, 8, 1, "fastmm_fallback");
  if (fallback_fd_ < 0) return bail(fallback_fd_, "BPF_MAP_CREATE (percpu_array)");
  percpu_.assign(possible_cpus(), 0);

  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto line = static_cast<std::uint32_t>(i);
    const int rc = update_elem(subs_fd_, &keys[i], &line);
    if (rc < 0) return bail(rc, "BPF_MAP_UPDATE_ELEM (subscriptions)");
  }

  const xdp::Program prog = xdp::build_program({subs_fd_, xsks_fd_, fallback_fd_}, tcp);
  if (!prog.ok) {
    err = "XDP program assembly failed";
    close();
    return -EINVAL;
  }
  static constexpr char kLicense[] = "MIT";
  BpfProgLoadAttr a{};
  a.prog_type = detail::kBpfProgTypeXdp;
  a.insn_cnt = static_cast<std::uint32_t>(prog.size);
  a.insns = ptr_u64(prog.insns.data());
  a.license = ptr_u64(kLicense);
  std::strncpy(a.prog_name, "fastmm_xdp", sizeof(a.prog_name) - 1);
  prog_fd_ = sys_bpf(detail::kBpfProgLoad, a);
  if (prog_fd_ >= 0) return 0;
  const int load_err = prog_fd_;
  prog_fd_ = -1;
  if (load_err == -EPERM) return bail(load_err, "BPF_PROG_LOAD");
  // Load again with the verifier log to say why.
  std::vector<char> log(std::size_t{256} * 1024, '\0');
  a.log_level = 1;
  a.log_size = static_cast<std::uint32_t>(log.size());
  a.log_buf = ptr_u64(log.data());
  const int again = sys_bpf(detail::kBpfProgLoad, a);
  if (again >= 0) {  // cannot normally happen; keep the program
    prog_fd_ = again;
    return 0;
  }
  err = "BPF_PROG_LOAD: " + errno_text(load_err) + "; verifier log:\n" + std::string(log.data());
  close();
  return load_err;
}

int XdpFilter::attach(unsigned ifindex, bool generic) noexcept {
  if (prog_fd_ < 0) return -EBADF;
  if (link_fd_ >= 0) return -EALREADY;
  BpfLinkCreateAttr a{};
  a.prog_fd = static_cast<std::uint32_t>(prog_fd_);
  a.target_ifindex = ifindex;
  a.attach_type = detail::kBpfXdp;
  a.flags = generic ? detail::kXdpFlagsSkbMode : detail::kXdpFlagsDrvMode;
  const int fd = sys_bpf(detail::kBpfLinkCreate, a);
  if (fd < 0) return fd;
  link_fd_ = fd;
  return 0;
}

void XdpFilter::detach() noexcept {
  close_fd(link_fd_);
}

int XdpFilter::set_socket(std::uint32_t queue, int xsk_fd) noexcept {
  const auto value = static_cast<std::uint32_t>(xsk_fd);
  return update_elem(xsks_fd_, &queue, &value);
}

int XdpFilter::fallback_count(std::uint64_t& out) noexcept {
  out = 0;
  if (fallback_fd_ < 0) return -EBADF;
  const std::uint32_t key = 0;
  const int rc = lookup_elem(fallback_fd_, &key, percpu_.data());
  if (rc < 0) return rc;
  for (const std::uint64_t v : percpu_) out += v;
  return 0;
}

int XdpFilter::test_run(std::span<const std::byte> frame, std::uint32_t& action) noexcept {
  if (prog_fd_ < 0) return -EBADF;
  BpfTestRunAttr a{};
  a.prog_fd = static_cast<std::uint32_t>(prog_fd_);
  a.data_in = ptr_u64(frame.data());
  a.data_size_in = static_cast<std::uint32_t>(frame.size());
  a.repeat = 1;
  const int rc = sys_bpf(detail::kBpfProgTestRun, a);
  if (rc < 0) return rc;
  action = a.retval;
  return 0;
}

void XdpFilter::close() noexcept {
  close_fd(link_fd_);  // first: detaches the program
  close_fd(prog_fd_);
  close_fd(subs_fd_);
  close_fd(xsks_fd_);
  close_fd(fallback_fd_);
}

// ---- XdpDatagramSource --------------------------------------------------------------------------

int XdpDatagramSource::fail(int err, std::string msg) {
  close();
  error_ = std::move(msg);
  return err;
}

void XdpDatagramSource::close_socket(detail::XskSocket& s) noexcept {
  if (s.rx_map != nullptr) ::munmap(s.rx_map, s.rx_map_len);
  if (s.fill_map != nullptr) ::munmap(s.fill_map, s.fill_map_len);
  if (s.tx_map != nullptr) ::munmap(s.tx_map, s.tx_map_len);
  if (s.comp_map != nullptr) ::munmap(s.comp_map, s.comp_map_len);
  s.rx_map = nullptr;
  s.fill_map = nullptr;
  s.tx_map = nullptr;
  s.comp_map = nullptr;
  close_fd(s.fd);
  s.fill = {};
  s.rx = {};
  s.tx = {};
  s.comp = {};
}

void XdpDatagramSource::close() noexcept {
  for (auto& f : filters_) f.close();  // detach before the sockets go
  for (auto& s : sockets_) {
    close_socket(s);
    if (s.umem != nullptr) ::munmap(s.umem, s.umem_len);
    s.umem = nullptr;
  }
  for (int& fd : join_fds_) close_fd(fd);
  sockets_.clear();
  tx_sock_ = nullptr;
  tx_free_.clear();
  tx_pending_ = 0;
  sink_ = FrameSink{};
  routes_.clear();
  fds_.clear();
  filters_.clear();
  join_fds_.clear();
  interfaces_.clear();
  socket_stats_.clear();
  warnings_.clear();
  error_.clear();
  stats_ = {};
}

int XdpDatagramSource::create_socket(detail::XskSocket& s,
                                     bool zerocopy,
                                     const XdpConfig& cfg,
                                     std::string& why) {
  const auto step = [&why](const char* what) {
    const int e = errno;
    why = std::string(what) + ": " + std::strerror(e);
    return -e;
  };
  s.fd = ::socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
  if (s.fd < 0) return step("socket(AF_XDP)");

  detail::XdpUmemReg mr{};
  mr.addr = ptr_u64(s.umem);
  mr.len = s.umem_len;
  mr.chunk_size = cfg.frame_size;
  if (::setsockopt(s.fd, SOL_XDP, XDP_UMEM_REG, &mr, sizeof(mr)) != 0) return step("XDP_UMEM_REG");
  const int ring = static_cast<int>(cfg.frame_count);
  // Required by bind; only the UserTcp socket transmits.
  const int completion = s.tx_frames != 0 ? static_cast<int>(s.tx_frames) : 64;
  const int tx_ring = static_cast<int>(s.tx_frames);
  if (tx_ring != 0 && ::setsockopt(s.fd, SOL_XDP, XDP_TX_RING, &tx_ring, sizeof(tx_ring)) != 0)
    return step("XDP_TX_RING");
  if (::setsockopt(s.fd, SOL_XDP, XDP_UMEM_FILL_RING, &ring, sizeof(ring)) != 0)
    return step("XDP_UMEM_FILL_RING");
  if (::setsockopt(s.fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &completion, sizeof(completion)) != 0)
    return step("XDP_UMEM_COMPLETION_RING");
  if (::setsockopt(s.fd, SOL_XDP, XDP_RX_RING, &ring, sizeof(ring)) != 0)
    return step("XDP_RX_RING");

  detail::XdpMmapOffsets off{};
  socklen_t optlen = sizeof(off);
  if (::getsockopt(s.fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen) != 0)
    return step("XDP_MMAP_OFFSETS");
  if (optlen != sizeof(off)) {
    why = "XDP_MMAP_OFFSETS: kernel without ring flags (before Linux 5.4)";
    return -EINVAL;
  }

  s.fill_map_len = off.fr.desc + cfg.frame_count * sizeof(std::uint64_t);
  void* fm = ::mmap(nullptr,
                    s.fill_map_len,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE,
                    s.fd,
                    static_cast<off_t>(XDP_UMEM_PGOFF_FILL_RING));
  if (fm == MAP_FAILED) return step("mmap(fill ring)");
  s.fill_map = fm;
  s.rx_map_len = off.rx.desc + cfg.frame_count * sizeof(detail::XdpDesc);
  void* rm = ::mmap(nullptr,
                    s.rx_map_len,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE,
                    s.fd,
                    static_cast<off_t>(XDP_PGOFF_RX_RING));
  if (rm == MAP_FAILED) return step("mmap(rx ring)");
  s.rx_map = rm;

  const auto setup = [&cfg](detail::XskRing& r, void* base, const detail::XdpRingOffset& o) {
    auto* b = static_cast<char*>(base);
    r.producer = reinterpret_cast<std::atomic<std::uint32_t>*>(b + o.producer);
    r.consumer = reinterpret_cast<std::atomic<std::uint32_t>*>(b + o.consumer);
    r.flags = reinterpret_cast<std::atomic<std::uint32_t>*>(b + o.flags);
    r.entries = b + o.desc;
    r.size = cfg.frame_count;
    r.mask = cfg.frame_count - 1;
  };
  setup(s.fill, s.fill_map, off.fr);
  setup(s.rx, s.rx_map, off.rx);
  if (s.tx_frames != 0) {
    s.tx_map_len = off.tx.desc + s.tx_frames * sizeof(detail::XdpDesc);
    void* tm = ::mmap(nullptr,
                      s.tx_map_len,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE,
                      s.fd,
                      static_cast<off_t>(XDP_PGOFF_TX_RING));
    if (tm == MAP_FAILED) return step("mmap(tx ring)");
    s.tx_map = tm;
    s.comp_map_len = off.cr.desc + s.tx_frames * sizeof(std::uint64_t);
    void* cm = ::mmap(nullptr,
                      s.comp_map_len,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE,
                      s.fd,
                      static_cast<off_t>(XDP_UMEM_PGOFF_COMPLETION_RING));
    if (cm == MAP_FAILED) return step("mmap(completion ring)");
    s.comp_map = cm;
    const auto setup_tx = [&s](detail::XskRing& r, void* base, const detail::XdpRingOffset& o) {
      auto* b = static_cast<char*>(base);
      r.producer = reinterpret_cast<std::atomic<std::uint32_t>*>(b + o.producer);
      r.consumer = reinterpret_cast<std::atomic<std::uint32_t>*>(b + o.consumer);
      r.flags = reinterpret_cast<std::atomic<std::uint32_t>*>(b + o.flags);
      r.entries = b + o.desc;
      r.size = s.tx_frames;
      r.mask = s.tx_frames - 1;
    };
    setup_tx(s.tx, s.tx_map, off.tx);
    setup_tx(s.comp, s.comp_map, off.cr);
    s.tx.cached_prod = s.tx.producer->load(std::memory_order_relaxed);
    s.tx.cached_cons = s.tx.consumer->load(std::memory_order_relaxed) + s.tx.size;
    s.comp.cached_prod = s.comp.producer->load(std::memory_order_relaxed);
    s.comp.cached_cons = s.comp.consumer->load(std::memory_order_relaxed);
  }
  s.fill.cached_prod = s.fill.producer->load(std::memory_order_relaxed);
  s.fill.cached_cons = s.fill.consumer->load(std::memory_order_relaxed) + s.fill.size;
  s.rx.cached_prod = s.rx.producer->load(std::memory_order_relaxed);
  s.rx.cached_cons = s.rx.consumer->load(std::memory_order_relaxed);

  sockaddr_xdp sa{};
  sa.sxdp_family = AF_XDP;
  sa.sxdp_flags = static_cast<__u16>((zerocopy ? XDP_ZEROCOPY : XDP_COPY) | XDP_USE_NEED_WAKEUP);
  sa.sxdp_ifindex = s.ifindex;
  sa.sxdp_queue_id = s.queue;
  if (::bind(s.fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0)
    return step(zerocopy ? "bind(XDP_ZEROCOPY)" : "bind(XDP_COPY)");

  // Every frame starts on the fill ring.
  std::uint32_t idx = 0;
  if (s.fill.reserve(cfg.frame_count, idx) != cfg.frame_count) {
    why = "fill ring smaller than the UMEM";
    return -EINVAL;
  }
  auto* addrs = static_cast<std::uint64_t*>(s.fill.entries);
  for (std::uint32_t i = 0; i < cfg.frame_count; ++i)
    addrs[(idx + i) & s.fill.mask] = static_cast<std::uint64_t>(i) * cfg.frame_size;
  s.fill.submit();
  return 0;
}

int XdpDatagramSource::open_interface(std::size_t iface,
                                      std::span<const std::uint32_t> listed,
                                      std::uint32_t routes_begin,
                                      std::uint32_t routes_end,
                                      const XdpConfig& cfg) {
  XdpInterfaceStatus& st = interfaces_[iface];
  XdpFilter& filter = filters_[iface];

  std::vector<xdp::Key> keys;
  for (std::uint32_t r = routes_begin; r < routes_end; ++r)
    keys.push_back(xdp::Key{routes_[r].dst_ip, htons(routes_[r].dst_port), 0});
  const std::uint32_t slots =
      listed.empty() ? kXskMapSlots
                     : std::max(kXskMapSlots, *std::max_element(listed.begin(), listed.end()) + 1);
  const bool tcp_here = cfg.tcp_ip != 0 && cfg.tcp_interface == st.name;
  xdp::TcpMatch tcp;
  if (tcp_here) tcp = {cfg.tcp_ip, cfg.tcp_port, cfg.tcp_arp};
  std::string err;
  if (const int rc = filter.create(keys, slots, err, tcp); rc < 0)
    return fail(rc, "af_xdp " + st.name + ": " + err);

  const std::size_t first = sockets_.size();
  const auto drop_sockets = [&] {
    for (std::size_t i = first; i < sockets_.size(); ++i) {
      close_socket(sockets_[i]);
      if (sockets_[i].umem != nullptr) ::munmap(sockets_[i].umem, sockets_[i].umem_len);
    }
    sockets_.resize(first);
  };

  static constexpr std::array<XdpMode, 3> kAutoOrder = {
      XdpMode::ZeroCopy, XdpMode::NativeCopy, XdpMode::Generic};
  const std::span<const XdpMode> order = cfg.mode == XdpMode::Auto
                                             ? std::span<const XdpMode>(kAutoOrder)
                                             : std::span<const XdpMode>(&cfg.mode, 1);
  std::string tried;
  bool native_unsupported = false;
  bool attached_generic = false;
  int last_rc = -EOPNOTSUPP;
  for (const XdpMode m : order) {
    const bool generic = m == XdpMode::Generic;
    if (!generic && native_unsupported) continue;
    const auto note = [&](int rc, const std::string& why) {
      last_rc = rc;
      if (!tried.empty()) tried += "; ";
      tried += std::string(to_string(m)) + ": " + why;
      drop_sockets();
    };
    // Attach first: a driver may change its RX queue count with the program (virtio_net adds a
    // queue pair per CPU for XDP_TX, and the host then spreads the traffic over them too), and
    // bind() accepts only queues that exist.
    if (filter.link_fd() >= 0 && attached_generic != generic) filter.detach();
    if (filter.link_fd() < 0) {
      const int rc = filter.attach(st.ifindex, generic);
      if (rc == -EBUSY || rc == -EEXIST) {
        return fail(-EBUSY,
                    "af_xdp " + st.name +
                        ": the interface already has an XDP program (see `ip link show dev " +
                        st.name + "`)");
      }
      if (rc < 0) {
        if (!generic) native_unsupported = true;
        note(rc, std::string(generic ? "generic" : "native") + " attach: " + errno_text(rc));
        continue;
      }
      attached_generic = generic;
    }
    std::vector<std::uint32_t> queues(listed.begin(), listed.end());
    if (queues.empty()) {
      const std::uint32_t n = rx_queue_count(st.name);
      if (n > slots) {
        filter.detach();
        return fail(-EINVAL,
                    "af_xdp " + st.name + ": " + std::to_string(n) +
                        " RX queues; list the ones that carry the feed in `queues`");
      }
      for (std::uint32_t q = 0; q < n; ++q) queues.push_back(q);
    }
    bool bound = true;
    for (const std::uint32_t q : queues) {
      detail::XskSocket s;
      s.ifindex = st.ifindex;
      s.queue = q;
      s.routes_begin = routes_begin;
      s.routes_end = routes_end;
      // The first socket of the UserTcp interface transmits; its UMEM has the TX frames too.
      s.tx_frames = tcp_here && sockets_.size() == first ? cfg.tx_frames : 0;
      s.umem_len = static_cast<std::size_t>(cfg.frame_count + s.tx_frames) * cfg.frame_size;
      void* mem = ::mmap(nullptr,
                         s.umem_len,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                         -1,
                         0);
      if (mem == MAP_FAILED) {
        const int e = errno;
        drop_sockets();
        return fail(-e, "af_xdp " + st.name + ": UMEM mmap: " + std::strerror(e));
      }
      s.umem = static_cast<std::byte*>(mem);
      sockets_.push_back(s);
      detail::XskSocket& sock = sockets_.back();
      std::string why;
      if (const int rc = create_socket(sock, m == XdpMode::ZeroCopy, cfg, why); rc < 0) {
        note(rc, "queue " + std::to_string(q) + " " + why);
        bound = false;
        break;
      }
      if (const int rc = filter.set_socket(q, sock.fd); rc < 0) {
        note(rc, "BPF_MAP_UPDATE_ELEM (xskmap): " + errno_text(rc));
        bound = false;
        break;
      }
    }
    if (!bound) continue;
    st.mode = m;
    return 0;
  }
  filter.detach();
  return fail(last_rc, "af_xdp " + st.name + ": no mode worked (" + tried + ")");
}

int XdpDatagramSource::open(const XdpConfig& cfg) {
  close();
  if (cfg.subscriptions.empty()) return fail(-EINVAL, "af_xdp: no subscriptions");
  if (cfg.subscriptions.size() > 256) return fail(-EINVAL, "af_xdp: more than 256 subscriptions");
  if (!is_power_of_two(cfg.frame_count) || cfg.frame_count < 64)
    return fail(-EINVAL, "af_xdp: frame_count must be a power of two >= 64");
  const long page = ::sysconf(_SC_PAGESIZE);
  if ((cfg.frame_size != 2048 && cfg.frame_size != 4096) ||
      static_cast<long>(cfg.frame_size) > page)
    return fail(-EINVAL, "af_xdp: frame_size must be 2048 or 4096 (at most the page size)");
  if (cfg.batch == 0 || cfg.batch > cfg.frame_count)
    return fail(-EINVAL, "af_xdp: batch must be in [1, frame_count]");

  // Interfaces in order of first use; routes grouped by interface.
  std::vector<std::string> names;
  for (const auto& sub : cfg.subscriptions) {
    if (sub.group == 0) return fail(-EINVAL, "af_xdp: address 0.0.0.0");
    if (sub.port == 0) return fail(-EINVAL, "af_xdp: port 0");
    if (std::find(names.begin(), names.end(), sub.interface) == names.end())
      names.push_back(sub.interface);
  }
  for (std::size_t i = 0; i < cfg.subscriptions.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      const auto& a = cfg.subscriptions[i];
      const auto& b = cfg.subscriptions[j];
      if (a.interface == b.interface && a.group == b.group && a.port == b.port)
        return fail(-EINVAL,
                    "af_xdp: " + a.interface + " " + ip_text(a.group) + ":" +
                        std::to_string(a.port) +
                        " subscribed twice (the XDP map has no source address)");
    }
  }
  for (const auto& q : cfg.queues) {
    if (std::find(names.begin(), names.end(), q.interface) == names.end())
      return fail(-EINVAL, "af_xdp: queues for " + q.interface + ", which has no subscription");
  }
  if (cfg.tcp_ip != 0) {
    if (std::find(names.begin(), names.end(), cfg.tcp_interface) == names.end())
      return fail(-EINVAL, "af_xdp: tcp_interface '" + cfg.tcp_interface + "' has no subscription");
    if (!is_power_of_two(cfg.tx_frames) || cfg.tx_frames < 16 || cfg.tx_frames > 4096)
      return fail(-EINVAL, "af_xdp: tx_frames must be a power of two in [16, 4096]");
  }

  if (std::string e = xdp_kernel_error(); !e.empty()) return fail(-ENOSYS, std::move(e));
  if (std::string e = xdp_capability_error(); !e.empty()) return fail(-EPERM, std::move(e));

  interfaces_.resize(names.size());
  filters_.resize(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    interfaces_[i].name = names[i];
    interfaces_[i].ifindex = ::if_nametoindex(names[i].c_str());
    if (interfaces_[i].ifindex == 0) return fail(-ENODEV, "af_xdp: no interface " + names[i]);
    for (std::size_t k = 0; k < cfg.subscriptions.size(); ++k) {
      const auto& sub = cfg.subscriptions[k];
      if (sub.interface == names[i])
        routes_.push_back({sub.group, sub.port, static_cast<std::uint8_t>(k)});
    }
  }
  // Unlisted interfaces (empty): every RX queue, counted once the program is attached.
  std::vector<std::vector<std::uint32_t>> queues(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    for (const auto& q : cfg.queues)
      if (q.interface == names[i])
        queues[i].insert(queues[i].end(), q.queues.begin(), q.queues.end());
    std::sort(queues[i].begin(), queues[i].end());
    if (std::adjacent_find(queues[i].begin(), queues[i].end()) != queues[i].end())
      return fail(-EINVAL, "af_xdp: " + names[i] + " lists a queue twice");
  }

  std::uint32_t begin = 0;
  for (std::size_t i = 0; i < names.size(); ++i) {
    std::uint32_t end = begin;
    while (end < routes_.size() && cfg.subscriptions[routes_[end].line].interface == names[i])
      ++end;
    if (const int rc = open_interface(i, queues[i], begin, end, cfg); rc < 0) return rc;
    begin = end;
  }

  frame_mask_ = ~static_cast<std::uint64_t>(cfg.frame_size - 1);
  batch_ = cfg.batch;
  busy_poll_ = cfg.busy_poll;
  verify_checksums_ = cfg.verify_udp_checksum;
  for (const auto& s : sockets_) {
    fds_.push_back(s.fd);
    socket_stats_.push_back({s.ifindex, s.queue, 0, 0, 0, 0});
    if (!cfg.busy_poll) continue;
    const auto opt = [&](int name, int value, const char* text) {
      if (::setsockopt(s.fd, SOL_SOCKET, name, &value, sizeof(value)) != 0) {
        warnings_.push_back(std::string("af_xdp queue ") + std::to_string(s.queue) + ": " + text +
                            " refused: " + std::strerror(errno));
      }
    };
    opt(SO_PREFER_BUSY_POLL, 1, "SO_PREFER_BUSY_POLL");
    opt(SO_BUSY_POLL, cfg.busy_poll_usec, "SO_BUSY_POLL");
    opt(SO_BUSY_POLL_BUDGET, cfg.busy_poll_budget, "SO_BUSY_POLL_BUDGET");
  }

  if (cfg.tcp_ip != 0) {
    for (auto& s : sockets_) {
      if (s.tx_frames == 0) continue;
      tx_sock_ = &s;
      for (const XdpInterfaceStatus& st : interfaces_)
        if (st.ifindex == s.ifindex) tx_copy_ = st.mode != XdpMode::ZeroCopy;
      tx_free_.clear();
      for (std::uint32_t i = 0; i < s.tx_frames; ++i)
        tx_free_.push_back(static_cast<std::uint64_t>(cfg.frame_count + i) * cfg.frame_size);
      break;
    }
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, cfg.tcp_interface.c_str(), IFNAMSIZ - 1);
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      if (::ioctl(fd, SIOCGIFHWADDR, &ifr) == 0)
        std::memcpy(mac_.data(), ifr.ifr_hwaddr.sa_data, 6);
      if (::ioctl(fd, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu > 0)
        mtu_ = static_cast<std::uint32_t>(ifr.ifr_mtu);
      ::close(fd);
    }
  }
  frame_size_ = cfg.frame_size;

  for (const auto& sub : cfg.subscriptions) {
    if ((ntohl(sub.group) >> 28U) != 0xE) continue;  // unicast: nothing to join
    const auto it = std::find_if(interfaces_.begin(), interfaces_.end(), [&sub](const auto& st) {
      return st.name == sub.interface;
    });
    const int fd = join_group(it->ifindex, sub.group, sub.source);
    if (fd < 0) {
      return fail(
          fd,
          "af_xdp " + sub.interface + ": joining " + ip_text(sub.group) + ": " + errno_text(fd));
    }
    join_fds_.push_back(fd);
  }
  return 0;
}

void XdpDatagramSource::tx_reclaim() noexcept {
  detail::XskSocket& s = *tx_sock_;
  std::uint32_t idx = 0;
  const std::uint32_t n = s.comp.peek(s.tx_frames, idx);
  if (n == 0) return;
  const auto* addrs = static_cast<const std::uint64_t*>(s.comp.entries);
  for (std::uint32_t i = 0; i < n; ++i) tx_free_.push_back(addrs[(idx + i) & s.comp.mask]);
  s.comp.release();
}

bool XdpDatagramSource::tx_frame(std::span<const std::byte> frame) noexcept {
  if (FASTMM_UNLIKELY(tx_sock_ == nullptr || frame.size() > frame_size_)) {
    ++stats_.tx_drops;
    return false;
  }
  detail::XskSocket& s = *tx_sock_;
  if (tx_free_.empty()) tx_reclaim();
  std::uint32_t idx = 0;
  if (FASTMM_UNLIKELY(tx_free_.empty() || s.tx.reserve(1, idx) != 1)) {
    ++stats_.tx_drops;
    return false;
  }
  const std::uint64_t addr = tx_free_.back();
  tx_free_.pop_back();
  std::memcpy(s.umem + addr, frame.data(), frame.size());
  auto* descs = static_cast<detail::XdpDesc*>(s.tx.entries);
  detail::XdpDesc& d = descs[idx & s.tx.mask];
  d.addr = addr;
  d.len = static_cast<std::uint32_t>(frame.size());
  d.options = 0;
  ++tx_pending_;
  ++stats_.tx_frames;
  return true;
}

void XdpDatagramSource::tx_flush() noexcept {
  if (tx_sock_ == nullptr) return;
  detail::XskSocket& s = *tx_sock_;
  if (tx_pending_ != 0) {
    s.tx.submit();
    tx_pending_ = 0;
    // Copy mode transmits only from sendto(); zero-copy when the driver asks for a wake-up.
    if (tx_copy_ || s.tx.needs_wakeup()) {
      ::sendto(s.fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
      ++stats_.tx_kicks;
    }
  }
  tx_reclaim();
}

int XdpDatagramSource::refresh_stats() noexcept {
  int result = 0;
  for (std::size_t i = 0; i < sockets_.size(); ++i) {
    detail::XdpStatistics xs{};
    socklen_t len = sizeof(xs);
    if (::getsockopt(sockets_[i].fd, SOL_XDP, XDP_STATISTICS, &xs, &len) != 0) {
      result = -errno;
      continue;
    }
    XdpSocketStats& out = socket_stats_[i];
    out.rx_dropped = xs.rx_dropped;
    out.rx_invalid_descs = xs.rx_invalid_descs;
    out.rx_ring_full = xs.rx_ring_full;
    out.rx_fill_ring_empty_descs = xs.rx_fill_ring_empty_descs;
  }
  for (std::size_t i = 0; i < filters_.size(); ++i) {
    std::uint64_t n = 0;
    if (const int rc = filters_[i].fallback_count(n); rc < 0) {
      result = rc;
      continue;
    }
    interfaces_[i].fallback_packets = n;
  }
  return result;
}

}  // namespace fastmm::net
