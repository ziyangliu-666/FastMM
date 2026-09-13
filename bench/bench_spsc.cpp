// SPSC ring throughput and cross-core ping-pong latency.
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/spsc_ring.hpp"

#include <benchmark/benchmark.h>

#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <memory>
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
