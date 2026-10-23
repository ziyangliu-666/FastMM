// Timing guards for L3Book: the cost of a workload must not grow with a capacity (the window,
// the order pool) when the data it touches does not. Each check compares two runs of the same
// workload on the same machine, the best of several repetitions each, so a slow or busy runner
// moves both sides; the thresholds sit far below the regression they guard against.
#include "test_support.hpp"

#include "fastmm/core/book/l3_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>

using namespace fastmm;

namespace {

const Price kTick = Price::from_decimal("0.01").value();
Price px(std::int64_t ticks) {
  return Price::from_raw(ticks * kTick.raw);
}

template <class F>
double best_seconds(int reps, F&& f) {
  double best = 1e9;
  for (int r = 0; r < reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const std::chrono::duration<double> d = std::chrono::steady_clock::now() - t0;
    best = std::min(best, d.count());
  }
  return best;
}

// A one-sided book whose touch jumps three windows away and back, trading at each place long
// enough for the window to follow: a recentre every few operations, the worst case for the
// window. Returns the recentres.
std::uint64_t storm(L3Book& b, std::int64_t window, int jumps) {
  std::uint64_t ref = 1;
  b.clear();
  for (std::int64_t i = 0; i < 8; ++i) b.add(ref++, Side::Buy, px(100000 - i), Qty::from_int(1));
  const std::uint64_t before = b.recentre_count();
  for (int j = 0; j < jumps; ++j) {
    const std::int64_t at = 100001 + (j % 2 == 0 ? 3 * window : 0);
    const std::uint64_t first = ref;
    for (int k = 0; k < 8; ++k) b.add(ref++, Side::Buy, px(at), Qty::from_int(1));
    for (std::uint64_t r = first; r < ref; ++r) b.cancel(r);
  }
  return b.recentre_count() - before;
}

}  // namespace

TEST_CASE("core.l3_book_scaling: a recentre costs the same in a 2^12 and a 2^18 tick window") {
  // Before the window became a ring each recentre moved the whole window: ~70x slower at 2^18.
  constexpr int kJumps = 2000;
  auto small = std::make_unique<L3Book>(
      kTick, L3BookConfig{.price_window_ticks = 1U << 12, .max_orders = 1U << 12});
  auto large = std::make_unique<L3Book>(
      kTick, L3BookConfig{.price_window_ticks = 1U << 18, .max_orders = 1U << 12});
  std::uint64_t n_small = 0;
  std::uint64_t n_large = 0;
  const double t_small = best_seconds(5, [&] { n_small = storm(*small, 1 << 12, kJumps); });
  const double t_large = best_seconds(5, [&] { n_large = storm(*large, 1 << 18, kJumps); });
  CAPTURE(t_small);
  CAPTURE(t_large);
  CHECK(n_small >= kJumps / 2);
  CHECK(n_large == n_small);
  CHECK(t_large < 8 * t_small);
}

TEST_CASE("core.l3_book_scaling: clear() costs the orders resting, not the capacity") {
  // Before, clear() rewrote the order pool, the index and the window: ~75 MB for the default
  // book, on every resync of every instrument.
  auto small = std::make_unique<L3Book>(
      kTick, L3BookConfig{.price_window_ticks = 1U << 10, .max_orders = 1U << 10});
  auto large = std::make_unique<L3Book>(
      kTick, L3BookConfig{.price_window_ticks = 1U << 18, .max_orders = 1U << 20});
  auto fill_and_clear = [](L3Book& b) {
    for (int round = 0; round < 50; ++round) {
      for (std::uint64_t r = 1; r <= 100; ++r)
        b.add(r,
              r % 2 == 0 ? Side::Buy : Side::Sell,
              px(r % 2 == 0 ? 1000 - static_cast<std::int64_t>(r)
                            : 1001 + static_cast<std::int64_t>(r)),
              Qty::from_int(1));
      b.clear();
    }
  };
  const double t_small = best_seconds(5, [&] { fill_and_clear(*small); });
  const double t_large = best_seconds(5, [&] { fill_and_clear(*large); });
  CAPTURE(t_small);
  CAPTURE(t_large);
  CHECK(large->order_count() == 0);
  CHECK(t_large < 8 * t_small);
}
