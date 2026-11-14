// The hazard groups (core/hazard.hpp): the score sums evidence and halves every half-life, evidence
// that takes it to the threshold engages a side and later evidence extends the hold, a print and
// the fill report of one match count once. In the engine: a quote fill on one member pulls that
// side on every member and nowhere else, set_quotes places nothing on it during the hold, the last
// quotes come back when it ends; a print that reaches our quote engages ahead of the fill; a group
// in log mode holds on paper and sends nothing.
#include "fastmm/core/hazard.hpp"

#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;

namespace {

Timestamp at_ms(std::int64_t ms) {
  return Timestamp{seconds(1000).ns + milliseconds(ms).ns};
}

HazardGroupConfig group(double threshold, std::vector<std::uint16_t> members) {
  HazardGroupConfig g;
  g.name = "g";
  for (const std::uint16_t m : members) g.members.push_back(InstrumentId{m});
  g.threshold = threshold;
  g.half_life = milliseconds(20);
  g.hold = milliseconds(300);
  return g;
}

Hazard hazard(const HazardGroupConfig& g, std::size_t instruments = 8) {
  HazardConfig c;
  c.groups.push_back(g);
  Hazard h;
  h.configure(c, instruments);
  return h;
}

}  // namespace

TEST_CASE(
    "core.hazard: the score sums evidence, halves every half-life and engages at the "
    "threshold") {
  Hazard h = hazard(group(150.0, {0, 1, 2}));
  REQUIRE(h.enabled());
  CHECK(h.groups() == 1);
  CHECK(h.group_of(InstrumentId{1}) == 0);
  CHECK(h.group_of(InstrumentId{3}) == Hazard::kNoGroup);
  CHECK(h.group_of(InstrumentId{99}) == Hazard::kNoGroup);

  CHECK(h.add(0, Side::Buy, 100.0, at_ms(0)) == Hazard::Outcome::None);
  CHECK(h.view(0, at_ms(0)).score[0] == doctest::Approx(100.0));
  CHECK(h.view(0, at_ms(20)).score[0] == doctest::Approx(50.0));
  CHECK(h.view(0, at_ms(40)).score[0] == doctest::Approx(25.0));
  CHECK(h.view(0, at_ms(0)).score[1] == 0.0);
  // 50 left of the first piece, 100 more: at the threshold.
  CHECK(h.add(0, Side::Buy, 100.0, at_ms(20)) == Hazard::Outcome::Engaged);
  HazardView v = h.view(0, at_ms(20));
  CHECK(v.enabled);
  CHECK(v.held[0]);
  CHECK_FALSE(v.held[1]);
  CHECK(v.engagements[0] == 1);
  CHECK(v.held_since[0] == at_ms(20));
  CHECK(v.held_until[0] == at_ms(320));
  CHECK(v.threshold == 150.0);
  // Evidence that finds the score at the threshold extends the hold; less does not.
  CHECK(h.add(0, Side::Buy, 100.0, at_ms(30)) == Hazard::Outcome::Extended);
  CHECK(h.view(0, at_ms(30)).held_until[0] == at_ms(330));
  CHECK(h.view(0, at_ms(30)).held_since[0] == at_ms(20));
  CHECK(h.add(0, Side::Buy, 10.0, at_ms(200)) == Hazard::Outcome::None);
  CHECK(h.view(0, at_ms(200)).held_until[0] == at_ms(330));
  CHECK(h.view(0, at_ms(200)).engagements[0] == 1);

  // lift_due says a hold may have run out; lift() ends those that have.
  int lifted = 0;
  h.lift(at_ms(329), [&](std::uint32_t, Side) { ++lifted; });
  CHECK(lifted == 0);
  CHECK(h.view(0, at_ms(329)).held[0]);
  CHECK_FALSE(h.lift_due(at_ms(329)));
  REQUIRE(h.lift_due(at_ms(330)));
  h.lift(at_ms(330), [&](std::uint32_t g, Side s) {
    CHECK(g == 0);
    CHECK(s == Side::Buy);
    ++lifted;
  });
  CHECK(lifted == 1);
  CHECK_FALSE(h.view(0, at_ms(330)).held[0]);
  CHECK_FALSE(h.lift_due(at_ms(10'000)));
  // The next engagement counts again.
  CHECK(h.add(0, Side::Buy, 200.0, at_ms(1000)) == Hazard::Outcome::Engaged);
  CHECK(h.view(0, at_ms(1000)).engagements[0] == 2);
}

TEST_CASE("core.hazard: evidence against lowers the score, never below zero, and never engages") {
  Hazard h = hazard(group(100.0, {0}));
  CHECK(h.add(0, Side::Sell, 80.0, at_ms(0)) == Hazard::Outcome::None);
  CHECK(h.add(0, Side::Sell, -50.0, at_ms(0)) == Hazard::Outcome::None);
  CHECK(h.view(0, at_ms(0)).score[1] == doctest::Approx(30.0));
  CHECK(h.add(0, Side::Sell, -500.0, at_ms(0)) == Hazard::Outcome::None);
  CHECK(h.view(0, at_ms(0)).score[1] == 0.0);
  CHECK(h.add(0, Side::Sell, 99.0, at_ms(1)) == Hazard::Outcome::None);
  CHECK(h.add(0, Side::Sell, -1.0, at_ms(1)) == Hazard::Outcome::None);
  CHECK(h.add(0, Side::Sell, 2.0, at_ms(1)) == Hazard::Outcome::Engaged);
}

TEST_CASE("core.hazard: a side the group leaves out keeps no score") {
  HazardGroupConfig g = group(100.0, {0});
  g.sides = {true, false};
  Hazard h = hazard(g);
  CHECK(h.add(0, Side::Sell, 500.0, at_ms(0)) == Hazard::Outcome::None);
  CHECK(h.view(0, at_ms(0)).score[1] == 0.0);
  CHECK(h.wants_print(InstrumentId{0}, Side::Buy));
  CHECK_FALSE(h.wants_print(InstrumentId{0}, Side::Sell));
  CHECK_FALSE(h.wants_print(InstrumentId{1}, Side::Buy));
  CHECK(h.add(0, Side::Buy, 500.0, at_ms(0)) == Hazard::Outcome::Engaged);
}

TEST_CASE("core.hazard: an instrument belongs to its first group, weights follow the members") {
  HazardConfig c;
  HazardGroupConfig a = group(100.0, {0, 1, 40});
  a.weights = {1.0, 0.5, 2.0};
  HazardGroupConfig b = group(100.0, {1, 2});
  b.name = "b";
  HazardGroupConfig off = group(0.0, {3});
  c.groups = {a, b, off};
  Hazard h;
  h.configure(c, 8);
  CHECK(h.groups() == 2);           // a group without a threshold is none
  CHECK(h.members(0).size() == 2);  // 40 is outside the instrument table
  CHECK(h.members(1).size() == 1);
  CHECK(h.group_of(InstrumentId{1}) == 0);
  CHECK(h.group_of(InstrumentId{2}) == 1);
  CHECK(h.group_of(InstrumentId{3}) == Hazard::kNoGroup);
  CHECK(h.weight(InstrumentId{1}) == 0.5);
  CHECK(h.weight(InstrumentId{2}) == 1.0);
  CHECK(h.name(1) == "b");
  CHECK_FALSE(Hazard{}.enabled());
}

TEST_CASE("core.hazard: a print and the fill report of one match count once") {
  Hazard h = hazard(group(1e9, {0, 1}));
  const InstrumentId id{0};
  // The print first: it counts what our quote could have traded, the fill then adds nothing.
  CHECK(h.print_qty(id, Side::Buy, 500, 300, at_ms(0)) == 300);
  CHECK(h.fill_qty(id, Side::Buy, 300, 30.0, at_ms(1)) == 0);
  // The fill first: its print adds nothing, though quantity still rests at the price.
  CHECK(h.fill_qty(id, Side::Buy, 200, 20.0, at_ms(2)) == 200);
  CHECK(h.print_qty(id, Side::Buy, 200, 1000, at_ms(3)) == 0);
  // A print that covers part of a fill, and a fill larger than its print.
  CHECK(h.print_qty(id, Side::Buy, 100, 400, at_ms(4)) == 100);
  CHECK(h.fill_qty(id, Side::Buy, 250, 25.0, at_ms(5)) == 150);
  CHECK(h.print_qty(id, Side::Buy, 150, 400, at_ms(6)) == 0);
  // A print of someone else's order at our price is not matched for ever: after the window a fill
  // counts in full.
  CHECK(h.print_qty(id, Side::Buy, 100, 400, at_ms(10)) == 100);
  CHECK(h.fill_qty(id, Side::Buy, 100, 10.0, at_ms(10) + Hazard::kCopyWindow + milliseconds(1)) ==
        100);
  // Sides and members are matched apart.
  CHECK(h.print_qty(id, Side::Sell, 100, 100, at_ms(5000)) == 100);
  CHECK(h.fill_qty(id, Side::Buy, 100, 10.0, at_ms(5000)) == 100);
  CHECK(h.fill_qty(InstrumentId{1}, Side::Sell, 100, 10.0, at_ms(5000)) == 100);
  // A print with nothing of ours at the price is no evidence.
  CHECK(h.print_qty(InstrumentId{1}, Side::Buy, 100, 0, at_ms(5000)) == 0);
}

TEST_CASE("core.hazard: fills that arrive on a held side are counted") {
  Hazard h = hazard(group(50.0, {0, 1}));
  CHECK(h.fill_qty(InstrumentId{0}, Side::Buy, 10, 60.0, at_ms(0)) == 10);
  CHECK(h.view(0, at_ms(0)).held_fills[0] == 0);
  CHECK(h.add(0, Side::Buy, 60.0, at_ms(0)) == Hazard::Outcome::Engaged);
  CHECK(h.fill_qty(InstrumentId{1}, Side::Buy, 10, 40.0, at_ms(1)) == 10);
  CHECK(h.fill_qty(InstrumentId{1}, Side::Sell, 10, 40.0, at_ms(1)) == 10);
  const HazardView v = h.view(0, at_ms(1));
  CHECK(v.held_fills[0] == 1);
  CHECK(v.held_notional[0] == doctest::Approx(40.0));
  CHECK(v.held_fills[1] == 0);
}

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

// Adds evidence from its trade hook when the test asks it to.
struct Adds {
  static std::string_view name() noexcept { return "adds"; }
  double amount = 0.0;
  bool quoted_after = false;
  template <class Ctx>
  void on_trade(Ctx& ctx, InstrumentId id, const TradeMsg&) noexcept {
    if (amount == 0.0) return;
    ctx.hazard_add(ctx.hazard_group(id), Side::Buy, amount);
    if (!ctx.hazard(ctx.hazard_group(id)).held[0]) return;
    // The side is held before the call returns: this bid is not placed.
    DesiredQuotes q;
    q.bid(Price::from_decimal("99.80").value(), Qty::from_decimal("0.01").value());
    q.ask(Price::from_decimal("100.10").value(), Qty::from_decimal("0.01").value());
    quoted_after = ctx.set_quotes(id, q);
  }
};

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
  template <class M>
  [[nodiscard]] const M& at(std::size_t i) const {
    return *reinterpret_cast<const M*>(out[i].data());
  }
};
static_assert(TransportLike<Outbox>);

using HazardEngine = Engine<Adds, SimClock, Outbox, InlineFeed>;
constexpr std::uint16_t kInstruments = 4;

// Four instruments; the first three are one hazard group. A bid of 0.01 at 99.90 is 0.999 of
// notional.
struct FourBooks {
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  Outbox transport;
  InlineFeed feed{1 << 20};
  MsgRing journal_ring{1 << 20};
  Adds strategy;
  std::unique_ptr<HazardEngine> engine;
  std::uint64_t exec = 0;

  explicit FourBooks(HazardGroupConfig g) {
    for (std::uint16_t k = 0; k < kInstruments; ++k) {
      Instrument i{};
      const std::string sym = "S" + std::to_string(k);
      i.symbol = std::string_view(sym);
      i.venue = VenueId{0};
      i.flags = Instrument::kEnabled;
      i.tick = px("0.01");
      i.lot = qt("0.001");
      i.min_qty = qt("0.001");
      REQUIRE(table.add(i));
    }
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.quotes.min_requote_interval = Duration{};
    cfg.quotes.min_requote_ticks = 1;
    if (g.threshold > 0.0) cfg.hazard.groups.push_back(std::move(g));
    engine =
        std::make_unique<HazardEngine>(cfg, table, clock, transport, feed, strategy, &journal_ring);
    engine->warm_up();
    engine->start();
    for (std::uint16_t k = 0; k < kInstruments; ++k) push_book(InstrumentId{k});
    drain();
  }

  void push_book(InstrumentId id) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, id, VenueId{0}, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px("100.00"), qt("5")};
    d->levels()[1] = Level{px("100.02"), qt("5")};
    feed.commit();
  }
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
  }
  void drain() {
    while (engine->step() != 0) {
    }
  }
  void ack_all() {
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      if (transport.header(i).type != EventType::OutNewOrder) continue;
      const auto& n = transport.at<OutNewOrderMsg>(i);
      const Handle<Order> h = engine->oms().find(n.cl_ord_id);
      if (!h.valid() || engine->oms().get(h).state != OrderState::PendingNew) continue;
      OrderAckMsg a{};
      init_header(a, EventType::OrderAck, n.hdr.instrument, n.hdr.venue);
      a.cl_ord_id = n.cl_ord_id;
      a.venue_order_id = "V";
      push(a);
    }
    drain();
  }
  void cancel_ack_all() {
    for (std::size_t i = 0; i < transport.out.size(); ++i) {
      if (transport.header(i).type != EventType::OutCancel) continue;
      const auto& c = transport.at<OutCancelMsg>(i);
      const Handle<Order> h = engine->oms().find(c.cl_ord_id);
      if (!h.valid() || engine->oms().get(h).state != OrderState::PendingCancel) continue;
      OrderCancelAckMsg a{};
      init_header(a, EventType::OrderCancelAck, c.hdr.instrument, c.hdr.venue);
      a.cl_ord_id = c.cl_ord_id;
      push(a);
    }
    drain();
  }
  // A partial maker fill of the order, `qty` of it.
  void fill(InstrumentId id, Side side, ClientOrderId cl, const char* qty = "0.002") {
    const Handle<Order> h = engine->oms().find(cl);
    REQUIRE(h.valid());
    const Order& o = engine->oms().get(h);
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill, id, VenueId{0});
    f.cl_ord_id = cl;
    f.side = side;
    f.price = o.price;
    f.qty = qt(qty);
    f.cum_qty = o.cum_qty + qt(qty);
    f.liquidity = Liquidity::Maker;
    const std::string e = std::to_string(++exec);
    f.exec_id = std::string_view(e);
    push(f);
    drain();
  }
  // A public print: `aggressor` took `qty` at `price`.
  void print(InstrumentId id, Side aggressor, const char* price, const char* qty) {
    TradeMsg t{};
    init_header(t, EventType::Trade, id, VenueId{0});
    t.price = px(price);
    t.qty = qt(qty);
    t.aggressor = aggressor;
    t.trade_id = ++exec;
    push(t);
    drain();
  }
  [[nodiscard]] ClientOrderId quote(InstrumentId id, Side side) {
    return engine->oms().get(engine->quote_manager().slot_handle(id, side, 0)).cl_ord_id;
  }
  [[nodiscard]] std::size_t working(InstrumentId id, Side side) const {
    std::size_t n = 0;
    engine->oms().for_each_open_order(id, [&](Handle<Order>, const Order& o) {
      n += o.side == side && o.is_working() ? 1U : 0U;
    });
    return n;
  }
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < transport.out.size(); ++i)
      n += transport.header(i).type == t ? 1U : 0U;
    return n;
  }
  void quote_all() {
    DesiredQuotes q;
    q.bid(px("99.90"), qt("0.01"));
    q.ask(px("100.10"), qt("0.01"));
    for (std::uint16_t k = 0; k < kInstruments; ++k)
      REQUIRE(engine->set_quotes(InstrumentId{k}, q));
    ack_all();
  }
};

// 0.002 at 99.90 is 0.1998 of notional: two such fills within a half-life pass 0.3, one does not.
HazardGroupConfig three(double threshold = 0.3) {
  return group(threshold, {0, 1, 2});
}

}  // namespace

TEST_CASE("core.engine: quote fills on a hazard group's members pull that side on the group") {
  FourBooks f(three());
  f.quote_all();
  REQUIRE(f.count(EventType::OutNewOrder) == 8);
  REQUIRE(f.engine->context().send(
      NewOrderRequest::limit(InstrumentId{1}, Side::Buy, px("99.50"), qt("0.01"))));
  f.ack_all();
  CHECK(f.engine->context().hazard_groups() == 1);
  CHECK(f.engine->context().hazard_group(InstrumentId{2}) == 0);
  CHECK(f.engine->context().hazard_group(InstrumentId{3}) == Hazard::kNoGroup);

  // One fill is under the threshold.
  f.fill(InstrumentId{0}, Side::Buy, f.quote(InstrumentId{0}, Side::Buy));
  CHECK(f.count(EventType::OutCancel) == 0);
  CHECK(f.engine->context().hazard(0).score[0] == doctest::Approx(0.1998));
  CHECK_FALSE(f.engine->context().hazard(0).held[0]);
  // A second one 5 ms later, on another member: the members' bid quotes are cancelled; their asks,
  // the direct bid and the instrument outside the group stay.
  f.clock.advance(milliseconds(5));
  f.fill(InstrumentId{1}, Side::Buy, f.quote(InstrumentId{1}, Side::Buy));
  const HazardView v = f.engine->context().hazard(0);
  CHECK(v.held[0]);
  CHECK_FALSE(v.held[1]);
  CHECK(v.engagements[0] == 1);
  CHECK(v.held_until[0] == f.clock.now() + milliseconds(300));
  CHECK(f.count(EventType::OutCancel) == 3);
  for (std::uint16_t k = 0; k < kInstruments; ++k)
    CHECK(f.working(InstrumentId{k}, Side::Sell) == 1);
  CHECK(f.working(InstrumentId{1}, Side::Buy) == 1);  // the direct order
  CHECK(f.working(InstrumentId{3}, Side::Buy) == 1);  // not a member
  CHECK(f.engine->quote_manager().side_held(InstrumentId{2}, Side::Buy));
  CHECK_FALSE(f.engine->quote_manager().side_held(InstrumentId{3}, Side::Buy));
  f.cancel_ack_all();

  // During the hold set_quotes places no bid on a member; its ask still moves, and the instrument
  // outside the group moves both.
  std::size_t news = f.count(EventType::OutNewOrder);
  DesiredQuotes moved;
  moved.bid(px("99.95"), qt("0.01"));
  moved.ask(px("100.05"), qt("0.01"));
  f.clock.advance(milliseconds(100));
  CHECK(f.engine->set_quotes(InstrumentId{2}, moved));
  CHECK(f.engine->set_quotes(InstrumentId{3}, moved));
  f.cancel_ack_all();
  CHECK(f.count(EventType::OutNewOrder) == news + 3);
  f.ack_all();
  CHECK(f.working(InstrumentId{2}, Side::Buy) == 0);
  CHECK(f.working(InstrumentId{2}, Side::Sell) == 1);
  CHECK(f.working(InstrumentId{3}, Side::Buy) == 1);

  // The hold runs out: the next event puts the members' last bids back.
  news = f.count(EventType::OutNewOrder);
  f.clock.advance(milliseconds(250));
  f.push_book(InstrumentId{3});
  f.drain();
  CHECK_FALSE(f.engine->context().hazard(0).held[0]);
  CHECK(f.count(EventType::OutNewOrder) == news + 3);
  f.ack_all();
  for (std::uint16_t k = 0; k < 3; ++k) CHECK(f.working(InstrumentId{k}, Side::Buy) >= 1);
  const Handle<Order> h = f.engine->quote_manager().slot_handle(InstrumentId{2}, Side::Buy, 0);
  CHECK(f.engine->oms().get(h).price == px("99.95"));
  CHECK_FALSE(f.engine->quote_manager().side_held(InstrumentId{2}, Side::Buy));
}

TEST_CASE("core.engine: a print that reaches our quote counts ahead of its fill report") {
  FourBooks f(three(0.9));
  f.quote_all();
  // Prints above our bid, on the other side, or on an instrument outside the group are nothing.
  f.print(InstrumentId{0}, Side::Sell, "99.95", "1");
  f.print(InstrumentId{0}, Side::Buy, "100.10", "0.005");
  f.print(InstrumentId{3}, Side::Sell, "99.90", "1");
  CHECK(f.engine->context().hazard(0).score[0] == 0.0);
  CHECK(f.engine->context().hazard(0).score[1] == doctest::Approx(0.5005));
  // A sell print through our bid: all 0.01 of it is reached, 0.998 of notional at the print's
  // price. The bids are pulled before any fill report.
  f.print(InstrumentId{0}, Side::Sell, "99.80", "1");
  CHECK(f.engine->context().hazard(0).held[0]);
  CHECK(f.count(EventType::OutCancel) == 3);
  const double score = f.engine->context().hazard(0).score[0];
  CHECK(score == doctest::Approx(0.998));
  // Its fill report adds nothing.
  f.fill(InstrumentId{0}, Side::Buy, f.quote(InstrumentId{0}, Side::Buy), "0.01");
  CHECK(f.engine->context().hazard(0).score[0] == doctest::Approx(score));
  CHECK(f.engine->context().hazard(0).held_fills[0] == 1);
}

TEST_CASE("core.engine: with prints off only the fill report counts") {
  HazardGroupConfig g = three(0.9);
  g.prints = false;
  FourBooks f(g);
  f.quote_all();
  f.print(InstrumentId{0}, Side::Sell, "99.80", "1");
  CHECK(f.engine->context().hazard(0).score[0] == 0.0);
  f.fill(InstrumentId{0}, Side::Buy, f.quote(InstrumentId{0}, Side::Buy), "0.01");
  CHECK(f.engine->context().hazard(0).held[0]);
}

TEST_CASE("core.engine: a hazard group in log mode holds on paper and sends nothing") {
  HazardGroupConfig g = three();
  g.log_only = true;
  FourBooks f(g);
  f.quote_all();
  f.fill(InstrumentId{0}, Side::Buy, f.quote(InstrumentId{0}, Side::Buy));
  f.fill(InstrumentId{1}, Side::Buy, f.quote(InstrumentId{1}, Side::Buy));
  HazardView v = f.engine->context().hazard(0);
  CHECK(v.log_only);
  CHECK(v.held[0]);
  CHECK(v.engagements[0] == 1);
  CHECK(f.count(EventType::OutCancel) == 0);
  CHECK_FALSE(f.engine->quote_manager().side_held(InstrumentId{0}, Side::Buy));
  // A fill that arrives while it would have held is counted.
  f.clock.advance(milliseconds(50));
  f.fill(InstrumentId{2}, Side::Buy, f.quote(InstrumentId{2}, Side::Buy));
  v = f.engine->context().hazard(0);
  CHECK(v.held_fills[0] == 1);
  CHECK(v.held_notional[0] == doctest::Approx(0.1998));
  // Requotes go through, and the end of the hold sends nothing either.
  const std::size_t news = f.count(EventType::OutNewOrder);
  f.clock.advance(milliseconds(400));
  f.push_book(InstrumentId{0});
  f.drain();
  CHECK_FALSE(f.engine->context().hazard(0).held[0]);
  CHECK(f.count(EventType::OutNewOrder) == news);
  CHECK(f.count(EventType::OutCancel) == 0);
}

TEST_CASE("core.engine: evidence a strategy adds pulls the side before the call returns") {
  FourBooks f(three(10.0));
  f.quote_all();
  const std::size_t news = f.count(EventType::OutNewOrder);
  f.strategy.amount = 4.0;
  f.print(InstrumentId{1}, Side::Buy, "100.02", "0.001");
  CHECK(f.count(EventType::OutCancel) == 0);
  CHECK(f.engine->context().hazard(0).score[0] == doctest::Approx(4.0));
  f.strategy.amount = 7.0;
  f.clock.advance(milliseconds(1));
  f.print(InstrumentId{1}, Side::Buy, "100.02", "0.001");
  CHECK(f.strategy.quoted_after);
  CHECK(f.engine->context().hazard(0).held[0]);
  // The three bids are cancelled; the bid the strategy asked for after adding is not placed.
  CHECK(f.count(EventType::OutCancel) == 3);
  f.cancel_ack_all();
  CHECK(f.count(EventType::OutNewOrder) == news);
  // It is the one that comes back.
  f.strategy.amount = 0.0;
  f.clock.advance(milliseconds(301));
  f.push_book(InstrumentId{0});
  f.drain();
  f.ack_all();
  const Handle<Order> h = f.engine->quote_manager().slot_handle(InstrumentId{1}, Side::Buy, 0);
  REQUIRE(h.valid());
  CHECK(f.engine->oms().get(h).price == px("99.80"));
  // A group that does not exist takes no evidence.
  f.engine->context().hazard_add(7, Side::Buy, 1e9);
  f.engine->context().hazard_add(Hazard::kNoGroup, Side::Buy, 1e9);
  CHECK_FALSE(f.engine->context().hazard(7).enabled);
}

TEST_CASE("core.engine: fills of direct orders are no hazard evidence, and no group is no hazard") {
  FourBooks f(three(0.01));
  const auto id = f.engine->context().send(
      NewOrderRequest::limit(InstrumentId{0}, Side::Buy, px("99.50"), qt("0.01")));
  REQUIRE(id);
  f.ack_all();
  f.fill(InstrumentId{0}, Side::Buy, *id);
  CHECK(f.engine->context().hazard(0).score[0] == 0.0);
  CHECK(f.engine->context().hazard(0).engagements[0] == 0);

  FourBooks none(three(0.0));
  none.quote_all();
  CHECK(none.engine->context().hazard_groups() == 0);
  for (std::uint16_t k = 0; k < kInstruments; ++k)
    none.fill(InstrumentId{k}, Side::Buy, none.quote(InstrumentId{k}, Side::Buy), "0.01");
  CHECK(none.count(EventType::OutCancel) == 0);
}
