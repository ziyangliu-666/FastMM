// net::Reactor round trips over loopback TCP, epoll against io_uring.
//
//   BM_ReactorEchoInline/<backend>/<busy>  client socket and echo server socket on one reactor;
//                                          an iteration writes 64 bytes and runs the reactor
//                                          until the echo is back (two dispatches, no wake-up)
//   BM_ReactorEchoThread/<backend>/<busy>  echo server on its own reactor thread; the benchmark
//                                          thread writes and runs its reactor until the reply
//                                          arrives (two cross-thread wake-ups per iteration)
//   BM_ReactorTimerArmCancel               arm a 1 s timer and cancel it (a connection's heartbeat
//                                          or health timer on every pong or reconnect)
//   BM_ReactorTimerRearm                   one run_once(0) whose timer fires and re-arms itself
//                                          (a venue's housekeeping timer); includes epoll_wait
//   BM_ReactorIdlePoll/<backend>           one busy-polled run_once with nothing to do (a network
//                                          thread's idle spin)
//
// The timer callbacks capture a pointer and a std::weak_ptr, as the venues' timers do.
//
// Arguments: backend 0 = epoll, 1 = io_uring (skipped when unsupported); busy 0 = blocking waits,
// 1 = busy polling. Counters p50 / p99 are per-iteration round-trip times in ns.
//
// The threaded variant needs two cores and says so (FASTMM_BENCH_NEEDS_CORES): on one core the
// benchmark thread and the echo thread take turns, and a round trip costs a scheduler time slice
// (8 ms) instead of 10 us. It fails rather than report that number when it is run pinned;
// scripts/bench.sh runs it in a separate, unpinned pass.
#include "bench_pin.hpp"

#include "fastmm/core/latency.hpp"
#include "fastmm/net/reactor.hpp"
#include "fastmm/net/tcp_socket.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::net;

namespace {

constexpr std::size_t kMsgSize = 64;

class EchoServer final : public IoHandler {
 public:
  explicit EchoServer(TcpSocket& s) : sock_(s) {}
  void on_readable() override {
    std::array<std::byte, 1024> buf{};
    for (;;) {
      const IoResult r = sock_.read(buf);
      if (r.bytes > 0) sock_.write(std::span<const std::byte>(buf.data(), r.bytes));
      if (r.bytes == 0) return;
    }
  }
  void on_writable() override {}
  void on_error(int) override {}

 private:
  TcpSocket& sock_;
};

class ClientSink final : public IoHandler {
 public:
  explicit ClientSink(TcpSocket& s) : sock_(s) {}
  void on_readable() override {
    std::array<std::byte, 1024> buf{};
    for (;;) {
      const IoResult r = sock_.read(buf);
      received += r.bytes;
      if (r.bytes == 0) return;
    }
  }
  void on_writable() override {}
  void on_error(int) override {}
  std::size_t received = 0;

 private:
  TcpSocket& sock_;
};

// Connected loopback TCP pair with TCP_NODELAY on both ends.
bool connect_loopback(TcpSocket& client, TcpSocket& server) {
  TcpSocket listener = TcpSocket::listen(SockAddr::loopback_v4(0));
  if (!listener.valid()) return false;
  const auto addr = listener.local_addr();
  if (!addr) return false;
  if (client.connect(SockAddr::loopback_v4(addr->port())) == ConnectStatus::Error) return false;
  for (int i = 0; i < 1000 && !server.valid(); ++i) {
    server = listener.accept(nullptr);
    if (!server.valid()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!server.valid()) return false;
  if (client.connecting() && client.finish_connect() != 0) return false;
  return client.set_nodelay(true) && server.set_nodelay(true);
}

bool select_backend(benchmark::State& state, ReactorBackend& backend) {
  backend = state.range(0) == 0 ? ReactorBackend::Epoll : ReactorBackend::IoUring;
  const bool busy = state.range(1) != 0;
  state.SetLabel(std::string(to_string(backend)) + (busy ? "/busy" : "/blocking"));
  if (backend == ReactorBackend::IoUring && !Reactor::io_uring_supported()) {
    state.SkipWithMessage("io_uring is not available on this kernel");
    return false;
  }
  return true;
}

void report(benchmark::State& state, const LogLinearHistogram& h) {
  state.counters["p50"] = static_cast<double>(h.percentile(0.50));
  state.counters["p99"] = static_cast<double>(h.percentile(0.99));
}

// Writes one message per iteration and runs `client_reactor` until its echo is back.
void round_trips(benchmark::State& state,
                 Reactor& client_reactor,
                 TcpSocket& client,
                 ClientSink& sink,
                 bool busy) {
  std::array<std::byte, kMsgSize> msg{};
  msg.fill(std::byte{'x'});
  LogLinearHistogram hist;
  std::size_t expected = sink.received;
  const int wait_ms = busy ? 0 : 100;
  for (auto _ : state) {
    const std::int64_t t0 = Reactor::now_ns();
    if (!client.write(msg).ok()) {
      state.SkipWithError("write failed");
      break;
    }
    expected += kMsgSize;
    while (sink.received < expected) {
      if (client_reactor.run_once(wait_ms) < 0) {
        state.SkipWithError("run_once failed");
        return;
      }
    }
    hist.record(static_cast<std::uint64_t>(Reactor::now_ns() - t0));
  }
  report(state, hist);
}

void BM_ReactorEchoInline(benchmark::State& state) {
  ReactorBackend backend{};
  if (!select_backend(state, backend)) return;
  const bool busy = state.range(1) != 0;
  Reactor reactor(backend);
  reactor.set_busy_poll(busy);
  TcpSocket client;
  TcpSocket server;
  if (!connect_loopback(client, server)) {
    state.SkipWithError("loopback connect failed");
    return;
  }
  EchoServer echo(server);
  ClientSink sink(client);
  reactor.add(server.fd(), echo, IoEvent::Read);
  reactor.add(client.fd(), sink, IoEvent::Read);
  reactor.run_once(0);
  round_trips(state, reactor, client, sink, busy);
  reactor.remove(client.fd());
  reactor.remove(server.fd());
}
BENCHMARK(BM_ReactorEchoInline)->ArgsProduct({{0, 1}, {0, 1}})->UseRealTime();

void BM_ReactorEchoThread(benchmark::State& state) {
  if (fastmm::bench::affinity_cores() < 2) {
    state.SkipWithError("needs two cores: run it without --cpu / taskset");
    return;
  }
  ReactorBackend backend{};
  if (!select_backend(state, backend)) return;
  const bool busy = state.range(1) != 0;
  TcpSocket client;
  TcpSocket server;
  if (!connect_loopback(client, server)) {
    state.SkipWithError("loopback connect failed");
    return;
  }
  Reactor server_reactor(backend);
  server_reactor.set_busy_poll(busy);
  EchoServer echo(server);
  server_reactor.add(server.fd(), echo, IoEvent::Read);  // before the thread takes the reactor
  std::atomic<bool> stop{false};
  std::thread server_thread([&] {
    while (!stop.load(std::memory_order_relaxed)) server_reactor.run_once(busy ? 0 : 100);
    server_reactor.remove(server.fd());
  });

  Reactor client_reactor(backend);
  client_reactor.set_busy_poll(busy);
  ClientSink sink(client);
  client_reactor.add(client.fd(), sink, IoEvent::Read);
  client_reactor.run_once(0);
  round_trips(state, client_reactor, client, sink, busy);
  client_reactor.remove(client.fd());

  stop.store(true, std::memory_order_relaxed);
  server_reactor.wake();
  server_thread.join();
}
BENCHMARK(BM_ReactorEchoThread)->ArgsProduct({{0, 1}, {0, 1}})->UseRealTime();
FASTMM_BENCH_NEEDS_CORES(BM_ReactorEchoThread, 2);

void BM_ReactorTimerArmCancel(benchmark::State& state) {
  Reactor reactor;
  const auto alive = std::make_shared<int>(0);
  std::weak_ptr<int> weak = alive;
  int fired = 0;
  for (auto _ : state) {
    const TimerId id = reactor.add_timer_after(1'000'000'000, [p = &fired, weak] {
      if (!weak.expired()) ++*p;
    });
    benchmark::DoNotOptimize(reactor.cancel_timer(id));
  }
  benchmark::DoNotOptimize(fired);
}
BENCHMARK(BM_ReactorTimerArmCancel);

struct Rearm {
  Reactor* r;
  std::uint64_t* fired;
  std::weak_ptr<int> alive;
  void operator()() const {
    if (alive.expired()) return;
    ++*fired;
    static_cast<void>(r->add_timer_after(0, Rearm{r, fired, alive}));
  }
};

void BM_ReactorTimerRearm(benchmark::State& state) {
  Reactor reactor;
  const auto alive = std::make_shared<int>(0);
  std::uint64_t fired = 0;
  static_cast<void>(reactor.add_timer_after(0, Rearm{&reactor, &fired, alive}));
  for (auto _ : state) reactor.run_once(0);
  state.counters["fired_per_iter"] =
      static_cast<double>(fired) / static_cast<double>(state.iterations());
}
BENCHMARK(BM_ReactorTimerRearm);

void BM_ReactorIdlePoll(benchmark::State& state) {
  ReactorBackend backend{};
  if (!select_backend(state, backend)) return;
  Reactor reactor(backend);
  reactor.set_busy_poll(true);
  for (auto _ : state) benchmark::DoNotOptimize(reactor.run_once(0));
}
BENCHMARK(BM_ReactorIdlePoll)->ArgsProduct({{0, 1}, {1}});

}  // namespace
