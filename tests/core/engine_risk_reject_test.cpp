// on_risk_reject: a new order or replace the engine's own risk check refused is reported to the
// strategy after the hook that asked returns, never from inside its send or set_quotes; and
// ctx.order_budget, the [risk] token bucket beside what the venue's connector published.
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

const InstrumentId kBtc{0};
const VenueId kVenue{0};

// Keeps every reject; from inside the hook it may ask again (`retry`), which is refused again.
struct Rejected {
  static std::string_view name() noexcept { return "rejected"; }
  std::vector<RiskReject> rejects;
  int depth = 0;
  int max_depth = 0;
  int retry = 0;  // orders to send from the hook, each refused again
  bool send_from_timer = false;

  void on_risk_reject(auto& ctx, const RiskReject& r) noexcept {
    ++depth;
    if (depth > max_depth) max_depth = depth;
    rejects.push_back(r);
    if (retry > 0) {
      --retry;
      NewOrderRequest bad{};
      bad.instrument = kBtc;
      bad.side = Side::Sell;
      bad.price = px("150");  // outside the collar
      bad.qty = qt("0.01");
      static_cast<void>(ctx.send(bad));
    }
    --depth;
  }
  void on_timer(auto& ctx, TimerId, std::uint64_t) noexcept {
    if (!send_from_timer) return;
    NewOrderRequest bad{};
    bad.instrument = kBtc;
    bad.side = Side::Buy;
    bad.price = px("50");
    bad.qty = qt("0.01");
    static_cast<void>(ctx.send(bad));
  }
};

// A transport with a budget publication, as LiveTransport reads one from the connector.
struct Outbox {
  std::vector<std::vector<std::byte>> out;
  bool replace = true;
  Seqlocked<OrderBudget> budget;
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
  bool venue_budget(VenueId v, OrderBudget& out_budget) const noexcept {
    if (v != kVenue) return false;
    OrderBudget b;
    if (!budget.try_load(b) || !b.venue_known) return false;
    out_budget = b;
    return true;
  }
  template <class M>
  [[nodiscard]] const M& at(std::size_t i) const {
    return *reinterpret_cast<const M*>(out[i].data());
  }
};
static_assert(TransportLike<Outbox>);

struct Mute {
  static std::string_view name() noexcept { return "mute"; }
};
struct PlainOutbox {
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
};

template <class S, class T>
struct Fixture {
  using EngineType = Engine<S, SimClock, T, InlineFeed>;
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  T transport;
  InlineFeed feed{1 << 20};
  S strategy;
  std::unique_ptr<EngineType> engine;

  explicit Fixture(std::uint32_t orders_per_sec = 0, std::int64_t token_reserve = 0) {
    Instrument i{};
    i.symbol = "BTCUSDT";
    i.venue = kVenue;
    i.flags = Instrument::kEnabled;
    i.tick = px("0.01");
    i.lot = qt("0.001");
    i.min_qty = qt("0.001");
    REQUIRE(table.add(i));
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.risk.price_collar_bps = 500;
    cfg.risk.orders_per_sec = orders_per_sec;
    cfg.quotes.min_requote_interval = Duration{};
    cfg.quotes.token_reserve = token_reserve;
    engine = std::make_unique<EngineType>(cfg, table, clock, transport, feed, strategy, nullptr);
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
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    drain();
  }
  [[nodiscard]] static NewOrderRequest buy(const char* price, const char* qty = "0.01") {
    NewOrderRequest r{};
    r.instrument = kBtc;
    r.side = Side::Buy;
    r.price = px(price);
    r.qty = qt(qty);
    r.user_tag = 77;
    return r;
  }
  [[nodiscard]] std::size_t sent(EventType t) const {
    std::size_t n = 0;
    for (const auto& b : transport.out)
      n += reinterpret_cast<const EventHeader*>(b.data())->type == t ? 1U : 0U;
    return n;
  }
  // A timer event: the hook runs, then the engine delivers what it collected.
  void tick() {
    TimerMsg t{};
    init_header(t, EventType::Timer, InstrumentId::invalid(), kVenue);
    push(t);
  }
};

}  // namespace

TEST_CASE("core.engine: on_risk_reject reports a refused new order after the hook returns") {
  Fixture<Rejected, Outbox> f;
  f.strategy.send_from_timer = true;
  f.tick();
  REQUIRE(f.strategy.rejects.size() == 1);
  const RiskReject& r = f.strategy.rejects[0];
  CHECK(r.reason == RejectReason::PriceCollar);
  CHECK(r.instrument == kBtc);
  CHECK(r.side == Side::Buy);
  CHECK(r.type == OrderType::Limit);
  CHECK(r.price == px("50"));
  CHECK(r.qty == qt("0.01"));
  CHECK_FALSE(r.replace);
  CHECK_FALSE(r.order.valid());
  CHECK(r.time == f.clock.now());
  CHECK(f.strategy.max_depth == 1);  // never from inside send
  CHECK(f.engine->stats().risk_rejects == 1);
  CHECK(f.engine->stats().risk_reject_notices_dropped == 0);
  CHECK(f.transport.out.empty());
}

TEST_CASE("core.engine: on_risk_reject names the order a refused replace would have changed") {
  Fixture<Rejected, Outbox> f;
  auto& ctx = f.engine->context();
  const auto id = ctx.send(Fixture<Rejected, Outbox>::buy("99.50"));
  REQUIRE(id);
  OrderAckMsg a{};
  init_header(a, EventType::OrderAck, kBtc, kVenue);
  a.cl_ord_id = *id;
  a.venue_order_id = "V1";
  f.push(a);
  // Outside the engine the reject waits for the next event.
  CHECK(ctx.replace(*id, px("99.60"), qt("5")).error() == RejectReason::MaxOrderQty);
  CHECK(f.strategy.rejects.empty());
  f.tick();
  REQUIRE(f.strategy.rejects.size() == 1);
  const RiskReject& r = f.strategy.rejects[0];
  CHECK(r.replace);
  CHECK(r.order == *id);
  CHECK(r.reason == RejectReason::MaxOrderQty);
  CHECK(r.price == px("99.60"));
  CHECK(r.qty == qt("5"));
  CHECK(r.user_tag == 77);
}

TEST_CASE("core.engine: an order refused again from on_risk_reject is reported, a few rounds") {
  Fixture<Rejected, Outbox> f;
  f.strategy.send_from_timer = true;
  f.strategy.retry = 10;
  f.tick();
  // The timer's order, then one per round from the hook: bounded, and never nested.
  CHECK(f.strategy.rejects.size() == 4);
  CHECK(f.strategy.max_depth == 1);
  CHECK(f.strategy.rejects[1].side == Side::Sell);
}

TEST_CASE("core.engine: order_budget reads the token bucket and the venue's publication") {
  Fixture<Rejected, Outbox> f(/*orders_per_sec=*/4);
  auto& ctx = f.engine->context();
  OrderBudget b = ctx.order_budget(kVenue);
  CHECK(b.local_tokens == 4);
  CHECK_FALSE(b.venue_known);
  CHECK_FALSE(b.venue_paused);
  CHECK_FALSE(b.orders_10s.known());
  CHECK(b.orders_remaining() == 4);
  REQUIRE(ctx.send(Fixture<Rejected, Outbox>::buy("99.50")));
  REQUIRE(ctx.send(Fixture<Rejected, Outbox>::buy("99.40")));
  CHECK(ctx.order_budget(kVenue).local_tokens == 2);

  OrderBudget v;
  v.venue_known = true;
  v.orders_10s = RateWindow{10'000, 95, 100};
  v.orders_1d = RateWindow{86'400'000, 1000, 200'000};
  v.weight = RateWindow{60'000, 5500, 6000};
  // A publication from before the two orders reached the connector: they count on top of it.
  f.transport.budget.store(v);
  CHECK(ctx.order_budget(kVenue).orders_10s.used == 97);
  CHECK(ctx.order_budget(kVenue).orders_1d.used == 1002);
  CHECK(ctx.order_budget(kVenue).weight.used == 5500);
  v.orders_taken = 2;
  f.transport.budget.store(v);
  b = ctx.order_budget(kVenue);
  CHECK(b.venue_known);
  CHECK(b.local_tokens == 2);
  CHECK(b.orders_10s.remaining() == 5);
  CHECK(b.orders_1m.remaining() == OrderBudget::kUnlimited);
  CHECK(b.weight.used == 5500);
  CHECK(b.orders_remaining() == 2);
  v.orders_10s.used = 100;
  f.transport.budget.store(v);
  CHECK(ctx.order_budget(kVenue).orders_remaining() == 0);
  v.orders_10s.used = 0;
  v.venue_paused = true;
  f.transport.budget.store(v);
  b = ctx.order_budget(kVenue);
  CHECK(b.venue_paused);
  CHECK(b.orders_remaining() == 0);
  // Another venue: no publication.
  CHECK_FALSE(ctx.order_budget(VenueId{1}).venue_known);

  // The bucket refills at its rate.
  f.clock.advance(seconds(1));
  CHECK(ctx.order_budget(kVenue).local_tokens == 4);
}

TEST_CASE("core.engine: a transport without a budget and a strategy without the hook") {
  Fixture<Mute, PlainOutbox> f;
  auto& ctx = f.engine->context();
  const OrderBudget b = ctx.order_budget(kVenue);
  CHECK(b.local_tokens == OrderBudget::kUnlimited);
  CHECK_FALSE(b.venue_known);
  CHECK(b.orders_remaining() == OrderBudget::kUnlimited);
  CHECK(ctx.send(Fixture<Mute, PlainOutbox>::buy("50")).error() == RejectReason::PriceCollar);
  CHECK(f.engine->stats().risk_rejects == 1);
}

namespace {
DesiredQuotes two_levels() {
  DesiredQuotes q;
  q.bid(px("99.90"), qt("0.01"));
  q.bid(px("99.80"), qt("0.01"));
  q.ask(px("100.10"), qt("0.01"));
  q.ask(px("100.20"), qt("0.01"));
  return q;
}
}  // namespace

// A spike requotes every instrument at once: the quotes the bucket has no token for wait and are
// placed as it refills, instead of being refused (and logged) on every event until one gets
// through.
TEST_CASE("core.engine: a quote short of an order token waits for one, it is not a risk reject") {
  Fixture<Rejected, Outbox> f(/*orders_per_sec=*/2);
  auto& ctx = f.engine->context();
  REQUIRE(ctx.set_quotes(kBtc, two_levels()));
  CHECK(f.sent(EventType::OutNewOrder) == 2);
  CHECK(f.engine->quote_manager().stats().kept_rate_limit == 2);
  CHECK(f.engine->quote_manager().stats().rejected == 0);
  CHECK(f.engine->quote_manager().starved());
  CHECK(f.engine->stats().risk_rejects == 0);
  OrderBudget b = ctx.order_budget(kVenue);
  CHECK(b.local_tokens == 0);
  CHECK(b.local_wait_ns == milliseconds(500).ns);
  // A cancel never needs a token.
  const Order* bid = ctx.working_quote(kBtc, Side::Buy, 0);
  REQUIRE(bid != nullptr);
  OrderAckMsg a{};
  init_header(a, EventType::OrderAck, kBtc, kVenue);
  a.cl_ord_id = bid->cl_ord_id;
  a.venue_order_id = "V1";
  f.push(a);
  REQUIRE(ctx.cancel(a.cl_ord_id));
  CHECK(f.sent(EventType::OutCancel) == 1);
  // Half a second later the bucket holds one token: the next event places one waiting quote.
  f.clock.advance(milliseconds(500));
  CHECK(ctx.order_budget(kVenue).local_wait_ns == 0);
  f.tick();
  CHECK(f.sent(EventType::OutNewOrder) == 3);
  CHECK(f.engine->quote_manager().starved());
  f.clock.advance(milliseconds(500));
  f.tick();
  CHECK(f.sent(EventType::OutNewOrder) == 4);
  CHECK_FALSE(f.engine->quote_manager().starved());
  CHECK(f.strategy.rejects.empty());
  CHECK(f.engine->stats().risk_rejects == 0);
}

TEST_CASE("core.engine: quote_token_reserve keeps the last tokens for quotes that reduce") {
  Fixture<Rejected, Outbox> f(/*orders_per_sec=*/2, /*token_reserve=*/1);
  auto& ctx = f.engine->context();
  PositionUpdateMsg pos{};
  init_header(pos, EventType::PositionUpdate, kBtc, kVenue);
  pos.qty = qt("0.05");
  pos.avg_px = px("100");
  f.push(pos);
  REQUIRE(ctx.position(kBtc).qty == qt("0.05"));
  DesiredQuotes q;
  q.bid(px("99.90"), qt("0.01"));
  q.ask(px("100.10"), qt("0.01"));
  REQUIRE(ctx.set_quotes(kBtc, q));
  // Two tokens: the bid takes the one above the reserve, the ask (it sells the long down) the last.
  CHECK(f.sent(EventType::OutNewOrder) == 2);
  q.bid(px("99.80"), qt("0.01"));
  f.clock.advance(milliseconds(500));
  REQUIRE(ctx.set_quotes(kBtc, q));
  // One token, the reserve: the second bid adds to the position and waits.
  CHECK(f.sent(EventType::OutNewOrder) == 2);
  CHECK(f.engine->quote_manager().stats().kept_rate_limit == 1);
  // A direct order may take the reserve.
  CHECK(ctx.send(Fixture<Rejected, Outbox>::buy("99.00")));
  CHECK(f.sent(EventType::OutNewOrder) == 3);
}
