// Engine <-> strategy hook contract (ADR-0012): instrument filtering, on_quoting transitions, the
// Fill view, set_quotes results, the context additions and the direct-order tag check.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
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
  REQUIRE(t.add(i));
  return t;
}

constexpr InstrumentId kId{0};
constexpr InstrumentId kOutside{5};  // not in the table

struct FakeTransport {
  std::vector<std::vector<std::byte>> out;
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
  bool supports_replace(VenueId) const noexcept { return false; }
  template <class M>
  const M& at(std::size_t i) const {
    return *reinterpret_cast<const M*>(out[i].data());
  }
  std::vector<OutNewOrderMsg> news() const {
    std::vector<OutNewOrderMsg> v;
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (reinterpret_cast<const EventHeader*>(out[i].data())->type == EventType::OutNewOrder)
        v.push_back(at<OutNewOrderMsg>(i));
    }
    return v;
  }
  std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto& m : out) n += reinterpret_cast<const EventHeader*>(m.data())->type == t;
    return n;
  }
};

// Records every hook call; optional behaviours are switched on by the test.
struct Spy {
  std::vector<InstrumentId> books, tickers, trades, option_tickers;
  std::vector<Fill> fills;
  std::vector<OmsUpdate> fill_updates;  // copies of *fill.update, taken inside the call
  std::vector<OrderFillMsg> fill_msgs;  // copies of *fill.msg
  std::vector<bool> quoting;            // on_quoting arguments
  std::vector<bool> quoting_seen;       // ctx.quoting_enabled() inside on_quoting
  std::vector<bool> set_quotes_results;
  std::vector<std::uint64_t> timer_tags;
  bool in_hook = false;
  bool reentered = false;
  bool quote_on_book = false;
  bool pull_all_on_trade = false;

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book&) noexcept {
    in_hook = true;
    books.push_back(id);
    if (quote_on_book) {
      DesiredQuotes q;
      static_cast<void>(q.bids.push_back(Level{px("99.90"), qt("0.01")}));
      static_cast<void>(q.asks.push_back(Level{px("100.10"), qt("0.01")}));
      set_quotes_results.push_back(ctx.set_quotes(id, q));
    }
    in_hook = false;
  }
  template <class Ctx>
  void on_book_ticker(Ctx&, InstrumentId id, const BookTickerMsg&) noexcept {
    tickers.push_back(id);
  }
  template <class Ctx>
  void on_trade(Ctx& ctx, InstrumentId id, const TradeMsg&) noexcept {
    trades.push_back(id);
    if (pull_all_on_trade) ctx.pull_all_quotes();
  }
  template <class Ctx>
  void on_option_ticker(Ctx&, InstrumentId id, const OptionTickerMsg&) noexcept {
    option_tickers.push_back(id);
  }
  template <class Ctx>
  void on_fill(Ctx&, const Fill& f) noexcept {
    fills.push_back(f);
    fill_updates.push_back(f.update != nullptr ? *f.update : OmsUpdate{});
    fill_msgs.push_back(f.msg != nullptr ? *f.msg : OrderFillMsg{});
  }
  template <class Ctx>
  void on_timer(Ctx&, TimerId, std::uint64_t tag) noexcept {
    timer_tags.push_back(tag);
  }
  template <class Ctx>
  void on_quoting(Ctx& ctx, bool enabled) noexcept {
    if (in_hook) reentered = true;
    quoting.push_back(enabled);
    quoting_seen.push_back(ctx.quoting_enabled());
  }
};
static_assert(verify_strategy<Spy>());

template <class S>
struct Rig {
  using E = Engine<S, SimClock, FakeTransport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  FakeTransport transport;
  InlineFeed feed{1 << 20};
  S strategy;
  std::unique_ptr<E> engine;

  explicit Rig(const ParamMap& params = {}) {
    if constexpr (requires { strategy.configure(params); }) {
      REQUIRE_FALSE(strategy.configure(params));
    }
    EngineConfig cfg;
    cfg.risk.max_open_orders = 8;
    cfg.quotes.min_requote_interval = Duration{};
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy);
    engine->warm_up();
    engine->start();
  }
  auto& ctx() { return engine->context(); }
  void drain() {
    while (engine->step() > 0) {
    }
  }
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    drain();
  }
  void book(InstrumentId id, const char* bid, const char* ask) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, id, VenueId{0}, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->last_update_id = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  void control(ControlCommand c) {
    ControlMsg m{};
    init_header(m, EventType::Control);
    m.command = c;
    push(m);
  }
  void reconcile(ReconcileMsg::Kind kind) {
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, kId, VenueId{0});
    m.kind = kind;
    push(m);
  }
  void ack_all() {
    for (const OutNewOrderMsg& n : transport.news()) {
      const Handle<Order> h = engine->oms().find(n.cl_ord_id);
      if (!h.valid() || engine->oms().get(h).state != OrderState::PendingNew) continue;
      OrderAckMsg a{};
      init_header(a, EventType::OrderAck, n.hdr.instrument, VenueId{0});
      a.cl_ord_id = n.cl_ord_id;
      a.venue_order_id = "V";
      push(a);
    }
  }
  void cancel_ack_all() {
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      const auto* h = reinterpret_cast<const EventHeader*>(transport.out[i].data());
      if (h->type != EventType::OutCancel) continue;
      const auto& c = transport.at<OutCancelMsg>(i);
      const Handle<Order> oh = engine->oms().find(c.cl_ord_id);
      if (!oh.valid() || engine->oms().get(oh).state != OrderState::PendingCancel) continue;
      OrderCancelAckMsg a{};
      init_header(a, EventType::OrderCancelAck, kId, VenueId{0});
      a.cl_ord_id = c.cl_ord_id;
      push(a);
    }
  }
  void fill(InstrumentId inst,
            ClientOrderId id,
            Side side,
            const char* p,
            const char* q,
            const char* fee,
            FeeAsset asset,
            const char* exec) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, inst, VenueId{0});
    m.cl_ord_id = id;
    m.side = side;
    m.price = px(p);
    m.qty = qt(q);
    m.exec_id = exec;
    m.liquidity = Liquidity::Maker;
    m.fee = Notional::from_decimal(fee).value();
    m.fee_asset = asset;
    push(m);
  }
};

}  // namespace

TEST_CASE("core.engine.hooks: instrument-scoped hooks skip instruments outside the table") {
  Rig<Spy> r;
  for (const InstrumentId id : {kOutside, kId}) {
    TradeMsg t{};
    init_header(t, EventType::Trade, id, VenueId{0});
    t.price = px("100");
    t.qty = qt("1");
    r.push(t);
    BookTickerMsg bt{};
    init_header(bt, EventType::BookTicker, id, VenueId{0});
    r.push(bt);
    OptionTickerMsg ot{};
    init_header(ot, EventType::OptionTicker, id, VenueId{0});
    r.push(ot);
    r.book(id, "100.00", "100.02");
    r.fill(
        id, ClientOrderId{0xAB00 + id.value}, Side::Buy, "100", "0.01", "0", FeeAsset::Quote, "x");
  }
  CHECK(r.strategy.trades == std::vector<InstrumentId>{kId});
  CHECK(r.strategy.tickers == std::vector<InstrumentId>{kId});
  CHECK(r.strategy.option_tickers == std::vector<InstrumentId>{kId});
  CHECK(r.strategy.books == std::vector<InstrumentId>{kId});
  REQUIRE(r.strategy.fills.size() == 1);
  CHECK(r.strategy.fills[0].instrument == kId);
  CHECK(r.engine->stats().unknown_instrument_fills == 1);
  CHECK(r.engine->stats().fills == 2);  // both counted
  CHECK(r.engine->position(kId).qty == qt("0.01"));
}

TEST_CASE("core.engine.hooks: on_quoting fires once per transition, after the flags are final") {
  Rig<Spy> r;
  CHECK(r.strategy.quoting.empty());  // not at start
  r.control(ControlCommand::PullQuotes);
  r.control(ControlCommand::PullQuotes);  // no change, no call
  r.control(ControlCommand::ResumeQuotes);
  r.control(ControlCommand::ResumeQuotes);
  CHECK(r.strategy.quoting == std::vector<bool>{false, true});
  r.control(ControlCommand::TripKill);
  r.control(ControlCommand::TripKill);
  r.control(ControlCommand::ResetKill);
  CHECK(r.strategy.quoting == std::vector<bool>{false, true, false, true});
  r.reconcile(ReconcileMsg::Kind::Begin);
  CHECK_FALSE(r.engine->quoting_enabled());
  r.reconcile(ReconcileMsg::Kind::Position);
  r.reconcile(ReconcileMsg::Kind::End);
  CHECK(r.strategy.quoting == std::vector<bool>{false, true, false, true, false, true});
  // Inside the hook the context already reports the new state.
  CHECK(r.strategy.quoting_seen == r.strategy.quoting);
  // A pull during a reconciliation: disabled throughout, so End reports nothing.
  r.reconcile(ReconcileMsg::Kind::Begin);
  r.control(ControlCommand::PullQuotes);
  r.reconcile(ReconcileMsg::Kind::End);
  CHECK(r.strategy.quoting.size() == 7);
  r.control(ControlCommand::ResumeQuotes);
  CHECK(r.strategy.quoting.size() == 8);
  CHECK(r.strategy.quoting.back());
}

TEST_CASE("core.engine.hooks: a kill switch tripped inside set_quotes does not re-enter") {
  Rig<Spy> r;
  r.strategy.quote_on_book = true;
  r.transport.full = true;  // the quotes cannot be sent: the flush trips the kill switch
  r.book(kId, "100.00", "100.02");
  CHECK(r.engine->risk().killed());
  CHECK_FALSE(r.strategy.reentered);
  CHECK(r.strategy.quoting == std::vector<bool>{false});
  CHECK(r.strategy.set_quotes_results == std::vector<bool>{true});  // taken, then killed
  r.transport.full = false;
  r.book(kId, "100.00", "100.02");
  CHECK(r.strategy.set_quotes_results == std::vector<bool>{true, false});
}

TEST_CASE("core.engine.hooks: set_quotes returns false while quoting is disabled") {
  Rig<Spy> r;
  r.strategy.quote_on_book = true;
  r.control(ControlCommand::PullQuotes);
  r.book(kId, "100.00", "100.02");
  CHECK(r.strategy.set_quotes_results == std::vector<bool>{false});
  CHECK(r.transport.count(EventType::OutNewOrder) == 0);
  r.control(ControlCommand::ResumeQuotes);
  r.book(kId, "100.00", "100.02");
  CHECK(r.strategy.set_quotes_results == std::vector<bool>{false, true});
  CHECK(r.transport.count(EventType::OutNewOrder) == 2);
  CHECK_FALSE(r.ctx().set_quotes(kOutside, DesiredQuotes{}));
}

TEST_CASE("core.engine.hooks: BasicMM requotes after ResumeQuotes with the mid unchanged") {
  Rig<BasicMM> r({{"half_spread_bps", "10"},
                  {"quote_qty", "0.01"},
                  {"max_inventory", "0.05"},
                  {"pull_on_stale_ms", "0"}});
  r.book(kId, "100.00", "100.02");
  r.ack_all();
  const std::vector<OutNewOrderMsg> quoted = r.transport.news();
  REQUIRE(quoted.size() == 2);
  r.control(ControlCommand::PullQuotes);
  REQUIRE(r.transport.count(EventType::OutCancel) == 2);
  r.cancel_ack_all();
  REQUIRE(r.engine->oms().open_count() == 0);
  r.book(kId, "100.00", "100.02");  // ignored while pulled
  CHECK(r.transport.news().size() == 2);
  r.control(ControlCommand::ResumeQuotes);  // no book update follows
  const std::vector<OutNewOrderMsg> requoted = r.transport.news();
  REQUIRE(requoted.size() == 4);
  CHECK(requoted[2].price == quoted[0].price);
  CHECK(requoted[3].price == quoted[1].price);
}

TEST_CASE("core.engine.hooks: Fill fields for own, base-fee, late, unknown and third-asset fills") {
  Rig<Spy> r;
  r.book(kId, "100.00", "100.02");
  auto& ctx = r.ctx();
  const auto id = ctx.send(NewOrderRequest::limit(kId, Side::Buy, px("99.90"), qt("0.01")));
  REQUIRE(id);
  r.ack_all();

  // Partial fill with the commission in the base asset: we hold 0.00399 of the 0.004.
  r.fill(kId, *id, Side::Buy, "99.90", "0.004", "0.00001", FeeAsset::Base, "f1");
  REQUIRE(r.strategy.fills.size() == 1);
  const Fill& a = r.strategy.fills[0];
  CHECK(a.instrument == kId);
  CHECK(a.side == Side::Buy);
  CHECK(a.price == px("99.90"));
  CHECK(a.qty == qt("0.004"));
  CHECK(a.position_delta == qt("0.00399"));
  CHECK(a.fee == Notional::from_decimal("0.000999").value());
  CHECK(a.fee_converted);
  CHECK(a.liquidity == Liquidity::Maker);
  CHECK(a.known);
  CHECK_FALSE(a.late);
  CHECK_FALSE(a.order_done);
  CHECK(a.update != nullptr);
  CHECK(a.msg != nullptr);
  CHECK(r.strategy.fill_updates[0].order.cl_ord_id == *id);
  CHECK(r.strategy.fill_updates[0].order.state == OrderState::PartiallyFilled);
  CHECK(r.strategy.fill_msgs[0].exec_id == "f1");
  CHECK(r.engine->position(kId).qty == qt("0.00399"));  // booked before the hook

  // The rest, quote-asset fee: the order is done with this fill.
  r.fill(kId, *id, Side::Buy, "99.90", "0.006", "0.001", FeeAsset::Quote, "f2");
  REQUIRE(r.strategy.fills.size() == 2);
  CHECK(r.strategy.fills[1].order_done);
  CHECK(r.strategy.fills[1].position_delta == qt("0.006"));
  CHECK(r.strategy.fills[1].fee == Notional::from_decimal("0.001").value());

  // A sell cancelled at the venue, then filled: late, known through the terminal record.
  const auto sell = ctx.send(NewOrderRequest::limit(kId, Side::Sell, px("100.10"), qt("0.002")));
  REQUIRE(sell);
  r.ack_all();
  REQUIRE(ctx.cancel(*sell));
  r.cancel_ack_all();
  REQUIRE(ctx.order(*sell) == nullptr);
  r.fill(kId, *sell, Side::Sell, "100.10", "0.002", "0", FeeAsset::Quote, "late");
  REQUIRE(r.strategy.fills.size() == 3);
  const Fill& late = r.strategy.fills[2];
  CHECK(late.late);
  CHECK(late.known);
  CHECK_FALSE(late.order_done);
  CHECK(late.side == Side::Sell);
  CHECK(late.position_delta == qt("-0.002"));
  CHECK(r.strategy.fill_updates[2].order.instrument == kId);

  // An id we never issued: not known, no update; commission in a third asset is not booked.
  r.fill(kId, ClientOrderId{0xABCD}, Side::Sell, "100.10", "0.001", "0.2", FeeAsset::Other, "u");
  REQUIRE(r.strategy.fills.size() == 4);
  const Fill& unknown = r.strategy.fills[3];
  CHECK_FALSE(unknown.known);
  CHECK_FALSE(unknown.late);
  CHECK(unknown.update == nullptr);
  CHECK(unknown.msg != nullptr);
  CHECK_FALSE(unknown.fee_converted);
  CHECK(unknown.fee == Notional{});
  CHECK(r.engine->stats().unconverted_fees == 1);
}

TEST_CASE("core.engine.hooks: ctx.pull_all_quotes records a serialize latency sample") {
  Rig<Spy> r;
  r.strategy.quote_on_book = true;
  r.book(kId, "100.00", "100.02");
  r.ack_all();
  const auto& serialize = r.engine->latency().histogram(LatencyInterval::Serialize);
  const std::uint64_t before = serialize.count();
  r.strategy.pull_all_on_trade = true;
  TradeMsg t{};
  init_header(t, EventType::Trade, kId, VenueId{0});
  t.hdr.t0_cycles = r.clock.cycles();
  r.push(t);
  CHECK(r.transport.count(EventType::OutCancel) == 2);
  CHECK(serialize.count() == before + 1);
}

TEST_CASE("core.engine.hooks: direct orders may not use the quote manager's tag range") {
  CHECK(static_cast<int>(RejectReason::InvalidTag) == 38);
  CHECK(static_cast<int>(RejectReason::NotReconciled) == 37);
  CHECK(to_string(RejectReason::InvalidTag) == "InvalidTag");
  Rig<Spy> r;
  r.book(kId, "100.00", "100.02");
  auto& ctx = r.ctx();
  const auto order = NewOrderRequest::limit(kId, Side::Buy, px("99.90"), qt("0.01"));
  const auto quote_tag = ctx.send(order.tag(QuoteManager::make_tag(Side::Buy, 0)));
  REQUIRE_FALSE(quote_tag);
  CHECK(quote_tag.error() == RejectReason::InvalidTag);
  CHECK(r.transport.count(EventType::OutNewOrder) == 0);
  const auto own_tag = ctx.send(order.tag(42).post_only());
  REQUIRE(own_tag);
  const OutNewOrderMsg sent = r.transport.news().at(0);
  CHECK(sent.type == OrderType::PostOnly);
  CHECK(ctx.order(*own_tag)->user_tag == 42);
}

TEST_CASE("core.engine.hooks: context queries, portfolio and timers") {
  Rig<Spy> r;
  r.strategy.quote_on_book = true;
  auto& ctx = r.ctx();
  CHECK(ctx.contains(kId));
  CHECK_FALSE(ctx.contains(kOutside));
  r.book(kId, "100.00", "100.02");
  const Order* bid = ctx.working_quote(kId, Side::Buy, 0);
  REQUIRE(bid != nullptr);
  CHECK(bid->price == px("99.90"));
  CHECK(ctx.working_quote(kId, Side::Buy, 1) == nullptr);
  CHECK(ctx.working_quote(kId, Side::Buy, 99) == nullptr);
  CHECK(ctx.working_quote(kOutside, Side::Buy, 0) == nullptr);
  CHECK(ctx.order(bid->cl_ord_id) == bid);
  CHECK(ctx.open_qty(kId, Side::Buy) == qt("0.01"));
  const auto reduce =
      NewOrderRequest::limit(kId, Side::Sell, px("101"), qt("0.01")).reduce_only().ioc();
  CHECK(static_cast<NewOrderRequest>(reduce).reduce_only);
  CHECK(static_cast<NewOrderRequest>(reduce).tif == TimeInForce::Ioc);

  r.fill(kId, bid->cl_ord_id, Side::Buy, "99.90", "0.01", "0.01", FeeAsset::Quote, "p1");
  const Portfolio p = ctx.portfolio();
  CHECK(p.fees == Notional::from_decimal("0.01").value());
  CHECK(p.net == p.realized + p.unrealized - p.fees);

  const TimerId every = ctx.every(milliseconds(10), 1);
  static_cast<void>(ctx.once(milliseconds(15), 2));
  for (int i = 0; i < 4; ++i) {
    r.clock.advance(milliseconds(10));
    r.drain();
  }
  CHECK(r.strategy.timer_tags == std::vector<std::uint64_t>{1, 2, 1, 1, 1});
  CHECK(ctx.cancel_timer(every));
}
