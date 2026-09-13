// MarketGenerator: determinism per seed and sanity of the produced market.
#include "test_support.hpp"

#include "fastmm/sim/market_generator.hpp"

#include <cmath>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}

struct Probe final : MatchingSink {
  std::vector<std::uint64_t> fingerprint;  // (uid, price raw, qty raw) stream
  std::uint64_t trades = 0;
  Qty traded{};
  void on_book_change(InstrumentId, Side, Price p, Qty q, std::uint64_t u) override {
    fingerprint.push_back(u ^ (static_cast<std::uint64_t>(p.raw) * 31U) ^
                          (static_cast<std::uint64_t>(q.raw) * 131U));
  }
  void on_trade(InstrumentId, Price, Qty q, Side, std::uint64_t, Timestamp) override {
    ++trades;
    traded += q;
  }
};

struct Run {
  Probe probe;
  MatchingEngine me{1, &probe};
  MarketGeneratorStats stats;
  Price final_mid;
  std::size_t resting;
  std::vector<double> spreads_ticks;
  std::vector<std::size_t> depths;
};

std::unique_ptr<Run> run_generator(std::uint64_t seed, Duration horizon) {
  auto r = std::make_unique<Run>();
  MarketGeneratorParams p;
  p.start_mid = px("60000");
  p.tick = px("0.01");
  const Timestamp start{seconds(1'700'000'000).ns};
  MarketGenerator gen(p, seed, InstrumentId{0}, start, start + horizon);
  gen.seed_book(r->me, 10, start);
  std::uint64_t n = 0;
  while (gen.next_ts() != Timestamp::max()) {
    gen.step(r->me);
    if (++n % 100 == 0) {
      const auto top = r->me.top_of_book(InstrumentId{0});
      if (top.bid.qty.is_positive() && top.ask.qty.is_positive()) {
        r->spreads_ticks.push_back(static_cast<double>((top.ask.price - top.bid.price).raw) /
                                   static_cast<double>(p.tick.raw));
      }
      r->depths.push_back(r->me.book(InstrumentId{0}).depth(Side::Buy) +
                          r->me.book(InstrumentId{0}).depth(Side::Sell));
    }
  }
  r->stats = gen.stats();
  r->final_mid = gen.mid();
  r->resting = r->me.open_orders();
  return r;
}
}  // namespace

TEST_CASE("sim.generator: same seed -> identical book mutation stream, different seed differs") {
  auto a = run_generator(42, seconds(20));
  auto b = run_generator(42, seconds(20));
  auto c = run_generator(43, seconds(20));
  CHECK(a->probe.fingerprint == b->probe.fingerprint);
  CHECK(a->probe.trades == b->probe.trades);
  CHECK(a->final_mid == b->final_mid);
  CHECK(a->probe.fingerprint != c->probe.fingerprint);
  CHECK(a->probe.fingerprint.size() > 1000);
}

TEST_CASE("sim.generator: distributions are sane") {
  auto r = run_generator(5, seconds(60));
  const auto& s = r->stats;
  // Poisson rates: limits ~200/s, markets ~10/s, mid steps ~2/s over 60 s
  CHECK(s.limits > 60 * 200 * 0.7);
  CHECK(s.limits < 60 * 200 * 1.3);
  CHECK(s.markets > 60 * 10 * 0.5);
  CHECK(s.markets < 60 * 10 * 2.5);  // regime switches scale the rate
  CHECK(s.mid_steps > 40);
  CHECK(s.cancels > s.limits / 4);
  CHECK(s.rejected == 0);
  CHECK(r->probe.trades > 0);
  CHECK(r->resting > 20);
  CHECK(r->resting <= 4000);
  // mid stays near 60000 (random walk of a few hundred ticks at most)
  CHECK((r->final_mid - px("60000")).abs() < px("50"));
  // spread is positive (never crossed) and mostly a handful of ticks
  REQUIRE_FALSE(r->spreads_ticks.empty());
  double sum = 0;
  double mx = 0;
  for (double v : r->spreads_ticks) {
    CHECK(v >= 1.0);
    sum += v;
    if (v > mx) mx = v;
  }
  CHECK(sum / static_cast<double>(r->spreads_ticks.size()) < 12.0);
  // depth is bounded and non-trivial
  for (std::size_t d : r->depths) CHECK(d < 2 * kMaxSimLevels);
  CHECK(r->depths.back() > 4);
}
