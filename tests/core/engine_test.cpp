// Engine wiring test: BasicMM against a fake transport that plays venue (acks, fills).
#include "fastmm/core/engine.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/basic_mm.hpp"

#include <cstring>
#include <memory>
#include <vector>

using namespace fastmm;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  i.min_notional = Notional::from_decimal("0.5").value();
  REQUIRE(t.add(i));
  return t;
}

// Records every outbound message; a tiny "venue" that the test drives explicitly.
struct FakeTransport {
  std::vector<std::vector<std::byte>> out;
  bool replace = false;
  bool full = false;
  bool send(const EventHeader& m) noexcept {
    if (full) return false;
    const auto* b = reinterpret_cast<const std::byte*>(&m);
    out.emplace_back(b, b + m.len);
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    std::size_t n = 0;
    for (const EventHeader* m : batch) n += send(*m) ? 1U : 0U;
    return n;
  }
  bool supports_replace(VenueId) const noexcept { return replace; }
  template <class M>
  const M& at(std::size_t i) const {
    return *reinterpret_cast<const M*>(out[i].data());
  }
  std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto& m : out)
      n += reinterpret_cast<const EventHeader*>(m.data())->type == t ? 1U : 0U;
    return n;
  }
};
static_assert(TransportLike<FakeTransport>);

using TestEngine = Engine<BasicMM, SimClock, FakeTransport, InlineFeed>;

struct Fixture {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  FakeTransport transport;
  InlineFeed feed{1 << 20};
  MsgRing journal_ring{1 << 20};
  BasicMM strategy;
  std::unique_ptr<TestEngine> engine;

  explicit Fixture(bool with_journal = true) {
    REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "10"},
                                      {"quote_qty", "0.01"},
                                      {"max_inventory", "0.05"},
                                      {"skew_bps_per_unit", "2"},
                                      {"requote_threshold_ticks", "1"},
                                      {"pull_on_stale_ms", "500"}}));
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.risk.max_position = qt("1");
    cfg.risk.max_open_orders = 8;
    cfg.risk.stale_md = seconds(5);
    cfg.risk.price_collar_bps = 500;
    cfg.quotes.min_requote_interval = Duration{};
    cfg.quotes.min_requote_ticks = 1;
    engine = std::make_unique<TestEngine>(
        cfg, table, clock, transport, feed, strategy, with_journal ? &journal_ring : nullptr);
    engine->warm_up();
    engine->start();
  }

  void push_book(const char* bid, const char* ask, std::uint64_t seq, bool snapshot = false) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d,
                snapshot ? EventType::BookSnapshot : EventType::BookDelta,
                InstrumentId{0},
                VenueId{0},
                BookDeltaMsg::size_for(1, 1));
    if (snapshot) d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->hdr.exch_ts = clock.now();
    d->hdr.t0_cycles = clock.cycles();
    d->hdr.t1_delta = 500;
    d->bid_count = d->ask_count = 1;
    d->last_update_id = seq;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
  }
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
  }
  void ack_all_new() {
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      const auto* h = reinterpret_cast<const EventHeader*>(transport.out[i].data());
      if (h->type != EventType::OutNewOrder) continue;
      const auto& n = transport.at<OutNewOrderMsg>(i);
      if (engine->oms().find(n.cl_ord_id).valid() &&
          engine->oms().get(engine->oms().find(n.cl_ord_id)).state == OrderState::PendingNew) {
        OrderAckMsg a{};
        init_header(a, EventType::OrderAck, InstrumentId{0}, VenueId{0});
        a.cl_ord_id = n.cl_ord_id;
        a.venue_order_id = "V";
        push(a);
      }
    }
  }
  void cancel_ack_all() {
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      const auto* h = reinterpret_cast<const EventHeader*>(transport.out[i].data());
      if (h->type != EventType::OutCancel) continue;
      const auto& c = transport.at<OutCancelMsg>(i);
      const Handle<Order> oh = engine->oms().find(c.cl_ord_id);
      if (oh.valid() && engine->oms().get(oh).state == OrderState::PendingCancel) {
        OrderCancelAckMsg a{};
        init_header(a, EventType::OrderCancelAck, InstrumentId{0}, VenueId{0});
        a.cl_ord_id = c.cl_ord_id;
        push(a);
      }
    }
  }
  void fill(ClientOrderId id, Side side, const char* p, const char* q, const char* exec) {
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill, InstrumentId{0}, VenueId{0});
    f.cl_ord_id = id;
    f.side = side;
    f.price = px(p);
    f.qty = qt(q);
    f.cum_qty = qt(q);
    f.exec_id = exec;
    f.liquidity = Liquidity::Maker;
    push(f);
  }
  std::size_t drain() {
    std::size_t total = 0;
    for (;;) {
      const std::size_t n = engine->step();
      if (n == 0) break;
      total += n;
    }
    return total;
  }
};
}  // namespace

TEST_CASE("core.engine: book -> quotes -> ack -> fill -> requote, journal and stats") {
  Fixture f;
  f.push_book("100.00", "100.02", 1, true);
  CHECK(f.drain() == 1);
  // BasicMM: mid 100.01, half spread 10 bps = 0.10001 -> bid 99.90, ask 100.12
  REQUIRE(f.transport.count(EventType::OutNewOrder) == 2);
  const auto& bid = f.transport.at<OutNewOrderMsg>(0);
  const auto& ask = f.transport.at<OutNewOrderMsg>(1);
  CHECK(bid.side == Side::Buy);
  CHECK(bid.price == px("99.90"));
  CHECK(bid.qty == qt("0.01"));
  CHECK(bid.type == OrderType::PostOnly);
  CHECK(ask.side == Side::Sell);
  CHECK(ask.price == px("100.12"));
  CHECK(f.engine->oms().open_count() == 2);
  CHECK(f.engine->stats().orders_sent == 2);
  CHECK(f.engine->stats().book_updates == 1);
  CHECK(f.engine->book(InstrumentId{0}).is_valid());
  // journal: 1 inbound event + 2 outbound + the first LatencySample publish
  CHECK(f.engine->journal().recorded() == 4);

  // small mid move below threshold: no requote
  f.push_book("100.00", "100.02", 2);
  f.drain();
  CHECK(f.transport.count(EventType::OutNewOrder) == 2);

  f.ack_all_new();
  f.drain();
  CHECK(f.engine->oms().get(f.engine->oms().find(bid.cl_ord_id)).state == OrderState::Live);

  // fill the bid: position +0.01 -> skew -> requote both sides via cancel-then-new
  f.fill(bid.cl_ord_id, Side::Buy, "99.90", "0.01", "e1");
  f.drain();
  CHECK(f.engine->position(InstrumentId{0}).qty == qt("0.01"));
  CHECK(f.engine->stats().fills == 1);
  CHECK(f.engine->oms().open_count() ==
        2);  // ask pending cancel + the re-quoted bid; old bid filled
  // ask was cancelled (skew moved it), bid slot is free: a new bid goes out immediately
  CHECK(f.transport.count(EventType::OutCancel) == 1);
  CHECK(f.transport.count(EventType::OutNewOrder) == 3);
  const auto& bid2 = f.transport.at<OutNewOrderMsg>(f.transport.out.size() - 1);
  CHECK(bid2.side == Side::Buy);
  CHECK(bid2.price < px("99.90"));  // skewed down by inventory
  f.cancel_ack_all();
  f.drain();
  CHECK(f.transport.count(EventType::OutNewOrder) == 4);  // replacement ask after cancel ack
  CHECK(f.engine->oms().open_count() == 2);
  // latency tracker saw the hops
  CHECK(f.engine->latency().histogram(LatencyInterval::TickToTrade).count() >= 1);
  CHECK(f.engine->latency().histogram(LatencyInterval::Decode).count() >= 1);
  const RunnerStats rs = f.engine->runner_stats();
  CHECK(rs.orders_sent == 4);
  CHECK(rs.fills == 1);
  CHECK(format_runner_stats(rs).find("orders=4") != std::string::npos);
}

TEST_CASE("core.engine: kill switch cancels everything and blocks new orders, reset resumes") {
  Fixture f;
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  ControlMsg c{};
  init_header(c, EventType::Control);
  c.command = ControlCommand::TripKill;
  f.push(c);
  f.drain();
  CHECK(f.engine->risk().killed());
  CHECK(f.transport.count(EventType::OutCancel) == 2);
  CHECK_FALSE(f.engine->quoting_enabled());
  f.cancel_ack_all();
  f.drain();
  CHECK(f.engine->oms().open_count() == 0);
  // market moves (fresh snapshot so the book does not cross): no new quotes while killed
  f.push_book("101.00", "101.02", 2, true);
  f.drain();
  CHECK(f.transport.count(EventType::OutNewOrder) == 2);
  NewOrderRequest r{};
  r.instrument = InstrumentId{0};
  r.side = Side::Buy;
  r.price = px("100");
  r.qty = qt("0.01");
  CHECK(f.engine->context().send(r).error() == RejectReason::KillSwitch);
  c.command = ControlCommand::ResetKill;
  f.push(c);
  f.push_book("101.10", "101.12", 3, true);
  f.drain();
  CHECK(f.engine->quoting_enabled());
  CHECK(f.transport.count(EventType::OutNewOrder) == 4);
}

TEST_CASE(
    "core.engine: stale timer pulls quotes; connection loss clears book; unknown ack cancelled") {
  Fixture f;
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  CHECK(f.engine->oms().open_count() == 2);
  // BasicMM pull_on_stale_ms = 500: advance 1 s with no book updates
  f.clock.advance(seconds(1));
  f.drain();
  CHECK(f.engine->stats().timers_fired >= 1);
  CHECK(f.transport.count(EventType::OutCancel) == 2);
  f.cancel_ack_all();
  f.drain();
  // fresh book: quotes again
  f.push_book("100.00", "100.02", 2);
  f.drain();
  CHECK(f.transport.count(EventType::OutNewOrder) == 4);
  f.ack_all_new();
  f.drain();
  // md disconnect: book cleared, quotes pulled
  ConnectionStateMsg cs{};
  init_header(cs, EventType::ConnectionState, InstrumentId{}, VenueId{0});
  cs.state = ConnState::Disconnected;
  cs.channel = 0;
  f.push(cs);
  f.drain();
  CHECK_FALSE(f.engine->book(InstrumentId{0}).has_snapshot());
  CHECK(f.transport.count(EventType::OutCancel) == 4);
  // ack for an id we never issued -> cancel it
  OrderAckMsg a{};
  init_header(a, EventType::OrderAck, InstrumentId{0}, VenueId{0});
  a.cl_ord_id = ClientOrderId{0xBEEF};
  a.venue_order_id = "ghost";
  f.push(a);
  f.drain();
  CHECK(f.transport.count(EventType::OutCancel) == 5);
  const auto& c = f.transport.at<OutCancelMsg>(f.transport.out.size() - 1);
  CHECK(c.cl_ord_id == ClientOrderId{0xBEEF});
  CHECK(c.venue_order_id == "ghost");
  CHECK(f.engine->stats().unknown_order_cancels == 1);
}

TEST_CASE("core.engine: direct order API, risk rejects, transport full trips the kill switch") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  auto& ctx = f.engine->context();
  NewOrderRequest r{};
  r.instrument = InstrumentId{0};
  r.side = Side::Buy;
  r.price = px("99.995");  // off tick
  r.qty = qt("0.01");
  CHECK(ctx.send(r).error() == RejectReason::InvalidTick);
  r.price = px("50");  // outside the 5 % collar
  CHECK(ctx.send(r).error() == RejectReason::PriceCollar);
  CHECK(f.engine->stats().risk_rejects == 2);
  r.price = px("99.50");
  auto id = ctx.send(r);
  REQUIRE(id);
  CHECK(f.transport.count(EventType::OutNewOrder) == 3);
  CHECK(ctx.replace(*id, px("99.60"), qt("0.01")).error() ==
        RejectReason::InvalidState);  // pending new
  f.ack_all_new();
  f.drain();
  CHECK(ctx.replace(*id, px("99.60"), qt("0.01")).error() ==
        RejectReason::VenueReject);  // no replace support
  f.transport.replace = true;
  CHECK(ctx.replace(*id, px("99.60"), qt("0.01")));
  CHECK(f.transport.count(EventType::OutReplace) == 1);
  CHECK(ctx.cancel(ClientOrderId{123}).error() == RejectReason::UnknownOrder);
  // transport full -> fatal: kill switch (requote attempt cannot be delivered)
  f.transport.full = true;
  f.push_book("100.50", "100.52", 2, true);
  f.drain();
  CHECK(f.engine->risk().killed());
  CHECK(f.engine->stats().transport_full >= 1);
  ctx.request_stop();
  CHECK(f.engine->stopped());
}

TEST_CASE("core.engine: reconcile marks unseen orders cancelled and suppresses quoting") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  const auto& bid = f.transport.at<OutNewOrderMsg>(0);
  ReconcileMsg m{};
  init_header(m, EventType::Reconcile, InstrumentId{0}, VenueId{0});
  m.kind = ReconcileMsg::Kind::Begin;
  f.push(m);
  f.drain();
  CHECK(f.engine->reconciling());
  CHECK_FALSE(f.engine->quoting_enabled());
  CHECK(f.transport.count(EventType::OutCancel) == 2);  // quotes pulled at begin
  m.kind = ReconcileMsg::Kind::OpenOrder;
  m.cl_ord_id = bid.cl_ord_id;
  m.venue_order_id = "VB";
  f.push(m);
  m.kind = ReconcileMsg::Kind::Position;
  m.position_qty = qt("0.003");
  m.avg_px = px("99");
  f.push(m);
  m.kind = ReconcileMsg::Kind::End;
  f.push(m);
  f.drain();
  CHECK_FALSE(f.engine->reconciling());
  CHECK(f.engine->position(InstrumentId{0}).qty == qt("0.003"));
  CHECK(f.engine->oms().open_count() == 1);  // the ask (unseen) was marked cancelled
  // The venue still has the bid, so reconciliation cleared its pending cancel (the pull's cancel
  // was lost). Quotes are pulled during reconciliation, so the quote manager cancels it again.
  CHECK(f.engine->oms().get(f.engine->oms().find(bid.cl_ord_id)).state ==
        OrderState::PendingCancel);
  CHECK(f.transport.count(EventType::OutCancel) == 3);
}

TEST_CASE("core.engine: serialize latency starts at the decision of the same event") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();  // BasicMM quotes: a strategy decision followed by a send in the same event
  const auto& serialize = f.engine->latency().histogram(LatencyInterval::Serialize);
  CHECK(serialize.count() >= 1);
  f.ack_all_new();  // working orders, so the stale pull below cancels them
  f.drain();
  const std::uint64_t after_book = serialize.count();
  const std::size_t sent_before = f.transport.count(EventType::OutCancel);
  f.clock.advance(seconds(1));  // no book updates: the stale timer pulls the quotes
  f.drain();
  REQUIRE(f.transport.count(EventType::OutCancel) > sent_before);
  // BasicMM's stale timer pulls the quotes itself: a decision of its own, measured from that timer
  // event. Before T3 was stamped at the decision, these cancels were measured from the book
  // event's T3 and read as the whole second.
  CHECK(serialize.count() > after_book);
  CHECK(serialize.percentile(1.0) < 500'000'000ULL);
}
