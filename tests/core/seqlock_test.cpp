#include "fastmm/core/seqlock.hpp"

#include "test_support.hpp"

#include <atomic>
#include <thread>

using namespace fastmm;

namespace {
// A payload whose fields must always agree; a torn read is detectable.
struct Snapshot {
  std::uint64_t a;
  std::uint64_t b;  // == a * 3
  std::uint64_t pad[14];
};
}  // namespace

TEST_CASE("core.seqlock: readers never observe torn writes") {
  Seqlocked<Snapshot> sl(Snapshot{0, 0, {}});
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> torn{0};
  std::thread writer([&] {
    Snapshot s{};
    for (std::uint64_t i = 1; i < 2'000'000; ++i) {
      s.a = i;
      s.b = i * 3;
      for (auto& p : s.pad) p = i;
      sl.store(s);
    }
    stop.store(true);
  });
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      const Snapshot s = sl.load();
      reads.fetch_add(1, std::memory_order_relaxed);
      if (s.b != s.a * 3) torn.fetch_add(1);
      for (auto p : s.pad) {
        if (p != s.a) torn.fetch_add(1);
      }
    }
  });
  writer.join();
  reader.join();
  CHECK(torn.load() == 0);
  CHECK(reads.load() > 0);
  CHECK(sl.load().a == 1'999'999);
  CHECK(sl.version() == 1'999'999);
}

TEST_CASE("core.seqlock: try_load single-shot") {
  Seqlocked<int> sl(7);
  int v = 0;
  CHECK(sl.try_load(v));
  CHECK(v == 7);
  sl.store(9);
  CHECK(sl.load() == 9);
}
