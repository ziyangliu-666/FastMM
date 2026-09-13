#include "fastmm/strategies/basic_mm.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/registry.hpp"

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
  TimerId add_timer(Duration, bool, std::uint64_t) { return TimerId{1}; }
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
std::unique_ptr<IEngineRunner> fake_factory(TransportKind, RunnerDeps&) {
  return nullptr;
}
}  // namespace
FASTMM_REGISTER_STRATEGY(BasicMM, fake_factory);

TEST_CASE("strategies.basic_mm: skewed quotes are clamped inside the touch") {
  const Price tick = Price::from_decimal("0.01").value();
  const Qty q1 = Qty::from_decimal("1").value();
  auto lvl = [&](const char* p) { return Level{Price::from_decimal(p).value(), q1}; };

  DesiredQuotes q;
  static_cast<void>(q.bids.push_back(lvl("100.20")));  // skewed through the 100.05 ask
  static_cast<void>(q.bids.push_back(lvl("100.15")));
  static_cast<void>(q.asks.push_back(lvl("100.30")));
  BasicMM::clamp_to_touch(
      q, Price::from_decimal("100.00").value(), Price::from_decimal("100.05").value(), tick);
  REQUIRE(q.bids.size() == 2);
  CHECK(q.bids[0].price == Price::from_decimal("100.04").value());  // one tick inside the ask
  CHECK(q.bids[1].price == Price::from_decimal("99.99").value());   // spacing preserved
  CHECK(q.asks[0].price == Price::from_decimal("100.30").value());  // already passive

  DesiredQuotes a;
  static_cast<void>(a.asks.push_back(lvl("99.90")));  // skewed through the 100.00 bid
  static_cast<void>(a.bids.push_back(lvl("99.80")));
  BasicMM::clamp_to_touch(
      a, Price::from_decimal("100.00").value(), Price::from_decimal("100.05").value(), tick);
  CHECK(a.asks[0].price == Price::from_decimal("100.01").value());
  CHECK(a.bids[0].price == Price::from_decimal("99.80").value());

  DesiredQuotes empty_book;
  static_cast<void>(empty_book.bids.push_back(lvl("100.20")));
  BasicMM::clamp_to_touch(empty_book, Price{}, Price{}, tick);  // no opposite side: unchanged
  CHECK(empty_book.bids[0].price == Price::from_decimal("100.20").value());
}

TEST_CASE("strategies.registry: registration, lookup, listing") {
  const auto& entries = list_strategies();
  REQUIRE_FALSE(entries.empty());
  const StrategyEntry* e = StrategyRegistry::instance().find("basic_mm");
  REQUIRE(e != nullptr);
  CHECK(e->schema == &BasicMM::schema());
  CHECK(StrategyRegistry::instance().find("nope") == nullptr);
  RunnerDeps deps;
  CHECK(StrategyRegistry::instance().make("basic_mm", TransportKind::Sim, deps) ==
        nullptr);  // fake factory
  CHECK(StrategyRegistry::instance().make("nope", TransportKind::Sim, deps) == nullptr);
  CHECK_FALSE(StrategyRegistry::instance().add(
      StrategyEntry{"basic_mm", &BasicMM::schema(), fake_factory}));  // dup
  CHECK(to_string(TransportKind::Live) == "live");
}
