// Kill switch scopes in the engine: ControlCommand::TripVenueKill (what a venue connector sends on
// a fatal error) trips one venue's flag, pulls that venue's quotes and cancels its orders while the
// other venues keep trading; every venue killed is a global kill. The first reason of each flag is
// kept and published in the live stats at once (fastmm-live's control loop reads it).
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
  [[nodiscard]] std::size_t count(EventType t, VenueId v) const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < out.size(); ++i)
      n += header(i).type == t && header(i).venue == v ? 1U : 0U;
    return n;
  }
};
static_assert(TransportLike<Outbox>);

using KillEngine = Engine<NoHooks, SimClock, Outbox, InlineFeed>;

const InstrumentId kBtc{0};  // on venue 0
const InstrumentId kEth{1};  // on venue 1
const VenueId kVenue0{0};
const VenueId kVenue1{1};

struct TwoVenues {
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  Outbox transport;
  InlineFeed feed{1 << 20};
  MsgRing journal_ring{1 << 20};
  NoHooks strategy;
  std::unique_ptr<KillEngine> engine;

  explicit TwoVenues(const char* max_loss = nullptr) {
    for (const auto& [symbol, venue] :
         {std::pair{"BTCUSDT", kVenue0}, std::pair{"ETHUSDT", kVenue1}}) {
      Instrument i{};
      i.symbol = symbol;
      i.venue = venue;
      i.flags = Instrument::kEnabled;
      i.tick = px("0.01");
      i.lot = qt("0.001");
      i.min_qty = qt("0.001");
      REQUIRE(table.add(i));
    }
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    if (max_loss != nullptr) cfg.risk.max_loss = Notional::from_decimal(max_loss).value();
    cfg.quotes.min_requote_interval = Duration{};
    cfg.quotes.min_requote_ticks = 1;
    engine =
        std::make_unique<KillEngine>(cfg, table, clock, transport, feed, strategy, &journal_ring);
    engine->warm_up();
    engine->start();
    push_book(kBtc, kVenue0, "100.00", "100.02");
    push_book(kEth, kVenue1, "100.00", "100.02");
    drain();
  }

  void push_book(InstrumentId id, VenueId venue, const char* bid, const char* ask) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, id, venue, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
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
  [[nodiscard]] static NewOrderRequest buy(InstrumentId id, const char* price) {
    NewOrderRequest r{};
    r.instrument = id;
    r.side = Side::Buy;
    r.price = px(price);
    r.qty = qt("0.01");
    return r;
  }
  // Acknowledges every new order still pending.
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
  void control(ControlCommand cmd,
               VenueId venue = VenueId{0},
               KillReason reason = KillReason::None) {
    ControlMsg c{};
    init_header(c, EventType::Control, InstrumentId::invalid(), venue);
    c.command = cmd;
    c.arg = static_cast<std::uint64_t>(reason);
    push(c);
    drain();
  }
  [[nodiscard]] std::uint64_t risk_rejects(RejectReason r) const {
    return engine->stats().risk_rejects_by_reason.by_reason[static_cast<std::size_t>(r)];
  }
};

DesiredQuotes quotes(const char* bid, const char* ask) {
  DesiredQuotes q;
  q.bid(px(bid), qt("0.01"));
  q.ask(px(ask), qt("0.01"));
  return q;
}

}  // namespace

TEST_CASE("core.engine: a venue kill pulls that venue's quotes and refuses its new orders only") {
  TwoVenues f;
  REQUIRE(f.engine->set_quotes(kBtc, quotes("99.90", "100.10")));
  REQUIRE(f.engine->set_quotes(kEth, quotes("99.90", "100.10")));
  REQUIRE(f.engine->context().send(TwoVenues::buy(kEth, "99.50")));
  f.ack_all();
  REQUIRE(f.transport.count(EventType::OutNewOrder, kVenue1) == 3);
  REQUIRE(f.transport.count(EventType::OutNewOrder, kVenue0) == 2);

  f.control(ControlCommand::TripVenueKill, kVenue1, KillReason::VenueFatal);
  CHECK(f.engine->risk().venue_killed(kVenue1));
  CHECK_FALSE(f.engine->risk().venue_killed(kVenue0));
  CHECK_FALSE(f.engine->risk().killed());
  CHECK(f.engine->quoting_enabled());
  CHECK(f.engine->context().venue_killed(kVenue1));
  CHECK(f.engine->stats().venue_kills == 1);
  CHECK(f.engine->stats().kills == 0);
  CHECK(f.engine->venue_kill_reason(kVenue1) == KillReason::VenueFatal);
  CHECK(f.engine->venue_kill_reason(kVenue0) == KillReason::None);
  CHECK(f.engine->kill_reason() == KillReason::None);
  // Both quotes and the direct order on venue 1 are cancelled; venue 0 is untouched.
  CHECK(f.transport.count(EventType::OutCancel, kVenue1) == 3);
  CHECK(f.transport.count(EventType::OutCancel, kVenue0) == 0);

  // New orders: refused by risk with the reason on venue 1, accepted on venue 0.
  CHECK(f.engine->context().send(TwoVenues::buy(kEth, "99.60")).error() ==
        RejectReason::VenueKilled);
  CHECK(f.risk_rejects(RejectReason::VenueKilled) == 1);
  CHECK(f.engine->context().send(TwoVenues::buy(kBtc, "99.60")));
  CHECK(f.transport.count(EventType::OutNewOrder, kVenue0) == 3);

  // Quotes: ignored for venue 1 (no order, no reject), still placed on venue 0.
  const std::size_t out_before = f.transport.out.size();
  CHECK_FALSE(f.engine->set_quotes(kEth, quotes("99.80", "100.20")));
  CHECK(f.transport.out.size() == out_before);
  CHECK(f.risk_rejects(RejectReason::VenueKilled) == 1);
  CHECK(f.engine->set_quotes(kBtc, quotes("99.80", "100.20")));
  CHECK(f.transport.out.size() > out_before);
  CHECK(f.transport.count(EventType::OutNewOrder, kVenue1) == 3);

  // Published at once, not with the next latency publication.
  const EngineLiveStats live = f.engine->live_stats();
  CHECK(live.kill_flags == RiskEngine::venue_bit(kVenue1));
  CHECK(live.venue_kills == 1);
  CHECK(live.venue_kill_reasons[1] == KillReason::VenueFatal);
  CHECK(live.venue_kill_reasons[0] == KillReason::None);
  CHECK(live.kill_reason == KillReason::None);

  // A second report for the same venue changes nothing.
  f.control(ControlCommand::TripVenueKill, kVenue1, KillReason::VenueHardStop);
  CHECK(f.engine->stats().venue_kills == 1);
  CHECK(f.engine->venue_kill_reason(kVenue1) == KillReason::VenueFatal);

  // The venue recovered: a ResetKill naming it clears that venue's bit and nothing else.
  f.control(ControlCommand::ResetKill, kVenue1);
  CHECK_FALSE(f.engine->risk().venue_killed(kVenue1));
  CHECK(f.engine->venue_kill_reason(kVenue1) == KillReason::None);
  CHECK(f.engine->live_stats().kill_flags == 0);
}

TEST_CASE("core.engine: killing every venue with instruments trips the global kill switch") {
  TwoVenues f;
  REQUIRE(f.engine->set_quotes(kBtc, quotes("99.90", "100.10")));
  f.ack_all();
  f.control(ControlCommand::TripVenueKill, kVenue0, KillReason::VenueHardStop);
  CHECK_FALSE(f.engine->risk().killed());
  CHECK(f.transport.count(EventType::OutCancel, kVenue0) == 2);
  // A venue without instruments does not count.
  f.control(ControlCommand::TripVenueKill, VenueId{5}, KillReason::VenueFatal);
  CHECK_FALSE(f.engine->risk().killed());
  f.control(ControlCommand::TripVenueKill, kVenue1, KillReason::VenueFatal);
  CHECK(f.engine->risk().killed());
  CHECK_FALSE(f.engine->quoting_enabled());
  CHECK(f.engine->kill_reason() == KillReason::AllVenuesKilled);
  CHECK(f.engine->stats().kills == 1);
  CHECK(f.engine->stats().venue_kills == 3);
  CHECK(f.engine->context().send(TwoVenues::buy(kBtc, "99.60")).error() ==
        RejectReason::KillSwitch);
  const EngineLiveStats live = f.engine->live_stats();
  CHECK((live.kill_flags & 1U) != 0);
  CHECK(live.kill_reason == KillReason::AllVenuesKilled);
  CHECK(live.venue_kill_reasons[0] == KillReason::VenueHardStop);
  CHECK(live.venue_kill_reasons[1] == KillReason::VenueFatal);
}

TEST_CASE("core.engine: the first global kill reason is kept and published, a reset clears it") {
  TwoVenues f("0.5");
  const auto id = f.engine->context().send(TwoVenues::buy(kBtc, "100.01"));
  REQUIRE(id);
  f.ack_all();
  // A fill with a commission above max_loss: net PnL -1 trips the switch.
  OrderFillMsg fill{};
  init_header(fill, EventType::OrderFill, kBtc, kVenue0);
  fill.cl_ord_id = *id;
  fill.side = Side::Buy;
  fill.price = px("100.01");
  fill.qty = qt("0.01");
  fill.cum_qty = qt("0.01");
  fill.exec_id = "E1";
  fill.fee = Notional::from_decimal("1").value();
  f.push(fill);
  f.drain();
  REQUIRE(f.engine->risk().killed());
  CHECK(f.engine->kill_reason() == KillReason::MaxLoss);
  CHECK(f.engine->live_stats().kill_reason == KillReason::MaxLoss);
  CHECK(f.engine->stats().kills == 1);

  // The shutdown's requested kill afterwards does not hide why trading stopped.
  f.control(ControlCommand::TripKill);
  CHECK(f.engine->kill_reason() == KillReason::MaxLoss);
  CHECK(f.engine->live_stats().kill_reason == KillReason::MaxLoss);
  CHECK(f.engine->stats().kills == 2);

  // A ResetKill with a venue clears that venue's bit only; without one, every bit.
  f.control(ControlCommand::ResetKill, kVenue0);
  CHECK(f.engine->risk().killed());
  f.control(ControlCommand::ResetKill, VenueId::invalid());
  CHECK_FALSE(f.engine->risk().killed());
  CHECK(f.engine->kill_reason() == KillReason::None);
  const EngineLiveStats live = f.engine->live_stats();
  CHECK(live.kill_flags == 0);
  CHECK(live.kill_reason == KillReason::None);

  // A requested kill on a fresh switch is recorded as requested.
  f.control(ControlCommand::TripKill);
  CHECK(f.engine->kill_reason() == KillReason::Requested);
  CHECK(to_string(KillReason::AllVenuesKilled) == "AllVenuesKilled");
  CHECK(to_string(ControlCommand::TripVenueKill) == "TripVenueKill");
}
