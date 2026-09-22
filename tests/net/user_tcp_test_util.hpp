#pragma once
// Frame helpers for the UserTcp tests: a scripted peer builds Ethernet/IPv4/TCP and ARP frames,
// and parses what the engine sends (checking both checksums).
#include "fastmm/net/user_tcp.hpp"

#include <doctest/doctest.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::net::test {

inline constexpr std::uint8_t kFin = 0x01;
inline constexpr std::uint8_t kSyn = 0x02;
inline constexpr std::uint8_t kRst = 0x04;
inline constexpr std::uint8_t kPsh = 0x08;
inline constexpr std::uint8_t kAck = 0x10;

inline std::uint32_t ip4(const char* text) {
  in_addr a{};
  REQUIRE(::inet_pton(AF_INET, text, &a) == 1);
  return a.s_addr;
}

inline std::uint64_t csum_add(std::uint64_t s, const std::uint8_t* p, std::size_t n) {
  std::size_t i = 0;
  for (; i + 1 < n; i += 2) s += static_cast<std::uint64_t>(p[i] << 8U | p[i + 1]);
  if (i < n) s += static_cast<std::uint64_t>(p[i]) << 8U;
  return s;
}
inline std::uint16_t csum_fold(std::uint64_t s) {
  while ((s >> 16U) != 0) s = (s & 0xFFFFU) + (s >> 16U);
  return static_cast<std::uint16_t>(s);
}

struct TcpFrame {
  bool tcp = false;
  bool arp = false;
  bool csum_ok = false;
  std::uint16_t arp_op = 0;
  std::uint32_t arp_tpa = 0;
  std::uint8_t dst_mac[6] = {};
  std::uint32_t src_ip = 0;
  std::uint32_t dst_ip = 0;
  std::uint16_t sport = 0;
  std::uint16_t dport = 0;
  std::uint32_t seq = 0;
  std::uint32_t ack = 0;
  std::uint8_t flags = 0;
  std::uint16_t wnd = 0;
  std::uint16_t mss = 0;
  std::string data;
  [[nodiscard]] bool has(std::uint8_t f) const { return (flags & f) != 0; }
};

inline TcpFrame parse(std::span<const std::byte> frame) {
  TcpFrame r;
  const auto* f = reinterpret_cast<const std::uint8_t*>(frame.data());
  REQUIRE(frame.size() >= 14);
  std::memcpy(r.dst_mac, f, 6);
  const unsigned type = f[12] << 8U | f[13];
  if (type == 0x0806) {
    r.arp = true;
    r.arp_op = static_cast<std::uint16_t>(f[20] << 8U | f[21]);
    std::memcpy(&r.arp_tpa, f + 38, 4);
    return r;
  }
  REQUIRE(type == 0x0800);
  const std::uint8_t* ip = f + 14;
  const std::size_t ihl = (ip[0] & 0x0FU) * 4U;
  const std::size_t total = static_cast<std::size_t>(ip[2] << 8U | ip[3]);
  REQUIRE(14 + total <= frame.size());
  REQUIRE(ip[9] == 6);
  std::memcpy(&r.src_ip, ip + 12, 4);
  std::memcpy(&r.dst_ip, ip + 16, 4);
  const std::uint8_t* t = ip + ihl;
  const std::size_t tcp_len = total - ihl;
  const std::size_t thl = (t[12] >> 4U) * 4U;
  std::uint64_t ps = csum_add(0, ip + 12, 8) + 6U + tcp_len;
  r.csum_ok =
      csum_fold(csum_add(0, ip, ihl)) == 0xFFFF && csum_fold(csum_add(ps, t, tcp_len)) == 0xFFFF;
  r.tcp = true;
  r.sport = static_cast<std::uint16_t>(t[0] << 8U | t[1]);
  r.dport = static_cast<std::uint16_t>(t[2] << 8U | t[3]);
  r.seq = static_cast<std::uint32_t>(t[4]) << 24U | static_cast<std::uint32_t>(t[5]) << 16U |
          static_cast<std::uint32_t>(t[6]) << 8U | t[7];
  r.ack = static_cast<std::uint32_t>(t[8]) << 24U | static_cast<std::uint32_t>(t[9]) << 16U |
          static_cast<std::uint32_t>(t[10]) << 8U | t[11];
  r.flags = t[13];
  r.wnd = static_cast<std::uint16_t>(t[14] << 8U | t[15]);
  if (thl >= 24 && t[20] == 2 && t[21] == 4)
    r.mss = static_cast<std::uint16_t>(t[22] << 8U | t[23]);
  r.data.assign(reinterpret_cast<const char*>(t + thl), tcp_len - thl);
  return r;
}

// The peer side of a scripted exchange.
struct Peer {
  MacAddr mac{0x02, 0, 0, 0, 0, 0x01};
  MacAddr our_mac{0x02, 0, 0, 0, 0, 0x02};
  std::uint32_t ip = ip4("10.9.0.1");
  std::uint32_t our_ip = ip4("10.9.0.2");
  std::uint16_t port = 5000;

  std::vector<std::byte> tcp(std::uint16_t dport,
                             std::uint32_t seq,
                             std::uint32_t ack,
                             std::uint8_t flags,
                             std::uint16_t wnd,
                             std::string_view data = {},
                             std::uint16_t mss = 0,
                             bool corrupt = false) const {
    const std::size_t thl = mss != 0 ? 24 : 20;
    const std::size_t total = 20 + thl + data.size();
    std::vector<std::uint8_t> f(14 + total, 0);
    std::copy_n(our_mac.begin(), 6, f.begin());
    std::copy_n(mac.begin(), 6, f.begin() + 6);
    f[12] = 0x08;
    std::uint8_t* ipp = f.data() + 14;
    ipp[0] = 0x45;
    ipp[2] = static_cast<std::uint8_t>(total >> 8U);
    ipp[3] = static_cast<std::uint8_t>(total);
    ipp[8] = 64;
    ipp[9] = 6;
    std::memcpy(ipp + 12, &ip, 4);
    std::memcpy(ipp + 16, &our_ip, 4);
    const std::uint16_t ic = static_cast<std::uint16_t>(~csum_fold(csum_add(0, ipp, 20)));
    ipp[10] = static_cast<std::uint8_t>(ic >> 8U);
    ipp[11] = static_cast<std::uint8_t>(ic);
    std::uint8_t* t = ipp + 20;
    t[0] = static_cast<std::uint8_t>(port >> 8U);
    t[1] = static_cast<std::uint8_t>(port);
    t[2] = static_cast<std::uint8_t>(dport >> 8U);
    t[3] = static_cast<std::uint8_t>(dport);
    for (int k = 0; k < 4; ++k) {
      t[4 + k] = static_cast<std::uint8_t>(seq >> (24 - 8 * k));
      t[8 + k] = static_cast<std::uint8_t>(ack >> (24 - 8 * k));
    }
    t[12] = static_cast<std::uint8_t>((thl / 4) << 4U);
    t[13] = flags;
    t[14] = static_cast<std::uint8_t>(wnd >> 8U);
    t[15] = static_cast<std::uint8_t>(wnd);
    if (mss != 0) {
      t[20] = 2;
      t[21] = 4;
      t[22] = static_cast<std::uint8_t>(mss >> 8U);
      t[23] = static_cast<std::uint8_t>(mss);
    }
    std::memcpy(t + thl, data.data(), data.size());
    const std::size_t tl = thl + data.size();
    const std::uint64_t ps = csum_add(0, ipp + 12, 8) + 6U + tl;
    std::uint16_t c = static_cast<std::uint16_t>(~csum_fold(csum_add(ps, t, tl)));
    if (corrupt) c ^= 0x5555;
    t[16] = static_cast<std::uint8_t>(c >> 8U);
    t[17] = static_cast<std::uint8_t>(c);
    std::vector<std::byte> out(f.size());
    std::memcpy(out.data(), f.data(), f.size());
    return out;
  }

  std::vector<std::byte> arp(std::uint16_t op, std::uint32_t tpa) const {
    std::vector<std::uint8_t> f(60, 0);
    std::memset(f.data(), 0xFF, 6);
    std::copy_n(mac.begin(), 6, f.begin() + 6);
    f[12] = 0x08;
    f[13] = 0x06;
    std::uint8_t* a = f.data() + 14;
    a[1] = 1;
    a[2] = 0x08;
    a[4] = 6;
    a[5] = 4;
    a[7] = static_cast<std::uint8_t>(op);
    std::memcpy(a + 8, mac.data(), 6);
    std::memcpy(a + 14, &ip, 4);
    std::memcpy(a + 24, &tpa, 4);
    std::vector<std::byte> out(f.size());
    std::memcpy(out.data(), f.data(), f.size());
    return out;
  }
};

struct CaptureTx final : FrameTx {
  std::vector<std::vector<std::byte>> frames;
  std::size_t flushes = 0;
  bool send_frame(std::span<const std::byte> f) noexcept override {
    frames.emplace_back(f.begin(), f.end());
    return true;
  }
  void flush() noexcept override { ++flushes; }
  std::vector<TcpFrame> take() {
    std::vector<TcpFrame> out;
    for (const auto& f : frames) out.push_back(parse(f));
    frames.clear();
    return out;
  }
};

struct RecordingHandler final : UserTcpHandler {
  bool connected = false;
  int closed = -1;
  std::string data;
  std::size_t consume_limit = SIZE_MAX;
  void on_tcp_connected() noexcept override { connected = true; }
  std::size_t on_tcp_data(std::span<const std::byte> b) noexcept override {
    const std::size_t n = std::min(b.size(), consume_limit);
    data.append(reinterpret_cast<const char*>(b.data()), n);
    return n;
  }
  void on_tcp_closed(int err) noexcept override { closed = err; }
};

inline std::span<const std::byte> bytes_of(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace fastmm::net::test
