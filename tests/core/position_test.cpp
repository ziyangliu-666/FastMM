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
  t.set(id, qt("3"), px("10"));
  CHECK(t[id].qty == qt("3"));
  t.reset(id);
  CHECK(t[id].fills == 0);
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
