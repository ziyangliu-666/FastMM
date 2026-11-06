// [capacity] in the engine: a quote level or a direct order worth less than the price of its order
// slot is priced out, a move towards the touch buys only its gain, a move away cancels for free,
// observe mode refuses nothing, and a priced-out level comes back when the window rolls over or a
// fill gives a slot back.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
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

const InstrumentId kBtc{0};
const VenueId kVenue{0};
constexpr std::int64_t kSec = 1'000'000'000;

struct Mute {
  static std::string_view name() noexcept { return "mute"; }
};

struct Outbox {
  std::vector<std::vector<std::byte>> out;
  bool replace = true;
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
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto& b : out) n += reinterpret_cast<const EventHeader*>(b.data())->type == t;
    return n;
  }
  template <class M>
  [[nodiscard]] M last(EventType t) const {
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
      if (reinterpret_cast<const EventHeader*>(it->data())->type != t) continue;
      M m;
      std::memcpy(&m, it->data(), sizeof m);
      return m;
    }
    FAIL("no such message");
    return M{};
  }
};
static_assert(TransportLike<Outbox>);

struct Fixture {
  using EngineType = Engine<Mute, SimClock, Outbox, InlineFeed>;
  InstrumentTable table;
  // A minute boundary plus 1 s: the 10 s windows and the minute tick are easy to cross.
  SimClock clock{Timestamp{(1'000'020 * 60 + 1) * kSec}};
  Outbox transport;
  InlineFeed feed{1 << 20};
  Mute strategy;
  MsgRing records{1U << 20};
  std::unique_ptr<EngineType> engine;
  std::uint64_t execs = 0;

  explicit Fixture(CapacityMode mode = CapacityMode::Enforce, bool replace = true) {
    transport.replace = replace;
    Instrument i{};
    i.symbol = "BTCUSDT";
    i.venue = kVenue;
    i.flags = Instrument::kEnabled;
    i.tick = px("0.01");
    i.lot = qt("0.001");
    i.min_qty = qt("0.001");
    i.min_notional = Notional::from_decimal("5").value();
    REQUIRE(table.add(i));
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.risk.max_order_notional = Notional::from_decimal("1200").value();
    cfg.risk.max_open_orders = 100;
    cfg.quotes.min_requote_interval = Duration{};
    cfg.quotes.supports_replace = replace;
    cfg.capacity.mode = mode;
    cfg.capacity.orders_10s = 10;
    cfg.capacity.fill_credit = true;
    engine = std::make_unique<EngineType>(
        cfg, table, clock, transport, feed, strategy, nullptr, &records);
    engine->warm_up();
    engine->start();
  }
  auto& ctx() { return engine->context(); }
  // L: what a minimum-notional order is worth at the session's yield.
  [[nodiscard]] float floor() { return static_cast<float>(ctx().capacity(kVenue).floor); }
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    while (engine->step() != 0) {
    }
  }
  // Any event: the engine looks at its clocks first.
  void touch() {
    TradeMsg t{};
    init_header(t, EventType::Trade, kBtc, kVenue);
    t.price = px("100");
    t.qty = qt("0.1");
    push(t);
  }
  void ack(ClientOrderId id) {
    OrderAckMsg a{};
    init_header(a, EventType::OrderAck, kBtc, kVenue);
    a.cl_ord_id = id;
    a.venue_order_id = "V1";
    push(a);
  }
  void cancel_ack(ClientOrderId id) {
    OrderCancelAckMsg a{};
    init_header(a, EventType::OrderCancelAck, kBtc, kVenue);
    a.cl_ord_id = id;
    push(a);
  }
  void fill(ClientOrderId id, const char* price, const char* q) {
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill, kBtc, kVenue);
    f.cl_ord_id = id;
    f.side = Side::Buy;
    f.price = px(price);
    f.qty = qt(q);
    f.cum_qty = qt(q);
    f.exec_id.assign(std::to_string(++execs));
    f.liquidity = Liquidity::Maker;
    push(f);
  }
  // One bid of 0.1 (10 USD) at `price`, worth `value`, with `keep` for the resting order.
  bool bid(const char* price, float value, float keep = kNoValue) {
    DesiredQuotes q;
    REQUIRE(q.bid(px(price), qt("0.1"), value, keep));
    return ctx().set_quotes(kBtc, q);
  }
  [[nodiscard]] const Order* resting() { return ctx().working_quote(kBtc, Side::Buy); }
  // `n` direct orders that state no value: not priced, but they use the window.
  std::vector<ClientOrderId> fill_window(int n) {
    std::vector<ClientOrderId> ids;
    for (int k = 0; k < n; ++k) {
      const auto r = ctx().send(NewOrderRequest::limit(kBtc, Side::Buy, px("90"), qt("0.1")));
      REQUIRE(r.has_value());
      ids.push_back(*r);
    }
    return ids;
  }
  // Moves the clock to the start of the next 10 s window.
  void next_window() {
    const std::int64_t w = 10 * kSec;
    clock.set(Timestamp{((clock.now().ns / w) + 1) * w});
  }
  [[nodiscard]] std::vector<RejectRecord> rejects() {
    std::vector<RejectRecord> out;
    while (const std::byte* p = records.try_peek()) {
      const auto* h = reinterpret_cast<const RecordHeader*>(p);
      if (h->type == RecordType::Reject) {
        RejectRecord r;
        std::memcpy(&r, p, sizeof r);
        out.push_back(r);
      } else if (h->type == RecordType::Capacity) {
        CapacityRecord r;
        std::memcpy(&r, p, sizeof r);
        minutes.push_back(r);
      }
      records.release();
    }
    return out;
  }
  std::vector<CapacityRecord> minutes;
};

}  // namespace

TEST_CASE("core.engine capacity: a level worth less than its slot is priced out until it rolls") {
  Fixture f;
  const float L = f.floor();
  REQUIRE(L > 0.0F);
  CHECK(f.ctx().capacity(kVenue).on);
  CHECK(f.ctx().capacity(kVenue).price == doctest::Approx(static_cast<double>(L)));
  // Eight orders in the window: the price of the next is L * h(0.8), far above 3 L.
  static_cast<void>(f.fill_window(8));
  CHECK(f.ctx().capacity(kVenue).window_used == 8);
  CHECK(f.ctx().capacity(kVenue).price > 50.0 * static_cast<double>(L));
  const std::size_t news = f.transport.count(EventType::OutNewOrder);
  CHECK(f.bid("99.50", 3.0F * L));
  CHECK(f.transport.count(EventType::OutNewOrder) == news);
  CHECK(f.resting() == nullptr);
  CHECK(f.engine->quote_manager().stats().priced_out == 1);
  CHECK(f.engine->stats().priced_out == 1);
  CHECK(f.engine->runner_stats().capacity_priced_out == 1);
  const std::vector<RejectRecord> r = f.rejects();
  REQUIRE(r.size() == 1);
  CHECK(r[0].reason == RejectReason::PricedOut);
  CHECK(r[0].source == RejectSource::Capacity);
  // Within the window it stays priced out.
  f.touch();
  CHECK(f.resting() == nullptr);
  // The window rolls over: the level is applied again at the floor price and goes.
  f.next_window();
  f.touch();
  REQUIRE(f.resting() != nullptr);
  CHECK(f.resting()->price == px("99.50"));
  CHECK(f.transport.count(EventType::OutNewOrder) == news + 1);
  // A level worth less than a minimum-notional order never goes.
  f.engine->context().pull_quotes(kBtc);
  f.next_window();
  CHECK(f.bid("99.40", 0.5F * L));
  f.touch();
  CHECK(f.engine->quote_manager().stats().priced_out >= 2);
}

TEST_CASE("core.engine capacity: a replace towards the touch buys its gain") {
  Fixture f;
  const float L = f.floor();
  REQUIRE(f.bid("100.00", 3.0F * L));
  REQUIRE(f.resting() != nullptr);
  f.ack(f.resting()->cl_ord_id);
  const std::size_t replaces = f.transport.count(EventType::OutReplace);
  // 1 bps up: the gain is a tenth of 3 L (full_move_bps 10), below the floor. It stays.
  CHECK(f.bid("100.01", 3.0F * L));
  CHECK(f.transport.count(EventType::OutReplace) == replaces);
  CHECK(f.resting()->price == px("100.00"));
  CHECK(f.engine->quote_manager().stats().priced_out_moves == 1);
  // A keep value of 0 says the resting order is worth nothing where it is: the move is.
  CHECK(f.bid("100.01", 3.0F * L, 0.0F));
  CHECK(f.transport.count(EventType::OutReplace) == replaces + 1);
}

TEST_CASE("core.engine capacity: a replace away from the touch below its price cancels free") {
  Fixture f;
  const float L = f.floor();
  REQUIRE(f.bid("100.00", 3.0F * L));
  REQUIRE(f.resting() != nullptr);
  f.ack(f.resting()->cl_ord_id);
  // Away from the touch, worth less than its slot: the resting order is cancelled (free) and
  // the level stays empty.
  const ClientOrderId was = f.resting()->cl_ord_id;
  const std::size_t cancels = f.transport.count(EventType::OutCancel);
  CHECK(f.bid("99.00", 0.5F * L));
  CHECK(f.transport.count(EventType::OutCancel) == cancels + 1);
  const std::size_t news = f.transport.count(EventType::OutNewOrder);
  f.cancel_ack(was);
  CHECK(f.transport.count(EventType::OutNewOrder) == news);
  CHECK(f.resting() == nullptr);
}

TEST_CASE("core.engine capacity: without replace the move's cancel is what is bought") {
  Fixture f(CapacityMode::Enforce, /*replace=*/false);
  const float L = f.floor();
  REQUIRE(f.bid("100.00", 3.0F * L));
  f.ack(f.resting()->cl_ord_id);
  const std::size_t cancels = f.transport.count(EventType::OutCancel);
  // Towards the touch, gain below the floor: no cancel, the order rests.
  CHECK(f.bid("100.01", 3.0F * L));
  CHECK(f.transport.count(EventType::OutCancel) == cancels);
  CHECK(f.resting()->price == px("100.00"));
  // Bought: the cancel goes and its New is not priced again, even in a window that is full by
  // then.
  CHECK(f.bid("100.11", 3.0F * L));
  CHECK(f.transport.count(EventType::OutCancel) == cancels + 1);
  const ClientOrderId was = f.ctx().working_quote(kBtc, Side::Buy)->cl_ord_id;
  static_cast<void>(f.fill_window(9));
  const std::size_t news = f.transport.count(EventType::OutNewOrder);
  f.cancel_ack(was);
  CHECK(f.transport.count(EventType::OutNewOrder) == news + 1);
  REQUIRE(f.resting() != nullptr);
  CHECK(f.resting()->price == px("100.11"));
}

TEST_CASE("core.engine capacity: observe prices and counts every action and refuses none") {
  Fixture f(CapacityMode::Observe);
  const float L = f.floor();
  const std::size_t news = f.transport.count(EventType::OutNewOrder);
  CHECK(f.bid("99.50", 0.5F * L));
  CHECK(f.transport.count(EventType::OutNewOrder) == news + 1);
  CHECK(f.engine->quote_manager().stats().priced_out == 0);
  CHECK(f.engine->runner_stats().capacity_observed == 1);
  CHECK(f.engine->runner_stats().capacity_priced_out == 0);
  CHECK(f.rejects().empty());
}

TEST_CASE("core.engine capacity: mode new refuses new orders and observes moves") {
  Fixture f(CapacityMode::New);
  const float L = f.floor();
  CHECK(f.bid("99.50", 0.5F * L));
  CHECK(f.resting() == nullptr);
  CHECK(f.engine->runner_stats().capacity_priced_out == 1);
  f.engine->context().pull_quotes(kBtc);
  REQUIRE(f.bid("100.00", 3.0F * L));
  f.ack(f.resting()->cl_ord_id);
  const std::size_t replaces = f.transport.count(EventType::OutReplace);
  CHECK(f.bid("100.01", 3.0F * L));
  CHECK(f.transport.count(EventType::OutReplace) == replaces + 1);
  CHECK(f.engine->runner_stats().capacity_observed == 1);
}

TEST_CASE("core.engine capacity: a direct order is priced when it states a value") {
  Fixture f;
  const float L = f.floor();
  auto order = NewOrderRequest::limit(kBtc, Side::Buy, px("99"), qt("0.1"));
  CHECK(f.ctx().send(order.value(0.5F * L)).error() == RejectReason::PricedOut);
  CHECK(f.ctx().send(order.value(2.0F * L)).has_value());
  CHECK(f.ctx().send(order).has_value());
  CHECK(f.engine->stats().priced_out == 1);
}

TEST_CASE("core.engine capacity: a first fill gives a slot back and re-checks priced-out levels") {
  Fixture f;
  const float L = f.floor();
  const std::vector<ClientOrderId> ids = f.fill_window(9);
  // At 9 of 10 the price is about 125 L; at 8 of 10 about 66 L.
  CHECK(f.bid("99.50", 100.0F * L));
  CHECK(f.resting() == nullptr);
  f.ack(ids[0]);
  f.fill(ids[0], "90", "0.1");
  CHECK(f.ctx().capacity(kVenue).window_used == 8);
  f.touch();
  REQUIRE(f.resting() != nullptr);
  CHECK(f.resting()->price == px("99.50"));
}

TEST_CASE("core.engine capacity: each active account's minute is recorded") {
  Fixture f;
  const float L = f.floor();
  f.touch();  // the first tick arms the minute clock
  CHECK(f.bid("99.50", 3.0F * L));
  const auto order = NewOrderRequest::limit(kBtc, Side::Buy, px("99"), qt("0.1"));
  CHECK(f.ctx().send(order.value(0.5F * L)).error() == RejectReason::PricedOut);
  f.clock.advance(seconds(60));
  f.touch();
  static_cast<void>(f.rejects());
  REQUIRE(f.minutes.size() == 1);
  const CapacityRecord& m = f.minutes[0];
  CHECK(m.hdr.venue == kVenue);
  CHECK(m.mode == CapacityMode::Enforce);
  CHECK(m.decided == 2);
  CHECK(m.sent == 1);
  CHECK(m.priced_out_new == 1);
  CHECK(m.floor > 0.0);
  CHECK(m.price_min == doctest::Approx(static_cast<double>(L)));
}
