// A venue's mark, index and funding (PerpStateMsg, core/perp_book.hpp): the table the engine keeps
// per instrument, its staleness, the strategy's view (ctx.mark, ctx.index, ctx.funding,
// on_perp_state) and the valuation of a position at the venue's mark ([accounting] mark).
#include "fastmm/core/engine.hpp"
#include "fastmm/core/perp_book.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value_or(Price{});
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value_or(Qty{});
}
Notional nt(const char* s) {
  return Notional::from_decimal(s).value_or(Notional{});
}

constexpr InstrumentId kSpot{0};  // BTCUSDT spot, venue 0
constexpr InstrumentId kPerp{1};  // BTCUSDT linear perpetual, venue 1
constexpr std::int64_t kHourNs = 3'600'000'000'000;

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument s{};
  s.symbol = "BTCUSDT";
  s.base = "BTC";
  s.quote = "USDT";
  s.venue = VenueId{0};
  s.asset_class = AssetClass::Spot;
  s.flags = Instrument::kEnabled;
  s.tick = px("0.1");
  s.lot = qt("0.001");
  s.min_qty = qt("0.001");
  REQUIRE(t.add(s));
  Instrument p = s;
  p.venue = VenueId{1};
  p.asset_class = AssetClass::Perpetual;
  REQUIRE(t.add(p));
  return t;
}

PerpStateMsg perp_state(InstrumentId id, std::uint8_t fields) {
  PerpStateMsg m{};
  init_header(m, EventType::PerpState, id, VenueId{1});
  m.fields = fields;
  return m;
}
PerpStateMsg mark_msg(InstrumentId id, const char* mark) {
  PerpStateMsg m = perp_state(id, PerpStateMsg::kMark);
  m.mark_price = px(mark);
  return m;
}

struct Transport {
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
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return true; }
};

// Records what on_perp_state saw and what the context said at that moment.
struct Watcher {
  std::vector<PerpStateMsg> seen;
  RefPrice mark_at_hook;
  template <class Ctx>
  void on_perp_state(Ctx& ctx, InstrumentId id, const PerpStateMsg& m) {
    seen.push_back(m);
    mark_at_hook = ctx.mark(id);
  }
};

EngineConfig make_config() {
  EngineConfig cfg;
  cfg.perp.stale_mark = seconds(5);
  cfg.perp.stale_funding = seconds(60);
  return cfg;
}

struct Rig {
  using E = Engine<Watcher, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Watcher strategy;
  std::unique_ptr<E> engine;

  explicit Rig(const EngineConfig& cfg = make_config()) {
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy);
    engine->warm_up();
    engine->start();
    book(kSpot, "49990", "50010");
    book(kPerp, "49990", "50010");
  }
  void drain() {
    while (engine->step() > 0) {
    }
  }
  void advance(Duration d) { clock.set(clock.now() + d); }
  template <class M>
  void push(M m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    drain();
  }
  void book(InstrumentId id, const char* bid, const char* ask) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, id, table.get(id).venue, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  // A long of `qty` on `id` at `price`: an order and its fill.
  void buy(InstrumentId id, const char* price, const char* qty) {
    const auto r = engine->send_order(NewOrderRequest::limit(id, Side::Buy, px(price), qt(qty)));
    REQUIRE(r);
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill, id, table.get(id).venue);
    f.cl_ord_id = *r;
    f.side = Side::Buy;
    f.price = px(price);
    f.qty = qt(qty);
    f.cum_qty = qt(qty);
    f.exec_id.assign("e" + std::to_string(id.value));
    push(f);
  }
  [[nodiscard]] Notional unrealized(InstrumentId id) const {
    return engine->position(id).unrealized;
  }
};

}  // namespace

TEST_CASE("core.perp_state: each field is kept with the time it arrived") {
  Rig r;
  auto& ctx = r.engine->context();
  CHECK_FALSE(ctx.mark(kPerp).usable());
  CHECK(ctx.mark(kPerp).stale);
  CHECK_FALSE(ctx.funding(kPerp).usable());

  PerpStateMsg m = mark_msg(kPerp, "50100");
  m.hdr.exch_ts = Timestamp{1'700'000'000'000'000'000};
  r.push(m);
  const Timestamp t_mark = r.clock.now();
  CHECK(ctx.mark(kPerp).usable());
  CHECK(ctx.mark(kPerp).price == px("50100"));
  CHECK(ctx.mark(kPerp).at == t_mark);
  CHECK_FALSE(ctx.index(kPerp).usable());  // a message without the field leaves it unknown
  CHECK(ctx.perp_state(kPerp).venue_ts == m.hdr.exch_ts);

  r.advance(seconds(2));
  PerpStateMsg f = perp_state(kPerp, PerpStateMsg::kFunding | PerpStateMsg::kIndex);
  f.index_price = px("50050");
  f.funding_rate = 0.0001;
  f.funding_interval = Duration{8 * kHourNs};
  f.next_funding = Timestamp{1'700'000'000'000'000'000 + 8 * kHourNs};
  r.push(f);
  CHECK(ctx.mark(kPerp).price == px("50100"));  // kept from the first message
  CHECK(ctx.mark(kPerp).at == t_mark);
  CHECK(ctx.index(kPerp).price == px("50050"));
  const FundingView fv = ctx.funding(kPerp);
  CHECK(fv.usable());
  CHECK(fv.rate == doctest::Approx(0.0001));
  CHECK(fv.interval.ns == 8 * kHourNs);
  CHECK(fv.next == f.next_funding);
  CHECK(fv.over(Duration{kHourNs}) == doctest::Approx(0.0001 / 8));

  // Past stale_mark (5 s) from the mark's arrival: the mark and index are flagged, the funding
  // (60 s) is not.
  r.advance(seconds(4));
  CHECK(ctx.mark(kPerp).stale);
  CHECK_FALSE(ctx.mark(kPerp).usable());
  CHECK(ctx.mark(kPerp).price == px("50100"));  // the value stays readable
  CHECK_FALSE(ctx.index(kPerp).stale);
  CHECK(ctx.funding(kPerp).usable());
  r.advance(seconds(60));
  CHECK(ctx.funding(kPerp).stale);
  CHECK(r.engine->perps().instruments() == 1);
}

TEST_CASE("core.perp_state: on_perp_state runs after the table has the message") {
  Rig r;
  r.push(mark_msg(kPerp, "50100"));
  REQUIRE(r.strategy.seen.size() == 1);
  CHECK(r.strategy.seen[0].mark_price == px("50100"));
  CHECK(r.strategy.mark_at_hook.usable());
  CHECK(r.strategy.mark_at_hook.price == px("50100"));
  // An instrument outside the table reaches neither the table nor the hook.
  r.push(mark_msg(InstrumentId{7}, "1"));
  CHECK(r.strategy.seen.size() == 1);
  CHECK(r.engine->perps().instruments() == 1);
}

TEST_CASE("core.perp_state: a position is valued at the venue mark while it is fresh") {
  Rig r;
  r.buy(kPerp, "50000", "1");
  CHECK(r.unrealized(kPerp) == nt("0"));  // at the mid, 50000
  r.push(mark_msg(kPerp, "50100"));
  CHECK(r.unrealized(kPerp) == nt("100"));
  CHECK(r.engine->perps().valuation(kPerp) == px("50100"));
  // Book updates do not move it while the mark is fresh.
  r.advance(seconds(1));
  r.book(kPerp, "49890", "49910");
  CHECK(r.unrealized(kPerp) == nt("100"));
  // Stale: the next book update values it at the mid again.
  r.advance(seconds(5));
  r.book(kPerp, "49880", "49900");
  CHECK(r.unrealized(kPerp) == nt("-110"));
  CHECK(r.engine->perps().valuation(kPerp) == Price{});
  CHECK(r.engine->perps().stats().mark_fallbacks == 1);
  // A fresh mark takes over again.
  r.push(mark_msg(kPerp, "49950"));
  CHECK(r.unrealized(kPerp) == nt("-50"));
  // An instrument without a mark (spot) is valued at its mid as before.
  r.buy(kSpot, "50000", "1");
  r.book(kSpot, "50090", "50110");
  CHECK(r.unrealized(kSpot) == nt("100"));
}

TEST_CASE("core.perp_state: max_loss trips on the venue mark") {
  EngineConfig cfg = make_config();
  cfg.risk.max_loss = nt("500");
  Rig r(cfg);
  r.buy(kPerp, "50000", "1");
  REQUIRE_FALSE(r.engine->risk().killed());
  // The book's mid is unchanged; the venue marks the position 600 down.
  r.push(mark_msg(kPerp, "49400"));
  CHECK(r.unrealized(kPerp) == nt("-600"));
  CHECK(r.engine->risk().killed());
  CHECK(r.engine->kill_reason() == KillReason::MaxLoss);
}

TEST_CASE("core.perp_state: mark = mid keeps the book's mid") {
  EngineConfig cfg = make_config();
  cfg.perp.venue_mark = false;
  cfg.risk.max_loss = nt("500");
  Rig r(cfg);
  r.buy(kPerp, "50000", "1");
  r.push(mark_msg(kPerp, "49400"));
  CHECK(r.unrealized(kPerp) == nt("0"));
  CHECK_FALSE(r.engine->risk().killed());
  CHECK(r.engine->context().mark(kPerp).price == px("49400"));  // the table keeps it all the same
  r.book(kPerp, "50090", "50110");
  CHECK(r.unrealized(kPerp) == nt("100"));
}

TEST_CASE("core.perp_state: PerpBook ignores an id out of range and a message without a mark") {
  PerpBook b;
  PerpStateMsg m = mark_msg(InstrumentId{kMaxInstruments + 3}, "1");
  CHECK_FALSE(b.on_report(m, Timestamp{1}));
  CHECK(b.stats().unknown_instrument == 1);
  CHECK(b.instruments() == 0);
  PerpStateMsg f = perp_state(kPerp, PerpStateMsg::kFunding);
  f.funding_rate = -0.0002;
  f.funding_interval = Duration{kHourNs};
  CHECK_FALSE(b.on_report(f, Timestamp{1}));  // no mark: nothing to value the position at
  CHECK_FALSE(b.marking());
  CHECK(b.funding(kPerp, Timestamp{2}).rate == doctest::Approx(-0.0002));
  CHECK(b.funding(kPerp, Timestamp{2}).next == Timestamp{});
  CHECK(b.on_report(mark_msg(kPerp, "2"), Timestamp{3}));
  CHECK(b.marking());
  CHECK_FALSE(b.mid_marks(kPerp, Timestamp{3} + seconds(15)));
  CHECK(b.mid_marks(kPerp, Timestamp{3} + seconds(16)));
}
