// The execution view: our quantity in the feed (OwnQuantity, ctx.own_qty, ctx.best_ex_self), the
// queue position estimate (QueueTracker, ctx.queue_ahead) and order times (Oms::times).
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/own_quantity.hpp"
#include "fastmm/core/queue_tracker.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kT0 = 1'700'000'000'000'000'000;
constexpr InstrumentId kId{0};

Timestamp at(std::int64_t ms, std::int64_t ns = 0) {
  return Timestamp{kT0 + ms * kMs + ns};
}

// ---- message builders -----------------------------------------------------------------------

OutNewOrderMsg out_new(std::uint64_t id, Side side, const char* p, const char* q) {
  OutNewOrderMsg m{};
  init_header(m, EventType::OutNewOrder, kId, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  m.side = side;
  m.type = OrderType::Limit;
  m.tif = TimeInForce::Gtc;
  m.price = px(p);
  m.qty = qt(q);
  return m;
}
OutReplaceMsg out_replace(std::uint64_t id, std::uint64_t orig, const char* p, const char* q) {
  OutReplaceMsg m{};
  init_header(m, EventType::OutReplace, kId, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  m.orig_cl_ord_id = ClientOrderId{orig};
  m.price = px(p);
  m.qty = qt(q);
  return m;
}
template <class M>
void stamp(M& m, Timestamp exch, Timestamp recv) {
  m.hdr.exch_ts = exch;
  m.hdr.recv_ts = recv.valid() ? recv : exch;
}
OrderAckMsg ack(std::uint64_t id, Timestamp exch, Timestamp recv = {}) {
  OrderAckMsg m{};
  init_header(m, EventType::OrderAck, kId, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  stamp(m, exch, recv.valid() ? recv : exch);
  return m;
}
OrderFillMsg fill(std::uint64_t id,
                  const char* q,
                  const char* cum,
                  const char* leaves,
                  Timestamp exch,
                  const char* exec) {
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill, kId, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  m.exec_id = ExecId{exec};
  m.qty = qt(q);
  m.cum_qty = qt(cum);
  m.leaves_qty = qt(leaves);
  m.price = px("100.00");
  stamp(m, exch, exch);
  return m;
}
OrderCancelAckMsg cancel_ack(std::uint64_t id, Timestamp exch) {
  OrderCancelAckMsg m{};
  init_header(m, EventType::OrderCancelAck, kId, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  stamp(m, exch, exch);
  return m;
}
ReconcileMsg reconcile(ReconcileMsg::Kind k, std::uint64_t id, Timestamp t) {
  ReconcileMsg m{};
  init_header(m, EventType::Reconcile, kId, VenueId{0});
  m.kind = k;
  m.cl_ord_id = ClientOrderId{id};
  stamp(m, Timestamp{}, t);
  return m;
}

Qty own(const OwnQuantity& q, const char* p, Timestamp t, Side s = Side::Buy) {
  return q.own_at(kId, s, px(p), t);
}

}  // namespace

// ---- OwnQuantity -----------------------------------------------------------------------------

TEST_CASE("exec_view.own_quantity: an order rests from its ack to its end, less its fills") {
  OwnQuantity q;
  q.on_outbound(out_new(1, Side::Buy, "100.00", "3").hdr);
  CHECK(own(q, "100.00", at(5)).is_zero());  // sent, not acknowledged
  q.on_inbound(ack(1, at(1), at(2)).hdr);
  CHECK(own(q, "100.00", at(1) - Duration{1}).is_zero());
  CHECK(own(q, "100.00", at(1)) == qt("3"));
  CHECK(own(q, "100.00", at(1), Side::Sell).is_zero());
  CHECK(own(q, "100.01", at(1)).is_zero());
  // A fill at 3 ms steps it down from then on; a depth update stamped before still shows 3.
  q.on_inbound(fill(1, "1", "1", "2", at(3), "77").hdr);
  CHECK(own(q, "100.00", at(3) - Duration{1}) == qt("3"));
  CHECK(own(q, "100.00", at(3)) == qt("2"));
  // The same execution again (API response and user stream) counts once.
  q.on_inbound(fill(1, "1", "1", "2", at(3), "77").hdr);
  CHECK(own(q, "100.00", at(3)) == qt("2"));
  // Cancelled at 6 ms (whole-millisecond venue times): it rests to the end of that millisecond,
  // so a depth update stamped just before the cancel still includes it.
  q.on_inbound(cancel_ack(1, at(6)).hdr);
  CHECK(q.ms_order_times());
  CHECK(own(q, "100.00", at(5)) == qt("2"));
  CHECK(own(q, "100.00", at(6, kMs - 1)) == qt("2"));
  CHECK(own(q, "100.00", at(7)).is_zero());
}

TEST_CASE("exec_view.own_quantity: venue times as the collector takes them") {
  SUBCASE("the second ack's venue time replaces a receive time") {
    OwnQuantity q;
    q.on_outbound(out_new(1, Side::Buy, "100.00", "1").hdr);
    q.on_inbound(ack(1, Timestamp{}, at(9)).hdr);
    CHECK(own(q, "100.00", at(9)) == qt("1"));
    q.on_inbound(ack(1, at(4), at(10)).hdr);
    CHECK(own(q, "100.00", at(4)) == qt("1"));
  }
  SUBCASE("a replaced order rests until its replacement's ack when that is later") {
    OwnQuantity q;
    q.on_outbound(out_new(1, Side::Buy, "100.00", "1").hdr);
    q.on_inbound(ack(1, at(1)).hdr);
    q.on_outbound(out_replace(2, 1, "100.01", "1").hdr);
    q.on_inbound(cancel_ack(1, at(3)).hdr);
    CHECK(own(q, "100.00", at(5)).is_zero());
    q.on_inbound(ack(2, at(5)).hdr);
    CHECK(own(q, "100.00", at(5)) == qt("1"));
    CHECK(own(q, "100.00", at(6)).is_zero());
    CHECK(own(q, "100.01", at(5)) == qt("1"));
  }
  SUBCASE("a reconciliation that no longer lists it ends it") {
    OwnQuantity q;
    q.on_outbound(out_new(1, Side::Buy, "100.00", "1").hdr);
    q.on_outbound(out_new(2, Side::Buy, "100.00", "2").hdr);
    q.on_inbound(ack(1, at(1)).hdr);
    q.on_inbound(ack(2, at(1)).hdr);
    q.on_inbound(reconcile(ReconcileMsg::Kind::Begin, 0, at(8)).hdr);
    q.on_inbound(reconcile(ReconcileMsg::Kind::OpenOrder, 2, at(8)).hdr);
    q.on_inbound(reconcile(ReconcileMsg::Kind::End, 0, at(8)).hdr);
    CHECK(own(q, "100.00", at(8)) == qt("3"));  // the end is inclusive
    CHECK(own(q, "100.00", at(8) + Duration{1}) == qt("2"));
  }
  SUBCASE("the OMS ends an order the messages do not end") {
    OwnQuantity q;
    q.on_outbound(out_new(1, Side::Buy, "100.00", "1").hdr);
    q.on_outbound(out_new(2, Side::Buy, "100.00", "1").hdr);
    q.on_inbound(ack(1, at(1)).hdr);
    q.on_gone(ClientOrderId{1}, at(4, 5));
    q.on_gone(ClientOrderId{2}, at(4));  // never acked: forgotten
    CHECK(own(q, "100.00", at(4)) == qt("1"));
    CHECK(own(q, "100.00", at(5)) == qt("1"));  // the rest of the end's millisecond
    CHECK(own(q, "100.00", at(5, 5)).is_zero());
  }
  SUBCASE("segments older than the retention are dropped") {
    OwnQuantity q(false, seconds(1));
    q.on_outbound(out_new(1, Side::Buy, "100.00", "1").hdr);
    q.on_inbound(ack(1, at(1)).hdr);
    q.on_inbound(cancel_ack(1, at(2)).hdr);
    CHECK_FALSE(q.empty());
    q.on_outbound(out_new(2, Side::Sell, "101.00", "1").hdr);
    q.on_inbound(ack(2, at(3000)).hdr);
    CHECK(own(q, "100.00", at(2)).is_zero());
    CHECK(own(q, "101.00", at(3000), Side::Sell) == qt("1"));
  }
}

// ---- engine fixture ---------------------------------------------------------------------------

namespace {

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  REQUIRE(t.add(i));
  return t;
}

// A venue the test plays; own_in_feed says whether its feed shows our orders.
struct Venue {
  std::vector<std::vector<std::byte>> out;
  bool feed_shows_own = true;
  bool replace = true;
  bool send(const EventHeader& m) noexcept {
    const auto* b = reinterpret_cast<const std::byte*>(&m);
    out.emplace_back(b, b + m.len);
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  bool supports_replace(VenueId) const noexcept { return replace; }
  bool own_in_feed(VenueId) const noexcept { return feed_shows_own; }
};

// Records what the context says inside the hooks.
struct Probe {
  std::vector<OmsUpdate> updates;
  std::vector<std::optional<Qty>> ahead_in_update;
  bool track = true;
  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    if (track) static_cast<void>(ctx.queue_ahead(ClientOrderId{}));
  }
  template <class Ctx>
  void on_order_update(Ctx& ctx, const OmsUpdate& u) noexcept {
    updates.push_back(u);
    if (track) ahead_in_update.push_back(ctx.queue_ahead(u.order.cl_ord_id));
  }
};

using ProbeEngine = Engine<Probe, SimClock, Venue, InlineFeed>;

struct Rig {
  InstrumentTable table = make_table();
  SimClock clock{at(0)};
  Venue venue;
  InlineFeed feed{1 << 20};
  Probe probe;
  std::unique_ptr<ProbeEngine> engine;

  explicit Rig(bool feed_shows_own = true, bool track = true, std::int64_t bps = 5'000) {
    venue.feed_shows_own = feed_shows_own;
    probe.track = track;
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("100");
    cfg.risk.max_position = qt("100");
    cfg.risk.max_open_orders = 16;
    cfg.risk.price_collar_bps = 5000;
    cfg.risk.stale_md = seconds(60);
    cfg.queue_conservatism_bps = bps;
    engine = std::make_unique<ProbeEngine>(cfg, table, clock, venue, feed, probe);
    engine->warm_up();
    engine->start();
  }
  auto& ctx() { return engine->context(); }
  void step() {
    while (!feed.empty()) engine->step();
  }
  // Depth at venue time t: levels (price, qty) of one side, a snapshot or a delta.
  void book(Timestamp t,
            std::initializer_list<Level> bids,
            std::initializer_list<Level> asks,
            bool snapshot = false) {
    const auto nb = static_cast<std::uint32_t>(bids.size());
    const auto na = static_cast<std::uint32_t>(asks.size());
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(nb, na));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d,
                snapshot ? EventType::BookSnapshot : EventType::BookDelta,
                kId,
                VenueId{0},
                BookDeltaMsg::size_for(nb, na));
    if (snapshot) d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.exch_ts = t;
    d->hdr.recv_ts = t;
    d->bid_count = nb;
    d->ask_count = na;
    std::size_t k = 0;
    for (const Level& l : bids) d->levels()[k++] = l;
    for (const Level& l : asks) d->levels()[k++] = l;
    feed.commit();
    step();
  }
  template <class M>
  void in(M m) {
    REQUIRE(feed.push(m.hdr));
    step();
  }
  void ticker(Timestamp t,
              std::uint64_t id,
              const char* bid,
              const char* bq,
              const char* ask,
              const char* aq) {
    BookTickerMsg m{};
    init_header(m, EventType::BookTicker, kId, VenueId{0});
    m.hdr.exch_ts = t;
    m.hdr.recv_ts = t;
    m.hdr.venue_seq = id;
    m.bid_px = px(bid);
    m.bid_qty = qt(bq);
    m.ask_px = px(ask);
    m.ask_qty = qt(aq);
    in(m);
  }
  void trade(Timestamp t, const char* p, const char* q, Side aggressor) {
    TradeMsg m{};
    init_header(m, EventType::Trade, kId, VenueId{0});
    m.hdr.exch_ts = t;
    m.hdr.recv_ts = t;
    m.price = px(p);
    m.qty = qt(q);
    m.aggressor = aggressor;
    in(m);
  }
  ClientOrderId buy(const char* p, const char* q) {
    auto r = ctx().send(NewOrderRequest::limit(kId, Side::Buy, px(p), qt(q)));
    REQUIRE(r.has_value());
    return *r;
  }
  std::optional<Qty> ahead(ClientOrderId id) { return ctx().queue_ahead(id); }
};

}  // namespace

// ---- own quantity through the engine ---------------------------------------------------------

TEST_CASE("exec_view.engine: own_qty and best_ex_self on a feed that shows our orders") {
  Rig r(true);
  r.book(at(0), {{px("100.00"), qt("5")}, {px("99.99"), qt("4")}}, {{px("100.02"), qt("3")}}, true);
  r.clock.set(at(1));
  const ClientOrderId id = r.buy("100.01", "2");
  r.in(ack(id.value, at(1)));
  CHECK(r.ctx().own_qty(kId, Side::Buy, px("100.01")).is_zero());  // the book is older than the ack
  r.book(at(2), {{px("100.01"), qt("2")}}, {});                    // our order shows up
  CHECK(r.ctx().own_qty(kId, Side::Buy, px("100.01")) == qt("2"));
  CHECK(r.ctx().own_qty(kId, Side::Buy, px("100.01"), at(0)).is_zero());
  // The best bid is only ours: the helper skips to the level behind it.
  const Level best = r.ctx().best_ex_self(kId, Side::Buy);
  CHECK(best.price == px("100.00"));
  CHECK(best.qty == qt("5"));
  CHECK(r.ctx().best_ex_self(kId, Side::Sell).price == px("100.02"));
  // Someone joins behind us: 3 shown, 1 not ours.
  r.book(at(3), {{px("100.01"), qt("3")}}, {});
  CHECK(r.ctx().best_ex_self(kId, Side::Buy).qty == qt("1"));
}

TEST_CASE("exec_view.engine: own_qty is zero where the feed does not show our orders") {
  Rig r(false);
  r.book(at(0), {{px("100.00"), qt("5")}}, {{px("100.02"), qt("3")}}, true);
  const ClientOrderId id = r.buy("100.01", "2");
  r.in(ack(id.value, at(1)));
  r.book(at(2), {{px("100.01"), qt("2")}}, {});
  CHECK(r.ctx().own_qty(kId, Side::Buy, px("100.01")).is_zero());
  CHECK(r.ctx().best_ex_self(kId, Side::Buy).price == px("100.01"));
  CHECK(r.engine->own_quantity() == nullptr);
}

// ---- order times -----------------------------------------------------------------------------

TEST_CASE("exec_view.engine: send and ack times of an order and its replacement") {
  Rig r;
  r.book(at(0), {{px("100.00"), qt("5")}}, {{px("100.02"), qt("3")}}, true);
  r.clock.set(at(10));
  const ClientOrderId id = r.buy("100.00", "1");
  const OrderTimes* t = r.ctx().order_times(id);
  REQUIRE(t != nullptr);
  CHECK(t->sent == at(10));
  CHECK_FALSE(t->venue_ack.valid());
  CHECK_FALSE(t->local_ack.valid());
  // The first ack has no venue time, the duplicate from the other stream has one.
  r.in(ack(id.value, Timestamp{}, at(12)));
  t = r.ctx().order_times(id);
  CHECK(t->local_ack == at(12));
  CHECK_FALSE(t->venue_ack.valid());
  r.in(ack(id.value, at(11), at(13)));
  t = r.ctx().order_times(id);
  CHECK(t->venue_ack == at(11));
  CHECK(t->local_ack == at(12));
  // Replace at 20: the times stay the original's until the replacement's ack.
  r.clock.set(at(20));
  REQUIRE(r.ctx().replace(id, px("99.99"), qt("1")).has_value());
  const auto& rep = *reinterpret_cast<const OutReplaceMsg*>(r.venue.out.back().data());
  CHECK(r.ctx().order_times(id)->sent == at(10));
  r.clock.set(at(25));
  r.in(ack(rep.cl_ord_id.value, at(22), at(23)));
  t = r.ctx().order_times(rep.cl_ord_id);
  REQUIRE(t != nullptr);
  CHECK(t->sent == at(20));
  CHECK(t->venue_ack == at(22));
  CHECK(t->local_ack == at(23));
  // The terminal update carries them after the slot is gone.
  r.in(cancel_ack(rep.cl_ord_id.value, at(30)));
  REQUIRE_FALSE(r.probe.updates.empty());
  const OmsUpdate& last = r.probe.updates.back();
  CHECK(last.terminal);
  CHECK(last.times.sent == at(20));
  CHECK(last.times.venue_ack == at(22));
  CHECK(r.ctx().order_times(rep.cl_ord_id) == nullptr);
}

// ---- queue position --------------------------------------------------------------------------

TEST_CASE("exec_view.engine: the queue ahead follows the level, trades and conservatism") {
  Rig r(false, true, 5'000);  // conservatism 0.5
  r.book(at(0), {{px("100.00"), qt("10")}}, {{px("100.02"), qt("3")}}, true);
  const ClientOrderId id = r.buy("100.00", "1");
  CHECK_FALSE(r.ahead(id).has_value());  // not acknowledged yet
  r.in(ack(id.value, at(1)));
  REQUIRE(r.ahead(id).has_value());
  CHECK(*r.ahead(id) == qt("10"));
  CHECK(*r.probe.ahead_in_update.back() == qt("10"));
  // The level shrinks 10 -> 6: half of the proportional share of 4 is ours, 10 - 4 * 10/10 * 0.5.
  r.book(at(2), {{px("100.00"), qt("6")}}, {});
  CHECK(*r.ahead(id) == qt("8"));
  // Growth behind us changes nothing.
  r.book(at(3), {{px("100.00"), qt("9")}}, {});
  CHECK(*r.ahead(id) == qt("8"));
  // A level elsewhere changes nothing.
  r.book(at(4), {{px("99.99"), qt("1")}}, {{px("100.01"), qt("1")}});
  CHECK(*r.ahead(id) == qt("8"));
  // Sells at our price consume the queue ahead of us; a buy at our price does not.
  r.trade(at(5), "100.00", "3", Side::Sell);
  CHECK(*r.ahead(id) == qt("5"));
  r.trade(at(5), "100.00", "3", Side::Buy);
  CHECK(*r.ahead(id) == qt("5"));
  // A snapshot caps it at what the level shows.
  r.book(at(6), {{px("100.00"), qt("4")}}, {{px("100.02"), qt("3")}}, true);
  CHECK(*r.ahead(id) == qt("4"));
  // A trade through our price empties it.
  r.trade(at(7), "99.99", "1", Side::Sell);
  CHECK(r.ahead(id)->is_zero());
  r.in(cancel_ack(id.value, at(8)));
  CHECK_FALSE(r.ahead(id).has_value());
}

TEST_CASE("exec_view.engine: a level that goes away leaves nothing ahead") {
  Rig r(false, true, 10'000);
  r.book(at(0), {{px("100.00"), qt("10")}}, {{px("100.02"), qt("3")}}, true);
  const ClientOrderId id = r.buy("100.00", "1");
  r.in(ack(id.value, at(1)));
  r.book(at(2), {{px("100.00"), qt("6")}}, {});
  CHECK(*r.ahead(id) == qt("10"));  // conservatism 1: cancels never move us up
  r.book(at(3), {{px("100.00"), Qty{}}}, {});
  CHECK(r.ahead(id)->is_zero());
}

TEST_CASE("exec_view.engine: our own quantity is not ahead of us") {
  Rig r(true, true, 10'000);
  r.book(at(0), {{px("100.00"), qt("10")}}, {{px("100.02"), qt("3")}}, true);
  r.clock.set(at(1));
  const ClientOrderId a = r.buy("100.00", "2");
  r.in(ack(a.value, at(1)));
  r.book(at(2), {{px("100.00"), qt("12")}}, {});  // ours shows up behind the 10
  const ClientOrderId b = r.buy("100.00", "1");
  r.in(ack(b.value, at(3)));
  CHECK(*r.ahead(b) == qt("10"));
  r.book(at(4), {{px("100.00"), qt("13")}}, {});
  // b is placed behind the 10 others show (our 2 are not someone else's queue).
  CHECK(*r.ahead(b) == qt("10"));
  // Our own cancel shrinks the level without moving anyone: 13 -> 11 is our 2 leaving.
  r.clock.set(at(5));
  REQUIRE(r.ctx().cancel(a).has_value());
  r.in(cancel_ack(a.value, at(5)));
  r.book(at(6), {{px("100.00"), qt("11")}}, {});
  CHECK(*r.ahead(b) == qt("10"));
}

TEST_CASE("exec_view.engine: a replace keeps its place at the same price and no more quantity") {
  Rig r(false, true, 10'000);
  r.book(
      at(0), {{px("100.00"), qt("10")}, {px("99.99"), qt("7")}}, {{px("100.02"), qt("3")}}, true);
  const ClientOrderId id = r.buy("100.00", "2");
  r.in(ack(id.value, at(1)));
  r.trade(at(2), "100.00", "4", Side::Sell);
  CHECK(*r.ahead(id) == qt("6"));
  REQUIRE(r.ctx().replace(id, px("100.00"), qt("1")).has_value());
  const auto keep = reinterpret_cast<const OutReplaceMsg*>(r.venue.out.back().data())->cl_ord_id;
  r.in(ack(keep.value, at(3)));
  CHECK(*r.ahead(keep) == qt("6"));
  REQUIRE(r.ctx().replace(keep, px("99.99"), qt("1")).has_value());
  const auto moved = reinterpret_cast<const OutReplaceMsg*>(r.venue.out.back().data())->cl_ord_id;
  r.in(ack(moved.value, at(4)));
  CHECK(*r.ahead(moved) == qt("7"));
}

TEST_CASE("exec_view.engine: a BookTicker newer than the depth book caps the queue") {
  Rig r(true, true, 10'000);
  r.book(
      at(0), {{px("100.00"), qt("10")}, {px("99.99"), qt("7")}}, {{px("100.02"), qt("3")}}, true);
  const ClientOrderId a = r.buy("100.00", "1");
  const ClientOrderId b = r.buy("99.99", "1");
  // The ticker (update id 0: by venue time) is newer than the depth: 4 at the touch, less ours.
  r.ticker(at(1), 0, "100.00", "4", "100.02", "3");
  r.in(ack(a.value, at(2)));
  r.in(ack(b.value, at(2)));
  CHECK(*r.ahead(a) == qt("4"));
  r.ticker(at(3), 0, "100.00", "5", "100.02", "3");  // our 1 is in it now
  CHECK(*r.ahead(a) == qt("4"));
  CHECK(*r.ahead(b) == qt("7"));  // behind the touch: the depth's estimate
  // An older ticker is not used.
  r.book(at(5), {{px("99.99"), qt("7")}}, {});
  r.ticker(at(4), 0, "100.00", "2", "100.02", "3");
  CHECK(*r.ahead(a) == qt("4"));
  // The touch moved below our bid: nothing is ahead of it any more.
  r.ticker(at(6), 0, "99.99", "8", "100.02", "3");
  CHECK(r.ahead(a)->is_zero());
  CHECK(*r.ahead(b) == qt("7"));  // at the touch: 8 shown, 1 of them ours
}

TEST_CASE("exec_view.engine: tracking starts at the first queue_ahead call") {
  Rig r(false, /*track=*/false, 10'000);
  r.book(at(0), {{px("100.00"), qt("10")}}, {{px("100.02"), qt("3")}}, true);
  const ClientOrderId id = r.buy("100.00", "1");
  r.in(ack(id.value, at(1)));
  r.book(at(2), {{px("100.00"), qt("4")}}, {});
  CHECK_FALSE(r.engine->queue().enabled());
  // The first call places the resting order at the back of the level as it shows now.
  REQUIRE(r.ahead(id).has_value());
  CHECK(*r.ahead(id) == qt("4"));
  r.trade(at(3), "100.00", "1", Side::Sell);
  CHECK(*r.ahead(id) == qt("3"));
}

// ---- the simulator's l2_queue model and the engine's estimate agree -------------------------

namespace {

// Market data from a list, in venue time.
class ListSource final : public sim::MdSource {
 public:
  void add(const EventHeader& h) {
    const auto* b = reinterpret_cast<const std::byte*>(&h);
    msgs_.emplace_back(b, b + h.len);
  }
  const EventHeader* next() override {
    if (i_ >= msgs_.size()) return nullptr;
    std::memcpy(buf_.bytes, msgs_[i_].data(), msgs_[i_].size());
    ++i_;
    return &buf_.hdr();
  }
  void reset() override { i_ = 0; }

 private:
  std::vector<std::vector<std::byte>> msgs_;
  std::size_t i_ = 0;
  sim::EventBuf buf_;
};

// Places passive orders and compares its queue estimate with the fill model's whenever both have
// seen the same market data: in the market-data hooks and at an ack. A fill reaches the engine
// before the trade that caused it (the venue sends it first), so on_order_update for a fill is
// not such a point.
struct Twin {
  const sim::SimTransport* venue = nullptr;
  std::vector<ClientOrderId> ids;
  std::uint64_t compared = 0;
  std::uint64_t differed = 0;
  std::uint64_t books = 0;

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    static_cast<void>(ctx.queue_ahead(ClientOrderId{}));
  }
  template <class Ctx>
  void compare(Ctx& ctx) noexcept {
    for (const ClientOrderId id : ids) {
      const auto mine = ctx.queue_ahead(id);
      const auto h = venue->queue().find(id);
      if (!mine || !h.valid()) continue;
      ++compared;
      if (*mine != venue->queue().get(h).ahead) {
        ++differed;
        MESSAGE("order " << id.value << " at " << (ctx.now().ns - kT0) / kMs << " ms: engine "
                         << mine->raw << " venue " << venue->queue().get(h).ahead.raw);
      }
    }
  }
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book&) noexcept {
    // Orders at three levels after the first snapshot; then keep comparing.
    if (books++ == 0) {
      for (const char* p : {"100.00", "99.99", "99.98"}) {
        auto r = ctx.send(NewOrderRequest::limit(id, Side::Buy, px(p), qt("1")));
        if (r) ids.push_back(*r);
      }
      auto r = ctx.send(NewOrderRequest::limit(id, Side::Sell, px("100.03"), qt("1")));
      if (r) ids.push_back(*r);
    }
    compare(ctx);
  }
  template <class Ctx>
  void on_trade(Ctx& ctx, InstrumentId, const TradeMsg&) noexcept {
    compare(ctx);
  }
  template <class Ctx>
  void on_order_update(Ctx& ctx, const OmsUpdate& u) noexcept {
    if (u.prev == OrderState::PendingNew && u.order.state == OrderState::Live) compare(ctx);
  }
};

using TwinEngine = Engine<Twin, SimClock, sim::SimTransport, InlineFeed>;

void delta(ListSource& src,
           std::int64_t ms,
           std::initializer_list<Level> bids,
           std::initializer_list<Level> asks,
           bool snapshot = false) {
  const auto nb = static_cast<std::uint32_t>(bids.size());
  const auto na = static_cast<std::uint32_t>(asks.size());
  alignas(64) std::byte buf[BookDeltaMsg::size_for(8, 8)] = {};
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
  init_header(*d,
              snapshot ? EventType::BookSnapshot : EventType::BookDelta,
              kId,
              VenueId{0},
              BookDeltaMsg::size_for(nb, na));
  if (snapshot) d->hdr.flags |= EventHeader::kSnapshot;
  d->hdr.exch_ts = at(ms);
  d->hdr.recv_ts = at(ms);
  d->bid_count = nb;
  d->ask_count = na;
  std::size_t k = 0;
  for (const Level& l : bids) d->levels()[k++] = l;
  for (const Level& l : asks) d->levels()[k++] = l;
  src.add(d->hdr);
}
void top(ListSource& src,
         std::int64_t ms,
         const char* bid,
         const char* bq,
         const char* ask,
         const char* aq) {
  BookTickerMsg m{};
  init_header(m, EventType::BookTicker, kId, VenueId{0});
  m.hdr.exch_ts = at(ms);
  m.hdr.recv_ts = at(ms);
  m.bid_px = px(bid);
  m.bid_qty = qt(bq);
  m.ask_px = px(ask);
  m.ask_qty = qt(aq);
  src.add(m.hdr);
}
void print(ListSource& src, std::int64_t ms, const char* p, const char* q, Side aggressor) {
  TradeMsg m{};
  init_header(m, EventType::Trade, kId, VenueId{0});
  m.hdr.exch_ts = at(ms);
  m.hdr.recv_ts = at(ms);
  m.price = px(p);
  m.qty = qt(q);
  m.aggressor = aggressor;
  src.add(m.hdr);
}

}  // namespace

TEST_CASE("exec_view.sim: the strategy's queue estimate equals the l2_queue fill model's") {
  for (const std::int64_t bps : {std::int64_t{0}, std::int64_t{3'000}, std::int64_t{10'000}}) {
    CAPTURE(bps);
    const InstrumentTable table = make_table();
    SimClock clock{at(0)};
    sim::SimTransportConfig tc;
    tc.fill_model = sim::FillModel::L2Queue;
    tc.queue_conservatism_bps = bps;
    tc.order_out = sim::LatencyParams{Duration{}, Duration{}};
    tc.ack_in = sim::LatencyParams{Duration{}, Duration{}};
    sim::SimTransport venue(clock, table, tc);
    InlineFeed feed(1 << 20);
    Twin twin;
    twin.venue = &venue;
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("100");
    cfg.risk.max_position = qt("100");
    cfg.risk.max_open_orders = 16;
    cfg.risk.price_collar_bps = 5000;
    cfg.risk.stale_md = seconds(60);
    cfg.queue_conservatism_bps = bps;
    TwinEngine engine(cfg, table, clock, venue, feed, twin);

    ListSource src;
    delta(src,
          1,
          {{px("100.00"), qt("5")}, {px("99.99"), qt("8")}, {px("99.98"), qt("2")}},
          {{px("100.02"), qt("4")}, {px("100.03"), qt("6")}},
          true);
    std::int64_t ms = 2;
    // Levels shrink, grow and go; trades consume and go through.
    delta(src, ms++, {{px("100.00"), qt("3")}}, {});
    top(src, ms++, "100.00", "2.5", "100.02", "4");  // newer than the depth: caps 100.00
    delta(src, ms++, {{px("99.99"), qt("11")}}, {{px("100.03"), qt("2")}});
    print(src, ms++, "100.00", "1", Side::Sell);
    delta(src, ms++, {{px("100.00"), qt("2")}, {px("99.99"), qt("5")}}, {});
    print(src, ms++, "100.03", "1", Side::Buy);
    delta(src, ms++, {{px("99.98"), Qty{}}}, {{px("100.03"), qt("1")}});
    delta(src, ms++, {{px("99.98"), qt("4")}}, {});
    print(src, ms++, "99.99", "2", Side::Sell);
    top(src, ms++, "99.99", "1", "100.03", "1");  // the touch below the 100.00 bid
    delta(src,
          ms++,
          {{px("100.00"), qt("2")}, {px("99.99"), qt("2")}, {px("99.98"), qt("3")}},
          {{px("100.02"), qt("4")}, {px("100.03"), qt("1")}},
          true);
    delta(src, ms++, {{px("99.98"), qt("1")}}, {});
    print(src, ms++, "99.98", "1", Side::Sell);

    sim::SimDriver driver(clock, venue, feed, sim::EngineHooks::for_engine(engine));
    driver.set_source(&src);
    driver.run_all();
    driver.finish();
    CHECK(twin.ids.size() == 4);
    CHECK(twin.compared > 40);
    CHECK(twin.differed == 0);
  }
}
