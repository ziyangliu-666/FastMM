// Quoting helpers (strategies/quoting.hpp) and DesiredQuotes::bid/ask/uncross (ADR-0012).
#include "fastmm/strategies/quoting.hpp"

#include "test_support.hpp"

#include "fastmm/core/instrument.hpp"

using namespace fastmm;
using namespace fastmm::literals;

TEST_CASE("strategies.quoting: mid, microprice, spread_ratio, away_from, ticks") {
  CHECK(mid(100_px, 101_px) == 100.5_px);
  CHECK(mid(Price::from_raw(1), Price::from_raw(2)) == Price::from_raw(1));  // truncates like books

  // (100 * 3 + 101 * 1) / 4 = 100.25: the heavier ask pulls the price towards the bid.
  CHECK(microprice(Level{100_px, 1_qty}, Level{101_px, 3_qty}) == 100.25_px);
  CHECK(microprice(Level{100_px, 3_qty}, Level{101_px, 1_qty}) == 100.75_px);
  CHECK(microprice(Level{100_px, 2_qty}, Level{101_px, 2_qty}) == 100.5_px);
  CHECK(microprice(Level{100_px, Qty{}}, Level{101_px, Qty{}}) == 100.5_px);  // no size: mid
  // Truncated toward zero: (1 * 1e-8 + 2 * 2e-8) / 3e-8 raw = 5 / 3.
  CHECK(microprice(Level{Price::from_raw(1), Qty::from_raw(2)},
                   Level{Price::from_raw(2), Qty::from_raw(1)}) == Price::from_raw(1));
  // Large prices and sizes stay exact through Int128.
  CHECK(microprice(Level{90000_px, 1000000_qty}, Level{90000.02_px, 1000000_qty}) == 90000.01_px);

  CHECK(spread_ratio(99.99_px, 100.01_px) == 2_bps);  // 0.02 / 100
  CHECK(spread_ratio(99.99_px, 100.01_px).to_bps() == doctest::Approx(2.0));
  CHECK(spread_ratio(Price{}, Price{}).is_zero());
  CHECK(spread_ratio(100_px, 100_px).is_zero());

  CHECK(away_from(100_px, Side::Buy, 0.05_px) == 99.95_px);
  CHECK(away_from(100_px, Side::Sell, 0.05_px) == 100.05_px);

  Instrument inst{};
  inst.tick = 0.01_px;
  CHECK(inst.ticks(3) == 0.03_px);
  CHECK(inst.ticks(-2) == -0.02_px);
  CHECK(inst.ticks(0).is_zero());
}

TEST_CASE("strategies.quoting: inventory_allows truth table") {
  struct Row {
    Side side;
    Qty position;
    Qty qty;
    Qty limit;
    bool allowed;
  };
  const Qty q = 0.01_qty;
  const Qty lim = 0.02_qty;
  const Row rows[] = {
      // no cap
      {Side::Buy, 1000_qty, q, Qty{}, true},
      {Side::Sell, -1000_qty, q, Qty{}, true},
      // flat
      {Side::Buy, Qty{}, q, lim, true},
      {Side::Sell, Qty{}, q, lim, true},
      // up to the limit exactly
      {Side::Buy, 0.01_qty, q, lim, true},
      {Side::Sell, -0.01_qty, q, lim, true},
      // one more would pass the limit
      {Side::Buy, 0.02_qty, q, lim, false},
      {Side::Buy, 0.015_qty, q, lim, false},
      {Side::Sell, -0.02_qty, q, lim, false},
      {Side::Sell, -0.015_qty, q, lim, false},
      // the reducing side is allowed at the limit
      {Side::Sell, 0.02_qty, q, lim, true},
      {Side::Buy, -0.02_qty, q, lim, true},
      // beyond the limit (e.g. a late fill): reducing is allowed while the result stays within
      {Side::Sell, 0.05_qty, q, lim, true},
      {Side::Buy, -0.05_qty, q, lim, true},
      {Side::Buy, 0.05_qty, q, lim, false},
      {Side::Sell, -0.05_qty, q, lim, false},
      // a single order larger than the whole range
      {Side::Buy, Qty{}, 0.05_qty, lim, false},
      {Side::Sell, Qty{}, 0.05_qty, lim, false},
      // a sell that would flip from the long limit through the short limit
      {Side::Sell, 0.02_qty, 0.05_qty, lim, false},
  };
  for (const Row& r : rows) {
    CAPTURE(to_string(r.side));
    CAPTURE(r.position.raw);
    CAPTURE(r.qty.raw);
    CAPTURE(r.limit.raw);
    CHECK(inventory_allows(r.side, r.position, r.qty, r.limit) == r.allowed);
  }
}

TEST_CASE("strategies.quoting: DesiredQuotes::bid and ask drop non-positive levels") {
  DesiredQuotes q;
  CHECK(q.bid(100_px, 1_qty));
  CHECK_FALSE(q.bid(Price{}, 1_qty));
  CHECK_FALSE(q.bid(-1_px, 1_qty));
  CHECK_FALSE(q.bid(99_px, Qty{}));
  CHECK_FALSE(q.bid(99_px, -1_qty));
  CHECK(q.ask(101_px, 1_qty));
  CHECK_FALSE(q.ask(Price{}, 1_qty));
  CHECK_FALSE(q.ask(102_px, Qty{}));
  REQUIRE(q.bids.size() == 1);
  REQUIRE(q.asks.size() == 1);
  CHECK(q.bids[0] == Level{100_px, 1_qty});
  CHECK(q.asks[0] == Level{101_px, 1_qty});

  DesiredQuotes full;
  for (std::size_t k = 0; k < kMaxQuoteLevels; ++k)
    CHECK(full.ask(Price::from_int(100 + static_cast<std::int64_t>(k)), 1_qty));
  CHECK_FALSE(full.ask(200_px, 1_qty));  // beyond kMaxQuoteLevels
  CHECK(full.asks.size() == kMaxQuoteLevels);
}

TEST_CASE("strategies.quoting: uncross moves a crossing level-0 ask above the bid") {
  const Price tick = 0.01_px;
  DesiredQuotes q;
  static_cast<void>(q.bid(100.05_px, 1_qty));
  static_cast<void>(q.bid(100.02_px, 1_qty));
  static_cast<void>(q.ask(100.03_px, 1_qty));
  static_cast<void>(q.ask(100.06_px, 1_qty));
  q.uncross(tick);
  CHECK(q.asks[0].price == 100.06_px);  // bid + tick
  CHECK(q.asks[1].price == 100.06_px);  // other levels untouched
  CHECK(q.bids[0].price == 100.05_px);

  DesiredQuotes equal;
  static_cast<void>(equal.bid(100_px, 1_qty));
  static_cast<void>(equal.ask(100_px, 1_qty));
  equal.uncross(tick);
  CHECK(equal.asks[0].price == 100.01_px);

  DesiredQuotes fine;
  static_cast<void>(fine.bid(99.99_px, 1_qty));
  static_cast<void>(fine.ask(100_px, 1_qty));
  fine.uncross(tick);
  CHECK(fine.asks[0].price == 100_px);

  DesiredQuotes one_sided;
  static_cast<void>(one_sided.bid(100_px, 1_qty));
  one_sided.uncross(tick);
  CHECK(one_sided.asks.empty());
  CHECK(one_sided.bids[0].price == 100_px);
}

TEST_CASE("strategies.quoting: keep_passive shifts a skewed ladder inside the touch") {
  const Price tick = 0.01_px;
  const Qty q1 = 1_qty;

  DesiredQuotes q;
  static_cast<void>(q.bids.push_back(Level{100.20_px, q1}));  // skewed through the 100.05 ask
  static_cast<void>(q.bids.push_back(Level{100.15_px, q1}));
  static_cast<void>(q.asks.push_back(Level{100.30_px, q1}));
  keep_passive(q, 100.00_px, 100.05_px, tick);
  REQUIRE(q.bids.size() == 2);
  CHECK(q.bids[0].price == 100.04_px);  // one tick inside the ask
  CHECK(q.bids[1].price == 99.99_px);   // spacing preserved
  CHECK(q.asks[0].price == 100.30_px);  // already passive

  DesiredQuotes a;
  static_cast<void>(a.asks.push_back(Level{99.90_px, q1}));  // skewed through the 100.00 bid
  static_cast<void>(a.asks.push_back(Level{99.95_px, q1}));
  static_cast<void>(a.bids.push_back(Level{99.80_px, q1}));
  keep_passive(a, 100.00_px, 100.05_px, tick);
  CHECK(a.asks[0].price == 100.01_px);
  CHECK(a.asks[1].price == 100.06_px);
  CHECK(a.bids[0].price == 99.80_px);

  DesiredQuotes empty_book;
  static_cast<void>(empty_book.bids.push_back(Level{100.20_px, q1}));
  keep_passive(empty_book, Price{}, Price{}, tick);  // no opposite side: unchanged
  CHECK(empty_book.bids[0].price == 100.20_px);

  // Levels shifted to a non-positive price are removed.
  DesiredQuotes low;
  static_cast<void>(low.bids.push_back(Level{0.05_px, q1}));
  static_cast<void>(low.bids.push_back(Level{0.03_px, q1}));
  static_cast<void>(low.bids.push_back(Level{0.01_px, q1}));
  keep_passive(low, Price{}, 0.03_px, tick);
  REQUIRE(low.bids.size() == 1);
  CHECK(low.bids[0].price == 0.02_px);
}
