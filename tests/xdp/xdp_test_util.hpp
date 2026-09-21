#pragma once
// Crafted Ethernet frames for the AF_XDP tests, and the case list that the parser, the BPF
// interpreter and BPF_PROG_TEST_RUN are all checked against.
#include "fastmm/net/udp_frame.hpp"
#include "fastmm/net/xdp_program.hpp"

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::net::test {

inline std::uint32_t ip(const char* dotted) {
  in_addr a{};
  ::inet_pton(AF_INET, dotted, &a);
  return a.s_addr;
}

struct FrameSpec {
  bool vlan = false;
  std::uint16_t ethertype = 0x0800;        // without the 802.1Q tag
  std::uint16_t inner_ethertype = 0x0800;  // after the 802.1Q tag
  std::uint8_t version = 4;
  std::uint8_t ihl = 5;
  std::uint16_t frag = 0x4000;  // DF, offset 0
  std::uint8_t proto = 17;
  std::uint32_t src_ip = ip("10.203.0.1");
  std::uint32_t dst_ip = ip("239.10.0.1");
  std::uint16_t src_port = 40000;
  std::uint16_t dst_port = 31001;
  std::string payload = "hello";
  int tot_len_delta = 0;  // added to the correct IPv4 total length
  int udp_len_delta = 0;  // added to the correct UDP length
  bool udp_checksum = true;
  bool bad_udp_checksum = false;
  bool bad_ip_checksum = false;
  std::size_t trailer = 0;  // Ethernet padding after the IPv4 packet
};

inline void put16(std::vector<std::byte>& f, std::size_t at, std::uint16_t v) {
  f[at] = static_cast<std::byte>(v >> 8U);
  f[at + 1] = static_cast<std::byte>(v & 0xFFU);
}
inline void put_raw32(std::vector<std::byte>& f, std::size_t at, std::uint32_t raw) {
  const auto* p = reinterpret_cast<const std::byte*>(&raw);
  for (std::size_t k = 0; k < 4; ++k) f[at + k] = p[k];
}

inline std::vector<std::byte> build_frame(const FrameSpec& s) {
  const std::size_t l3 = s.vlan ? 18 : 14;
  const std::size_t ihl = static_cast<std::size_t>(s.ihl & 0x0FU) * 4;
  const std::size_t iph = ihl < 20 ? 20 : ihl;
  const std::size_t udp_len = 8 + s.payload.size();
  std::vector<std::byte> f(l3 + iph + udp_len + s.trailer, std::byte{0});
  // Ethernet: multicast MAC of the group, locally administered source.
  const std::uint32_t host_dst = ntohl(s.dst_ip);
  const std::uint8_t mac[12] = {0x01,
                                0x00,
                                0x5e,
                                static_cast<std::uint8_t>((host_dst >> 16U) & 0x7FU),
                                static_cast<std::uint8_t>((host_dst >> 8U) & 0xFFU),
                                static_cast<std::uint8_t>(host_dst & 0xFFU),
                                0x02,
                                0,
                                0,
                                0,
                                0,
                                1};
  for (std::size_t k = 0; k < 12; ++k) f[k] = static_cast<std::byte>(mac[k]);
  put16(f, 12, s.vlan ? std::uint16_t{0x8100} : s.ethertype);
  if (s.vlan) {
    put16(f, 14, 0x0064);  // PCP 0, VID 100
    put16(f, 16, s.inner_ethertype);
  }
  // IPv4.
  f[l3] = static_cast<std::byte>((s.version << 4U) | (s.ihl & 0x0FU));
  const auto total = static_cast<int>(iph + udp_len) + s.tot_len_delta;
  put16(f, l3 + 2, static_cast<std::uint16_t>(total));
  put16(f, l3 + 4, 0x1234);
  put16(f, l3 + 6, s.frag);
  f[l3 + 8] = std::byte{1};
  f[l3 + 9] = static_cast<std::byte>(s.proto);
  put_raw32(f, l3 + 12, s.src_ip);
  put_raw32(f, l3 + 16, s.dst_ip);
  for (std::size_t k = 20; k < iph; ++k) f[l3 + k] = std::byte{1};  // NOP options
  std::uint16_t ipc = ipv4_header_checksum(std::span<const std::byte>(f).subspan(l3, iph));
  if (s.bad_ip_checksum) ipc ^= 0x0101U;
  put16(f, l3 + 10, ipc);
  // UDP.
  const std::size_t l4 = l3 + iph;
  put16(f, l4, s.src_port);
  put16(f, l4 + 2, s.dst_port);
  put16(f, l4 + 4, static_cast<std::uint16_t>(static_cast<int>(udp_len) + s.udp_len_delta));
  for (std::size_t k = 0; k < s.payload.size(); ++k)
    f[l4 + 8 + k] = static_cast<std::byte>(s.payload[k]);
  if (s.udp_checksum) {
    std::uint16_t c =
        udp_checksum(s.src_ip, s.dst_ip, std::span<const std::byte>(f).subspan(l4, udp_len));
    if (s.bad_udp_checksum) c ^= 0x00FFU;
    put16(f, l4 + 6, c);
  }
  return f;
}

struct FrameCase {
  const char* name;
  std::vector<std::byte> frame;
};

// Subscriptions the cases are matched against.
inline std::vector<xdp::Key> case_keys() {
  return {xdp::Key{ip("239.10.0.1"), htons(31001), 0}, xdp::Key{ip("239.10.0.2"), htons(31002), 0}};
}

inline bool key_matches(const UdpFrame& f) {
  for (const auto& k : case_keys())
    if (k.dst_ip == f.dst_ip && k.dst_port == htons(f.dst_port)) return true;
  return false;
}

inline std::vector<FrameCase> frame_cases() {
  std::vector<FrameCase> c;
  const auto add = [&c](const char* name, const FrameSpec& s) {
    c.push_back({name, build_frame(s)});
  };
  FrameSpec base;
  add("plain", base);
  {
    FrameSpec s;
    s.dst_ip = ip("239.10.0.2");
    s.dst_port = 31002;
    s.payload = std::string(1200, 'x');
    add("second line, 1200 bytes", s);
  }
  {
    FrameSpec s;
    s.vlan = true;
    add("802.1Q", s);
  }
  {
    FrameSpec s;
    s.vlan = true;
    s.inner_ethertype = 0x8100;
    add("double 802.1Q", s);
  }
  {
    FrameSpec s;
    s.ethertype = 0x88A8;
    add("802.1ad", s);
  }
  {
    FrameSpec s;
    s.ethertype = 0x0806;
    add("ARP ethertype", s);
  }
  {
    FrameSpec s;
    s.ethertype = 0x86DD;
    add("IPv6 ethertype", s);
  }
  for (const int ihl : {6, 10, 15}) {
    FrameSpec s;
    s.ihl = static_cast<std::uint8_t>(ihl);
    add(ihl == 15 ? "IHL 15 (40 option bytes)" : "IHL options", s);
    s.vlan = true;
    add("IHL options with 802.1Q", s);
  }
  {
    FrameSpec s;
    s.ihl = 4;
    add("IHL 4", s);
  }
  {
    FrameSpec s;
    s.ihl = 0;
    add("IHL 0", s);
  }
  {
    FrameSpec s;
    s.version = 6;
    add("version 6 in an IPv4 ethertype", s);
  }
  {
    FrameSpec s;
    s.frag = 0x2000;
    add("first fragment (MF)", s);
  }
  {
    FrameSpec s;
    s.frag = 0x0010;
    add("later fragment (offset)", s);
  }
  {
    FrameSpec s;
    s.frag = 0x1FFF;
    add("last fragment, max offset", s);
  }
  {
    FrameSpec s;
    s.frag = 0x8000;
    add("reserved flag only", s);
  }
  {
    FrameSpec s;
    s.proto = 6;
    add("TCP", s);
  }
  {
    FrameSpec s;
    s.dst_port = 31002;
    add("group of line 0, port of line 1", s);
  }
  {
    FrameSpec s;
    s.dst_ip = ip("239.10.0.9");
    add("unsubscribed group", s);
  }
  {
    FrameSpec s;
    s.dst_ip = ip("10.203.0.2");
    add("unicast", s);
  }
  {
    FrameSpec s;
    s.tot_len_delta = 5;
    add("total length past the frame", s);
  }
  {
    FrameSpec s;
    s.tot_len_delta = -3;
    add("total length short of UDP", s);
  }
  {
    FrameSpec s;
    s.udp_len_delta = 3;
    add("UDP length past IPv4", s);
  }
  {
    FrameSpec s;
    s.udp_len_delta = -10;
    add("UDP length below 8", s);
  }
  {
    FrameSpec s;
    s.udp_len_delta = -2;
    s.tot_len_delta = 0;
    add("UDP length shorter than IPv4 payload", s);
  }
  {
    FrameSpec s;
    s.payload = "";
    s.trailer = 18;
    add("empty payload with Ethernet padding", s);
  }
  {
    FrameSpec s;
    s.udp_checksum = false;
    add("UDP checksum 0", s);
  }
  {
    FrameSpec s;
    s.bad_udp_checksum = true;
    add("bad UDP checksum", s);
  }
  {
    FrameSpec s;
    s.bad_ip_checksum = true;
    add("bad IPv4 header checksum", s);
  }
  {
    FrameSpec s;
    s.payload = "odd";
    add("odd payload length", s);
  }
  return c;
}

}  // namespace fastmm::net::test
