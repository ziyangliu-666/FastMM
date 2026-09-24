// DpdkDatagramSource (see dpdk_datagram_source.hpp): the DPDK half, built with FASTMM_WITH_DPDK.
#include "fastmm/net/dpdk_datagram_source.hpp"

#include "fastmm/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <mutex>

namespace fastmm::net {

namespace {

std::mutex g_eal_mutex;
bool g_eal_done = false;
int g_eal_result = 0;
std::vector<std::string> g_eal_args;

int eal_init(const std::vector<std::string>& args, std::string& err) {
  const std::lock_guard<std::mutex> lock(g_eal_mutex);
  if (g_eal_done) {
    if (args != g_eal_args) {
      err = "EAL already initialised with other arguments";
      return -EALREADY;
    }
    if (g_eal_result != 0) err = "EAL initialisation failed earlier";
    return g_eal_result;
  }
  g_eal_done = true;
  g_eal_args = args;
  std::vector<std::string> storage;
  storage.emplace_back("fastmm");
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& s : storage) argv.push_back(s.data());
  argv.push_back(nullptr);
  // rte_eal_init pins the calling thread to the main lcore's CPU; the caller keeps its own.
  cpu_set_t saved;
  const bool have_affinity = ::pthread_getaffinity_np(::pthread_self(), sizeof saved, &saved) == 0;
  const int r = ::rte_eal_init(static_cast<int>(storage.size()), argv.data());
  if (have_affinity)
    static_cast<void>(::pthread_setaffinity_np(::pthread_self(), sizeof saved, &saved));
  if (r < 0) {
    g_eal_result = -(rte_errno != 0 ? rte_errno : EINVAL);
    err = std::string("rte_eal_init: ") + ::rte_strerror(rte_errno);
    return g_eal_result;
  }
  g_eal_result = 0;
  return 0;
}

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

rte_ether_addr group_mac(std::uint32_t group_be) {
  const std::uint32_t g = ntohl(group_be);
  rte_ether_addr a{};
  a.addr_bytes[0] = 0x01;
  a.addr_bytes[1] = 0x00;
  a.addr_bytes[2] = 0x5E;
  a.addr_bytes[3] = static_cast<std::uint8_t>((g >> 16U) & 0x7FU);
  a.addr_bytes[4] = static_cast<std::uint8_t>((g >> 8U) & 0xFFU);
  a.addr_bytes[5] = static_cast<std::uint8_t>(g & 0xFFU);
  return a;
}

std::atomic<unsigned> g_pool_seq{0};

constexpr std::size_t kEth = 14;
constexpr std::uint16_t kEtherArp = 0x0806;
constexpr std::uint16_t kEtherIpv4 = 0x0800;
constexpr std::uint16_t kExcBurst = 32;

std::uint16_t be16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>((std::to_integer<unsigned>(p[0]) << 8U) |
                                    std::to_integer<unsigned>(p[1]));
}
std::uint32_t raw32(const std::byte* p) noexcept {
  std::uint32_t v = 0;
  std::memcpy(&v, p, 4);
  return v;
}

// "a.b.c.d/len" on `ifname` (SIOCSIFADDR, SIOCSIFNETMASK), and the link up. 0 or -errno.
int set_interface_ip(const std::string& ifname, const std::string& cidr, std::string& err) {
  const std::size_t slash = cidr.find('/');
  std::uint32_t addr = 0;
  unsigned len = 32;
  const std::string host = cidr.substr(0, slash);
  bool ok = ::inet_pton(AF_INET, host.c_str(), &addr) == 1;
  if (ok && slash != std::string::npos) {
    const char* b = cidr.data() + slash + 1;
    const char* e = cidr.data() + cidr.size();
    const auto r = std::from_chars(b, e, len);
    ok = r.ec == std::errc{} && r.ptr == e && len <= 32;
  }
  if (!ok) {
    err = "exception_ip must be a.b.c.d/len: " + cidr;
    return -EINVAL;
  }
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
  auto* sin = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
  const auto step = [&](unsigned long req, const char* what) {
    if (::ioctl(fd, req, &ifr) == 0) return 0;
    const int e = errno;
    err = std::string(what) + " on " + ifname + ": " + std::strerror(e);
    return -e;
  };
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = addr;
  int rc = step(SIOCSIFADDR, "SIOCSIFADDR");
  if (rc == 0) {
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = len == 0 ? 0 : htonl(~std::uint32_t{0} << (32U - len));
    rc = step(SIOCSIFNETMASK, "SIOCSIFNETMASK");
  }
  if (rc == 0) rc = step(SIOCGIFFLAGS, "SIOCGIFFLAGS");
  if (rc == 0) {
    ifr.ifr_flags = static_cast<short>(ifr.ifr_flags | IFF_UP);
    rc = step(SIOCSIFFLAGS, "SIOCSIFFLAGS");
  }
  ::close(fd);
  return rc;
}

}  // namespace

bool dpdk_available() noexcept {
  return true;
}

int DpdkDatagramSource::fail(int err, std::string msg) {
  error_ = std::move(msg);
  close();
  return err;
}

int DpdkDatagramSource::open(const DpdkConfig& cfg) {
  close();
  error_.clear();
  if (cfg.subscriptions.empty() || cfg.subscriptions.size() > 256)
    return fail(-EINVAL, "1 to 256 subscriptions");
  if (cfg.batch == 0 || cfg.batch > 256) return fail(-EINVAL, "batch must be 1 to 256");
  std::string err;
  if (const int r = eal_init(cfg.eal_args, err); r != 0) return fail(r, err);

  std::uint16_t port = RTE_MAX_ETHPORTS;
  if (cfg.port.empty()) {
    port = ::rte_eth_find_next(0);
    if (port >= RTE_MAX_ETHPORTS) return fail(-ENODEV, "no DPDK port (add --vdev or a PCI device)");
  } else if (::rte_eth_dev_get_port_by_name(cfg.port.c_str(), &port) != 0) {
    return fail(-ENODEV, "no DPDK port named " + cfg.port);
  }
  port_ = port;
  char name[RTE_ETH_NAME_MAX_LEN] = {};
  if (::rte_eth_dev_get_name_by_port(port, name) == 0) port_name_ = name;

  const int socket = ::rte_eth_dev_socket_id(port);
  const std::string pool_name = "fmrx" + std::to_string(g_pool_seq.fetch_add(1));
  rte_mempool* pool = ::rte_pktmbuf_pool_create(pool_name.c_str(),
                                                cfg.mbufs,
                                                256,
                                                0,
                                                RTE_MBUF_DEFAULT_BUF_SIZE,
                                                socket < 0 ? SOCKET_ID_ANY : socket);
  if (pool == nullptr)
    return fail(-(rte_errno != 0 ? rte_errno : ENOMEM),
                std::string("rte_pktmbuf_pool_create: ") + ::rte_strerror(rte_errno));
  pool_ = pool;

  rte_eth_conf conf{};
  if (const int r = ::rte_eth_dev_configure(port, 1, 1, &conf); r < 0)
    return fail(r, std::string("rte_eth_dev_configure: ") + ::rte_strerror(-r));
  std::uint16_t rxd = cfg.rx_descriptors;
  std::uint16_t txd = cfg.tx_descriptors;
  static_cast<void>(::rte_eth_dev_adjust_nb_rx_tx_desc(port, &rxd, &txd));
  const unsigned sock =
      socket < 0 ? static_cast<unsigned>(SOCKET_ID_ANY) : static_cast<unsigned>(socket);
  if (const int r = ::rte_eth_rx_queue_setup(port, 0, rxd, sock, nullptr, pool); r < 0)
    return fail(r, std::string("rte_eth_rx_queue_setup: ") + ::rte_strerror(-r));
  if (const int r = ::rte_eth_tx_queue_setup(port, 0, txd, sock, nullptr); r < 0)
    return fail(r, std::string("rte_eth_tx_queue_setup: ") + ::rte_strerror(-r));
  if (const int r = ::rte_eth_dev_start(port); r < 0)
    return fail(r, std::string("rte_eth_dev_start: ") + ::rte_strerror(-r));
  started_ = true;
  rte_ether_addr own{};
  if (::rte_eth_macaddr_get(port, &own) == 0) std::memcpy(mac_.data(), own.addr_bytes, 6);

  batch_ = static_cast<std::uint16_t>(cfg.batch);
  frames_.assign(batch_, {});
  mbufs_.assign(batch_, nullptr);
  to_exc_.assign(batch_, nullptr);
  exc_rx_.assign(kExcBurst, nullptr);

  if (!cfg.exception_port.empty()) {
    if (const int r = open_exception(cfg, sock); r != 0) return r;
  }

  // Group MAC filters where the PMD has them, all-multicast otherwise.
  std::vector<rte_ether_addr> macs;
  for (const DpdkSubscription& s : cfg.subscriptions)
    if (is_ipv4_multicast(s.group)) macs.push_back(group_mac(s.group));
  if (!macs.empty() && ::rte_eth_dev_set_mc_addr_list(
                           port, macs.data(), static_cast<std::uint32_t>(macs.size())) != 0)
    static_cast<void>(::rte_eth_allmulticast_enable(port));

  routes_.clear();
  unicast_.clear();
  for (std::size_t i = 0; i < cfg.subscriptions.size(); ++i) {
    const DpdkSubscription& s = cfg.subscriptions[i];
    routes_.push_back(Route{s.group, s.port, static_cast<std::uint8_t>(i)});
    if (!is_ipv4_multicast(s.group)) {
      if (std::find(unicast_.begin(), unicast_.end(), s.group) == unicast_.end())
        unicast_.push_back(s.group);
      continue;
    }
    if (s.interface.empty()) continue;
    const unsigned ifindex = ::if_nametoindex(s.interface.c_str());
    if (ifindex == 0) return fail(-ENODEV, "no interface " + s.interface + " for the IGMP join");
    const int fd = join_group(ifindex, s.group, s.source);
    if (fd < 0) return fail(fd, "joining the group on " + s.interface + ": " + std::strerror(-fd));
    join_fds_.push_back(fd);
  }
  verify_checksums_ = cfg.verify_udp_checksum;
  stats_ = DpdkStats{};
  return 0;
}

int DpdkDatagramSource::open_exception(const DpdkConfig& cfg, unsigned sock) {
  std::uint16_t exc = RTE_MAX_ETHPORTS;
  if (::rte_eth_dev_get_port_by_name(cfg.exception_port.c_str(), &exc) != 0)
    return fail(-ENODEV,
                "no exception port named " + cfg.exception_port +
                    " (add --vdev=" + cfg.exception_port + ",iface=<name> to the EAL arguments)");
  if (exc == port_) return fail(-EINVAL, "the exception port is the receive port");
  rte_eth_conf conf{};
  if (const int r = ::rte_eth_dev_configure(exc, 1, 1, &conf); r < 0)
    return fail(r, std::string("exception port: rte_eth_dev_configure: ") + ::rte_strerror(-r));
  std::uint16_t rxd = 512;
  std::uint16_t txd = 512;
  static_cast<void>(::rte_eth_dev_adjust_nb_rx_tx_desc(exc, &rxd, &txd));
  auto* pool = static_cast<rte_mempool*>(pool_);
  if (const int r = ::rte_eth_rx_queue_setup(exc, 0, rxd, sock, nullptr, pool); r < 0)
    return fail(r, std::string("exception port: rte_eth_rx_queue_setup: ") + ::rte_strerror(-r));
  if (const int r = ::rte_eth_tx_queue_setup(exc, 0, txd, sock, nullptr); r < 0)
    return fail(r, std::string("exception port: rte_eth_tx_queue_setup: ") + ::rte_strerror(-r));
  // The kernel's frames leave with the port's MAC, and it answers ARP with it.
  rte_ether_addr m{};
  std::memcpy(m.addr_bytes, mac_.data(), 6);
  if (const int r = ::rte_eth_dev_default_mac_addr_set(exc, &m); r != 0)
    return fail(r, std::string("exception port: setting the MAC: ") + ::rte_strerror(-r));
  if (const int r = ::rte_eth_dev_start(exc); r < 0)
    return fail(r, std::string("exception port: rte_eth_dev_start: ") + ::rte_strerror(-r));
  exc_ = exc;
  exc_started_ = true;
  rte_eth_dev_info info{};
  char name[IF_NAMESIZE] = {};
  if (::rte_eth_dev_info_get(exc, &info) == 0 && info.if_index != 0 &&
      ::if_indextoname(info.if_index, name) != nullptr)
    exc_ifname_ = name;
  if (!cfg.exception_ip.empty()) {
    if (exc_ifname_.empty()) return fail(-ENODEV, "the exception port has no kernel interface");
    std::string err;
    if (const int r = set_interface_ip(exc_ifname_, cfg.exception_ip, err); r != 0)
      return fail(r, err);
  }
  exc_interval_cycles_ = static_cast<std::uint64_t>(static_cast<double>(cfg.exception_interval_ns) *
                                                    static_cast<double>(::rte_get_tsc_hz()) / 1e9);
  exc_next_ = 0;
  return 0;
}

void DpdkDatagramSource::close() noexcept {
  for (const int fd : join_fds_) ::close(fd);
  join_fds_.clear();
  if (to_exc_n_ != 0)
    ::rte_pktmbuf_free_bulk(reinterpret_cast<rte_mbuf**>(to_exc_.data()), to_exc_n_);
  to_exc_n_ = 0;
  if (exc_started_) {
    static_cast<void>(::rte_eth_dev_stop(exc_));
    exc_started_ = false;
  }
  exc_ = kNoPort;
  exc_ifname_.clear();
  if (started_) {
    static_cast<void>(::rte_eth_dev_stop(port_));
    started_ = false;
  }
  if (pool_ != nullptr) ::rte_mempool_free(static_cast<rte_mempool*>(pool_));
  pool_ = nullptr;
  routes_.clear();
  unicast_.clear();
}

std::uint32_t DpdkDatagramSource::rx_burst() noexcept {
  if (FASTMM_UNLIKELY(!started_)) return 0;
  // Non-EAL threads get an lcore id so the mempool's per-lcore cache is used.
  if (FASTMM_UNLIKELY(::rte_lcore_id() == LCORE_ID_ANY)) static_cast<void>(::rte_thread_register());
  auto** m = reinterpret_cast<rte_mbuf**>(mbufs_.data());
  const std::uint16_t n = ::rte_eth_rx_burst(port_, 0, m, batch_);
  for (std::uint16_t i = 0; i < n; ++i) {
    const rte_mbuf* b = m[i];
    if (FASTMM_UNLIKELY(b->nb_segs != 1)) {
      frames_[i] = {};
      continue;
    }
    frames_[i] = std::span<const std::byte>(rte_pktmbuf_mtod(b, const std::byte*), b->data_len);
  }
  return n;
}

void DpdkDatagramSource::free_burst(std::uint32_t n) noexcept {
  // Diverted mbufs are null here; rte_pktmbuf_free_bulk skips them.
  ::rte_pktmbuf_free_bulk(reinterpret_cast<rte_mbuf**>(mbufs_.data()), n);
}

void DpdkDatagramSource::divert(std::uint32_t i) noexcept {
  const std::span<const std::byte> f = frames_[i];
  if (f.size() < kEth) return;
  const auto to_kernel = [this, i]() noexcept {
    to_exc_[to_exc_n_++] = mbufs_[i];
    mbufs_[i] = nullptr;
    ++stats_.to_kernel;
  };
  const std::uint16_t type = be16(f.data() + 12);
  if (type == kEtherArp) {
    if (exc_ != kNoPort) {
      to_kernel();
      return;
    }
    // Requests for a unicast subscription address.
    if (f.size() < kEth + 28 || unicast_.empty()) return;
    const std::byte* a = f.data() + kEth;
    if (be16(a) != 1 || be16(a + 2) != kEtherIpv4 || be16(a + 6) != 1) return;
    const std::uint32_t tpa = raw32(a + 24);
    if (std::find(unicast_.begin(), unicast_.end(), tpa) == unicast_.end()) return;
    std::byte r[60] = {};
    std::memcpy(r, a + 8, 6);  // to the requester
    std::memcpy(r + 6, mac_.data(), 6);
    r[12] = std::byte{0x08};
    r[13] = std::byte{0x06};
    std::byte* ra = r + kEth;
    std::memcpy(ra, a, 6);  // htype, ptype, hlen, plen as asked
    ra[6] = std::byte{0};
    ra[7] = std::byte{2};
    std::memcpy(ra + 8, mac_.data(), 6);
    std::memcpy(ra + 14, a + 24, 4);  // spa: the address asked for
    std::memcpy(ra + 18, a + 8, 10);  // tha, tpa: the requester's
    if (send_frame(std::span<const std::byte>(r, sizeof r))) ++stats_.arp_replies;
    return;
  }
  if (exc_ != kNoPort) to_kernel();
}

void DpdkDatagramSource::service_exception() noexcept {
  if (to_exc_n_ != 0) {
    auto** m = reinterpret_cast<rte_mbuf**>(to_exc_.data());
    const std::uint16_t sent =
        ::rte_eth_tx_burst(exc_, 0, m, static_cast<std::uint16_t>(to_exc_n_));
    if (sent < to_exc_n_) ::rte_pktmbuf_free_bulk(m + sent, to_exc_n_ - sent);
    to_exc_n_ = 0;
  }
  const std::uint64_t now = ::rte_rdtsc();
  if (now < exc_next_) return;
  exc_next_ = now + exc_interval_cycles_;
  auto** m = reinterpret_cast<rte_mbuf**>(exc_rx_.data());
  const std::uint16_t n = ::rte_eth_rx_burst(exc_, 0, m, kExcBurst);
  if (n == 0) return;
  const std::uint16_t sent = ::rte_eth_tx_burst(port_, 0, m, n);
  if (sent < n) ::rte_pktmbuf_free_bulk(m + sent, n - sent);
  stats_.from_kernel += sent;
  if (n == kExcBurst) exc_next_ = 0;  // more waiting: read again on the next poll
}

bool DpdkDatagramSource::send_frame(std::span<const std::byte> frame) noexcept {
  rte_mbuf* m = ::rte_pktmbuf_alloc(static_cast<rte_mempool*>(pool_));
  if (FASTMM_UNLIKELY(m == nullptr)) {
    ++stats_.tx_drops;
    return false;
  }
  char* p = ::rte_pktmbuf_append(m, static_cast<std::uint16_t>(frame.size()));
  if (FASTMM_UNLIKELY(p == nullptr)) {
    ::rte_pktmbuf_free(m);
    ++stats_.tx_drops;
    return false;
  }
  std::memcpy(p, frame.data(), frame.size());
  if (FASTMM_UNLIKELY(::rte_eth_tx_burst(port_, 0, &m, 1) != 1)) {
    ::rte_pktmbuf_free(m);
    ++stats_.tx_drops;
    return false;
  }
  return true;
}

int DpdkDatagramSource::refresh_stats() noexcept {
  if (!started_) return -ENODEV;
  rte_eth_stats s{};
  const int r = ::rte_eth_stats_get(port_, &s);
  if (r != 0) return r;
  stats_.ipackets = s.ipackets;
  stats_.imissed = s.imissed;
  stats_.ierrors = s.ierrors;
  stats_.rx_nombuf = s.rx_nombuf;
  return 0;
}

}  // namespace fastmm::net
