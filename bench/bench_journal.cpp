#include "fastmm/core/crc32c.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// A checkpoint's strategy bytes into the journal ring as the engine writes them: StrategyStateMsg
// records of up to 16 KiB each (Engine::write_state_part). The ring is drained between iterations,
// outside the timing, as the journal's writer thread does.
static void BM_Journal_Checkpoint(benchmark::State& state) {
  const auto total = static_cast<std::size_t>(state.range(0));
  MsgRing ring(1 << 24);
  JournalWriter w(&ring);
  std::vector<std::byte> bytes(total, std::byte{0x5A});
  for (auto _ : state) {
    StrategyStateMsg m{};
    init_header(m, EventType::StrategyState);
    m.total = static_cast<std::uint32_t>(total);
    std::size_t off = 0;
    do {
      const std::size_t n = std::min<std::size_t>(total - off, StrategyStateMsg::kChunkBytes);
      m.offset = static_cast<std::uint32_t>(off);
      m.bytes = static_cast<std::uint32_t>(n);
      m.hdr.len = StrategyStateMsg::size_for(m.bytes);
      auto r = w.record_with(m.hdr, bytes.data() + off, n);
      benchmark::DoNotOptimize(r);
      off += n;
    } while (off < total);
    state.PauseTiming();
    while (ring.try_peek() != nullptr) ring.release();
    state.ResumeTiming();
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(total));
}
BENCHMARK(BM_Journal_Checkpoint)->Arg(64 << 10)->Arg(1 << 20)->Arg(4 << 20);

static void BM_Crc32c_1MiB(benchmark::State& state) {
  std::vector<unsigned char> data(1 << 20, 0x5A);
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c(data.data(), data.size()));
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}
BENCHMARK(BM_Crc32c_1MiB);
