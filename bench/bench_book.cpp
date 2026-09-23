// Order book micro-benchmarks (plan 5.12 budget: L2 near-top update 15-30 ns, 20-level
// delta ~300 ns, L3 add/cancel/execute 30-60 ns).
#include "fastmm/core/book/book_features.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/book/l3_book.hpp"
#include "fastmm/core/rng.hpp"

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace fastmm;

namespace {
const Price kTick = Price::from_decimal("0.01").value();
Price px(std::int64_t t) {
  return Price::from_raw(t * kTick.raw);
}

template <std::size_t D>
void fill_book(L2Book<D>& b, int levels) {
  for (int i = 0; i < levels; ++i) {
    b.apply_level(Side::Buy, px(10000 - i), Qty::from_int(1 + i));
    b.apply_level(Side::Sell, px(10001 + i), Qty::from_int(1 + i));
  }
  b.mark_snapshot();
}

std::vector<std::byte> make_delta(int levels_per_side, std::int64_t offset, Xoshiro256ss& rng) {
  const auto n = static_cast<std::uint32_t>(levels_per_side);
  std::vector<std::byte> buf(BookDeltaMsg::size_for(n, n));
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf.data());
  init_header(*d,
              EventType::BookDelta,
              InstrumentId{0},
              VenueId{0},
              static_cast<std::uint32_t>(buf.size()));
  d->bid_count = d->ask_count = n;
  for (std::uint32_t i = 0; i < n; ++i) {
    d->levels()[i] = Level{px(10000 - offset - static_cast<std::int64_t>(i)),
                           Qty::from_int(static_cast<std::int64_t>(rng.uniform(50)))};
    d->levels()[n + i] = Level{px(10001 + offset + static_cast<std::int64_t>(i)),
                               Qty::from_int(static_cast<std::int64_t>(rng.uniform(50)))};
  }
  return buf;
}
}  // namespace

// Dependent latency: the level the next update touches is derived from the best bid this one
// produced, so one update cannot start before the previous one is visible. It measures the same
// 2.2 ns as the version without the chain, because ClobberMemory() already made each iteration
// reload the book; the chain is here so that stays true if the barrier goes.
static void BM_L2_UpdateNearTop(benchmark::State& state) {
  L2Book<256> b;
  fill_book(b, 100);
  benchmark::DoNotOptimize(&b);  // escape so ClobberMemory() applies to the book
  std::int64_t i = 0;
  for (auto _ : state) {
    // update one of the top 4 bid levels in place (no memmove), rotating the level
    b.apply_level(Side::Buy, px(10000 - (i & 3)), Qty::from_int(1 + (i & 7)));
    const Level top = b.best_bid();
    benchmark::ClobberMemory();  // the book's stores stay; DoNotOptimize(top) would add one
    i += 1 + (top.qty.raw & 1);
  }
  benchmark::DoNotOptimize(i);
}
BENCHMARK(BM_L2_UpdateNearTop);

static void BM_L2_InsertEraseTop(benchmark::State& state) {
  L2Book<256> b;
  fill_book(b, 100);
  bool in = false;
  for (auto _ : state) {
    b.apply_level(
        Side::Sell, px(10000), in ? Qty{} : Qty::from_int(3));  // new best ask then delete
    in = !in;
    benchmark::DoNotOptimize(b.best_ask());
  }
}
BENCHMARK(BM_L2_InsertEraseTop);

static void BM_L2_ApplyDelta(benchmark::State& state) {
  const int n = static_cast<int>(state.range(0));
  L2Book<256> b;
  fill_book(b, 150);
  Xoshiro256ss rng(1);
  std::vector<std::vector<std::byte>> deltas;
  for (int i = 0; i < 64; ++i) deltas.push_back(make_delta(n, 0, rng));
  std::size_t k = 0;
  for (auto _ : state) {
    b.apply_delta(*reinterpret_cast<const BookDeltaMsg*>(deltas[k & 63].data()));
    ++k;
    benchmark::DoNotOptimize(b.seq());
  }
  state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_L2_ApplyDelta)->Arg(20)->Arg(100);

static void BM_L2_PriceForQty(benchmark::State& state) {
  L2Book<256> b;
  fill_book(b, 100);
  for (auto _ : state) {
    benchmark::DoNotOptimize(b.price_for_qty(Side::Buy, Qty::from_int(100)));
  }
}
BENCHMARK(BM_L2_PriceForQty);

static void BM_L2_Features(benchmark::State& state) {
  L2Book<256> b;
  fill_book(b, 100);
  for (auto _ : state) {
    benchmark::DoNotOptimize(imbalance(b, 5));
    benchmark::DoNotOptimize(microprice(b));
  }
}
BENCHMARK(BM_L2_Features);

static void BM_L3_AddCancelExecMix(benchmark::State& state) {
  auto b = std::make_unique<L3Book>(
      kTick, L3BookConfig{.price_window_ticks = 1U << 16, .max_orders = 1U << 20});
  Xoshiro256ss rng(7);
  std::vector<std::uint64_t> live;
  live.reserve(1 << 16);  // > kHigh + 1: no reallocation in the timed loop
  std::uint64_t next = 1;
  for (int i = 0; i < 20000; ++i) {
    const Side s = (i & 1) ? Side::Buy : Side::Sell;
    b->add(next,
           s,
           px(s == Side::Buy ? 10000 - static_cast<std::int64_t>(rng.uniform(50))
                             : 10001 + static_cast<std::int64_t>(rng.uniform(50))),
           Qty::from_int(1 + static_cast<std::int64_t>(rng.uniform(10))));
    live.push_back(next++);
  }
  // Keep the resting population inside a fixed band. An unconditional 1/3 add, 2/3 remove mix
  // drifts to an empty book on long runs (then `% live.size()` divides by zero), and growing
  // past the reserve would allocate inside the timed loop.
  static constexpr std::size_t kLow = 10'000;
  static constexpr std::size_t kHigh = 30'000;
  for (auto _ : state) {
    const std::uint64_t r = rng.next();
    std::uint64_t op = r % 3;
    if (live.size() < kLow) {
      op = 0;
    } else if (live.size() > kHigh) {
      op = 1;
    }
    switch (op) {
      case 0: {
        const Side s = (r & 8) ? Side::Buy : Side::Sell;
        b->add(next,
               s,
               px(s == Side::Buy ? 10000 - static_cast<std::int64_t>((r >> 8) % 50)
                                 : 10001 + static_cast<std::int64_t>((r >> 8) % 50)),
               Qty::from_int(1 + static_cast<std::int64_t>((r >> 16) % 10)));
        live.push_back(next++);
        break;
      }
      case 1: {
        const std::size_t k = (r >> 8) % live.size();
        b->cancel(live[k]);
        live[k] = live.back();
        live.pop_back();
        break;
      }
      default: {
        const std::size_t k = (r >> 8) % live.size();
        const L3Order* o = b->order(live[k]);
        if (o != nullptr && o->qty > Qty::from_int(1)) {
          b->execute(live[k], Qty::from_int(1));
        } else {
          b->execute(live[k], Qty::from_int(1));
          live[k] = live.back();
          live.pop_back();
        }
        break;
      }
    }
    benchmark::DoNotOptimize(b->best_bid());
  }
}
BENCHMARK(BM_L3_AddCancelExecMix);

// Add and cancel stub quotes far outside the price window (overflow store) while 20 000 orders
// rest near the touch and 64 stub levels per side stay in the store.
static void BM_L3_OverflowAddCancel(benchmark::State& state) {
  auto b = std::make_unique<L3Book>(
      kTick, L3BookConfig{.price_window_ticks = 1U << 12, .max_orders = 1U << 16});
  Xoshiro256ss rng(9);
  std::uint64_t next = 1;
  for (int i = 0; i < 20000; ++i) {
    const Side s = (i & 1) ? Side::Buy : Side::Sell;
    const auto off = static_cast<std::int64_t>(rng.uniform(50));
    b->add(next++, s, px(s == Side::Buy ? 10000 - off : 10001 + off), Qty::from_int(1));
  }
  for (std::int64_t i = 0; i < 64; ++i) {
    b->add(next++, Side::Buy, px(100 + i * 10), Qty::from_int(1));
    b->add(next++, Side::Sell, px(90000 + i * 10), Qty::from_int(1));
  }
  for (auto _ : state) {
    const std::uint64_t r = rng.next();
    const Side s = (r & 1) ? Side::Buy : Side::Sell;
    const auto off = static_cast<std::int64_t>((r >> 8) % 640);
    b->add(next, s, px(s == Side::Buy ? 100 + off : 90000 + off), Qty::from_int(1));
    b->cancel(next++);
    benchmark::DoNotOptimize(b->best_bid());
  }
}
BENCHMARK(BM_L3_OverflowAddCancel);
