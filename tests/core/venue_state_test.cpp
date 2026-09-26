// What the engine knows about a venue and hands to strategies: per-instrument fee rates
// (StrategyContext::fees), what each risk limit still admits (RiskHeadroom: a request of exactly a
// room passes that check, one lot more is refused) and venue health (feed lag against its
// baseline, ack round trip) with the feed-lag gate ([risk] max_feed_lag_ms).
#include "test_support.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/venue_health.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

Instrument make_inst() {
  Instrument i{};
  i.id = InstrumentId{0};
  i.venue = VenueId{0};
  i.symbol = "BTCUSDT";
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = i.lot;
  return i;
}

OrderIntent intent(Side s, const char* p, const char* q, TimeInForce tif = TimeInForce::Gtc) {
  OrderIntent o{};
  o.instrument = InstrumentId{0};
  o.venue = VenueId{0};
  o.side = s;
  o.price = px(p);
  o.qty = qt(q);
  o.tif = tif;
  return o;
}

// One RiskEngine with only `l` set, a book at 100 and the inputs of a buy and a sell.
struct RiskRig {
  Instrument inst = make_inst();
  Timestamp now{seconds(100).ns};
  RiskEngine risk;
  Position pos{};
  RiskInputs buy{};
  RiskInputs sell{};

  explicit RiskRig(const RiskLimits& l) : risk(l, Timestamp{seconds(100).ns}) {
    risk.on_book(inst.id, px("100"), now);
    buy.now = sell.now = now;
    buy.position = sell.position = &pos;
  }
  RiskHeadroom headroom(Notional net_pnl = {}) const {
    return risk.headroom(inst, buy, sell, net_pnl);
  }
  RejectReason order(Side s, Qty q) {
    const RiskInputs& in = s == Side::Buy ? buy : sell;
    OrderIntent o = intent(s, "100", "0.001");
    o.qty = q;
    return risk.check_new(o, inst, in);
  }
};

}  // namespace

// ---- risk headroom ------------------------------------------------------------------------------

TEST_CASE("core.venue_state: max_position room per side is exactly what check_new admits") {
  RiskLimits l;
  l.max_position = qt("1");
  RiskRig r(l);
  const Qty lot = r.inst.lot;
  for (const char* position : {"0", "0.3", "-0.4", "1.5", "-1.2"}) {
    for (const char* open : {"0", "0.2"}) {
      CAPTURE(position);
      CAPTURE(open);
      r.pos.qty = qt(position);
      r.buy.open_same_side = qt(open);
      r.sell.open_same_side = qt(open);
      const RiskHeadroom h = r.headroom();
      for (const Side s : {Side::Buy, Side::Sell}) {
        const Qty room = s == Side::Buy ? h.buy_qty : h.sell_qty;
        if (room.is_positive()) CHECK(r.order(s, room) == RejectReason::None);
        CHECK(r.order(s, room + lot) == RejectReason::MaxPosition);
      }
    }
  }
  // Long 0.3 with nothing open: buy up to 0.7, sell up to 1.3 (through zero to -1).
  r.pos.qty = qt("0.3");
  r.buy.open_same_side = r.sell.open_same_side = Qty{};
  CHECK(r.headroom().buy_qty == qt("0.7"));
  CHECK(r.headroom().sell_qty == qt("1.3"));
  // A position off the lot grid (a commission paid in the base asset): the room is rounded down.
  r.pos.qty = qt("0.3005");
  CHECK(r.headroom().buy_qty == qt("0.699"));
  CHECK(r.order(Side::Buy, r.headroom().buy_qty) == RejectReason::None);
  // Over the cap long: buys have no room; a sell may go through zero as far as -1.5, where
  // |position| has not grown.
  r.pos.qty = qt("1.5");
  CHECK(r.headroom().buy_qty == Qty{});
  CHECK(r.headroom().sell_qty == qt("3"));
}

TEST_CASE("core.venue_state: gross and net exposure rooms are what check_new admits") {
  RiskLimits l;
  l.max_gross_notional = nt("1000");
  l.max_net_notional = nt("600");
  RiskRig r(l);
  const Qty lot = r.inst.lot;
  r.buy.gross_exposure = r.sell.gross_exposure = nt("500");
  r.buy.net_exposure = r.sell.net_exposure = nt("-200");
  const RiskHeadroom h = r.headroom();
  CHECK(h.gross_notional == nt("500"));
  CHECK(h.net_buy_notional == nt("800"));
  CHECK(h.net_sell_notional == nt("400"));
  // At a price of 100 a notional room of N is N / 100 in quantity.
  const auto qty_of = [](Notional n) { return Qty::from_raw(n.raw / 100); };
  // Sell: the net room (400) binds before the gross one (500).
  CHECK(r.order(Side::Sell, qty_of(h.net_sell_notional)) == RejectReason::None);
  CHECK(r.order(Side::Sell, qty_of(h.net_sell_notional) + lot) == RejectReason::MaxNetNotional);
  // Buy: the gross room (500) binds before the net one (800).
  CHECK(r.order(Side::Buy, qty_of(h.gross_notional)) == RejectReason::None);
  CHECK(r.order(Side::Buy, qty_of(h.gross_notional) + lot) == RejectReason::MaxGrossNotional);

  // Net already past the cap short (-700 against 600): a buy may take it through zero to +700,
  // where |net| has not grown.
  RiskLimits net_only;
  net_only.max_net_notional = nt("600");
  RiskRig n(net_only);
  n.buy.net_exposure = n.sell.net_exposure = nt("-700");
  const RiskHeadroom hn = n.headroom();
  CHECK(hn.net_buy_notional == nt("1400"));
  CHECK(hn.net_sell_notional == Notional{});
  CHECK(n.order(Side::Buy, qty_of(hn.net_buy_notional)) == RejectReason::None);
  CHECK(n.order(Side::Buy, qty_of(hn.net_buy_notional) + lot) == RejectReason::MaxNetNotional);
  CHECK(n.order(Side::Sell, lot) == RejectReason::MaxNetNotional);
}

TEST_CASE("core.venue_state: order tokens, open orders and the loss budget") {
  RiskLimits l;
  l.orders_per_sec = 10;
  l.burst = 3;
  l.max_open_orders = 5;
  l.max_loss = nt("50");
  RiskRig r(l);
  CHECK(r.headroom().order_tokens == 3);
  for (int i = 0; i < 3; ++i) REQUIRE(r.order(Side::Buy, qt("0.001")) == RejectReason::None);
  CHECK(r.headroom().order_tokens == 0);
  CHECK(r.order(Side::Buy, qt("0.001")) == RejectReason::RateLimit);
  // 250 ms later: 2.5 tokens refilled, 2 whole ones, and exactly two orders pass.
  r.buy.now = r.sell.now = r.now + milliseconds(250);
  CHECK(r.headroom().order_tokens == 2);
  CHECK(r.order(Side::Buy, qt("0.001")) == RejectReason::None);
  CHECK(r.order(Side::Buy, qt("0.001")) == RejectReason::None);
  CHECK(r.order(Side::Buy, qt("0.001")) == RejectReason::RateLimit);

  r.buy.open_orders = 3;
  CHECK(r.headroom().open_orders == 2);
  r.buy.open_orders = 7;
  CHECK(r.headroom().open_orders == 0);

  CHECK(r.headroom(nt("-20")).loss_budget == nt("30"));
  CHECK_FALSE(r.risk.on_pnl(nt("-49.99999999")));
  CHECK(r.risk.on_pnl(nt("-50")));  // a budget of zero trips
  CHECK(r.headroom(nt("-50")).loss_budget == Notional{});
}

TEST_CASE("core.venue_state: a limit that is off reports no bound") {
  RiskRig r(RiskLimits{});
  const RiskHeadroom h = r.headroom();
  CHECK(h.order_tokens == RiskHeadroom::kUnlimited);
  CHECK(h.open_orders == RiskHeadroom::kUnlimited);
  CHECK(h.buy_qty == Qty::max());
  CHECK(h.sell_qty == Qty::max());
  CHECK(h.max_order_qty == Qty::max());
  CHECK(h.gross_notional == Notional::max());
  CHECK(h.net_buy_notional == Notional::max());
  CHECK(h.loss_budget == Notional::max());
  RiskLimits l;
  l.max_order_qty = qt("2");
  l.max_order_notional = nt("300");
  RiskRig s(l);
  CHECK(s.headroom().max_order_qty == qt("2"));
  CHECK(s.headroom().max_order_notional == nt("300"));
}

// ---- the FeedLag reject ------------------------------------------------------------------------

TEST_CASE(
    "core.venue_state: while the feed lags, orders that could rest are refused unless they "
    "reduce") {
  RiskRig r(RiskLimits{});
  r.buy.feed_lagged = r.sell.feed_lagged = true;
  const auto check = [&](Side s, const char* q, TimeInForce tif, const RiskInputs& in) {
    return r.risk.check_new(intent(s, "100", q, tif), r.inst, in);
  };
  CHECK(check(Side::Buy, "0.1", TimeInForce::Gtc, r.buy) == RejectReason::FeedLag);
  CHECK(check(Side::Buy, "0.1", TimeInForce::Ioc, r.buy) == RejectReason::None);
  CHECK(check(Side::Buy, "0.1", TimeInForce::Fok, r.buy) == RejectReason::None);
  r.pos.qty = qt("0.5");
  CHECK(check(Side::Sell, "0.5", TimeInForce::Gtc, r.sell) == RejectReason::None);
  CHECK(check(Side::Sell, "0.501", TimeInForce::Gtc, r.sell) == RejectReason::FeedLag);  // flips
  r.sell.open_same_side = qt("0.4");  // with what already works on that side, it would flip
  CHECK(check(Side::Sell, "0.2", TimeInForce::Gtc, r.sell) == RejectReason::FeedLag);
  CHECK(check(Side::Buy, "0.1", TimeInForce::Gtc, r.buy) == RejectReason::FeedLag);  // adds
  r.buy.feed_lagged = false;
  CHECK(check(Side::Buy, "0.1", TimeInForce::Gtc, r.buy) == RejectReason::None);
}

// ---- VenueHealth -------------------------------------------------------------------------------

TEST_CASE("core.venue_state: feed lag baseline is the minimum of the last 8 s") {
  VenueHealth h;
  const VenueId v{0};
  Timestamp t{seconds(1000).ns};
  const Duration off{};
  const auto md = [&](std::int64_t lag_us) { return h.on_md(v, microseconds(lag_us).ns, t, off); };
  CHECK(h.view(v, t).md_samples == 0);
  md(500);
  CHECK(h.view(v, t).feed_lag_base == microseconds(500));
  CHECK(h.view(v, t).feed_lag_excess == Duration{});
  t = t + milliseconds(10);
  md(300);
  md(12'000);
  VenueHealthView s = h.view(v, t);
  CHECK(s.feed_lag == microseconds(12'000));
  CHECK(s.feed_lag_base == microseconds(300));
  CHECK(s.feed_lag_excess == microseconds(11'700));
  CHECK(s.md_samples == 3);
  CHECK(s.md_updated == t);
  // A host clock step of +5 ms: every lag is 5 ms higher. The old floor stays the baseline until
  // its bucket leaves the 8 s window, then the new floor takes over.
  for (int i = 1; i <= 7; ++i) {
    t = t + seconds(1);
    md(5'300);
    CHECK(h.view(v, t).feed_lag_base == microseconds(300));
  }
  for (int i = 0; i < 2; ++i) {
    t = t + seconds(1);
    md(5'300);
  }
  CHECK(h.view(v, t).feed_lag_base == microseconds(5'300));
  CHECK(h.view(v, t).feed_lag_excess == Duration{});
  // A step down is taken at once.
  md(-2'000);
  CHECK(h.view(v, t).feed_lag_base == microseconds(-2'000));
  // After a silence longer than the window the first message starts a new baseline.
  t = t + seconds(30);
  md(9'000);
  CHECK(h.view(v, t).feed_lag_base == microseconds(9'000));
  // Other venues are separate; ids past the table are ignored.
  CHECK(h.view(VenueId{1}, t).md_samples == 0);
  CHECK_FALSE(h.on_md(VenueId{200}, 1, t, milliseconds(1)));
}

TEST_CASE("core.venue_state: the gate engages over the limit and holds 100 ms after the last one") {
  VenueHealth h;
  const VenueId v{0};
  const Duration limit = milliseconds(2);
  Timestamp t{seconds(50).ns};
  CHECK_FALSE(h.on_md(v, microseconds(300).ns, t, limit));
  CHECK_FALSE(h.on_md(v, microseconds(2'300).ns, t, limit));  // excess exactly 2 ms: not over
  CHECK_FALSE(h.gated(v, t));
  CHECK(h.on_md(v, microseconds(2'301).ns, t, limit));  // engages
  CHECK(h.gated(v, t));
  t = t + milliseconds(60);
  CHECK_FALSE(h.on_md(v, microseconds(9'000).ns, t, limit));  // already held: extends
  t = t + milliseconds(99);
  CHECK_FALSE(h.on_md(v, microseconds(400).ns, t, limit));
  CHECK(h.gated(v, t));
  t = t + milliseconds(1);
  CHECK_FALSE(h.gated(v, t));
  CHECK(h.view(v, t).gate_engagements == 1);
  CHECK(h.on_md(v, microseconds(5'000).ns, t, limit));
  CHECK(h.view(v, t).gate_engagements == 2);
  // Limit 0: never gated.
  VenueHealth off;
  CHECK_FALSE(off.on_md(v, seconds(1).ns, t, Duration{}));
  CHECK_FALSE(off.gated(v, t));
}

TEST_CASE("core.venue_state: ack round trip, last and smoothed") {
  VenueHealth h;
  const VenueId v{1};
  const Timestamp t{seconds(7).ns};
  h.on_ack(v, microseconds(1'600), t);
  CHECK(h.view(v, t).ack_rtt == microseconds(1'600));
  CHECK(h.view(v, t).ack_rtt_smoothed == microseconds(1'600));
  h.on_ack(v, microseconds(9'600), t);
  CHECK(h.view(v, t).ack_rtt == microseconds(9'600));
  CHECK(h.view(v, t).ack_rtt_smoothed == microseconds(2'600));  // 1.6 + (9.6 - 1.6) / 8
  CHECK(h.view(v, t).ack_samples == 2);
  CHECK(h.view(v, t).ack_updated == t);
}

// ---- in the engine ------------------------------------------------------------------------------

namespace {

struct Outbox {
  std::vector<std::vector<std::byte>> out;
  bool send(const EventHeader& m) noexcept {
    const auto* b = reinterpret_cast<const std::byte*>(&m);
    out.emplace_back(b, b + m.len);
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  bool supports_replace(VenueId) const noexcept { return false; }
  [[nodiscard]] const EventHeader& header(std::size_t i) const {
    return *reinterpret_cast<const EventHeader*>(out[i].data());
  }
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < out.size(); ++i) n += header(i).type == t ? 1U : 0U;
    return n;
  }
};
static_assert(TransportLike<Outbox>);

// Quotes one tick inside the touch on every book update; remembers what set_quotes said.
struct Quoter {
  static std::string_view name() noexcept { return "quoter"; }
  std::vector<bool> accepted;
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) {
    DesiredQuotes q;
    q.bid(b.best_bid().price - px("0.01"), qt("0.01"));
    q.ask(b.best_ask().price + px("0.01"), qt("0.01"));
    accepted.push_back(ctx.set_quotes(id, q));
  }
};

using GateEngine = Engine<Quoter, SimClock, Outbox, InlineFeed>;

struct Rig {
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  Outbox transport;
  InlineFeed feed{1 << 20};
  Quoter strategy;
  std::unique_ptr<GateEngine> engine;
  std::uint64_t seq = 0;

  explicit Rig(std::uint32_t max_feed_lag_ms, FeeTable fees = {}) {
    REQUIRE(table.add(make_inst()));
    EngineConfig cfg;
    cfg.risk.max_feed_lag_ms = max_feed_lag_ms;
    cfg.risk.orders_per_sec = 20;
    cfg.risk.burst = 5;
    cfg.quotes.min_requote_interval = Duration{};
    cfg.quotes.min_requote_ticks = 1;
    cfg.fees = fees;
    engine = std::make_unique<GateEngine>(cfg, table, clock, transport, feed, strategy);
    engine->warm_up();
    engine->start();
  }
  // A book update with the venue time `lag_us` before its receive time (now).
  void book(const char* bid, const char* ask, std::int64_t lag_us) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(
        *d, EventType::BookDelta, InstrumentId{0}, VenueId{0}, BookDeltaMsg::size_for(1, 1));
    if (seq == 0) d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->hdr.exch_ts = clock.now() - microseconds(lag_us);
    d->first_update_id = d->last_update_id = ++seq;
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  void ack_all() {
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      if (transport.header(i).type != EventType::OutNewOrder) continue;
      const auto& n = *reinterpret_cast<const OutNewOrderMsg*>(transport.out[i].data());
      const Handle<Order> h = engine->oms().find(n.cl_ord_id);
      if (!h.valid() || engine->oms().get(h).state != OrderState::PendingNew) continue;
      OrderAckMsg a{};
      init_header(a, EventType::OrderAck, n.hdr.instrument, n.hdr.venue);
      a.cl_ord_id = n.cl_ord_id;
      a.venue_order_id = "V";
      a.hdr.recv_ts = clock.now();
      REQUIRE(feed.push(a.hdr));
    }
    drain();
  }
  void cancel_ack_all() {
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      if (transport.header(i).type != EventType::OutCancel) continue;
      const auto& c = *reinterpret_cast<const OutCancelMsg*>(transport.out[i].data());
      const Handle<Order> h = engine->oms().find(c.cl_ord_id);
      if (!h.valid() || engine->oms().get(h).state != OrderState::PendingCancel) continue;
      OrderCancelAckMsg a{};
      init_header(a, EventType::OrderCancelAck, c.hdr.instrument, c.hdr.venue);
      a.cl_ord_id = c.cl_ord_id;
      a.hdr.recv_ts = clock.now();
      REQUIRE(feed.push(a.hdr));
    }
    drain();
  }
  void drain() {
    while (engine->step() != 0) {
    }
  }
};

}  // namespace

TEST_CASE("core.venue_state: a feed-lag burst pulls the quotes and they come back after it") {
  Rig r(/*max_feed_lag_ms=*/5);
  auto& ctx = r.engine->context();
  r.book("100.00", "100.10", 300);  // snapshot: no lag sample
  r.book("100.00", "100.10", 300);
  REQUIRE(r.transport.count(EventType::OutNewOrder) == 2);
  r.clock.advance(milliseconds(2));
  r.ack_all();
  // The ack round trip is engine time from the send to the ack.
  CHECK(ctx.venue_health(VenueId{0}).ack_rtt == milliseconds(2));
  CHECK(ctx.venue_health(VenueId{0}).ack_samples == 2);
  r.clock.advance(milliseconds(10));
  r.book("100.01", "100.11", 350);  // a requote
  r.cancel_ack_all();
  r.ack_all();
  const std::size_t news_before = r.transport.count(EventType::OutNewOrder);
  const std::size_t cancels_before = r.transport.count(EventType::OutCancel);
  CHECK(ctx.venue_health(VenueId{0}).feed_lag_base == microseconds(300));

  // Burst: 20 ms late. The quotes are cancelled and set_quotes is ignored.
  r.clock.advance(milliseconds(10));
  r.book("100.02", "100.12", 20'300);
  CHECK(ctx.venue_health(VenueId{0}).gated);
  CHECK(ctx.venue_health(VenueId{0}).feed_lag_excess == milliseconds(20));
  CHECK_FALSE(r.engine->quoting_enabled(InstrumentId{0}));
  CHECK(r.engine->quoting_enabled());  // the session is not paused, only the venue
  CHECK(r.transport.count(EventType::OutCancel) == cancels_before + 2);
  CHECK_FALSE(r.strategy.accepted.back());
  NewOrderRequest passive{};
  passive.instrument = InstrumentId{0};
  passive.side = Side::Buy;
  passive.price = px("99.50");
  passive.qty = qt("0.01");
  CHECK(ctx.send(passive).error() == RejectReason::FeedLag);
  CHECK(r.engine->stats().risk_rejects_by_reason[RejectReason::FeedLag] == 1);
  NewOrderRequest ioc = passive;
  ioc.tif = TimeInForce::Ioc;
  CHECK(ctx.send(ioc).has_value());
  const std::size_t news_in_burst = r.transport.count(EventType::OutNewOrder);
  CHECK(news_in_burst == news_before + 1);  // only the IOC
  r.cancel_ack_all();
  for (int i = 0; i < 5; ++i) {
    r.clock.advance(milliseconds(30));
    r.book("100.02", "100.12", 15'000);
  }
  CHECK(r.transport.count(EventType::OutNewOrder) == news_in_burst);
  // Back under the limit: still held until 100 ms after the last late message.
  r.clock.advance(milliseconds(30));
  r.book("100.02", "100.12", 400);
  CHECK(ctx.venue_health(VenueId{0}).gated);
  CHECK_FALSE(r.strategy.accepted.back());
  r.clock.advance(milliseconds(70));
  r.book("100.03", "100.13", 400);
  CHECK_FALSE(ctx.venue_health(VenueId{0}).gated);
  CHECK(r.strategy.accepted.back());
  CHECK(r.transport.count(EventType::OutNewOrder) == news_in_burst + 2);
  CHECK(ctx.venue_health(VenueId{0}).gate_engagements == 1);
}

TEST_CASE("core.venue_state: without max_feed_lag_ms the lag is measured and nothing is gated") {
  Rig r(/*max_feed_lag_ms=*/0);
  r.book("100.00", "100.10", 0);  // a snapshot: its venue time is not a live one, no sample
  r.book("100.00", "100.10", 300);
  r.clock.advance(milliseconds(10));
  r.book("100.01", "100.11", 50'000);
  const VenueHealthView v = r.engine->context().venue_health(VenueId{0});
  CHECK(v.feed_lag_excess == microseconds(49'700));
  CHECK(v.md_samples == 2);
  CHECK_FALSE(v.gated);
  CHECK(v.gate_engagements == 0);
  CHECK(r.strategy.accepted.back());
}

TEST_CASE("core.venue_state: ctx.fees and ctx.risk_headroom read the engine's tables") {
  FeeTable fees = FeeTable::from_bps(1.0, 5.0);
  fees.set_instrument(InstrumentId{0}, FeeRates::from_bps(-0.25, 7.5));
  Rig r(0, fees);
  auto& ctx = r.engine->context();
  CHECK(ctx.fees(InstrumentId{0}).maker_cbps == -25);
  CHECK(ctx.fees(InstrumentId{0}).taker_cbps == 750);
  CHECK(ctx.fees(InstrumentId{0}).maker_bps() == doctest::Approx(-0.25));
  CHECK(ctx.fees(InstrumentId{1}).taker_bps() == doctest::Approx(5.0));  // default
  r.book("100.00", "100.10", 300);
  r.book("100.00", "100.10", 300);  // two quotes: two of the five tokens
  CHECK(ctx.risk_headroom(InstrumentId{0}).order_tokens == 3);
  NewOrderRequest o{};
  o.instrument = InstrumentId{0};
  o.side = Side::Buy;
  o.price = px("99.00");
  o.qty = qt("0.01");
  for (int i = 0; i < 3; ++i) CHECK(ctx.send(o).has_value());
  CHECK(ctx.risk_headroom(InstrumentId{0}).order_tokens == 0);
  CHECK(ctx.send(o).error() == RejectReason::RateLimit);
  r.clock.advance(milliseconds(100));  // 20/s: two tokens
  r.book("100.00", "100.10", 300);     // the same quotes: no order
  CHECK(ctx.risk_headroom(InstrumentId{0}).order_tokens == 2);
  CHECK(ctx.risk_headroom(InstrumentId{7}).order_tokens == RiskHeadroom::kUnlimited);  // unknown
}

// ---- fee table from the configuration
// ------------------------------------------------------------

TEST_CASE("core.venue_state: fee_table takes the instrument's override, else its venue's fees") {
  const Config cfg = Config::parse(R"(
[engine]
name = "t"
[venues.a]
kind = "sim"
[venues.a.fees]
maker_bps = 1.0
taker_bps = 10.0
[venues.b]
kind = "sim"
[venues.b.fees]
maker_bps = -0.5
taker_bps = 4.0
[[instruments]]
venue = "a"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.001"
[[instruments]]
venue = "a"
symbol = "ETHUSDT"
tick = "0.01"
lot = "0.001"
maker_bps = 0.0
[[instruments]]
venue = "b"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.001"
[strategy]
name = "basic_mm"
)");
  const FeeTable t = fee_table(cfg);
  CHECK(t.schedule(InstrumentId{0}) == FeeRates::from_bps(1.0, 10.0));
  CHECK(t.schedule(InstrumentId{1}) == FeeRates::from_bps(0.0, 10.0));
  CHECK(t.schedule(InstrumentId{2}) == FeeRates::from_bps(-0.5, 4.0));
  CHECK(t.schedule(InstrumentId{9}) == FeeRates::from_bps(1.0, 10.0));  // the first venue's

  // A gateway's table: other order, venue ids named by the gateway, an instrument the strategy's
  // configuration does not list (its venue's fees).
  InstrumentTable gw;
  const auto add = [&](const char* symbol, std::uint8_t venue) {
    Instrument i = make_inst();
    i.symbol = symbol;
    i.venue = VenueId{venue};
    REQUIRE(gw.add(i));
  };
  add("BTCUSDT", 0);  // b
  add("SOLUSDT", 1);  // a, not configured
  add("ETHUSDT", 1);  // a
  const std::vector<std::string> names = {"b", "a"};
  const FeeTable g = fee_table(cfg, &gw, &names);
  CHECK(g.schedule(InstrumentId{0}) == FeeRates::from_bps(-0.5, 4.0));
  CHECK(g.schedule(InstrumentId{1}) == FeeRates::from_bps(1.0, 10.0));
  CHECK(g.schedule(InstrumentId{2}) == FeeRates::from_bps(0.0, 10.0));
}

TEST_CASE(
    "core.venue_state: [risk] max_feed_lag_ms is parsed, refused below 0 and only written "
    "when set") {
  const char* base = R"(
[engine]
name = "t"
[venues.a]
kind = "sim"
[[instruments]]
venue = "a"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.001"
[strategy]
name = "basic_mm"
)";
  const Config off = Config::parse(base);
  CHECK(off.risk_limits().max_feed_lag_ms == 0);
  CHECK(off.effective_toml().find("max_feed_lag_ms") == std::string::npos);
  const Config on = Config::parse(std::string(base) + "[risk]\nmax_feed_lag_ms = 5\n");
  CHECK(on.warnings.empty());
  CHECK(on.risk_limits().max_feed_lag_ms == 5);
  CHECK(Config::parse(on.effective_toml()).risk_limits().max_feed_lag_ms == 5);
  CHECK_THROWS(Config::parse(std::string(base) + "[risk]\nmax_feed_lag_ms = -1\n"));
}
