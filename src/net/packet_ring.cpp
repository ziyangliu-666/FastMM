// PacketRing (see packet_ring.hpp).
#include "fastmm/net/packet_ring.hpp"

#include <arpa/inet.h>
#include <linux/filter.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

// manylinux's kernel headers predate this one (Linux 4.20).
#ifndef PACKET_IGNORE_OUTGOING
#define PACKET_IGNORE_OUTGOING 23  // NOLINT(cppcoreguidelines-macro-usage): <linux/if_packet.h>
#endif

#include <cerrno>
#include <cstring>

namespace fastmm::net {

namespace {

constexpr std::uint32_t kBlockSize = 1U << 16;  // 32 frames of 2 KiB per block

int fail(std::string& err, const char* step) {
  const int e = errno;
  err = std::string(step) + ": " + std::strerror(e);
  return -e;
}

// Accepts ARP and IPv4 TCP to `ip_host` (host byte order; 0 = any IPv4 TCP), untagged frames.
void build_filter(std::uint32_t ip_host, sock_filter (&f)[8], unsigned& len) {
  unsigned i = 0;
  f[i++] = BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12);                    // EtherType
  f[i++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETHERTYPE_ARP, 5, 0);  // -> accept
  f[i++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETHERTYPE_IP, 0, 5);   // -> drop
  f[i++] = BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 23);                    // IPv4 protocol
  f[i++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_TCP, 0, 3);    // -> drop
  f[i++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 30);                    // IPv4 destination
  f[i++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ip_host, 0, 1);        // -> drop
  f[i++] = BPF_STMT(BPF_RET | BPF_K, 0xFFFF);                         // accept
  len = i;
  if (ip_host == 0) f[6] = BPF_JUMP(BPF_JMP | BPF_JA, 0, 0, 0);
}

}  // namespace

int PacketRing::open(const PacketRingConfig& cfg, std::string& err) {
  close();
  const unsigned ifindex = ::if_nametoindex(cfg.interface.c_str());
  if (ifindex == 0) return fail(err, ("interface " + cfg.interface).c_str());
  // Protocol 0: nothing is received before bind(), so no frame skips the filter.
  const int fd = ::socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return fail(err, "socket(AF_PACKET) (needs CAP_NET_RAW)");
  fd_ = fd;
  const int version = TPACKET_V2;
  if (::setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof version) != 0) {
    const int r = fail(err, "PACKET_VERSION");
    close();
    return r;
  }
  sock_filter code[8];
  unsigned len = 0;
  // The last drop target: an extra instruction after accept.
  sock_filter prog[9];
  build_filter(ntohl(cfg.local_ip), code, len);
  for (unsigned k = 0; k < len; ++k) prog[k] = code[k];
  prog[len] = BPF_STMT(BPF_RET | BPF_K, 0);  // drop
  sock_fprog fprog{static_cast<unsigned short>(len + 1), prog};
  if (::setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof fprog) != 0) {
    const int r = fail(err, "SO_ATTACH_FILTER");
    close();
    return r;
  }
  const std::uint32_t per_block = kBlockSize / kFrameSize;
  const std::uint32_t rx_blocks = (cfg.rx_frames + per_block - 1) / per_block;
  const std::uint32_t tx_blocks = (cfg.tx_frames + per_block - 1) / per_block;
  tpacket_req rx{kBlockSize, rx_blocks, kFrameSize, rx_blocks * per_block};
  tpacket_req tx{kBlockSize, tx_blocks, kFrameSize, tx_blocks * per_block};
  if (::setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &rx, sizeof rx) != 0) {
    const int r = fail(err, "PACKET_RX_RING");
    close();
    return r;
  }
  if (::setsockopt(fd, SOL_PACKET, PACKET_TX_RING, &tx, sizeof tx) != 0) {
    const int r = fail(err, "PACKET_TX_RING");
    close();
    return r;
  }
  if (cfg.qdisc_bypass) {
    const int one = 1;
    static_cast<void>(::setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &one, sizeof one));
  }
  {
    const int one = 1;
    static_cast<void>(::setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one));
  }
  map_len_ = static_cast<std::size_t>(rx_blocks + tx_blocks) * kBlockSize;
  void* m = ::mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
  if (m == MAP_FAILED) {
    map_len_ = 0;
    const int r = fail(err, "mmap");
    close();
    return r;
  }
  map_ = static_cast<std::byte*>(m);
  rx_ = map_;
  tx_ = map_ + static_cast<std::size_t>(rx_blocks) * kBlockSize;
  rx_count_ = rx_blocks * per_block;
  tx_count_ = tx_blocks * per_block;
  rx_idx_ = 0;
  tx_idx_ = 0;
  tx_pending_ = 0;
  sockaddr_ll sll{};
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex = static_cast<int>(ifindex);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&sll), sizeof sll) != 0) {
    const int r = fail(err, "bind");
    close();
    return r;
  }
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, cfg.interface.c_str(), IFNAMSIZ - 1);
  if (::ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
    const int r = fail(err, "SIOCGIFHWADDR");
    close();
    return r;
  }
  std::memcpy(mac_.data(), ifr.ifr_hwaddr.sa_data, 6);
  if (::ioctl(fd, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu > 0) {
    mtu_ = static_cast<std::uint32_t>(ifr.ifr_mtu);
  }
  ifindex_ = ifindex;
  return 0;
}

void PacketRing::close() noexcept {
  if (map_ != nullptr) ::munmap(map_, map_len_);
  map_ = nullptr;
  map_len_ = 0;
  rx_ = nullptr;
  tx_ = nullptr;
  rx_count_ = 0;
  tx_count_ = 0;
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool PacketRing::send_frame(std::span<const std::byte> frame) noexcept {
  constexpr std::size_t kData =
      (sizeof(tpacket2_hdr) + TPACKET_ALIGNMENT - 1) & ~std::size_t{TPACKET_ALIGNMENT - 1};
  if (tx_ == nullptr || frame.size() > kFrameSize - kData) return false;
  auto* hdr = reinterpret_cast<tpacket2_hdr*>(tx_ + static_cast<std::size_t>(tx_idx_) * kFrameSize);
  std::atomic_ref<std::uint32_t> status(hdr->tp_status);
  const std::uint32_t st = status.load(std::memory_order_acquire);
  if (st != TP_STATUS_AVAILABLE && st != TP_STATUS_WRONG_FORMAT) {
    ++stats_.tx_ring_full;
    return false;
  }
  std::memcpy(reinterpret_cast<std::byte*>(hdr) + kData, frame.data(), frame.size());
  hdr->tp_len = static_cast<std::uint32_t>(frame.size());
  status.store(TP_STATUS_SEND_REQUEST, std::memory_order_release);
  tx_idx_ = tx_idx_ + 1 == tx_count_ ? 0 : tx_idx_ + 1;
  ++tx_pending_;
  return true;
}

void PacketRing::flush() noexcept {
  if (tx_pending_ == 0) return;
  stats_.tx_frames += tx_pending_;
  tx_pending_ = 0;
  if (::sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0) < 0 && errno != EAGAIN) {
    ++stats_.tx_errors;
    stats_.last_error = errno;
  }
}

}  // namespace fastmm::net
