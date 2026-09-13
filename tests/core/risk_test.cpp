#include "fastmm/core/risk.hpp"

#include "test_support.hpp"

using namespace fastmm;

namespace {
Instrument make_inst() {
  Instrument i{};
  i.id = InstrumentId{0};
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.min_qty = i.lot;
  i.min_notional = Notional::from_int(5);
  return i;
}
RiskLimits limits() {
  RiskLimits l;
  l.max_order_qty = Qty::from_int(10);
  l.max_order_notional = Notional::from_int(100'000);
  l.max_position = Qty::from_int(20);
  l.max_open_orders = 4;
  l.price_collar_bps = 100;  // 1 %
  l.fat_finger_bps = 500;    // 5 %
  l.stale_md = milliseconds(500);
  l.max_loss = Notional::from_int(1000);
  l.orders_per_sec = 1'000'000;
  l.burst = 1'000'000;
  l.stp = true;
  return l;
}
OrderIntent intent(Side s, const char* p, const char* q) {
  OrderIntent o{};
  o.instrument = InstrumentId{0};
  o.venue = VenueId{0};
  o.side = s;
  o.price = Price::from_decimal(p).value();
  o.qty = Qty::from_decimal(q).value();
  return o;
}
}  // namespace

TEST_CASE("core.risk: token bucket integer refill") {
  TokenBucket b(10, 3, Timestamp{0});  // 10/s, burst 3
  CHECK(b.try_take(Timestamp{0}));
  CHECK(b.try_take(Timestamp{0}));
  CHECK(b.try_take(Timestamp{0}));
  CHECK_FALSE(b.try_take(Timestamp{0}));
  CHECK_FALSE(b.try_take(Timestamp{milliseconds(99).ns}));
  CHECK(b.try_take(Timestamp{milliseconds(100).ns}));  // one token per 100 ms
  CHECK_FALSE(b.try_take(Timestamp{milliseconds(100).ns}));
  CHECK(b.try_take(Timestamp{milliseconds(250).ns}));
  CHECK(b.tokens_micro() == 500'000);  // half a token left
  b.refill(Timestamp{seconds(10).ns});
  CHECK(b.tokens() == 3);  // capped at burst
  TokenBucket off;
  CHECK(off.try_take(Timestamp{}));
  CHECK_FALSE(off.enabled());
}

TEST_CASE("core.risk: every reject reason in isolation and in order") {
  const Instrument inst = make_inst();
  const Timestamp now{seconds(100).ns};
  RiskEngine risk(limits(), now);
  risk.on_book(inst.id, Price::from_int(100), now);
  risk.on_trade(inst.id, Price::from_int(100));
  Position pos{};
  RiskInputs in{};
  in.now = now;
  in.position = &pos;
  auto ok = intent(Side::Buy, "99.5", "1");
  CHECK(risk.check_new(ok, inst, in) == RejectReason::None);
  CHECK(risk.stats().passed == 1);

  SUBCASE("kill switch global then venue") {
    risk.trip();
    CHECK(risk.killed());
    CHECK(risk.check_new(ok, inst, in) == RejectReason::KillSwitch);
    CHECK(RiskEngine::cancel_allowed());
    risk.reset();
    risk.trip_venue(VenueId{0});
    CHECK(risk.venue_killed(VenueId{0}));
    CHECK_FALSE(risk.venue_killed(VenueId{1}));
    CHECK(risk.check_new(ok, inst, in) == RejectReason::VenueKilled);
    risk.reset_venue(VenueId{0});
    CHECK(risk.check_new(ok, inst, in) == RejectReason::None);
    CHECK(risk.stats().trips == 2);
  }
  SUBCASE("instrument disabled") {
    Instrument off = inst;
    off.flags = 0;
    CHECK(risk.check_new(ok, off, in) == RejectReason::InstrumentDisabled);
  }
  SUBCASE("tick / lot / min notional") {
    CHECK(risk.check_new(intent(Side::Buy, "99.505", "1"), inst, in) == RejectReason::InvalidTick);
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "1.0005"), inst, in) ==
          RejectReason::InvalidLot);
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "0"), inst, in) == RejectReason::InvalidLot);
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "0.001"), inst, in) ==
          RejectReason::BelowMinNotional);
  }
  SUBCASE("stale market data") {
    in.now = now + milliseconds(501);
    CHECK(risk.check_new(ok, inst, in) == RejectReason::StaleMarketData);
    in.now = now + milliseconds(500);
    CHECK(risk.check_new(ok, inst, in) == RejectReason::None);
    RiskEngine fresh(limits(), now);  // no book yet
    CHECK(fresh.check_new(ok, inst, in) == RejectReason::StaleMarketData);
  }
  SUBCASE("collar vs mid (1 %)") {
    CHECK(risk.check_new(intent(Side::Buy, "98.99", "1"), inst, in) == RejectReason::PriceCollar);
    CHECK(risk.check_new(intent(Side::Sell, "101.01", "1"), inst, in) == RejectReason::PriceCollar);
    CHECK(risk.check_new(intent(Side::Buy, "99", "1"), inst, in) == RejectReason::None);
    CHECK(risk.check_new(intent(Side::Sell, "101", "1"), inst, in) == RejectReason::None);
  }
  SUBCASE("fat finger vs last trade (5 %) evaluated after collar") {
    risk.on_book(inst.id, Price::from_int(100), now);
    risk.on_trade(inst.id, Price::from_int(90));  // collar around 100, fat finger around 90
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "1"), inst, in) ==
          RejectReason::FatFinger);  // > 94.5
    RiskLimits l = limits();
    l.price_collar_bps = 0;
    risk.set_limits(l, now);
    risk.on_book(inst.id, Price::from_int(100), now);
    CHECK(risk.check_new(intent(Side::Buy, "94.51", "1"), inst, in) == RejectReason::FatFinger);
    CHECK(risk.check_new(intent(Side::Buy, "85.49", "1"), inst, in) == RejectReason::FatFinger);
    CHECK(risk.check_new(intent(Side::Buy, "94.5", "1"), inst, in) == RejectReason::None);
  }
  SUBCASE("max qty then max notional") {
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "10.001"), inst, in) ==
          RejectReason::MaxOrderQty);
    RiskLimits l = limits();
    l.max_order_qty = Qty::from_int(100000);
    l.max_order_notional = Notional::from_int(500);
    risk.set_limits(l, now);
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "6"), inst, in) ==
          RejectReason::MaxOrderNotional);
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "5"), inst, in) == RejectReason::None);
  }
  SUBCASE("predicted position includes same-side open orders; reducing is allowed") {
    pos.qty = Qty::from_int(15);
    in.open_same_side = Qty::from_int(4);
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "2"), inst, in) ==
          RejectReason::MaxPosition);  // 15+4+2 > 20
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "1"), inst, in) == RejectReason::None);
    in.open_same_side = Qty{};
    CHECK(risk.check_new(intent(Side::Sell, "100.5", "10"), inst, in) ==
          RejectReason::None);  // reduces
    pos.qty =
        Qty::from_int(-25);  // already beyond the cap short: buying reduces, selling not allowed
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "1"), inst, in) == RejectReason::None);
    CHECK(risk.check_new(intent(Side::Sell, "100.5", "1"), inst, in) == RejectReason::MaxPosition);
  }
  SUBCASE("open order cap (new only, not replace)") {
    in.open_orders = 4;
    CHECK(risk.check_new(ok, inst, in) == RejectReason::MaxOpenOrders);
    Order existing{};
    existing.qty = Qty::from_int(1);
    CHECK(risk.check_replace(ok, existing, inst, in) == RejectReason::None);
  }
  SUBCASE("self trade prevention") {
    in.best_own_opposite = Price::from_decimal("99.5").value();  // our ask at 99.5
    CHECK(risk.check_new(intent(Side::Buy, "99.5", "1"), inst, in) ==
          RejectReason::SelfTradePrevention);
    CHECK(risk.check_new(intent(Side::Buy, "99.49", "1"), inst, in) == RejectReason::None);
    in.best_own_opposite = Price::from_decimal("100.5").value();  // our bid at 100.5
    CHECK(risk.check_new(intent(Side::Sell, "100.5", "1"), inst, in) ==
          RejectReason::SelfTradePrevention);
    CHECK(risk.check_new(intent(Side::Sell, "100.51", "1"), inst, in) == RejectReason::None);
    RiskLimits l = limits();
    l.stp = false;
    risk.set_limits(l, now);
    CHECK(risk.check_new(intent(Side::Sell, "100.5", "1"), inst, in) == RejectReason::None);
  }
  SUBCASE("rate limit last; tokens only consumed on pass") {
    RiskLimits l = limits();
    l.orders_per_sec = 10;
    l.burst = 2;
    risk.set_limits(l, now);
    CHECK(risk.check_new(ok, inst, in) == RejectReason::None);  // token 1
    CHECK(risk.check_new(intent(Side::Buy, "98", "1"), inst, in) ==
          RejectReason::PriceCollar);  // no token used
    CHECK(risk.check_new(ok, inst, in) == RejectReason::None);
    CHECK(risk.check_new(ok, inst, in) == RejectReason::RateLimit);
    in.now = now + milliseconds(100);
    CHECK(risk.check_new(ok, inst, in) == RejectReason::None);
    CHECK(risk.stats().rejects[static_cast<int>(RejectReason::RateLimit)] == 1);
  }
  SUBCASE("max loss trips the kill switch") {
    CHECK_FALSE(risk.on_pnl(Notional::from_int(-999)));
    CHECK(risk.on_pnl(Notional::from_int(-1000)));
    CHECK(risk.killed());
    CHECK(risk.check_new(ok, inst, in) == RejectReason::KillSwitch);
  }
  SUBCASE("market orders skip price checks and use mid for notional") {
    auto mkt = intent(Side::Buy, "0", "1");
    mkt.type = OrderType::Market;
    CHECK(risk.check_new(mkt, inst, in) == RejectReason::None);
  }
}
