// Log emit cost (caller side only; the sink is stopped so the ring simply fills and
// then drops, which is the same code path minus the commit).
#include "fastmm/core/log.hpp"

#include <benchmark/benchmark.h>

using namespace fastmm;

static void BM_Log_Emit_3Args(benchmark::State& state) {
  auto& lg = Logger::instance();
  lg.set_level(LogLevel::Trace);
  auto* ring = lg.attach_current_thread();
  const Price px = Price::from_int(50000);
  const Qty q = Qty::from_int(1);
  std::uint64_t i = 0;
  for (auto _ : state) {
    FASTMM_LOG_INFO("fill {} @ {} x {}", i, px, q);
    ++i;
    // keep the ring from filling so we measure the enqueue path
    LogRecord rec;
    if ((i & 1023) == 0) {
      while (ring->try_pop(rec)) {
      }
    }
  }
}
BENCHMARK(BM_Log_Emit_3Args);

static void BM_Log_Disabled_Level(benchmark::State& state) {
  auto& lg = Logger::instance();
  lg.set_level(LogLevel::Error);
  int x = 0;
  for (auto _ : state) {
    FASTMM_LOG_DEBUG("x={}", x);
    benchmark::DoNotOptimize(x += 1);
  }
}
BENCHMARK(BM_Log_Disabled_Level);

static void BM_Log_FormatRecord(benchmark::State& state) {
  static constexpr LogDescriptor desc{"fill {} @ {} x {}", __FILE__, __LINE__, LogLevel::Info};
  LogRecord r{};
  r.desc = &desc;
  r.ts_ns = 1'700'000'000'000'000'000LL;
  detail::ArgPacker pk{r.args, r.args + sizeof(r.args)};
  detail::pack_arg(pk, 42);
  detail::pack_arg(pk, Price::from_int(50000));
  detail::pack_arg(pk, Qty::from_int(1));
  r.nargs = pk.n;
  r.used = static_cast<std::uint8_t>(pk.p - r.args);
  std::string out;
  for (auto _ : state) {
    out.clear();
    Logger::format_record(r, out);
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_Log_FormatRecord);
