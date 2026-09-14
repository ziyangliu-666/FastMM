#include "fastmm/strategies/basic_mm.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/avellaneda_stoikov.hpp"
#include "fastmm/strategies/registry.hpp"

#include <array>
#include <cstdio>
#include <random>
#include <string>

using namespace fastmm;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
Instrument inst() {
  Instrument i{};
  i.id = InstrumentId{0};
  i.tick = px("0.01");
  i.lot = qt("0.001");
  return i;
}
struct NoCtx {
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
};
}  // namespace

TEST_CASE("strategies.basic_mm: deterministic quotes, skew, inventory cap, levels") {
  BasicMM s;
  REQUIRE_FALSE(s.configure({{"half_spread_bps", "10"},
                             {"skew_bps_per_unit", "5"},
                             {"quote_qty", "0.01"},
                             {"max_inventory", "0.02"},
                             {"levels", "2"},
                             {"level_step_ticks", "3"}}));
  NoCtx ctx;
  s.on_start(ctx);
  const Instrument i = inst();
  // mid 100: half = 0.10 -> bid 99.90 / ask 100.10, level 2 at +-0.03
  DesiredQuotes q = s.compute_quotes(px("100"), Qty{}, i);
  REQUIRE(q.bids.size() == 2);
  REQUIRE(q.asks.size() == 2);
  CHECK(q.bids[0] == Level{px("99.90"), qt("0.01")});
  CHECK(q.bids[1] == Level{px("99.87"), qt("0.01")});
  CHECK(q.asks[0] == Level{px("100.10"), qt("0.01")});
  CHECK(q.asks[1] == Level{px("100.13"), qt("0.01")});
  // long 1 unit: skew both down by 5 bps = 0.05
  q = s.compute_quotes(px("100"), qt("0.01"), i);
  CHECK(q.bids[0].price == px("99.85"));
  CHECK(q.asks[0].price == px("100.05"));
  // short 1 unit: skew up 0.05
  q = s.compute_quotes(px("100"), qt("-0.01"), i);
  CHECK(q.bids[0].price == px("99.95"));
  CHECK(q.asks[0].price == px("100.15"));
  // at the cap: no bids when long 0.02 (0.02 + 0.01 > 0.02)
  q = s.compute_quotes(px("100"), qt("0.02"), i);
  CHECK(q.bids.empty());
  CHECK(q.asks.size() == 2);
  q = s.compute_quotes(px("100"), qt("-0.02"), i);
  CHECK(q.asks.empty());
  // rounding is passive: mid 100.005 -> bid floor, ask ceil
  q = s.compute_quotes(px("100.005"), Qty{}, i);
  CHECK(q.bids[0].price == px("99.90"));   // 99.904995 floored
  CHECK(q.asks[0].price == px("100.11"));  // 100.105005 ceiled
  // same inputs -> same outputs (determinism)
  const DesiredQuotes a = s.compute_quotes(px("12345.67"), qt("0.005"), i);
  const DesiredQuotes b = s.compute_quotes(px("12345.67"), qt("0.005"), i);
  for (std::size_t k = 0; k < a.bids.size(); ++k) CHECK(a.bids[k] == b.bids[k]);
  CHECK(std::string(BasicMM::name()) == "basic_mm");
  CHECK(BasicMM::schema().find("half_spread_bps") != nullptr);
}

namespace {
struct FakeBook {
  Price bid, ask;
  bool is_valid() const { return true; }
  Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  Level best_bid() const { return Level{bid, qt("1")}; }
  Level best_ask() const { return Level{ask, qt("1")}; }
};
struct FakePosition {
  Qty qty{};
};
struct QuoteCtx {
  std::array<Instrument, 1> list{[] {
    Instrument i = inst();
    i.venue = VenueId{0};
    return i;
  }()};
  FakeBook b{px("100.00"), px("100.10")};
  int set_quotes_calls = 0;
  bool quoting = true;
  const std::array<Instrument, 1>& instruments() const { return list; }
  const Instrument& instrument(InstrumentId) const { return list[0]; }
  const FakeBook& book(InstrumentId) const { return b; }
  Timestamp now() const { return Timestamp{}; }
  FakePosition position(InstrumentId) const { return {}; }
  bool set_quotes(InstrumentId, const DesiredQuotes&) {
    ++set_quotes_calls;
    return quoting;
  }
  void pull_quotes(InstrumentId) {}
};
ConnectionStateMsg connection(ConnState state) {
  ConnectionStateMsg m{};
  m.hdr.venue = VenueId{0};
  m.state = state;
  return m;
}
}  // namespace

TEST_CASE("strategies: a connection change forces the next requote even if the mid is unchanged") {
  const auto check = [](auto& strategy) {
    QuoteCtx ctx;
    const InstrumentId id{0};
    strategy.on_book(ctx, id, ctx.b);
    strategy.on_book(ctx, id, ctx.b);
    CHECK(ctx.set_quotes_calls == 1);  // mid unchanged: no requote
    strategy.on_connection(ctx, connection(ConnState::Disconnected));
    CHECK(ctx.set_quotes_calls == 1);
    strategy.on_book(ctx, id, ctx.b);
    CHECK(ctx.set_quotes_calls == 2);  // the drop forgot the last quoted mid
    strategy.on_connection(ctx, connection(ConnState::Live));
    CHECK(ctx.set_quotes_calls == 3);  // Live again: requote at once
    strategy.on_book(ctx, id, ctx.b);
    CHECK(ctx.set_quotes_calls == 3);
  };
  SUBCASE("basic_mm") {
    BasicMM s;
    check(s);
  }
  SUBCASE("avellaneda_stoikov") {  // used to fail: the variance update re-set the gating mid
    AvellanedaStoikov s;
    check(s);
  }
}

namespace {
// BasicMM::compute_quotes as it was before the fixed-point helpers (centi-bps and Int128 by hand),
// kept to prove the port is bit-identical.
DesiredQuotes centi_bps_quotes(Price mid,
                               Qty position,
                               const Instrument& inst,
                               std::int64_t half_spread_cbps,
                               std::int64_t skew_cbps,
                               Qty quote_qty,
                               Qty max_inventory,
                               int levels,
                               int level_step_ticks) {
  DesiredQuotes q;
  if (quote_qty.is_zero()) return q;
  const std::int64_t inv_units = position.raw / quote_qty.raw;
  const auto half = Price::from_raw(
      static_cast<std::int64_t>(static_cast<Int128>(mid.raw) * half_spread_cbps / 1'000'000));
  const auto skew = Price::from_raw(
      static_cast<std::int64_t>(static_cast<Int128>(mid.raw) * skew_cbps * -inv_units / 1'000'000));
  const Price centre = mid + skew;
  const Price step = Price::from_raw(inst.tick.raw * level_step_ticks);
  const bool can_buy = max_inventory.is_zero() || position + quote_qty <= max_inventory;
  const bool can_sell = max_inventory.is_zero() || position - quote_qty >= -max_inventory;
  const Qty qty = inst.round_qty(quote_qty);
  for (int l = 0; l < levels; ++l) {
    const Price off = Price::from_raw(step.raw * l);
    if (can_buy) {
      Price bid = inst.round_price(centre - half - off, Side::Buy);
      if (bid.is_positive()) static_cast<void>(q.bids.push_back(Level{bid, qty}));
    }
    if (can_sell) {
      Price ask = inst.round_price(centre + half + off, Side::Sell);
      static_cast<void>(q.asks.push_back(Level{ask, qty}));
    }
  }
  if (!q.bids.empty() && !q.asks.empty() && q.bids[0].price >= q.asks[0].price) {
    q.asks[0].price = q.bids[0].price + inst.tick;
  }
  return q;
}

std::string centi(std::int64_t cbps) {
  char buf[32];
  std::snprintf(buf,
                sizeof(buf),
                "%lld.%02lld",
                static_cast<long long>(cbps / 100),
                static_cast<long long>(cbps % 100));
  return buf;
}
}  // namespace

TEST_CASE("strategies.basic_mm: compute_quotes is bit-identical to the centi-bps formula") {
  std::mt19937_64 rng(4242);
  std::uniform_int_distribution<std::int64_t> mid_raw(100'000'000, 10'000'000'000'000);
  std::uniform_int_distribution<std::int64_t> half_cbps(0, 5'000);
  std::uniform_int_distribution<std::int64_t> skew_cbps(0, 500);
  std::uniform_int_distribution<std::int64_t> pos_raw(-3'000'000, 3'000'000);  // +-0.03
  std::uniform_int_distribution<int> small(0, 7);
  const Instrument i = inst();
  NoCtx ctx;
  int compared = 0;
  for (int n = 0; n < 400; ++n) {
    const std::int64_t h = half_cbps(rng);
    const std::int64_t k = skew_cbps(rng);
    const Qty quote_qty = Qty::from_raw(100'000 * (1 + small(rng)));  // 0.001 .. 0.008
    const Qty max_inventory = (n % 3 == 0) ? Qty{} : qt("0.02");
    const int levels = 1 + small(rng);
    const int step = 1 + small(rng);
    BasicMM s;
    REQUIRE_FALSE(s.configure({{"half_spread_bps", centi(h)},
                               {"skew_bps_per_unit", centi(k)},
                               {"quote_qty", std::to_string(quote_qty.raw) + "e-8"},
                               {"max_inventory", std::to_string(max_inventory.raw) + "e-8"},
                               {"levels", std::to_string(levels)},
                               {"level_step_ticks", std::to_string(step)}}));
    s.on_start(ctx);
    for (int m = 0; m < 50; ++m) {
      const Price mid = Price::from_raw(mid_raw(rng));
      const Qty position = Qty::from_raw(pos_raw(rng));
      const DesiredQuotes got = s.compute_quotes(mid, position, i);
      const DesiredQuotes want =
          centi_bps_quotes(mid, position, i, h, k, quote_qty, max_inventory, levels, step);
      CAPTURE(mid.raw);
      CAPTURE(position.raw);
      CAPTURE(h);
      CAPTURE(k);
      REQUIRE(got.bids.size() == want.bids.size());
      REQUIRE(got.asks.size() == want.asks.size());
      for (std::size_t x = 0; x < got.bids.size(); ++x) CHECK(got.bids[x] == want.bids[x]);
      for (std::size_t x = 0; x < got.asks.size(); ++x) CHECK(got.asks[x] == want.asks[x]);
      ++compared;
    }
  }
  CHECK(compared == 20'000);
}

TEST_CASE("strategies.basic_mm: bps keep four decimals and quantities parse exactly") {
  BasicMM s;
  // 0.003 bps of 60000 is 0.018: the centi-bps version rounded it to 0 bps.
  REQUIRE_FALSE(s.configure({{"half_spread_bps", "0.003"},
                             {"skew_bps_per_unit", "0"},
                             {"quote_qty", "2e-05"},
                             {"max_inventory", "0"},
                             {"pull_on_stale_ms", "0"}}));
  CHECK(s.params().half_spread_bps.raw == 30);
  CHECK(s.params().quote_qty.raw == 2'000);
  Instrument i = inst();
  i.lot = qt("0.00001");
  const DesiredQuotes q = s.compute_quotes(px("60000"), Qty{}, i);
  REQUIRE(q.bids.size() == 1);
  REQUIRE(q.asks.size() == 1);
  CHECK(q.bids[0].price == px("59999.98"));  // 60000 - 0.018, rounded down
  CHECK(q.asks[0].price == px("60000.02"));  // 60000 + 0.018, rounded up
  CHECK(q.bids[0].qty == qt("0.00002"));
  CHECK(s.configure({{"half_spread_bps", "0.00001"}}).value().find("at most 4 decimals") !=
        std::string::npos);
  CHECK(BasicMM::schema().find("half_spread_bps")->type == ParamType::Bps);
  CHECK(BasicMM::schema().find("quote_qty")->type == ParamType::Decimal);
  CHECK(BasicMM::schema().find("pull_on_stale_ms")->type == ParamType::Millis);
}
