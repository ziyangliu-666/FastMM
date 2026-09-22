// UDP receive over loopback: net::UdpSocket unicast and net::KernelDatagramSource multicast.
//
//   BM_UdpUnicast             sendto, then recvfrom until the datagram is back (one thread)
//   BM_KernelSourceMulticast  sendto to a joined group on lo, then poll() until it is delivered;
//                             counters also give the kernel receive stamp -> T0 hop (sw_t0_*)
//   BM_KernelSourceBurst/<n>  n datagrams sent, then poll() until all are delivered (batch 32)
//
// The multicast benchmarks run in a user and network namespace entered at the first multicast
// benchmark (tests/net/netns.hpp; unprivileged where user namespaces are allowed) and are skipped
// when that fails. Counters p50 / p99 are per-iteration times in ns. Payload 64 bytes.
#include "../tests/net/netns.hpp"

#include "fastmm/core/latency.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/kernel_datagram_source.hpp"
#include "fastmm/net/udp_socket.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

using namespace fastmm;
using namespace fastmm::net;

namespace {

constexpr std::size_t kMsgSize = 64;
constexpr int kMaxSpins = 10'000'000;  // a lost datagram ends the benchmark with an error

void report(benchmark::State& state, const LogLinearHistogram& h, const char* prefix = "") {
  state.counters[std::string(prefix) + "p50"] = static_cast<double>(h.percentile(0.50));
  state.counters[std::string(prefix) + "p99"] = static_cast<double>(h.percentile(0.99));
}

// "" once the process is in the namespace; the error otherwise (the attempt is made once).
const std::string& netns_error() {
  static const std::string err = test::enter_multicast_netns();
  return err;
}

void BM_UdpUnicast(benchmark::State& state) {
  UdpSocket rx = UdpSocket::open();
  UdpSocket tx = UdpSocket::open();
  if (rx.bind(SockAddr::loopback_v4(0)) != 0 || tx.connect(*rx.local_addr()) != 0) {
    state.SkipWithError("loopback socket setup failed");
    return;
  }
  std::array<std::byte, kMsgSize> msg{};
  std::array<std::byte, 2048> buf{};
  LogLinearHistogram hist;
  for (auto _ : state) {
    const std::int64_t t0 = steady_now().ns;
    if (!tx.send(msg).ok()) {
      state.SkipWithError("send failed");
      break;
    }
    int spins = 0;
    while (rx.recv_from(buf).would_block() && ++spins < kMaxSpins) {
    }
    if (spins == kMaxSpins) {
      state.SkipWithError("datagram lost");
      break;
    }
    hist.record(static_cast<std::uint64_t>(steady_now().ns - t0));
  }
  report(state, hist);
}
BENCHMARK(BM_UdpUnicast)->UseRealTime();

struct Multicast {
  KernelDatagramSource src;
  UdpSocket tx;
  SockAddr group;
};

bool open_multicast(benchmark::State& state, Multicast& m) {
  if (!netns_error().empty()) {
    state.SkipWithMessage("no user and network namespace: " + netns_error());
    return false;
  }
  KernelSourceConfig cfg;
  cfg.subscriptions = {{.interface = "lo", .group = "239.3.3.1", .port = 31000, .source = ""}};
  McastInterface lo;
  if (!m.src.open(cfg).ok() || McastInterface::resolve("lo", lo) != 0) {
    state.SkipWithError("KernelDatagramSource::open failed");
    return false;
  }
  m.tx = UdpSocket::open();
  if (m.tx.set_multicast_if(lo) != 0 || m.tx.set_multicast_loop(true) != 0) {
    state.SkipWithError("multicast sender setup failed");
    return false;
  }
  m.group = *SockAddr::from_ip("239.3.3.1", 31000);
  return true;
}

void BM_KernelSourceMulticast(benchmark::State& state) {
  Multicast m;
  if (!open_multicast(state, m)) return;
  std::array<std::byte, kMsgSize> msg{};
  LogLinearHistogram hist;
  LogLinearHistogram sw_to_t0;
  std::size_t got = 0;
  auto handler = [&](std::span<const std::byte>, const RxMeta& meta) noexcept {
    ++got;
    if (meta.sw_ts_ns != 0)
      sw_to_t0.record(static_cast<std::uint64_t>(meta.t0_wall_ns - meta.sw_ts_ns));
  };
  for (auto _ : state) {
    const std::int64_t t0 = steady_now().ns;
    if (!m.tx.send_to(msg, m.group).ok()) {
      state.SkipWithError("send failed");
      break;
    }
    const std::size_t want = got + 1;
    for (int spins = 0; got < want && spins < kMaxSpins; ++spins) m.src.poll(handler);
    if (got < want) {
      state.SkipWithError("datagram lost");
      break;
    }
    hist.record(static_cast<std::uint64_t>(steady_now().ns - t0));
  }
  report(state, hist);
  report(state, sw_to_t0, "sw_t0_");
}
BENCHMARK(BM_KernelSourceMulticast)->UseRealTime();

void BM_KernelSourceBurst(benchmark::State& state) {
  Multicast m;
  if (!open_multicast(state, m)) return;
  const auto n = static_cast<std::size_t>(state.range(0));
  std::array<std::byte, kMsgSize> msg{};
  std::size_t got = 0;
  auto handler = [&](std::span<const std::byte>, const RxMeta&) noexcept { ++got; };
  for (auto _ : state) {
    for (std::size_t i = 0; i < n; ++i) static_cast<void>(m.tx.send_to(msg, m.group));
    const std::size_t want = got + n;
    for (int spins = 0; got < want && spins < kMaxSpins; ++spins) m.src.poll(handler);
    if (got < want) {
      state.SkipWithError("datagram lost");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_KernelSourceBurst)->Arg(32)->UseRealTime();

}  // namespace
