// Order-entry send path over a veth pair: a kernel TCP socket against net::UserTcp over an
// AF_PACKET ring (order_transport = "user_tcp"), both talking to a kernel TCP server on the other
// end, all in one thread.
//
//   BM_KernelTcpSend  write(2) of a 64-byte message on a TCP_NODELAY socket, then read(2) on the
//                     server until it has the bytes
//   BM_UserTcpSend    UserTcp::send (segment built, TX ring slot, sendto kick), then the same
//                     server read; the ring is polled for the ACKs afterwards (not timed)
//
// Counters: send_p50 / send_p99 time the send call alone, p50 / p99 the send until the server's
// read returned (ns). On veth the send call runs the peer's receive path in the same system call,
// so most of the server side is inside the send. Client and server sit in two network namespaces
// of one user namespace (tests/net/netns.hpp); needs iproute2, nsenter and ethtool.
#include "../tests/net/netns.hpp"

#include "fastmm/core/latency.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/packet_ring.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/user_tcp.hpp"

#include <arpa/inet.h>
#include <sched.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::net;

namespace {

constexpr std::size_t kMsg = 64;

bool sh(const std::string& cmd) {
  return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
}

std::uint32_t ip4(const char* s) {
  in_addr a{};
  ::inet_pton(AF_INET, s, &a);
  return a.s_addr;
}

int listen_on(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = ip4("10.79.0.1");
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 4) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct Setup {
  std::string error;
  int kernel_listen = -1;  // port 7100
  int user_listen = -1;    // port 7101
};

// The server end lives in a second network namespace (entered by a helper thread, whose sockets
// stay there), so the kernel client crosses the veth instead of lo. Made once per process.
const Setup& setup() {
  static const Setup st = [] {
    Setup r;
    r.error = test::enter_multicast_netns();
    if (!r.error.empty()) return r;
    std::atomic<int> tid{0};
    std::atomic<bool> configured{false};
    std::thread server([&] {
      if (::unshare(CLONE_NEWNET) != 0) {
        tid = -1;
        return;
      }
      tid = static_cast<int>(::gettid());
      while (!configured.load()) std::this_thread::yield();
      r.kernel_listen = listen_on(7100);
      r.user_listen = listen_on(7101);
    });
    while (tid.load() == 0) std::this_thread::yield();
    const std::string ns = "nsenter --net=/proc/" + std::to_string(getpid()) + "/task/" +
                           std::to_string(tid.load()) + "/ns/net ";
    const bool ok = tid.load() > 0 && sh("ip link add obt0 type veth peer name obt1") &&
                    sh("ip link set obt1 netns " + std::to_string(tid.load())) &&
                    sh("ip addr add 10.79.0.2/24 dev obt0") && sh("ip link set obt0 up") &&
                    sh(ns + "ip addr add 10.79.0.1/24 dev obt1") &&
                    sh(ns + "ip link set obt1 up") && sh(ns + "ip link set lo up") &&
                    sh(ns + "ethtool -K obt1 tso off gso off");
    configured = true;
    server.join();
    if (!ok) r.error = "needs iproute2, nsenter and ethtool";
    if (r.error.empty() && (r.kernel_listen < 0 || r.user_listen < 0)) r.error = "listen failed";
    return r;
  }();
  return st;
}

// Reads until `n` bytes arrived; false after too many empty reads.
bool drain(int fd, std::size_t n) {
  std::array<char, 4096> buf{};
  std::size_t got = 0;
  for (long spins = 0; got < n && spins < 100'000'000; ++spins) {
    const ssize_t r = ::read(fd, buf.data(), buf.size());
    if (r > 0) got += static_cast<std::size_t>(r);
  }
  return got >= n;
}

int accept_one(int lfd) {
  for (int i = 0; i < 2'000'000; ++i) {
    const int c = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c >= 0) return c;
  }
  return -1;
}

void report(benchmark::State& state, const LogLinearHistogram& send, const LogLinearHistogram& all) {
  state.counters["send_p50"] = static_cast<double>(send.percentile(0.50));
  state.counters["send_p99"] = static_cast<double>(send.percentile(0.99));
  state.counters["p50"] = static_cast<double>(all.percentile(0.50));
  state.counters["p99"] = static_cast<double>(all.percentile(0.99));
}

void BM_KernelTcpSend(benchmark::State& state) {
  if (!setup().error.empty()) {
    state.SkipWithError(setup().error.c_str());
    return;
  }
  const int lfd = setup().kernel_listen;
  const int cfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const int one = 1;
  ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(7100);
  to.sin_addr.s_addr = ip4("10.79.0.1");
  if (lfd < 0 || ::connect(cfd, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) {
    state.SkipWithError("connect failed");
    return;
  }
  const int sfd = accept_one(lfd);
  std::array<std::byte, kMsg> msg{};
  LogLinearHistogram send_h;
  LogLinearHistogram all_h;
  for (auto _ : state) {
    const std::int64_t t0 = steady_now().ns;
    if (::write(cfd, msg.data(), msg.size()) != static_cast<ssize_t>(msg.size())) {
      state.SkipWithError("write failed");
      break;
    }
    const std::int64_t t1 = steady_now().ns;
    if (!drain(sfd, msg.size())) {
      state.SkipWithError("server read nothing");
      break;
    }
    send_h.record(static_cast<std::uint64_t>(t1 - t0));
    all_h.record(static_cast<std::uint64_t>(steady_now().ns - t0));
  }
  report(state, send_h, all_h);
  ::close(sfd);
  ::close(cfd);
}
BENCHMARK(BM_KernelTcpSend)->UseRealTime();

struct Quiet final : UserTcpHandler {
  bool up = false;
  int closed = -1;
  void on_tcp_connected() noexcept override { up = true; }
  std::size_t on_tcp_data(std::span<const std::byte> b) noexcept override { return b.size(); }
  void on_tcp_closed(int err) noexcept override { closed = err; }
};

void BM_UserTcpSend(benchmark::State& state) {
  if (!setup().error.empty()) {
    state.SkipWithError(setup().error.c_str());
    return;
  }
  const int lfd = setup().user_listen;
  PacketRing ring;
  PacketRingConfig pc;
  pc.interface = "obt0";
  pc.local_ip = ip4("10.79.0.3");
  std::string err;
  if (lfd < 0 || ring.open(pc, err) != 0) {
    state.SkipWithError(("packet ring: " + err).c_str());
    return;
  }
  Quiet h;
  UserTcpConfig tc;
  tc.local_mac = ring.mac();
  tc.local_ip = ip4("10.79.0.3");
  tc.remote_ip = ip4("10.79.0.1");
  tc.remote_port = 7101;
  auto tcp = std::make_unique<UserTcp>(ring, h, tc);
  const auto poll = [&] {
    const std::int64_t now = Reactor::now_ns();
    ring.poll([&](std::span<const std::byte> f, bool unverified) { tcp->on_frame(f, now, unverified); });
    tcp->flush();
    if (now >= tcp->next_timer_ns()) tcp->on_timer(now);
  };
  tcp->connect(Reactor::now_ns());
  int sfd = -1;
  const std::int64_t deadline = Reactor::now_ns() + 5'000'000'000;
  while (Reactor::now_ns() < deadline && (sfd < 0 || !h.up)) {
    poll();
    if (sfd < 0) sfd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
  }
  if (sfd < 0 || !h.up) {
    state.SkipWithError("user TCP connect failed");
    return;
  }
  std::array<std::byte, kMsg> msg{};
  LogLinearHistogram send_h;
  LogLinearHistogram all_h;
  for (auto _ : state) {
    const std::int64_t t0 = steady_now().ns;
    if (!tcp->send(msg)) {
      state.SkipWithError("send failed");
      break;
    }
    const std::int64_t t1 = steady_now().ns;
    if (!drain(sfd, msg.size())) {
      state.SkipWithError("server read nothing");
      break;
    }
    send_h.record(static_cast<std::uint64_t>(t1 - t0));
    all_h.record(static_cast<std::uint64_t>(steady_now().ns - t0));
    state.PauseTiming();
    for (int i = 0; i < 64 && tcp->unacked() != 0; ++i) poll();
    state.ResumeTiming();
  }
  report(state, send_h, all_h);
  state.counters["retransmits"] = static_cast<double>(tcp->stats().retransmits);
  tcp->abort();
  ::close(sfd);
}
BENCHMARK(BM_UserTcpSend)->UseRealTime();

}  // namespace
