// Order-entry send path over a veth pair: a kernel TCP socket talking to a kernel TCP server on
// the other end, all in one thread.
//
//   BM_KernelTcpSend  write(2) of a 64-byte message on a TCP_NODELAY socket, then read(2) on the
//                     server until it has the bytes
//
// Counters: send_p50 / send_p99 time the send call alone, p50 / p99 the send until the server's
// read returned (ns). On veth the send call runs the peer's receive path in the same system call,
// so most of the server side is inside the send. Client and server sit in two network namespaces
// of one user namespace (tests/net/netns.hpp); needs iproute2 and nsenter.
#include "../tests/net/netns.hpp"

#include "fastmm/core/latency.hpp"
#include "fastmm/core/time.hpp"

#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdlib>
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
      tid = ::gettid();
      while (!configured.load()) std::this_thread::yield();
      r.kernel_listen = listen_on(7100);
    });
    while (tid.load() == 0) std::this_thread::yield();
    const std::string ns = "nsenter --net=/proc/" + std::to_string(getpid()) + "/task/" +
                           std::to_string(tid.load()) + "/ns/net ";
    const bool ok = tid.load() > 0 && sh("ip link add obt0 type veth peer name obt1") &&
                    sh("ip link set obt1 netns " + std::to_string(tid.load())) &&
                    sh("ip addr add 10.79.0.2/24 dev obt0") && sh("ip link set obt0 up") &&
                    sh(ns + "ip addr add 10.79.0.1/24 dev obt1") &&
                    sh(ns + "ip link set obt1 up") && sh(ns + "ip link set lo up");
    configured = true;
    server.join();
    if (!ok) r.error = "needs iproute2 and nsenter";
    if (r.error.empty() && r.kernel_listen < 0) r.error = "listen failed";
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

void report(benchmark::State& state,
            const LogLinearHistogram& send,
            const LogLinearHistogram& all) {
  state.counters["send_p50"] = static_cast<double>(send.percentile(0.50));
  state.counters["send_p99"] = static_cast<double>(send.percentile(0.99));
  state.counters["p50"] = static_cast<double>(all.percentile(0.50));
  state.counters["p99"] = static_cast<double>(all.percentile(0.99));
}

void BM_KernelTcpSend(benchmark::State& state) {
  if (!setup().error.empty()) {
    state.SkipWithMessage(setup().error.c_str());  // no namespaces here (CI runners)
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

}  // namespace
