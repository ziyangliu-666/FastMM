// SPSC ring throughput and cross-core ping-pong latency.
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/shm_ring.hpp"
#include "fastmm/core/spsc_ring.hpp"

#include <benchmark/benchmark.h>

#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

using namespace fastmm;

namespace {
void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  sched_setaffinity(0, sizeof(set), &set);
}
// A core different from the one the benchmark thread runs on (the process may already be
// pinned by --cpu, so look at the whole machine rather than the affinity mask).
int other_cpu() {
  const int cur = sched_getcpu();
  const long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n <= 1) return cur;
  return (cur + 1) % static_cast<int>(n);
}
}  // namespace

// Producer thread pushes as fast as it can; the benchmark thread drains. Reports items/s.
static void BM_SpscRing_Throughput(benchmark::State& state) {
  auto ring = std::make_unique<SpscRing<std::uint64_t, 4096>>();
  std::atomic<bool> stop{false};
  std::thread producer([&] {
    pin(other_cpu());
    std::uint64_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (ring->try_push(i)) ++i;
    }
  });
  std::uint64_t v = 0;
  for (auto _ : state) {
    while (!ring->try_pop(v)) {
    }
    benchmark::DoNotOptimize(v);
  }
  stop.store(true);
  producer.join();
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscRing_Throughput);

// Round trip across two rings on two cores; one iteration = one ping + one pong, so a
// single cross-core hop is half the reported time.
static void BM_SpscRing_PingPong(benchmark::State& state) {
  auto ping = std::make_unique<SpscRing<std::uint64_t, 64>>();
  auto pong = std::make_unique<SpscRing<std::uint64_t, 64>>();
  std::atomic<bool> stop{false};
  std::thread echo([&] {
    pin(other_cpu());
    std::uint64_t v = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (ping->try_pop(v)) {
        while (!pong->try_push(v)) {
        }
      }
    }
  });
  std::uint64_t v = 0;
  std::uint64_t i = 0;
  for (auto _ : state) {
    while (!ping->try_push(i)) {
    }
    while (!pong->try_pop(v)) {
    }
    ++i;
  }
  stop.store(true);
  echo.join();
  state.counters["hop_ns"] = benchmark::Counter(
      2.0, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_SpscRing_PingPong)->UseRealTime();

// A 128-byte message and its echo, between two threads (MsgRing) and between two processes
// (ShmRing): what moving the connectors into a gateway process costs a hop.
namespace {
template <class Ring>
void push_msg(Ring& r, std::uint64_t seq) {
  std::byte* p = nullptr;
  while ((p = r.try_reserve(128)) == nullptr) {
  }
  auto* pre = reinterpret_cast<RingMsgPrefix*>(p);
  pre->len = 128;
  pre->type = 1;
  __builtin_memcpy(p + 8, &seq, sizeof seq);
  r.commit();
}
template <class Ring>
std::uint64_t pop_msg(Ring& r) {
  const std::byte* m = nullptr;
  while ((m = r.try_peek()) == nullptr) {
  }
  std::uint64_t seq = 0;
  __builtin_memcpy(&seq, m + 8, sizeof seq);
  r.release();
  return seq;
}
}  // namespace

static void BM_MsgRing_PingPong_Thread(benchmark::State& state) {
  MsgRing ping(1 << 16);
  MsgRing pong(1 << 16);
  std::atomic<bool> stop{false};
  std::thread echo([&] {
    pin(other_cpu());
    while (!stop.load(std::memory_order_relaxed)) {
      if (const std::byte* m = ping.try_peek()) {
        std::uint64_t seq = 0;
        __builtin_memcpy(&seq, m + 8, sizeof seq);
        ping.release();
        push_msg(pong, seq);
      }
    }
  });
  std::uint64_t i = 0;
  for (auto _ : state) {
    push_msg(ping, i);
    benchmark::DoNotOptimize(pop_msg(pong));
    ++i;
  }
  stop.store(true);
  echo.join();
  state.counters["hop_ns"] = benchmark::Counter(
      2.0, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_MsgRing_PingPong_Thread)->UseRealTime();

static void BM_ShmRing_PingPong_Process(benchmark::State& state) {
  const std::string dir = "/dev/shm";
  const std::string ping_path = dir + "/fastmm-bench-ping-" + std::to_string(::getpid());
  const std::string pong_path = dir + "/fastmm-bench-pong-" + std::to_string(::getpid());
  auto ping = ShmRing::create(ping_path, 1 << 16);
  auto pong = ShmRing::create(pong_path, 1 << 16);
  if (!ping || !pong) {
    state.SkipWithError("cannot create the rings in /dev/shm");
    return;
  }
  const pid_t child = ::fork();
  if (child == 0) {
    pin(other_cpu());
    auto in = ShmRing::open(ping_path);
    auto out = ShmRing::open(pong_path);
    if (!in || !out) ::_exit(1);
    for (;;) {
      const std::uint64_t seq = pop_msg(*in);
      if (seq == ~0ULL) ::_exit(0);
      push_msg(*out, seq);
    }
  }
  std::uint64_t i = 0;
  for (auto _ : state) {
    push_msg(*ping, i);
    benchmark::DoNotOptimize(pop_msg(*pong));
    ++i;
  }
  push_msg(*ping, ~0ULL);
  int status = 0;
  ::waitpid(child, &status, 0);
  ::unlink(ping_path.c_str());
  ::unlink(pong_path.c_str());
  state.counters["hop_ns"] = benchmark::Counter(
      2.0, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_ShmRing_PingPong_Process)->UseRealTime();

static void BM_MsgRing_PushPop_128B(benchmark::State& state) {
  MsgRing ring(1 << 16);
  alignas(64) std::byte msg[128] = {};
  auto* pre = reinterpret_cast<RingMsgPrefix*>(msg);
  pre->len = 128;
  pre->type = 1;
  for (auto _ : state) {
    std::byte* p = ring.try_reserve(128);
    __builtin_memcpy(p, msg, 128);
    ring.commit();
    const std::byte* q = ring.try_peek();
    benchmark::DoNotOptimize(q);
    ring.release();
  }
}
BENCHMARK(BM_MsgRing_PushPop_128B);
