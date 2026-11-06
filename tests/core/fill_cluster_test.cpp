// The fill-cluster guard (core/fill_cluster.hpp): distinct instruments per side within the window,
// the hold and its extension, the lift; and in the engine, quote fills on several instruments pull
// that side everywhere, set_quotes places nothing on it during the hold, and the last quotes come
// back when it ends. Direct orders are neither counted nor pulled.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/fill_cluster.hpp"

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

FillCluster guard(std::uint32_t names, std::int64_t window_ms, std::int64_t hold_ms) {
  FillCluster g;
  g.configure(FillClusterConfig{names, milliseconds(window_ms), milliseconds(hold_ms)});
  return g;
}

}  // namespace

TEST_CASE("core.fill_cluster: distinct instruments within the window engage the side") {
  FillCluster g = guard(3, 500, 400);
  CHECK_FALSE(g.on_fill(InstrumentId{1}, Side::Buy, at_ms(0)));
  CHECK_FALSE(g.on_fill(InstrumentId{1}, Side::Buy, at_ms(10)));  // the same instrument again
  CHECK_FALSE(g.on_fill(InstrumentId{2}, Side::Buy, at_ms(20)));
  CHECK_FALSE(g.on_fill(InstrumentId{3}, Side::Sell, at_ms(30)));  // the other side
  CHECK_FALSE(g.held(Side::Buy));
  CHECK(g.on_fill(InstrumentId{3}, Side::Buy, at_ms(40)));
  CHECK(g.held(Side::Buy));
  CHECK_FALSE(g.held(Side::Sell));
  CHECK(g.view().held_until[0] == at_ms(440));
  CHECK(g.view().engagements[0] == 1);
  // Another fill that completes a cluster extends the hold without engaging again.
  CHECK_FALSE(g.on_fill(InstrumentId{4}, Side::Buy, at_ms(100)));
  CHECK(g.view().held_until[0] == at_ms(500));
  CHECK_FALSE(g.lift_due(at_ms(499)));
  CHECK_FALSE(g.lift(Side::Buy, at_ms(499)));
  CHECK(g.lift_due(at_ms(500)));
  CHECK(g.lift(Side::Buy, at_ms(500)));
  CHECK_FALSE(g.held(Side::Buy));
  CHECK_FALSE(g.lift_due(at_ms(600)));
  CHECK(g.view().engagements[0] == 1);
}

TEST_CASE("core.fill_cluster: fills older than the window do not count") {
  FillCluster g = guard(3, 500, 400);
  CHECK_FALSE(g.on_fill(InstrumentId{1}, Side::Sell, at_ms(0)));
  CHECK_FALSE(g.on_fill(InstrumentId{2}, Side::Sell, at_ms(300)));
  CHECK_FALSE(g.on_fill(InstrumentId{3}, Side::Sell, at_ms(501)));  // instrument 1 is out
  CHECK(g.on_fill(InstrumentId{1}, Side::Sell, at_ms(600)));        // back in: 2, 3, 1
  CHECK(g.held(Side::Sell));
}

TEST_CASE("core.fill_cluster: more instruments than slots still count as a cluster") {
  FillCluster g = guard(FillClusterConfig::kMaxNames, 1000, 100);
  for (std::uint16_t i = 0; i < 40; ++i) {
    const bool engaged = g.on_fill(InstrumentId{i}, Side::Buy, at_ms(i));
    CHECK(engaged == (i + 1U == FillClusterConfig::kMaxNames));
  }
  CHECK(g.held(Side::Buy));
  CHECK(g.view().engagements[0] == 1);
}

TEST_CASE("core.fill_cluster: one name holds the side after every fill") {
  FillCluster g = guard(1, 500, 200);
  CHECK(g.on_fill(InstrumentId{7}, Side::Buy, at_ms(0)));
  CHECK(g.lift(Side::Buy, at_ms(200)));
  CHECK(g.on_fill(InstrumentId{7}, Side::Buy, at_ms(250)));
  CHECK(g.view().engagements[0] == 2);
  CHECK_FALSE(FillClusterConfig{}.enabled());
}

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

struct NoHooks {
  static std::string_view name() noexcept { return "no_hooks"; }
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

using ClusterEngine = Engine<NoHooks, SimClock, Outbox, InlineFeed>;
constexpr std::uint16_t kInstruments = 4;

struct FourBooks {
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  Outbox transport;
  InlineFeed feed{1 << 20};
  MsgRing journal_ring{1 << 20};
  NoHooks strategy;
  std::unique_ptr<ClusterEngine> engine;
  std::uint64_t exec = 0;

  explicit FourBooks(std::uint32_t names) {
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
    cfg.fill_cluster = FillClusterConfig{names, milliseconds(500), milliseconds(300)};
    engine =
        std::make_unique<ClusterEngine>(cfg, table, clock, transport, feed, strategy, &journal_ring);
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
  // The working order of `side` on `id`, as the venue would fill it: a partial maker fill.
  void fill(InstrumentId id, Side side, ClientOrderId cl) {
    const Handle<Order> h = engine->oms().find(cl);
    REQUIRE(h.valid());
    const Order& o = engine->oms().get(h);
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill, id, VenueId{0});
    f.cl_ord_id = cl;
    f.side = side;
    f.price = o.price;
    f.qty = qt("0.001");
    f.cum_qty = qt("0.001");
    f.liquidity = Liquidity::Maker;
    const std::string e = std::to_string(++exec);
    f.exec_id = std::string_view(e);
    push(f);
    drain();
  }
  [[nodiscard]] ClientOrderId quote(InstrumentId id, Side side) {
    return engine->oms().get(engine->quote_manager().slot_handle(id, side, 0)).cl_ord_id;
  }
  // Working orders (acked, not cancelled or pending a cancel) of `side` on `id`.
  [[nodiscard]] std::size_t working(InstrumentId id, Side side) const {
    std::size_t n = 0;
    engine->oms().for_each_open_order(id, [&](Handle<Order>, const Order& o) {
      n += o.side == side && o.is_working() ? 1U : 0U;
    });
    return n;
  }
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < transport.out.size(); ++i) n += transport.header(i).type == t ? 1U : 0U;
    return n;
  }
};

DesiredQuotes two_sided() {
  DesiredQuotes q;
  q.bid(px("99.90"), qt("0.01"));
  q.ask(px("100.10"), qt("0.01"));
  return q;
}

}  // namespace

TEST_CASE("core.engine: quote fills on several instruments hold that side everywhere") {
  FourBooks f(2);
  for (std::uint16_t k = 0; k < kInstruments; ++k)
    REQUIRE(f.engine->set_quotes(InstrumentId{k}, two_sided()));
  REQUIRE(f.engine->context().send(
      NewOrderRequest::limit(InstrumentId{3}, Side::Buy, px("99.50"), qt("0.01"))));
  f.ack_all();
  REQUIRE(f.count(EventType::OutNewOrder) == 9);

  // One instrument filled: nothing happens.
  f.fill(InstrumentId{0}, Side::Buy, f.quote(InstrumentId{0}, Side::Buy));
  CHECK(f.count(EventType::OutCancel) == 0);
  CHECK_FALSE(f.engine->context().fill_cluster().held[0]);
  // A second instrument within the window: every bid quote is cancelled, the asks and the direct
  // bid stay.
  f.clock.advance(milliseconds(100));
  f.fill(InstrumentId{1}, Side::Buy, f.quote(InstrumentId{1}, Side::Buy));
  const FillClusterView v = f.engine->context().fill_cluster();
  CHECK(v.enabled);
  CHECK(v.held[0]);
  CHECK_FALSE(v.held[1]);
  CHECK(v.engagements[0] == 1);
  CHECK(v.held_until[0] == f.clock.now() + milliseconds(300));
  CHECK(f.count(EventType::OutCancel) == 4);
  for (std::uint16_t k = 0; k < kInstruments; ++k) CHECK(f.working(InstrumentId{k}, Side::Sell) == 1);
  CHECK(f.working(InstrumentId{3}, Side::Buy) == 1);  // the direct order

  f.cancel_ack_all();

  // During the hold set_quotes places no bid, and the ask still moves.
  std::size_t news = f.count(EventType::OutNewOrder);
  DesiredQuotes moved;
  moved.bid(px("99.95"), qt("0.01"));
  moved.ask(px("100.05"), qt("0.01"));
  f.clock.advance(milliseconds(100));
  CHECK(f.engine->set_quotes(InstrumentId{2}, moved));
  f.cancel_ack_all();  // cancel-then-new: the ask's New follows its cancel
  CHECK(f.count(EventType::OutNewOrder) == news + 1);
  f.ack_all();
  CHECK(f.working(InstrumentId{2}, Side::Sell) == 1);
  CHECK(f.working(InstrumentId{2}, Side::Buy) == 0);

  // The hold runs out: the next event puts every instrument's last bid back.
  news = f.count(EventType::OutNewOrder);
  f.clock.advance(milliseconds(250));
  f.push_book(InstrumentId{0});
  f.drain();
  CHECK_FALSE(f.engine->context().fill_cluster().held[0]);
  CHECK(f.count(EventType::OutNewOrder) == news + 4);
  f.ack_all();
  for (std::uint16_t k = 0; k < 3; ++k) CHECK(f.working(InstrumentId{k}, Side::Buy) == 1);
  CHECK(f.working(InstrumentId{3}, Side::Buy) == 2);  // its quote and the direct order
  const Handle<Order> h = f.engine->quote_manager().slot_handle(InstrumentId{2}, Side::Buy, 0);
  CHECK(f.engine->oms().get(h).price == px("99.95"));  // the last bid asked for
}

TEST_CASE("core.engine: fills of direct orders do not count towards a fill cluster") {
  FourBooks f(2);
  for (std::uint16_t k = 0; k < 2; ++k) {
    const auto id = f.engine->context().send(
        NewOrderRequest::limit(InstrumentId{k}, Side::Buy, px("99.50"), qt("0.01")));
    REQUIRE(id);
    f.ack_all();
    f.fill(InstrumentId{k}, Side::Buy, *id);
  }
  CHECK_FALSE(f.engine->context().fill_cluster().held[0]);
  CHECK(f.engine->context().fill_cluster().engagements[0] == 0);
}

TEST_CASE("core.engine: without fill_cluster_names the guard stays off") {
  FourBooks f(0);
  for (std::uint16_t k = 0; k < kInstruments; ++k)
    REQUIRE(f.engine->set_quotes(InstrumentId{k}, two_sided()));
  f.ack_all();
  for (std::uint16_t k = 0; k < kInstruments; ++k)
    f.fill(InstrumentId{k}, Side::Buy, f.quote(InstrumentId{k}, Side::Buy));
  CHECK(f.count(EventType::OutCancel) == 0);
  CHECK_FALSE(f.engine->context().fill_cluster().enabled);
}
