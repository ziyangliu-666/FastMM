// A venue's trading rules changing under a running engine (InstrumentRulesMsg): the changed fields
// replace the instrument's for every later check and for the strategy; an instrument that stops
// trading is sent nothing, not even a cancel, and when it trades again its venue reconciles before
// the quotes come back.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

struct RulesHook {
  static std::string_view name() noexcept { return "rules_hook"; }
  int calls = 0;
  Qty lot_seen{};
  TradingStatus status_seen = TradingStatus::Trading;
  template <class Ctx>
  void on_instrument_rules(Ctx& ctx, InstrumentId id, const InstrumentRulesMsg& m) noexcept {
    ++calls;
    lot_seen = ctx.instrument(id).lot;
    status_seen = m.status;
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
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < out.size(); ++i) n += header(i).type == t ? 1U : 0U;
    return n;
  }
  [[nodiscard]] std::size_t reconciles() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
      n += header(i).type == EventType::Control &&
                   at<ControlMsg>(i).command == ControlCommand::Reconcile
               ? 1U
               : 0U;
    }
    return n;
  }
};
static_assert(TransportLike<Outbox>);

using RulesEngine = Engine<RulesHook, SimClock, Outbox, InlineFeed>;

const InstrumentId kBtc{0};
const VenueId kVenue{0};

struct Fixture {
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  Outbox transport;
  InlineFeed feed{1 << 20};
  MsgRing journal_ring{1 << 20};
  RulesHook strategy;
  std::unique_ptr<RulesEngine> engine;

  explicit Fixture(TradingStatus start = TradingStatus::Trading) {
    Instrument i{};
    i.symbol = "BTCUSDT";
    i.venue = kVenue;
    i.flags = Instrument::kEnabled;
    i.tick = px("0.01");
    i.lot = qt("0.001");
    i.min_qty = qt("0.001");
    i.status = start;
    REQUIRE(table.add(i));
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.quotes.min_requote_interval = Duration{};
    cfg.quotes.min_requote_ticks = 1;
    engine =
        std::make_unique<RulesEngine>(cfg, table, clock, transport, feed, strategy, &journal_ring);
    engine->warm_up();
    engine->start();
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, kBtc, kVenue, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px("100.00"), qt("5")};
    d->levels()[1] = Level{px("100.02"), qt("5")};
    feed.commit();
    drain();
  }
  void drain() {
    while (engine->step() != 0) {
    }
  }
  void rules(std::uint8_t changed, const char* lot, TradingStatus status) {
    InstrumentRulesMsg m{};
    init_header(m, EventType::InstrumentRules, kBtc, kVenue);
    m.tick = px("0.01");
    m.lot = qt(lot);
    m.min_qty = qt(lot);
    m.status = status;
    m.changed = changed;
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    drain();
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
      a.hdr.recv_ts = clock.now();
      REQUIRE(feed.push(a.hdr));
    }
    drain();
  }
  [[nodiscard]] NewOrderRequest buy(const char* qty) const {
    NewOrderRequest r{};
    r.instrument = kBtc;
    r.side = Side::Buy;
    r.price = px("99.50");
    r.qty = qt(qty);
    return r;
  }
};

DesiredQuotes quotes(const char* qty) {
  DesiredQuotes q;
  q.bid(px("99.90"), qt(qty));
  q.ask(px("100.10"), qt(qty));
  return q;
}

}  // namespace

TEST_CASE("core.engine: a new lot step from the venue applies to every later order") {
  Fixture f;
  REQUIRE(f.engine->context().send(f.buy("0.001")));
  const InstrumentTable& before = f.engine->instruments();
  CHECK(&before != &f.table);  // the engine's own copy

  f.rules(InstrumentRulesMsg::kLot | InstrumentRulesMsg::kMinQty, "0.01", TradingStatus::Trading);
  CHECK(f.engine->instrument(kBtc).lot == qt("0.01"));
  CHECK(f.engine->instrument(kBtc).min_qty == qt("0.01"));
  CHECK(f.engine->instrument(kBtc).tick == px("0.01"));
  CHECK(f.table.get(kBtc).lot == qt("0.001"));  // the session's table is not the engine's
  CHECK(f.engine->stats().rules_changes == 1);
  CHECK(f.strategy.calls == 1);
  CHECK(f.strategy.lot_seen == qt("0.01"));  // the hook sees the new rules
  CHECK(f.engine->context().instrument(kBtc).round_qty(qt("0.019")) == qt("0.01"));

  // The old step is refused by the engine's own check now; the new one goes.
  const auto old_step = f.engine->context().send(f.buy("0.005"));
  REQUIRE_FALSE(old_step);
  CHECK(f.engine->context().send(f.buy("0.02")));
  CHECK(f.engine->stats().reconcile_requests == 0);  // still trading: nothing to reconcile
}

TEST_CASE("core.engine: a field the message does not name stays as it was") {
  Fixture f;
  InstrumentRulesMsg m{};
  init_header(m, EventType::InstrumentRules, kBtc, kVenue);
  m.tick = px("0.1");
  m.lot = qt("5");  // not named
  m.status = TradingStatus::Trading;
  m.changed = InstrumentRulesMsg::kTick;
  REQUIRE(f.feed.push(m.hdr));
  f.drain();
  CHECK(f.engine->instrument(kBtc).tick == px("0.1"));
  CHECK(f.engine->instrument(kBtc).lot == qt("0.001"));
}

TEST_CASE("core.engine: a halted instrument is sent nothing, and trading again reconciles first") {
  Fixture f;
  REQUIRE(f.engine->set_quotes(kBtc, quotes("0.001")));
  f.ack_all();
  REQUIRE(f.transport.count(EventType::OutNewOrder) == 2);

  f.rules(InstrumentRulesMsg::kStatus, "0.001", TradingStatus::Halted);
  CHECK(f.strategy.status_seen == TradingStatus::Halted);
  CHECK_FALSE(f.engine->quoting_enabled(kBtc));
  CHECK(f.engine->quoting_enabled());  // the session quotes on

  const std::size_t sent = f.transport.out.size();
  // New quotes are ignored, a direct order is refused, and the working quotes are not cancelled:
  // the venue would refuse the cancel.
  CHECK_FALSE(f.engine->set_quotes(kBtc, quotes("0.002")));
  CHECK(f.engine->context().send(f.buy("0.001")).error() == RejectReason::InstrumentDisabled);
  f.engine->pull_quotes(kBtc);
  std::vector<ClientOrderId> ids;
  f.engine->oms().for_each_open_order(
      [&](Handle<Order>, const Order& o) { ids.push_back(o.cl_ord_id); });
  REQUIRE(ids.size() == 2);
  for (const ClientOrderId id : ids)
    CHECK(f.engine->context().cancel(id).error() == RejectReason::InstrumentDisabled);
  CHECK(f.transport.out.size() == sent);
  CHECK(f.engine->oms().open_count(kBtc) == 2);

  // Trading again: the venue reconciles (it may have ended the orders meanwhile), once.
  f.rules(InstrumentRulesMsg::kStatus, "0.001", TradingStatus::Trading);
  CHECK(f.transport.reconciles() == 1);
  CHECK(f.engine->stats().reconcile_requests == 1);
  CHECK(f.engine->quoting_enabled(kBtc));
  f.rules(InstrumentRulesMsg::kLot, "0.001", TradingStatus::Trading);  // trading already
  CHECK(f.transport.reconciles() == 1);
}

TEST_CASE("core.engine: an instrument the venue does not trade at start is held from the start") {
  Fixture f(TradingStatus::Halted);
  CHECK_FALSE(f.engine->quoting_enabled(kBtc));
  CHECK_FALSE(f.engine->set_quotes(kBtc, quotes("0.001")));
  CHECK(f.transport.count(EventType::OutNewOrder) == 0);
  f.rules(InstrumentRulesMsg::kStatus, "0.001", TradingStatus::Trading);
  CHECK(f.transport.reconciles() == 1);
  CHECK(f.engine->set_quotes(kBtc, quotes("0.001")));
  CHECK(f.transport.count(EventType::OutNewOrder) == 2);
}
