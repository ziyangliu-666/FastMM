// Engine wiring test: BasicMM against a fake transport that plays venue (acks, fills).
#include "fastmm/core/engine.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/basic_mm.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
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

  explicit Fixture(bool with_journal = true, void (*tweak)(EngineConfig&) = nullptr) {
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
    if (tweak != nullptr) tweak(cfg);
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
  OrderState state(ClientOrderId id) const {
    const Handle<Order> h = engine->oms().find(id);
    REQUIRE(h.valid());
    return engine->oms().get(h).state;
  }
  std::vector<OutNewOrderMsg> news() const {
    std::vector<OutNewOrderMsg> v;
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      if (reinterpret_cast<const EventHeader*>(transport.out[i].data())->type ==
          EventType::OutNewOrder)
        v.push_back(transport.at<OutNewOrderMsg>(i));
    }
    return v;
  }
  void reconcile(ReconcileMsg::Kind kind,
                 ClientOrderId id = ClientOrderId{},
                 std::optional<ClientOrderId> sent_watermark = std::nullopt) {
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, InstrumentId{0}, VenueId{0});
    m.kind = kind;
    m.cl_ord_id = id;
    m.venue_order_id = "V";
    if (sent_watermark) {
      m.sent_watermark = *sent_watermark;
      m.flags = ReconcileMsg::kSentWatermark;
    }
    push(m);
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
  // journal: the absolute engine clock + 1 inbound event (a clock delta) + 2 outbound + the first
  // LatencySample publish
  CHECK(f.engine->journal().recorded() == 5);

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

TEST_CASE("core.engine: risk and venue rejects are counted per reason") {
  // Quoting off: only the test's orders take rate-limit tokens.
  Fixture f(false, [](EngineConfig& cfg) {
    cfg.quoting_enabled = false;
    cfg.risk.orders_per_sec = 4;
    cfg.risk.burst = 4;
  });
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  auto& ctx = f.engine->context();
  NewOrderRequest r{};
  r.instrument = InstrumentId{0};
  r.side = Side::Buy;
  r.price = px("50");  // outside the 5 % collar
  r.qty = qt("0.01");
  CHECK(ctx.send(r).error() == RejectReason::PriceCollar);
  r.price = px("99.50");
  r.qty = qt("2");  // max_order_qty 1
  CHECK(ctx.send(r).error() == RejectReason::MaxOrderQty);
  r.qty = qt("0.6");
  const auto big = ctx.send(r);  // token 1
  REQUIRE(big);
  CHECK(ctx.send(r).error() == RejectReason::MaxPosition);  // 0.6 open + 0.6 > max_position 1
  // The replace path counts too.
  f.ack_all_new();
  f.drain();
  f.transport.replace = true;
  CHECK(ctx.replace(*big, px("99.60"), qt("2")).error() == RejectReason::MaxOrderQty);
  // A venue reject, delivered twice: the duplicate does not change the order and is not counted.
  r.qty = qt("0.01");
  const auto crossed = ctx.send(r);  // token 2
  REQUIRE(crossed);
  for (int i = 0; i < 2; ++i) {
    OrderRejectMsg rej{};
    init_header(rej, EventType::OrderReject, InstrumentId{0}, VenueId{0});
    rej.cl_ord_id = *crossed;
    rej.reason = RejectReason::PostOnlyWouldCross;
    rej.venue_code = -2010;
    f.push(rej);
    f.drain();
  }
  REQUIRE(ctx.send(r));  // token 3
  REQUIRE(ctx.send(r));  // token 4
  CHECK(ctx.send(r).error() == RejectReason::RateLimit);

  const EngineStats& es = f.engine->stats();
  CHECK(es.risk_rejects == 5);
  CHECK(es.risk_rejects_by_reason[RejectReason::PriceCollar] == 1);
  CHECK(es.risk_rejects_by_reason[RejectReason::MaxOrderQty] == 2);
  CHECK(es.risk_rejects_by_reason[RejectReason::MaxPosition] == 1);
  CHECK(es.risk_rejects_by_reason[RejectReason::RateLimit] == 1);
  CHECK(es.risk_rejects_by_reason.total() == es.risk_rejects);
  CHECK(es.venue_rejects == 1);
  CHECK(es.venue_rejects_by_reason[RejectReason::PostOnlyWouldCross] == 1);
  CHECK(es.venue_rejects_by_reason.total() == 1);
  const RunnerStats rs = f.engine->runner_stats();
  CHECK(rs.venue_rejects == 1);
  CHECK(format_reject_counts(rs.risk_rejects_by_reason) ==
        "MaxOrderQty 2, PriceCollar 1, MaxPosition 1, RateLimit 1");
  CHECK(format_reject_counts(rs.venue_rejects_by_reason) == "PostOnlyWouldCross 1");
}

namespace {
std::string read_from(std::FILE* f, long offset) {
  std::fflush(f);
  std::fseek(f, offset, SEEK_SET);
  std::string s;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  return s;
}
std::size_t occurrences(const std::string& s, std::string_view what) {
  std::size_t n = 0;
  for (std::size_t pos = s.find(what); pos != std::string::npos; pos = s.find(what, pos + 1)) ++n;
  return n;
}
}  // namespace

TEST_CASE(
    "core.engine: risk rejects are logged once per reason per interval with a suppressed count") {
  Fixture f(false, [](EngineConfig& cfg) {
    cfg.quoting_enabled = false;
    cfg.reject_log_interval = seconds(10);
  });
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  std::FILE* out = std::tmpfile();
  REQUIRE(out != nullptr);
  Logger& lg = Logger::instance();
  const LogLevel prev_level = lg.level();
  lg.set_level(LogLevel::Info);
  lg.start(out, LogLevel::Off);
  lg.flush();  // records left by earlier tests on this thread
  const long mark = std::ftell(out);

  auto& ctx = f.engine->context();
  NewOrderRequest r{};
  r.instrument = InstrumentId{0};
  r.side = Side::Buy;
  r.price = px("99.50");
  r.qty = qt("2");
  for (int i = 0; i < 3; ++i) CHECK(ctx.send(r).error() == RejectReason::MaxOrderQty);
  r.price = px("50");
  r.qty = qt("0.01");
  CHECK(ctx.send(r).error() == RejectReason::PriceCollar);  // another reason: logged at once
  // Past the interval (with a fresh book, or the reject would be StaleMarketData).
  f.clock.advance(seconds(11));
  f.push_book("100.00", "100.02", 2, true);
  f.drain();
  r.price = px("99.50");
  r.qty = qt("2");
  CHECK(ctx.send(r).error() == RejectReason::MaxOrderQty);
  lg.flush();
  const std::string log = read_from(out, mark);
  INFO(log);
  CHECK(occurrences(log, "risk reject MaxOrderQty on new order: BTCUSDT Buy 2 @ 99.5") == 2);
  CHECK(occurrences(log, "risk reject PriceCollar on new order: BTCUSDT Buy 0.01 @ 50") == 1);
  CHECK(occurrences(log, "(2 more suppressed)") == 1);
  CHECK(occurrences(log, "WARN") == 3);
  // Every reject is counted, logged or not.
  CHECK(f.engine->stats().risk_rejects_by_reason[RejectReason::MaxOrderQty] == 4);
  lg.stop();
  lg.set_level(prev_level);
  std::fclose(out);
}

TEST_CASE("core.engine: reconcile marks unseen orders cancelled, pauses quoting until End") {
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
  const std::vector<OutNewOrderMsg> sent = f.news();
  // The ask (unseen) was marked cancelled. The venue still has the bid: the pull's cancel is in
  // flight, so the bid stays pending and is not cancelled again.
  CHECK(f.engine->oms().classify(sent[1].cl_ord_id) == OrderClass::RecentlyTerminal);
  CHECK(f.state(bid.cl_ord_id) == OrderState::PendingCancel);
  CHECK(f.transport.count(EventType::OutCancel) == 2);
  // Quoting resumes at End: the free ask level at once, the bid level once its cancel ack arrives.
  REQUIRE(sent.size() == 3);
  CHECK(sent[2].side == Side::Sell);
  CHECK(sent[2].price == px("100.12"));
  CHECK(f.engine->oms().open_count() == 2);
  f.cancel_ack_all();
  f.drain();
  REQUIRE(f.news().size() == 4);
  CHECK(f.news()[3].side == Side::Buy);
  CHECK(f.news()[3].price == px("99.90"));
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

TEST_CASE(
    "core.engine: commission in the base asset adjusts the position and is valued at the fill") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  const auto& bid = f.transport.at<OutNewOrderMsg>(0);
  REQUIRE(bid.side == Side::Buy);
  const auto push_fill = [&](ClientOrderId id,
                             Side side,
                             const char* p,
                             const char* q,
                             const char* fee,
                             FeeAsset asset,
                             const char* exec) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, InstrumentId{0}, VenueId{0});
    m.cl_ord_id = id;
    m.side = side;
    m.price = px(p);
    m.qty = qt(q);
    m.cum_qty = qt(q);
    m.exec_id = exec;
    m.liquidity = Liquidity::Maker;
    m.fee = Notional::from_decimal(fee).value();
    m.fee_asset = asset;
    f.push(m);
    f.drain();
  };
  // Buy 0.01 at 99.90 paying 0.00001 base: we hold 0.00999, and the fee is 0.00001 * 99.90.
  push_fill(bid.cl_ord_id, Side::Buy, "99.90", "0.01", "0.00001", FeeAsset::Base, "b1");
  const Position& pos = f.engine->position(InstrumentId{0});
  CHECK(pos.qty == qt("0.00999"));
  CHECK(pos.fees == Notional::from_decimal("0.000999").value());
  // A quote-asset fee is booked as is and does not change the quantity.
  push_fill(ClientOrderId{0x1234}, Side::Sell, "100.10", "0.004", "0.0004", FeeAsset::Quote, "s1");
  CHECK(pos.qty == qt("0.00599"));
  CHECK(pos.fees == Notional::from_decimal("0.001399").value());
  // A fee in another asset (BNB) is counted, not booked.
  push_fill(ClientOrderId{0x1235}, Side::Sell, "100.10", "0.001", "0.00002", FeeAsset::Other, "s2");
  CHECK(pos.qty == qt("0.00499"));
  CHECK(pos.fees == Notional::from_decimal("0.001399").value());
  CHECK(f.engine->stats().unconverted_fees == 1);
}

TEST_CASE("core.engine: quotes pulled for a reconciliation work again after End, mid unchanged") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  const std::vector<OutNewOrderMsg> quoted = f.news();
  REQUIRE(quoted.size() == 2);
  f.reconcile(ReconcileMsg::Kind::Begin, ClientOrderId{}, quoted[1].cl_ord_id);
  f.drain();
  REQUIRE(f.transport.count(EventType::OutCancel) == 2);  // pulled at Begin
  // The snapshot still shows both quotes: the pull's cancels are in flight.
  f.reconcile(ReconcileMsg::Kind::OpenOrder, quoted[0].cl_ord_id);
  f.reconcile(ReconcileMsg::Kind::OpenOrder, quoted[1].cl_ord_id);
  f.reconcile(ReconcileMsg::Kind::End);
  f.drain();
  CHECK_FALSE(f.engine->reconciling());
  CHECK(f.engine->quoting_enabled());
  CHECK(f.state(quoted[0].cl_ord_id) == OrderState::PendingCancel);
  CHECK(f.state(quoted[1].cl_ord_id) == OrderState::PendingCancel);
  CHECK(f.transport.count(EventType::OutCancel) == 2);
  CHECK(f.transport.count(EventType::OutNewOrder) == 2);
  // The cancel acks arrive after End. The book has not moved, yet both levels are quoted again.
  f.cancel_ack_all();
  f.drain();
  const std::vector<OutNewOrderMsg> requoted = f.news();
  REQUIRE(requoted.size() == 4);
  CHECK(requoted[2].side == Side::Buy);
  CHECK(requoted[2].price == quoted[0].price);
  CHECK(requoted[3].side == Side::Sell);
  CHECK(requoted[3].price == quoted[1].price);
  f.ack_all_new();
  f.drain();
  CHECK(f.engine->oms().open_count() == 2);
  CHECK(f.state(requoted[2].cl_ord_id) == OrderState::Live);
  CHECK(f.state(requoted[3].cl_ord_id) == OrderState::Live);
}

TEST_CASE("core.engine: orders sent after the open-orders request stay tracked through End") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  const ClientOrderId last_sent = f.news()[1].cl_ord_id;
  // The order channel drops: the engine pulls the quotes and the venue cancels them.
  ConnectionStateMsg cs{};
  init_header(cs, EventType::ConnectionState, InstrumentId{}, VenueId{0});
  cs.state = ConnState::Disconnected;
  cs.channel = 1;
  f.push(cs);
  f.drain();
  REQUIRE(f.transport.count(EventType::OutCancel) == 2);
  f.cancel_ack_all();
  f.drain();
  REQUIRE(f.engine->oms().open_count() == 0);
  // Back: the venue reports Live, then requests its open orders. The strategy requotes on Live;
  // those orders leave after the request, so the snapshot cannot show them.
  cs.state = ConnState::Live;
  f.push(cs);
  f.drain();
  const std::vector<OutNewOrderMsg> requoted = f.news();
  REQUIRE(requoted.size() == 4);
  f.reconcile(ReconcileMsg::Kind::Begin, ClientOrderId{}, last_sent);
  f.reconcile(ReconcileMsg::Kind::End);
  f.drain();
  CHECK(f.engine->oms().open_count() == 2);
  CHECK(f.state(requoted[2].cl_ord_id) == OrderState::PendingNew);
  CHECK(f.state(requoted[3].cl_ord_id) == OrderState::PendingNew);
  CHECK(f.transport.count(EventType::OutNewOrder) == 4);  // nothing re-placed on top of them
  // Their acks are applied: both quotes are tracked and working, nothing is left untracked.
  f.ack_all_new();
  f.drain();
  CHECK(f.state(requoted[2].cl_ord_id) == OrderState::Live);
  CHECK(f.state(requoted[3].cl_ord_id) == OrderState::Live);
  CHECK(f.engine->stats().unknown_order_cancels == 0);
  CHECK(f.transport.count(EventType::OutCancel) == 2);
  CHECK(f.transport.count(EventType::OutNewOrder) == 4);
}

TEST_CASE("core.engine: late and unknown fills update position, fees and PnL") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  const OutNewOrderMsg bid = f.news()[0];
  REQUIRE(bid.side == Side::Buy);
  ControlMsg c{};
  init_header(c, EventType::Control);
  c.command = ControlCommand::PullQuotes;  // quotes cancelled, no requote to muddy the position
  f.push(c);
  f.drain();
  f.cancel_ack_all();
  f.drain();
  REQUIRE(f.engine->oms().open_count() == 0);
  const auto push_fill = [&](ClientOrderId id,
                             Side side,
                             const char* p,
                             const char* q,
                             const char* fee,
                             FeeAsset asset,
                             const char* exec) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, InstrumentId{0}, VenueId{0});
    m.cl_ord_id = id;
    m.side = side;
    m.price = px(p);
    m.qty = qt(q);
    m.cum_qty = qt(q);
    m.exec_id = exec;
    m.liquidity = Liquidity::Maker;
    m.fee = Notional::from_decimal(fee).value();
    m.fee_asset = asset;
    f.push(m);
    f.drain();
  };
  // The bid filled at the venue before the cancel took effect; the fill arrives after the cancel
  // ack. Commission in the base asset: we hold 0.00999 and pay 0.00001 * 99.90.
  push_fill(bid.cl_ord_id, Side::Buy, "99.90", "0.01", "0.00001", FeeAsset::Base, "late1");
  CHECK(f.engine->oms().stats().late_fills == 1);
  const Position& pos = f.engine->position(InstrumentId{0});
  CHECK(pos.qty == qt("0.00999"));
  CHECK(pos.fees == Notional::from_decimal("0.000999").value());
  CHECK(f.engine->stats().fills == 1);
  CHECK(f.engine->runner_stats().fees_raw == pos.fees.raw);
  // A fill for an id we never issued, on an instrument we trade, is booked too.
  push_fill(ClientOrderId{0xABCD}, Side::Sell, "100.10", "0.004", "0.0004", FeeAsset::Quote, "u1");
  CHECK(pos.qty == qt("0.00599"));
  CHECK(pos.fees == Notional::from_decimal("0.001399").value());
  CHECK(f.engine->oms().stats().unknown_ids >= 1);
  // Realized PnL: 0.004 sold at 100.10 against an average cost of 99.90.
  CHECK(pos.realized.is_positive());
}

TEST_CASE("core.engine: End frees the quote manager's slots of orders the venue no longer has") {
  Fixture f(false);
  f.push_book("100.00", "100.02", 1, true);
  f.drain();
  f.ack_all_new();
  f.drain();
  const QuoteManager& qm = f.engine->quote_manager();
  REQUIRE(qm.slot_handle(InstrumentId{0}, Side::Buy, 0).valid());
  REQUIRE(qm.slot_handle(InstrumentId{0}, Side::Sell, 0).valid());
  f.reconcile(ReconcileMsg::Kind::Begin);
  ControlMsg c{};
  init_header(c, EventType::Control);
  c.command = ControlCommand::PullQuotes;  // the operator stops quoting meanwhile: nothing resumes
  f.push(c);
  f.reconcile(ReconcileMsg::Kind::End);  // the snapshot shows neither quote
  f.drain();
  CHECK(f.engine->oms().open_count() == 0);
  CHECK_FALSE(qm.slot_handle(InstrumentId{0}, Side::Buy, 0).valid());
  CHECK_FALSE(qm.slot_handle(InstrumentId{0}, Side::Sell, 0).valid());
  CHECK(f.transport.count(EventType::OutNewOrder) == 2);
}
