// AF_XDP user-space frame checks (net/udp_frame.hpp): Ethernet/IPv4/UDP parse per received frame,
// without and with checksum verification. The receive path itself needs root (scripts/xdp-test.sh,
// bench-e2e).
#include "fastmm/net/udp_frame.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace fastmm::net;

namespace {

// A MoldUDP64-sized datagram: 14 + 20 + 8 + payload.
std::vector<std::byte> make_frame(std::size_t payload) {
  std::vector<std::byte> f(42 + payload, std::byte{0x5A});
  const auto put16 = [&f](std::size_t at, std::uint16_t v) {
    f[at] = static_cast<std::byte>(v >> 8U);
    f[at + 1] = static_cast<std::byte>(v & 0xFFU);
  };
  put16(12, 0x0800);
  f[14] = std::byte{0x45};
  put16(16, static_cast<std::uint16_t>(28 + payload));
  put16(20, 0x4000);
  f[22] = std::byte{1};
  f[23] = std::byte{17};
  const std::byte src[4] = {std::byte{10}, std::byte{0}, std::byte{0}, std::byte{1}};
  const std::byte dst[4] = {std::byte{239}, std::byte{1}, std::byte{1}, std::byte{1}};
  for (std::size_t k = 0; k < 4; ++k) {
    f[26 + k] = src[k];
    f[30 + k] = dst[k];
  }
  put16(24, 0);
  put16(24, ipv4_header_checksum(std::span<const std::byte>(f).subspan(14, 20)));
  put16(34, 40000);
  put16(36, 31001);
  put16(38, static_cast<std::uint16_t>(8 + payload));
  std::uint32_t s = 0;
  std::uint32_t d = 0;
  std::memcpy(&s, src, 4);
  std::memcpy(&d, dst, 4);
  put16(40, 0);
  put16(40, udp_checksum(s, d, std::span<const std::byte>(f).subspan(34)));
  return f;
}

void BM_UdpFrame_Parse(benchmark::State& state) {
  const auto f = make_frame(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    UdpFrame p = parse_udp_frame(f, false);
    benchmark::DoNotOptimize(p);
  }
}
BENCHMARK(BM_UdpFrame_Parse)->Arg(200)->Arg(1400);

void BM_UdpFrame_ParseVerifyChecksums(benchmark::State& state) {
  const auto f = make_frame(static_cast<std::size_t>(state.range(0)));
  if (parse_udp_frame(f, true).status != FrameStatus::Ok) {
    state.SkipWithError("frame does not verify");
    return;
  }
  for (auto _ : state) {
    UdpFrame p = parse_udp_frame(f, true);
    benchmark::DoNotOptimize(p);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(f.size()));
}
BENCHMARK(BM_UdpFrame_ParseVerifyChecksums)->Arg(200)->Arg(1400);

}  // namespace
