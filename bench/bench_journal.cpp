#include "fastmm/core/crc32c.hpp"
#include "fastmm/core/journal.hpp"

#include <benchmark/benchmark.h>

#include <vector>

using namespace fastmm;

static void BM_Journal_Record(benchmark::State& state) {
  const auto bytes = static_cast<std::uint32_t>(state.range(0));
  MsgRing ring(1 << 24);
  JournalWriter w(&ring);
  std::vector<std::byte> buf(bytes);
  auto* h = reinterpret_cast<EventHeader*>(buf.data());
  *h = EventHeader{};
  h->len = bytes;
  h->type = EventType::BookDelta;
  for (auto _ : state) {
    auto r = w.record(*h);
    benchmark::DoNotOptimize(r);
    // consumer side: release to keep the ring from filling
    if (ring.try_peek() != nullptr) ring.release();
  }
  state.SetBytesProcessed(state.iterations() * bytes);
}
BENCHMARK(BM_Journal_Record)->Arg(128)->Arg(1024);

static void BM_Crc32c_1MiB(benchmark::State& state) {
  std::vector<unsigned char> data(1 << 20, 0x5A);
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c(data.data(), data.size()));
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}
BENCHMARK(BM_Crc32c_1MiB);
