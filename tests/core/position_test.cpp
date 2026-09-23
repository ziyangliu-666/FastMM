#include "fastmm/core/position.hpp"

#include "test_support.hpp"

using namespace fastmm;

namespace {
Instrument spot() {
  Instrument i{};
  i.id = InstrumentId{0};
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  return i;
}
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}
}  // namespace

TEST_CASE("core.position: average cost, realized, flips through zero, marks, fees") {
  const Instrument inst = spot();
  PositionTracker t;
  const InstrumentId id = inst.id;
  CHECK(t[id].flat());
  t.on_fill(id, Side::Buy, px("100"), qt("1"), nt("0.1"), inst);
  CHECK(t[id].qty == qt("1"));
  CHECK(t[id].avg_px == px("100"));
  t.on_fill(id, Side::Buy, px("110"), qt("1"), nt("0.1"), inst);
  CHECK(t[id].qty == qt("2"));
  CHECK(t[id].avg_px == px("105"));
  CHECK(t[id].fees == nt("0.2"));
  // partial close at 115: realized = 1 * (115 - 105) = 10
  t.on_fill(id, Side::Sell, px("115"), qt("1"), Notional{}, inst);
  CHECK(t[id].qty == qt("1"));
  CHECK(t[id].avg_px == px("105"));
  CHECK(t[id].realized == nt("10"));
  // flip through zero: sell 3 at 100 -> close 1 (-5), open short 2 at 100
  t.on_fill(id, Side::Sell, px("100"), qt("3"), Notional{}, inst);
  CHECK(t[id].qty == qt("-2"));
  CHECK(t[id].avg_px == px("100"));
  CHECK(t[id].realized == nt("5"));
  // mark at 90: short 2 -> unrealized +20
  t.mark(id, px("90"), inst);
  CHECK(t[id].unrealized == nt("20"));
  CHECK(t[id].net_pnl() == nt("24.8"));  // 5 + 20 - 0.2
  // cover all at 95: realized += 2 * (100 - 95) = 10 -> 15; flat, unrealized 0
  t.on_fill(id, Side::Buy, px("95"), qt("2"), Notional{}, inst);
  CHECK(t[id].flat());
  CHECK(t[id].avg_px == Price{});
  CHECK(t[id].realized == nt("15"));
  CHECK(t[id].unrealized == Notional{});
  CHECK(t[id].fills == 5);
  CHECK(t[id].gross_traded == qt("8"));
  CHECK(t.total_realized() == nt("15"));
  CHECK(t.net_pnl() == nt("14.8"));
  // fractional sizes / prices keep exactness
  t.on_fill(id, Side::Buy, px("50000.5"), qt("0.002"), Notional{}, inst);
  t.on_fill(id, Side::Sell, px("50001.5"), qt("0.002"), Notional{}, inst);
  CHECK(t[id].realized == nt("15.002"));
  t.set(id, qt("3"), px("10"), inst);
  CHECK(t[id].qty == qt("3"));
  t.reset(id);
  CHECK(t[id].fills == 0);
}

namespace {
// An inverse (coin-margined) contract: 10 USD of notional each, PnL in the base coin.
Instrument inverse_future() {
  Instrument i{};
  i.id = InstrumentId{0};
  i.tick = Price::from_decimal("0.5").value();
  i.lot = Qty::from_int(1);
  i.contract_multiplier = Qty::from_int(10);
  i.flags = Instrument::kEnabled | Instrument::kInverse;
  static_cast<void>(i.base.assign("BTC"));
  static_cast<void>(i.quote.assign("USD"));
  return i;
}
}  // namespace

// Hand-computed against qty * multiplier * (1/avg - 1/px), the coin PnL of an inverse contract.
// The linear formula (px - avg) * qty used to be applied here, which books USD as if it were coin.
TEST_CASE("core.position: inverse contract pnl, harmonic average and a long to short flip") {
  const Instrument inst = inverse_future();
  const InstrumentId id = inst.id;
  PositionTracker t;

  // 10 contracts (100 USD) at 50000 cost 100/50000 = 0.002 BTC.
  t.on_fill(id, Side::Buy, px("50000"), qt("10"), Notional{}, inst);
  CHECK(t[id].avg_px == px("50000"));
  // 10 more at 40000 cost 0.0025 BTC: 200 USD for 0.0045 BTC, i.e. 44444.44444444 on average.
  t.on_fill(id, Side::Buy, px("40000"), qt("10"), Notional{}, inst);
  CHECK(t[id].qty == qt("20"));
  CHECK(t[id].avg_px == Price::from_raw(4444444444444));

  // Mark at 45000: 200 * (1/44444.44444444 - 1/45000) = 0.00005555 BTC.
  t.mark(id, px("45000"), inst);
  CHECK(t[id].unrealized == Notional::from_raw(5555));
  CHECK(t.total_unrealized() == Notional::from_raw(5555));

  // Sell 30 at 50000: closes 20 for 200 * (1/44444.44444444 - 1/50000) = 0.0005 BTC and opens a
  // short of 10 at 50000.
  t.on_fill(id, Side::Sell, px("50000"), qt("30"), Notional{}, inst);
  CHECK(t[id].realized == Notional::from_decimal("0.0005").value());
  CHECK(t[id].qty == qt("-10"));
  CHECK(t[id].avg_px == px("50000"));

  // Cover the short at 40000: 100 * (1/40000 - 1/50000) = 0.0005 BTC, so 0.001 BTC in total.
  t.on_fill(id, Side::Buy, px("40000"), qt("10"), Notional{}, inst);
  CHECK(t[id].flat());
  CHECK(t[id].realized == Notional::from_decimal("0.001").value());
  CHECK(t[id].unrealized == Notional{});
  CHECK(t.total_realized() == Notional::from_decimal("0.001").value());
}

TEST_CASE("core.position: inverse notional and tick value") {
  const Instrument inst = inverse_future();
  // 10 contracts at 50000 are 100 USD, i.e. 0.002 BTC, not 5,000,000.
  CHECK(inst.notional(px("50000"), qt("10")) == Notional::from_decimal("0.002").value());
  CHECK(inst.notional(Price{}, qt("10")) == Notional{});
  // The tick value of an inverse contract depends on the price, so it has no fixed answer.
  CHECK(inst.pnl_per_tick() == Notional{});
  CHECK(inst.settlement_ccy() == "BTC");
  CHECK(spot().settlement_ccy().empty());
}

// set() used to leave unrealized (and the totals the max-loss budget reads) at the value of the
// position it replaced.
TEST_CASE("core.position: set() remeasures unrealized at the last mark") {
  const Instrument inst = spot();
  const InstrumentId id = inst.id;
  PositionTracker t;
  t.on_fill(id, Side::Buy, px("100"), qt("2"), Notional{}, inst);
  t.mark(id, px("110"), inst);
  CHECK(t[id].unrealized == nt("20"));
  CHECK(t.total_unrealized() == nt("20"));
  // The venue says we are flat: the 20 of open profit is not ours any more.
  t.set(id, Qty{}, Price{}, inst);
  CHECK(t[id].unrealized == Notional{});
  CHECK(t.total_unrealized() == Notional{});
  CHECK(t[id].realized == Notional{});  // realized history is kept
  // A different position marks at the same last mark.
  t.set(id, qt("-1"), px("120"), inst);
  CHECK(t[id].unrealized == nt("10"));  // short 1 from 120 marked at 110
  CHECK(t.total_unrealized() == nt("10"));
}

TEST_CASE("core.position: contract multiplier scales pnl") {
  Instrument fut = spot();
  fut.contract_multiplier = Qty::from_int(10);
  PositionTracker t;
  t.on_fill(fut.id, Side::Buy, px("100"), qt("2"), Notional{}, fut);
  t.mark(fut.id, px("101"), fut);
  CHECK(t[fut.id].unrealized == nt("20"));  // 2 contracts * 1 * 10
  t.on_fill(fut.id, Side::Sell, px("103"), qt("2"), Notional{}, fut);
  CHECK(t[fut.id].realized == nt("60"));
  CHECK(t[fut.id].unrealized == Notional{});
}
