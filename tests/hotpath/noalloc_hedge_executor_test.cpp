// HedgeExecutor must not allocate (5.2): hedges and their ends, failover and back, benches and
// halts, uncertain holds, held hedges, de-risk steps and the status, with its logging on; and in
// maker mode its orders resting, following the touch, re-sizing and crossing.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/strategies/hedge_executor.hpp"

#include <memory>

using namespace fastmm;
using fastmm::test::NoAllocScope;

namespace {

struct Book {
  Price bid{};
  Price ask{};
  Timestamp t{};
  [[nodiscard]] bool is_valid() const { return true; }
  [[nodiscard]] Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  [[nodiscard]] Level best_bid() const { return Level{bid, Qty::from_int(1)}; }
  [[nodiscard]] Level best_ask() const { return Level{ask, Qty::from_int(1)}; }
  [[nodiscard]] Timestamp last_update() const { return t; }
};
struct Pos {
  Qty qty{};
};
struct Ctx {
  const InstrumentTable* table = nullptr;
  Book books[3];
  Qty pos[3] = {};
  InstrumentId open_id{};
  Side open_side{};
  Qty open{};
  ClientOrderId last{};
  Order last_order{};
  bool killed[3] = {};
  bool refuse = false;
  Qty room = Qty::max();
  Timestamp t{1'789'344'931'096LL * 1'000'000};
  std::uint64_t sends = 0;
  [[nodiscard]] const InstrumentTable& instruments() const { return *table; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table->get(id); }
  [[nodiscard]] const Book& book(InstrumentId id) const { return books[id.value]; }
  [[nodiscard]] Pos position(InstrumentId id) const { return Pos{pos[id.value]}; }
  [[nodiscard]] Timestamp now() const { return t; }
  [[nodiscard]] bool reconciling() const { return false; }
  [[nodiscard]] bool venue_killed(VenueId v) const { return killed[v.value]; }
  [[nodiscard]] std::string_view venue_name(VenueId) const { return "sim"; }
  [[nodiscard]] VenueHealthView venue_health(VenueId) const { return VenueHealthView{}; }
  Result<ClientOrderId, RejectReason> send(const NewOrderRequest& r) noexcept {
    if (refuse) return fail(RejectReason::MaxPosition);
    ++sends;
    open_id = r.instrument;
    open_side = r.side;
    open = r.qty;
    last = ClientOrderId{sends};
    last_order = Order{};
    last_order.cl_ord_id = last;
    last_order.instrument = r.instrument;
    last_order.side = r.side;
    last_order.price = r.price;
    last_order.qty = r.qty;
    last_order.tif = r.tif;
    last_order.state = OrderState::Live;
    return last;
  }
  [[nodiscard]] const Order* order(ClientOrderId id) const {
    return id == last && open.is_positive() ? &last_order : nullptr;
  }
  Result<void, RejectReason> cancel(ClientOrderId id) {
    if (id != last || !open.is_positive()) return fail(RejectReason::VenueUnknownOrder);
    last_order.state = OrderState::PendingCancel;
    return {};
  }
  [[nodiscard]] Qty open_qty(InstrumentId id, Side side) const {
    return id == open_id && side == open_side ? open : Qty{};
  }
  [[nodiscard]] Qty balance_room(InstrumentId id, Side, Price) const {
    return id.value == 1 ? room : Qty::max();
  }
};

Instrument linear(const char* sym, std::uint8_t venue) {
  Instrument i{};
  i.symbol = sym;
  i.venue = VenueId{venue};
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_decimal("0.1").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.min_qty = i.lot;
  i.contract_multiplier = Qty::from_int(1);
  return i;
}

// The open order ends having filled `cum` of it.
void end(HedgeExecutor& h, Ctx& c, Qty cum, bool acked) {
  OmsUpdate u;
  u.terminal = true;
  u.changed = true;
  u.order.cl_ord_id = c.last;
  u.order.instrument = c.open_id;
  u.order.side = c.open_side;
  u.order.tif = c.last_order.tif;
  u.order.state = cum.is_zero() ? OrderState::Canceled : OrderState::Filled;
  u.order.cum_qty = cum;
  if (acked) u.order.venue_order_id.assign("1");
  c.pos[c.open_id.value] += c.open_side == Side::Buy ? cum : -cum;
  c.open = Qty{};
  static_cast<void>(h.on_order_update(c, u));
}

}  // namespace

TEST_CASE("hotpath.noalloc: HedgeExecutor hedges, fails over, de-risks and recovers") {
  Logger::instance().set_level(LogLevel::Info);
  Logger::instance().attach_current_thread();
  auto table = std::make_unique<InstrumentTable>();
  REQUIRE(table->add(linear("BTCUSDT", 0)));
  REQUIRE(table->add(linear("BTCUSDT", 1)));
  REQUIRE(table->add(linear("BTCUSDT", 2)));
  Ctx c;
  c.table = table.get();
  auto h = std::make_unique<HedgeExecutor>();
  h->reset();
  REQUIRE(h->add_source(InstrumentId{0}));
  REQUIRE(h->add_hedge(InstrumentId{1}, Ratio::from_raw(5 * kRatioPerBp)));
  REQUIRE(h->add_hedge(InstrumentId{2}, Ratio::from_raw(5 * kRatioPerBp)));
  HedgeExecutor::Config cfg;
  cfg.retry = Duration{};
  cfg.max_failures = 2;
  cfg.bench = milliseconds(5);
  cfg.uncertain_hold = milliseconds(2);
  cfg.derisk_after = milliseconds(3);
  cfg.derisk_step = Qty::from_decimal("0.004").value();
  cfg.derisk_interval = milliseconds(1);
  REQUIRE(h->start(c, cfg));
  const Qty lot = Qty::from_decimal("0.01").value();
  ConnectionStateMsg down{};
  init_header(down, EventType::ConnectionState, InstrumentId{}, VenueId{1});
  down.channel = 1;
  std::uint64_t states = 0;
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 2000; ++i) {
      c.t = c.t + milliseconds(1);
      const Price mid = Price::from_int(100000 + (i % 50));
      for (Book& b : c.books) b = Book{mid - Price::from_decimal("0.1").value(), mid, c.t};
      // B's order channel and C's kill switch and B's balance change over the run.
      down.state = (i / 100) % 3 == 1 ? ConnState::Disconnected : ConnState::Live;
      h->on_connection(c, down);
      c.killed[2] = (i / 150) % 2 == 1;
      c.room = (i / 70) % 4 == 3 ? Qty{} : Qty::max();
      c.refuse = (i / 40) % 9 == 8;
      // A source fill and whatever it brings.
      c.pos[0] += (i % 2 == 0) ? lot : -lot / 2;
      Fill f;
      f.instrument = InstrumentId{0};
      f.qty = lot;
      static_cast<void>(h->on_fill(c, f));
      h->on_book(c, InstrumentId{1});
      if (c.open.is_positive()) end(*h, c, i % 5 == 0 ? Qty{} : c.open, i % 7 != 0);
      h->on_timer(c);
      if (h->halted()) h->restart();
      const HedgeExecutor::Status s = h->status(c);
      states += static_cast<std::uint64_t>(s.state);
    }
  }
  CHECK(c.sends > 500);
  CHECK(h->stats().failovers > 0);
  CHECK(h->stats().derisk_orders > 0);
  CHECK(h->stats().uncertain_ends > 0);
  CHECK(states > 0);
}

TEST_CASE("hotpath.noalloc: HedgeExecutor in maker mode rests, follows the touch and crosses") {
  Logger::instance().set_level(LogLevel::Info);
  Logger::instance().attach_current_thread();
  auto table = std::make_unique<InstrumentTable>();
  REQUIRE(table->add(linear("BTCUSDT", 0)));
  REQUIRE(table->add(linear("BTCUSDT", 1)));
  REQUIRE(table->add(linear("BTCUSDT", 2)));
  Ctx c;
  c.table = table.get();
  auto h = std::make_unique<HedgeExecutor>();
  h->reset();
  REQUIRE(h->add_source(InstrumentId{0}));
  REQUIRE(h->add_hedge(InstrumentId{1}, Ratio::from_raw(5 * kRatioPerBp)));
  HedgeExecutor::Config cfg;
  cfg.retry = Duration{};
  cfg.maker = true;
  cfg.band = Notional::from_int(2000);
  cfg.flat = Notional::from_int(300);
  cfg.resize = Notional::from_int(500);
  cfg.repeg = milliseconds(3);
  cfg.taker_after = milliseconds(25);
  REQUIRE(h->start(c, cfg));
  const Qty lot = Qty::from_decimal("0.01").value();
  std::uint64_t states = 0;
  {
    NoAllocScope guard(true);
    for (int i = 0; i < 2000; ++i) {
      c.t = c.t + milliseconds(1);
      const Price mid = Price::from_int(100000 + (i / 50) % 7);
      for (Book& b : c.books) b = Book{mid - Price::from_decimal("0.1").value(), mid, c.t};
      // Fills every ms for the first half (moves, re-sizes), then rarely (orders rest and cross).
      if (i < 1000 || i % 200 == 0) c.pos[0] += (i / 60) % 2 == 0 ? lot : -lot;
      Fill f;
      f.instrument = InstrumentId{0};
      f.qty = lot;
      static_cast<void>(h->on_fill(c, f));
      h->on_book(c, InstrumentId{1});
      if (i % 3 ==
          0) {  // a forwarded ticker, in the first half a tick off the book every other time
        BookTickerMsg m{};
        init_header(m, EventType::BookTicker, InstrumentId{1}, VenueId{1});
        m.bid_px = c.books[1].bid;
        m.ask_px = c.books[1].ask +
                   (i < 1000 && i % 2 == 0 ? Price::from_decimal("0.1").value() : Price{});
        m.bid_qty = m.ask_qty = lot;
        static_cast<void>(h->on_book_ticker(c, InstrumentId{1}, m));
      }
      const bool cancelling = c.last_order.state == OrderState::PendingCancel;
      if (c.open.is_positive() && (cancelling || (i < 1000 && i % 11 == 0)))
        end(*h, c, cancelling ? Qty{} : c.open, true);
      h->on_timer(c);
      states += static_cast<std::uint64_t>(h->status(c).state);
    }
  }
  CHECK(c.sends > 50);
  CHECK(h->stats().maker_moves > 0);
  CHECK(h->stats().maker_crossed > 0);
  CHECK(states > 0);
}
