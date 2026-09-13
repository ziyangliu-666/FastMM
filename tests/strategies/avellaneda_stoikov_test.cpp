#include "fastmm/strategies/avellaneda_stoikov.hpp"

#include "test_support.hpp"

#include <cmath>

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
  Timestamp now() const { return Timestamp{seconds(10).ns}; }
};
}  // namespace

TEST_CASE("strategies.avellaneda_stoikov: closed-form quotes for fixed inputs") {
  AvellanedaStoikov s;
  REQUIRE_FALSE(s.configure({{"gamma", "0.5"},
                             {"kappa", "1.5"},
                             {"quote_qty", "1"},
                             {"max_inventory", "5"},
                             {"sigma_init", "2"},  // sigma^2 = 4
                             {"infinite_horizon", "true"},
                             {"min_half_spread_ticks", "1"}}));
  NoCtx ctx;
  s.on_start(ctx);
  const Instrument i = inst();
  // r = m - q*gamma*sigma2*tau = 100 - 0 ; delta = 0.5*4*1/2 + (1/0.5)ln(1+0.5/1.5) = 1 + 2*ln(4/3)
  const double delta = 1.0 + 2.0 * std::log(4.0 / 3.0);  // ~1.5754
  DesiredQuotes q = s.compute_quotes(InstrumentId{0}, px("100"), Qty{}, i, ctx.now());
  REQUIRE(q.bids.size() == 1);
  REQUIRE(q.asks.size() == 1);
  CHECK(q.bids[0].price == Price::from_double(std::floor((100.0 - delta) * 100.0) / 100.0));
  CHECK(q.asks[0].price == Price::from_double(std::ceil((100.0 + delta) * 100.0) / 100.0));
  CHECK(q.bids[0].qty == qt("1"));
  // inventory q=2 shifts the reservation price down by 2*0.5*4 = 4
  q = s.compute_quotes(InstrumentId{0}, px("100"), qt("2"), i, ctx.now());
  CHECK(q.bids[0].price == Price::from_double(std::floor((96.0 - delta) * 100.0) / 100.0));
  CHECK(q.asks[0].price == Price::from_double(std::ceil((96.0 + delta) * 100.0) / 100.0));
  // short inventory shifts up symmetrically
  q = s.compute_quotes(InstrumentId{0}, px("100"), qt("-2"), i, ctx.now());
  CHECK(q.bids[0].price == Price::from_double(std::floor((104.0 - delta) * 100.0) / 100.0));
  // inventory cap
  q = s.compute_quotes(InstrumentId{0}, px("100"), qt("5"), i, ctx.now());
  CHECK(q.bids.empty());
  CHECK(q.asks.size() == 1);
  // determinism
  const DesiredQuotes a = s.compute_quotes(InstrumentId{0}, px("31337.42"), qt("1"), i, ctx.now());
  const DesiredQuotes b = s.compute_quotes(InstrumentId{0}, px("31337.42"), qt("1"), i, ctx.now());
  CHECK(a.bids[0] == b.bids[0]);
  CHECK(a.asks[0] == b.asks[0]);
  // min half spread floor: tiny gamma/sigma -> at least 1 tick each side
  AvellanedaStoikov tight;
  REQUIRE_FALSE(tight.configure({{"gamma", "0.001"},
                                 {"kappa", "1000"},
                                 {"sigma_init", "0.0001"},
                                 {"quote_qty", "1"},
                                 {"max_inventory", "5"},
                                 {"min_half_spread_ticks", "2"}}));
  tight.on_start(ctx);
  q = tight.compute_quotes(InstrumentId{0}, px("100"), Qty{}, i, ctx.now());
  CHECK(q.bids[0].price == px("99.98"));
  CHECK(q.asks[0].price == px("100.02"));
  CHECK(std::string(AvellanedaStoikov::name()) == "avellaneda_stoikov");
}

TEST_CASE("strategies.avellaneda_stoikov: finite horizon tau shrinks the spread over time") {
  AvellanedaStoikov s;
  REQUIRE_FALSE(s.configure({{"gamma", "0.5"},
                             {"kappa", "1.5"},
                             {"quote_qty", "1"},
                             {"max_inventory", "5"},
                             {"sigma_init", "0.1"},
                             {"infinite_horizon", "false"},
                             {"horizon_s", "100"},
                             {"min_half_spread_ticks", "0"}}));
  NoCtx ctx;
  s.on_start(ctx);  // start at t=10 s
  const Instrument i = inst();
  const DesiredQuotes early =
      s.compute_quotes(InstrumentId{0}, px("100"), Qty{}, i, Timestamp{seconds(10).ns});
  const DesiredQuotes late =
      s.compute_quotes(InstrumentId{0}, px("100"), Qty{}, i, Timestamp{seconds(105).ns});
  REQUIRE(early.bids.size() == 1);
  REQUIRE(late.bids.size() == 1);
  CHECK((early.asks[0].price - early.bids[0].price) > (late.asks[0].price - late.bids[0].price));
}
