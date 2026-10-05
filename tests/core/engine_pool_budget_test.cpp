// A pool's automatic routing against the budgets its connectors publish (ctx.order_budget): the
// orders sent since a publication count against the account they went to, so the orders of one
// burst spread over the pool instead of all reading the room the first one read; and accounts with
// the same room take turns.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <array>
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
const VenueId kPrimary{0};
const VenueId kMember1{1};
const VenueId kMember2{2};

struct Mute {
  static std::string_view name() noexcept { return "mute"; }
};

// Each account's connector publishes its own budget, as LiveTransport reads them.
struct PoolOutbox {
  std::vector<std::vector<std::byte>> out;
  std::array<Seqlocked<OrderBudget>, 3> budget;
  bool send(const EventHeader& m) noexcept {
    const auto* b = reinterpret_cast<const std::byte*>(&m);
    out.emplace_back(b, b + m.len);
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  bool supports_replace(VenueId) const noexcept { return true; }
  bool venue_budget(VenueId v, OrderBudget& out_budget) const noexcept {
    if (v.value >= budget.size()) return false;
    OrderBudget b;
    if (!budget[v.value].try_load(b) || !b.venue_known) return false;
    out_budget = b;
    return true;
  }
  // The connector of `v` publishes `used` of `limit` in its 10 s window, having taken `taken`.
  void publish(VenueId v, std::int64_t used, std::int64_t limit, std::uint64_t taken) {
    OrderBudget b;
    b.venue_known = true;
    b.orders_10s = RateWindow{10'000, used, limit};
    b.orders_taken = taken;
    budget[v.value].store(b);
  }
  // The account each OutNewOrder went from, in order.
  [[nodiscard]] std::vector<VenueId> new_order_accounts() const {
    std::vector<VenueId> v;
    for (const auto& b : out) {
      const auto* h = reinterpret_cast<const EventHeader*>(b.data());
      if (h->type == EventType::OutNewOrder) v.push_back(h->venue);
    }
    return v;
  }
};
static_assert(TransportLike<PoolOutbox>);

struct Fixture {
  using EngineType = Engine<Mute, SimClock, PoolOutbox, InlineFeed>;
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(1000).ns}};
  PoolOutbox transport;
  InlineFeed feed{1 << 20};
  Mute strategy;
  std::unique_ptr<EngineType> engine;

  Fixture() {
    Instrument i{};
    i.symbol = "BTCUSDT";
    i.venue = kPrimary;
    i.flags = Instrument::kEnabled;
    i.tick = px("0.01");
    i.lot = qt("0.001");
    i.min_qty = qt("0.001");
    REQUIRE(table.add(i));
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.risk.max_open_orders = 100;
    REQUIRE(cfg.pools.add(kMember1, kPrimary));
    REQUIRE(cfg.pools.add(kMember2, kPrimary));
    engine = std::make_unique<EngineType>(cfg, table, clock, transport, feed, strategy, nullptr);
    engine->warm_up();
    engine->start();
  }
  [[nodiscard]] static LimitOrder buy(const char* price = "99.50") {
    return NewOrderRequest::limit(kBtc, Side::Buy, px(price), qt("0.01"));
  }
};

}  // namespace

TEST_CASE("core.engine pool: a burst spreads over the accounts before any publication") {
  Fixture f;
  auto& ctx = f.engine->context();
  for (const VenueId v : {kPrimary, kMember1, kMember2}) f.transport.publish(v, 10, 100, 0);
  // Six orders in one go: the publications do not move, the orders sent since do.
  for (int k = 0; k < 6; ++k) REQUIRE(ctx.send(Fixture::buy()));
  const std::vector<VenueId> to = f.transport.new_order_accounts();
  REQUIRE(to.size() == 6);
  CHECK(to == std::vector<VenueId>{kPrimary, kMember1, kMember2, kPrimary, kMember1, kMember2});
  // What ctx.order_budget reads for each: the publication plus the two orders it has not seen.
  for (const VenueId v : {kPrimary, kMember1, kMember2}) {
    CHECK(ctx.order_budget(v).orders_10s.used == 12);
    CHECK(ctx.order_budget(v).orders_10s.remaining() == 88);
  }
  // The connectors catch up: a publication that has taken the two orders counts them itself.
  for (const VenueId v : {kPrimary, kMember1, kMember2}) f.transport.publish(v, 12, 100, 2);
  CHECK(ctx.order_budget(kMember1).orders_10s.used == 12);
  // Still tied: the turn goes on from where it was, the primary next.
  REQUIRE(ctx.send(Fixture::buy()));
  CHECK(f.transport.new_order_accounts().back() == kPrimary);
}

TEST_CASE("core.engine pool: the account with more room takes orders until the others match it") {
  Fixture f;
  auto& ctx = f.engine->context();
  f.transport.publish(kPrimary, 50, 100, 0);
  f.transport.publish(kMember1, 46, 100, 0);
  f.transport.publish(kMember2, 50, 100, 0);
  for (int k = 0; k < 7; ++k) REQUIRE(ctx.send(Fixture::buy()));
  const std::vector<VenueId> to = f.transport.new_order_accounts();
  REQUIRE(to.size() == 7);
  // Member 1 has four more: it takes four, then the three tie and take turns.
  for (std::size_t k = 0; k < 4; ++k) CHECK(to[k] == kMember1);
  CHECK(to[4] == kMember2);
  CHECK(to[5] == kPrimary);
  CHECK(to[6] == kMember1);
}

TEST_CASE("core.engine pool: orders the publication has not seen fill an account's window") {
  Fixture f;
  auto& ctx = f.engine->context();
  f.transport.publish(kPrimary, 0, 2, 0);
  f.transport.publish(kMember1, 0, 2, 0);
  f.transport.publish(kMember2, 0, 2, 0);
  // Two named orders fill the primary's window; the third is refused here, not sent.
  REQUIRE(ctx.send(Fixture::buy().account(kPrimary)));
  REQUIRE(ctx.send(Fixture::buy().account(kPrimary)));
  CHECK(ctx.send(Fixture::buy().account(kPrimary)).error() == RejectReason::RateLimit);
  // The automatic ones pass it over: two on each member, then nothing has room.
  for (int k = 0; k < 4; ++k) REQUIRE(ctx.send(Fixture::buy()));
  const std::vector<VenueId> to = f.transport.new_order_accounts();
  REQUIRE(to.size() == 6);
  CHECK(to[2] == kMember1);
  CHECK(to[3] == kMember2);
  CHECK(to[4] == kMember1);
  CHECK(to[5] == kMember2);
  CHECK(ctx.send(Fixture::buy()).error() == RejectReason::RateLimit);
  CHECK(f.engine->stats().risk_rejects_by_reason[RejectReason::RateLimit] == 2);
}

TEST_CASE("core.engine pool: a replace counts against its order's account") {
  Fixture f;
  auto& ctx = f.engine->context();
  for (const VenueId v : {kPrimary, kMember1, kMember2}) f.transport.publish(v, 0, 100, 0);
  const auto id = ctx.send(Fixture::buy().account(kMember2));
  REQUIRE(id);
  OrderAckMsg a{};
  init_header(a, EventType::OrderAck, kBtc, kMember2);
  a.hdr.recv_ts = f.clock.now();
  a.cl_ord_id = *id;
  a.venue_order_id = "V1";
  REQUIRE(f.feed.push(a.hdr));
  while (f.engine->step() != 0) {
  }
  REQUIRE(ctx.replace(*id, px("99.40"), qt("0.01")));
  CHECK(ctx.order_budget(kMember2).orders_10s.used == 2);
  CHECK(ctx.order_budget(kMember1).orders_10s.used == 0);
}

TEST_CASE("core.engine pool: without publications the first account in pool order takes them") {
  Fixture f;
  auto& ctx = f.engine->context();
  for (int k = 0; k < 3; ++k) REQUIRE(ctx.send(Fixture::buy()));
  for (const VenueId v : f.transport.new_order_accounts()) CHECK(v == kPrimary);
}
