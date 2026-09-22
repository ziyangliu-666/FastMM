#pragma once
// Ethernet / 802.1Q / IPv4 / UDP frame parser for the AF_XDP receive path (ADR-0015, section 3).
// Pure functions over a byte span: no allocation, no system calls.
//
// The checks run in two stages. The first repeats what the XDP program checks before it looks
// up the destination (Ethernet, at most one 802.1Q tag, IPv4 version and IHL, no fragment, UDP,
// a full UDP header after the IPv4 options). The second checks what the program does not: IPv4
// total length, UDP length and, when asked, the checksums. passes_xdp_filter() tells the stages
// apart, so tests can compare the program's verdicts with this parser.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::net {

enum class FrameStatus : std::uint8_t {
  Ok,
  // First stage: the XDP program passes these frames to the kernel.
  Truncated,    // shorter than the headers it announces
  NotIpv4,      // EtherType is not IPv4 (after at most one 802.1Q tag)
  BadIpHeader,  // version != 4 or IHL < 5
  Fragment,     // MF set or non-zero fragment offset
  NotUdp,       // IPv4 protocol != 17
  // Second stage: the XDP program redirects these frames when the destination matches.
  BadLength,    // IPv4 total length or UDP length inconsistent with the frame
  BadChecksum,  // IPv4 header or UDP checksum wrong (only with verify_checksums)
};

// True when the XDP program looks the frame's destination up (and redirects it on a match).
[[nodiscard]] constexpr bool passes_xdp_filter(FrameStatus s) noexcept {
  return s == FrameStatus::Ok || s == FrameStatus::BadLength || s == FrameStatus::BadChecksum;
}

struct UdpFrame {
  FrameStatus status = FrameStatus::Truncated;
  std::uint32_t src_ip = 0;    // network byte order
  std::uint32_t dst_ip = 0;    // network byte order
  std::uint16_t src_port = 0;  // host byte order
  std::uint16_t dst_port = 0;  // host byte order
  bool vlan = false;
  std::span<const std::byte> payload;  // UDP payload, set when status == Ok
};

namespace detail {

inline constexpr std::size_t kEthHeader = 14;
inline constexpr std::size_t kVlanTag = 4;
inline constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr std::uint16_t kEtherTypeVlan = 0x8100;
inline constexpr std::uint8_t kIpProtoUdp = 17;

[[nodiscard]] constexpr std::uint16_t load_be16(std::span<const std::byte> b,
                                                std::size_t at) noexcept {
  return static_cast<std::uint16_t>((std::to_integer<unsigned>(b[at]) << 8U) |
                                    std::to_integer<unsigned>(b[at + 1]));
}
// The four bytes at `at` as they sit in memory: a network-byte-order address.
[[nodiscard]] constexpr std::uint32_t load_raw32(std::span<const std::byte> b,
                                                 std::size_t at) noexcept {
  std::uint32_t v = 0;
  for (unsigned k = 0; k < 4; ++k) {
    const unsigned shift = std::endian::native == std::endian::little ? 8U * k : 8U * (3U - k);
    v |= std::to_integer<std::uint32_t>(b[at + k]) << shift;
  }
  return v;
}

// One's-complement sum of big-endian 16-bit words, not folded.
[[nodiscard]] constexpr std::uint64_t csum_add(std::uint64_t sum,
                                               std::span<const std::byte> b) noexcept {
  std::size_t i = 0;
  for (; i + 1 < b.size(); i += 2) sum += load_be16(b, i);
  if (i < b.size()) sum += std::to_integer<std::uint64_t>(b[i]) << 8U;
  return sum;
}
[[nodiscard]] constexpr std::uint16_t csum_fold(std::uint64_t sum) noexcept {
  while ((sum >> 16U) != 0) sum = (sum & 0xFFFFU) + (sum >> 16U);
  return static_cast<std::uint16_t>(sum);
}

}  // namespace detail

// RFC 768 checksum of a UDP datagram (header with its checksum field included, and payload) under
// the IPv4 pseudo-header. Addresses in network byte order. A correct datagram yields 0xFFFF
// (or carries checksum 0: not computed).
[[nodiscard]] constexpr std::uint16_t udp_checksum_sum(std::uint32_t src_ip_be,
                                                       std::uint32_t dst_ip_be,
                                                       std::span<const std::byte> udp) noexcept {
  std::byte pseudo[12] = {};
  const auto put32 = [&pseudo](std::size_t at, std::uint32_t raw) {
    for (unsigned k = 0; k < 4; ++k) {
      const unsigned shift = std::endian::native == std::endian::little ? 8U * k : 8U * (3U - k);
      pseudo[at + k] = static_cast<std::byte>((raw >> shift) & 0xFFU);
    }
  };
  put32(0, src_ip_be);
  put32(4, dst_ip_be);
  pseudo[9] = std::byte{detail::kIpProtoUdp};
  pseudo[10] = static_cast<std::byte>(udp.size() >> 8U);
  pseudo[11] = static_cast<std::byte>(udp.size() & 0xFFU);
  std::uint64_t sum = detail::csum_add(0, pseudo);
  sum = detail::csum_add(sum, udp);
  return detail::csum_fold(sum);
}

// Checksum field value for a UDP datagram whose checksum field is zero (0 maps to 0xFFFF).
[[nodiscard]] constexpr std::uint16_t udp_checksum(std::uint32_t src_ip_be,
                                                   std::uint32_t dst_ip_be,
                                                   std::span<const std::byte> udp) noexcept {
  const auto c = static_cast<std::uint16_t>(~udp_checksum_sum(src_ip_be, dst_ip_be, udp));
  return c == 0 ? std::uint16_t{0xFFFF} : c;
}

// Checksum field value for an IPv4 header whose checksum field is zero.
[[nodiscard]] constexpr std::uint16_t ipv4_header_checksum(
    std::span<const std::byte> header) noexcept {
  return static_cast<std::uint16_t>(~detail::csum_fold(detail::csum_add(0, header)));
}

[[nodiscard]] constexpr UdpFrame parse_udp_frame(std::span<const std::byte> f,
                                                 bool verify_checksums = false) noexcept {
  using namespace detail;
  UdpFrame r;
  if (f.size() < kEthHeader) return r;
  std::size_t l3 = kEthHeader;
  std::uint16_t ethertype = load_be16(f, 12);
  if (ethertype == kEtherTypeVlan) {
    if (f.size() < kEthHeader + kVlanTag) return r;
    ethertype = load_be16(f, 16);
    l3 += kVlanTag;
    r.vlan = true;
  }
  if (ethertype != kEtherTypeIpv4) {
    r.status = FrameStatus::NotIpv4;
    return r;
  }
  if (f.size() < l3 + 20) return r;
  const unsigned ver_ihl = std::to_integer<unsigned>(f[l3]);
  const std::size_t ihl = static_cast<std::size_t>(ver_ihl & 0x0FU) * 4;
  if ((ver_ihl >> 4U) != 4 || ihl < 20) {
    r.status = FrameStatus::BadIpHeader;
    return r;
  }
  if ((load_be16(f, l3 + 6) & 0x3FFFU) != 0) {
    r.status = FrameStatus::Fragment;
    return r;
  }
  if (std::to_integer<std::uint8_t>(f[l3 + 9]) != kIpProtoUdp) {
    r.status = FrameStatus::NotUdp;
    return r;
  }
  const std::size_t l4 = l3 + ihl;
  if (f.size() < l4 + 8) return r;
  r.src_ip = load_raw32(f, l3 + 12);
  r.dst_ip = load_raw32(f, l3 + 16);
  r.src_port = load_be16(f, l4);
  r.dst_port = load_be16(f, l4 + 2);

  // Second stage. Frames may carry Ethernet padding past the IPv4 total length.
  const std::size_t total = load_be16(f, l3 + 2);
  const std::size_t udp_len = load_be16(f, l4 + 4);
  if (total < ihl + 8 || l3 + total > f.size() || udp_len < 8 || udp_len > total - ihl) {
    r.status = FrameStatus::BadLength;
    return r;
  }
  const auto udp = f.subspan(l4, udp_len);
  if (verify_checksums) {
    const bool ip_ok = csum_fold(csum_add(0, f.subspan(l3, ihl))) == 0xFFFF;
    const bool udp_ok =
        load_be16(f, l4 + 6) == 0 || udp_checksum_sum(r.src_ip, r.dst_ip, udp) == 0xFFFF;
    if (!ip_ok || !udp_ok) {
      r.status = FrameStatus::BadChecksum;
      return r;
    }
  }
  r.payload = udp.subspan(8);
  r.status = FrameStatus::Ok;
  return r;
}

}  // namespace fastmm::net
