// Balances (BalanceMsg, core/balance_book.hpp): the table the engine keeps per venue and asset, the
// estimate its own orders and fills move between the venue's reports, the pre-trade check and the
// strategy's view (ctx.balance, ctx.margin, ctx.balance_room, on_balance).
#include "fastmm/core/balance_book.hpp"
#include "fastmm/core/engine.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
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
Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

constexpr InstrumentId kSpot{0};  // BTCUSDT spot, venue 0
constexpr InstrumentId kPerp{1};  // BTCUSDT linear perpetual, venue 1, USDT margin
constexpr InstrumentId kEth{2};   // ETHUSDT spot, venue 0: shares USDT with kSpot
constexpr VenueId kSpotVenue{0};
constexpr VenueId kPerpVenue{1};

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument s{};
  s.symbol = "BTCUSDT";
  s.base = "BTC";
  s.quote = "USDT";
  s.venue = kSpotVenue;
  s.asset_class = AssetClass::Spot;
  s.flags = Instrument::kEnabled;
  s.tick = px("0.01");
  s.lot = qt("0.0001");
  s.min_qty = qt("0.0001");
  REQUIRE(t.add(s));
  Instrument p = s;
  p.venue = kPerpVenue;
  p.asset_class = AssetClass::Perpetual;
  p.lot = qt("0.001");
  p.min_qty = qt("0.001");
  REQUIRE(t.add(p));
  Instrument e = s;
  e.symbol = "ETHUSDT";
  e.base = "ETH";
  REQUIRE(t.add(e));
  return t;
}

// 10 bps taker on every instrument; the perpetual's initial margin is 10 % of its notional.
EngineConfig make_config() {
  EngineConfig cfg;
  cfg.fees = FeeTable::from_bps(2, 10);
  cfg.balance.initial_margin[kPerp.value] = Ratio::from_decimal("0.1").value();
  return cfg;
}

BalanceMsg report(VenueId venue,
                  const char* asset,
                  const char* free,
                  const char* locked,
                  std::int64_t venue_ms,
                  std::uint8_t flags = 0) {
  BalanceMsg m{};
  init_header(m, EventType::Balance, InstrumentId::invalid(), venue);
  m.asset.assign(asset);
  m.free = nt(free);
  m.locked = nt(locked);
  m.total = m.free + m.locked;
  m.equity = m.total;
  m.flags = flags;
  if (venue_ms > 0) m.hdr.exch_ts = Timestamp{venue_ms * 1'000'000};
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
  [[nodiscard]] std::vector<const OutNewOrderMsg*> news() const {
    std::vector<const OutNewOrderMsg*> r;
    for (const auto& b : out) {
      const auto* h = reinterpret_cast<const EventHeader*>(b.data());
      if (h->type == EventType::OutNewOrder)
        r.push_back(reinterpret_cast<const OutNewOrderMsg*>(h));
    }
    return r;
  }
};

// Records what on_balance saw and what the context said at that moment; with sell_on_fill, sells
// 0.001 BTC at 51000 from every on_fill.
struct Watcher {
  std::vector<BalanceMsg> seen;
  Balance usdt_at_hook;
  bool sell_on_fill = false;
  std::vector<ClientOrderId> sent;
  template <class Ctx>
  void on_balance(Ctx& ctx, const BalanceMsg& m) {
    seen.push_back(m);
    usdt_at_hook = ctx.balance(kSpotVenue, "USDT");
  }
  template <class Ctx>
  void on_fill(Ctx& ctx, const Fill&) {
    if (!sell_on_fill) return;
    auto id = ctx.send(NewOrderRequest::limit(kSpot, Side::Sell, px("51000"), qt("0.001")));
    REQUIRE(id.has_value());
    sent.push_back(*id);
  }
};

struct Rig {
  using E = Engine<Watcher, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Watcher strategy;
  std::unique_ptr<E> engine;
  std::uint32_t execs = 0;

  explicit Rig(const EngineConfig& cfg = make_config()) {
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy);
    engine->warm_up();
    engine->start();
    book(kSpot, "49990", "50010");
    book(kPerp, "49990", "50010");
    book(kEth, "2999", "3001");
  }
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
    init_header(*d, EventType::BookSnapshot, id, table.get(id).venue, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  void balance(BalanceMsg m) { push(m); }
  Result<ClientOrderId, RejectReason> send(
      InstrumentId id, Side side, const char* price, const char* qty, bool reduce_only = false) {
    NewOrderRequest r = NewOrderRequest::limit(id, side, px(price), qt(qty));
    r.reduce_only = reduce_only;
    return engine->send_order(r);
  }
  void ack(InstrumentId id, ClientOrderId cl, std::int64_t venue_ms = 0) {
    OrderAckMsg m{};
    init_header(m, EventType::OrderAck, id, table.get(id).venue);
    m.cl_ord_id = cl;
    m.venue_order_id.assign("v" + std::to_string(cl.value));
    if (venue_ms > 0) m.hdr.exch_ts = Timestamp{venue_ms * 1'000'000};
    push(m);
  }
  void cancel_ack(InstrumentId id, ClientOrderId cl, std::int64_t venue_ms) {
    OrderCancelAckMsg m{};
    init_header(m, EventType::OrderCancelAck, id, table.get(id).venue);
    m.cl_ord_id = cl;
    m.hdr.exch_ts = Timestamp{venue_ms * 1'000'000};
    push(m);
  }
  void fill(InstrumentId id,
            ClientOrderId cl,
            Side side,
            const char* price,
            const char* qty,
            const char* cum,
            const char* leaves,
            const char* fee,
            FeeAsset fee_asset,
            std::int64_t venue_ms) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, id, table.get(id).venue);
    m.cl_ord_id = cl;
    m.side = side;
    m.price = px(price);
    m.qty = qt(qty);
    m.cum_qty = qt(cum);
    m.leaves_qty = qt(leaves);
    m.fee = nt(fee);
    m.fee_asset = fee_asset;
    m.exec_id.assign(std::to_string(++execs));
    m.hdr.exch_ts = Timestamp{venue_ms * 1'000'000};
    push(m);
  }
  [[nodiscard]] Balance bal(VenueId v, const char* asset) const {
    return engine->context().balance(v, asset);
  }
};

}  // namespace

TEST_CASE("core.balance: the table keeps the base and quote of each venue's instruments") {
  const InstrumentTable t = make_table();
  BalanceBook b;
  b.build(t, FeeTable::from_bps(0, 10));
  // venue 0: BTC, USDT, ETH; venue 1: USDT (the perpetual settles in it) and its account row.
  CHECK(b.size() == 5);
  CHECK_FALSE(b.live());
  CHECK_FALSE(b.balance(kSpotVenue, "USDT").known);
  CHECK(b.on_report(report(kSpotVenue, "usdt", "1000", "0", 5)));  // case-insensitive
  CHECK_FALSE(b.on_report(report(kSpotVenue, "DOGE", "7", "0", 5)));
  CHECK(b.stats().untracked == 1);
  CHECK(b.live());
  const Balance usdt = b.balance(kSpotVenue, "USDT");
  CHECK(usdt.known);
  CHECK(usdt.free == nt("1000"));
  CHECK(usdt.total == nt("1000"));
  CHECK(usdt.as_of == Timestamp{5'000'000});
  CHECK_FALSE(b.balance(kPerpVenue, "USDT").known);  // another venue's account
}

TEST_CASE("core.balance: a snapshot zeroes the assets it does not name") {
  const InstrumentTable t = make_table();
  BalanceBook b;
  b.build(t);
  REQUIRE(b.on_report(report(kSpotVenue, "BTC", "1", "0", 1)));
  REQUIRE(b.on_report(report(kSpotVenue, "ETH", "3", "0", 1)));
  b.on_report(report(kSpotVenue, "USDT", "500", "20", 9, BalanceMsg::kSnapshot));
  b.on_report(report(kSpotVenue, "BTC", "0.5", "0", 9, BalanceMsg::kSnapshot));
  CHECK(b.balance(kSpotVenue, "ETH").free == nt("3"));  // not over yet
  b.on_report(
      report(kSpotVenue, "", "0", "0", 9, BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd));
  CHECK(b.balance(kSpotVenue, "ETH").known);
  CHECK(b.balance(kSpotVenue, "ETH").free.is_zero());
  CHECK(b.balance(kSpotVenue, "BTC").free == nt("0.5"));
  CHECK(b.balance(kSpotVenue, "USDT").locked == nt("20"));
  CHECK(b.stats().snapshots == 1);
  // The perpetual venue did not report: its rows stay unknown, its account row too.
  CHECK_FALSE(b.balance(kPerpVenue, "USDT").known);
  CHECK_FALSE(b.margin(kPerpVenue).known);
}

TEST_CASE("core.balance: nothing is checked before a venue reports") {
  Rig r;
  auto id = r.send(kSpot, Side::Buy, "50000", "1000");  // 50 M USDT
  CHECK(id.has_value());
  CHECK_FALSE(r.engine->balances().live());
  CHECK(r.engine->context().balance_room(kSpot, Side::Buy, px("50000")) == Qty::max());
}

TEST_CASE("core.balance: spot orders hold the quote or the base and the check refuses the rest") {
  Rig r;
  r.balance(report(kSpotVenue, "USDT", "1000", "0", 100));
  r.balance(report(kSpotVenue, "BTC", "0.01", "0", 100));
  REQUIRE(r.strategy.seen.size() == 2);
  CHECK(r.strategy.usdt_at_hook.free == nt("1000"));

  // A buy of 0.01 at 50000 holds 500 plus the 10 bps taker fee.
  auto b1 = r.send(kSpot, Side::Buy, "49990", "0.01");
  REQUIRE(b1.has_value());
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("499.6001"));  // 1000 - 499.9 - 0.4999
  CHECK(r.bal(kSpotVenue, "USDT").locked == nt("500.3999"));
  // The same again does not fit.
  auto b2 = r.send(kSpot, Side::Buy, "49990", "0.01");
  REQUIRE_FALSE(b2.has_value());
  CHECK(b2.error() == RejectReason::BalanceShort);
  CHECK(r.engine->stats().risk_rejects_by_reason[RejectReason::BalanceShort] == 1);
  // ETHUSDT draws on the same USDT.
  CHECK(r.send(kEth, Side::Buy, "2999", "0.2").error() == RejectReason::BalanceShort);
  CHECK(r.send(kEth, Side::Buy, "2999", "0.1").has_value());
  // Sells draw on the base.
  CHECK(r.send(kSpot, Side::Sell, "50010", "0.02").error() == RejectReason::BalanceShort);
  auto s1 = r.send(kSpot, Side::Sell, "50010", "0.01");
  REQUIRE(s1.has_value());
  CHECK(r.bal(kSpotVenue, "BTC").free.is_zero());
  CHECK(r.bal(kSpotVenue, "BTC").locked == nt("0.01"));
  CHECK(r.bal(kSpotVenue, "BTC").total == nt("0.01"));

  // The room the context reports is what the check lets through.
  const Qty room = r.engine->context().balance_room(kSpot, Side::Buy, px("49990"));
  CHECK(room == qt("0.0039"));
  CHECK(r.send(kSpot, Side::Buy, "49990", "0.004").error() == RejectReason::BalanceShort);
  CHECK(r.send(kSpot, Side::Buy, "49990", "0.0039").has_value());
  const RiskHeadroom h = r.engine->context().risk_headroom(kSpot);
  CHECK(h.balance_sell_qty.is_zero());
  CHECK(h.balance_buy_qty < qt("0.0001"));  // the rest is fee rounding
}

TEST_CASE("core.balance: an order's end releases its hold and a fill moves the assets") {
  Rig r;
  r.balance(report(kSpotVenue, "USDT", "1000", "0", 100));
  r.balance(report(kSpotVenue, "BTC", "0", "0", 100));
  auto b1 = r.send(kSpot, Side::Buy, "50000", "0.01");  // holds 500.5
  REQUIRE(b1.has_value());
  r.ack(kSpot, *b1, 110);
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("499.5"));
  // Half fills at 49990 with 0.000005 BTC commission: the base arrives less the fee; the quote
  // pays the fill and the hold of the filled half goes.
  r.fill(
      kSpot, *b1, Side::Buy, "49990", "0.005", "0.005", "0.005", "0.000005", FeeAsset::Base, 120);
  const Balance usdt = r.bal(kSpotVenue, "USDT");
  CHECK(usdt.locked == nt("250.25"));
  CHECK(usdt.total == nt("750.05"));  // 1000 - 249.95
  CHECK(usdt.free == nt("499.8"));    // 750.05 - 250.25
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.004995"));
  // The cancel releases the rest.
  REQUIRE(r.engine->cancel_order(*b1).has_value());
  r.cancel_ack(kSpot, *b1, 130);
  CHECK(r.bal(kSpotVenue, "USDT").locked.is_zero());
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("750.05"));
  // A sell fill with the fee in the quote.
  auto s1 = r.send(kSpot, Side::Sell, "51000", "0.004");
  REQUIRE(s1.has_value());
  r.ack(kSpot, *s1, 140);
  r.fill(kSpot, *s1, Side::Sell, "51000", "0.004", "0.004", "0", "0.204", FeeAsset::Quote, 150);
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.000995"));
  CHECK(r.bal(kSpotVenue, "BTC").locked.is_zero());
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("953.846"));  // 750.05 + 204 - 0.204
}

TEST_CASE("core.balance: the venue's next report replaces the estimate and stamps decide") {
  Rig r;
  r.balance(report(kSpotVenue, "USDT", "1000", "0", 100));
  r.balance(report(kSpotVenue, "BTC", "0", "0", 100));
  auto b1 = r.send(kSpot, Side::Buy, "50000", "0.01");
  REQUIRE(b1.has_value());
  r.ack(kSpot, *b1, 110);
  // The venue reports after taking the order: its free already excludes the hold.
  r.balance(report(kSpotVenue, "USDT", "500", "500", 115));
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("500"));
  // A fill the venue stamped before that report is in it already: nothing moves.
  r.fill(kSpot, *b1, Side::Buy, "50000", "0.002", "0.002", "0.008", "0", FeeAsset::Quote, 112);
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("500"));
  CHECK(r.bal(kSpotVenue, "USDT").total == nt("1000"));
  // One stamped after it moves the estimate.
  r.fill(kSpot, *b1, Side::Buy, "50000", "0.002", "0.004", "0.006", "0", FeeAsset::Quote, 120);
  CHECK(r.bal(kSpotVenue, "USDT").total == nt("900"));
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("500.1"));  // the fee part of the hold comes back
  // A snapshot replaces everything again, and an asset it leaves out holds nothing.
  r.balance(report(kSpotVenue, "USDT", "600", "300", 130, BalanceMsg::kSnapshot));
  r.balance(
      report(kSpotVenue, "", "0", "0", 130, BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd));
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("600"));
  CHECK(r.bal(kSpotVenue, "BTC").known);
  CHECK(r.bal(kSpotVenue, "BTC").free.is_zero());
  CHECK(r.bal(kSpotVenue, "ETH").free.is_zero());
}

TEST_CASE("core.balance: orders sent before the first report hold from the report on") {
  Rig r;
  auto b1 = r.send(kSpot, Side::Buy, "50000", "0.01");
  REQUIRE(b1.has_value());
  r.ack(kSpot, *b1, 50);
  r.balance(report(kSpotVenue, "USDT", "499.5", "500.5", 100));
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("499.5"));
  // Its end releases what it holds.
  REQUIRE(r.engine->cancel_order(*b1).has_value());
  r.cancel_ack(kSpot, *b1, 110);
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("1000"));
}

TEST_CASE("core.balance: a derivative holds its initial margin, reducing orders pass") {
  Rig r;
  r.balance(report(kPerpVenue, "USDT", "1000", "0", 100));
  // 0.1 at 50000 is 5000 notional, 500 initial margin.
  auto b1 = r.send(kPerp, Side::Buy, "50000", "0.1");
  REQUIRE(b1.has_value());
  CHECK(r.engine->context().margin(kPerpVenue).known);
  CHECK(r.engine->context().margin(kPerpVenue).available == nt("500"));
  CHECK(r.engine->context().margin(kPerpVenue).asset.view() == "USDT");
  // The sell side nets against the buy: the larger side holds.
  auto s1 = r.send(kPerp, Side::Sell, "50010", "0.1");  // 500.1: adds 0.1
  REQUIRE(s1.has_value());
  CHECK(r.engine->context().margin(kPerpVenue).available == nt("499.9"));
  CHECK(r.send(kPerp, Side::Buy, "49990", "0.11").error() == RejectReason::BalanceShort);
  CHECK(r.engine->context().balance_room(kPerp, Side::Buy, px("50000")) == qt("0.099"));
  // A fill turns the order's margin into the position's.
  r.ack(kPerp, *b1, 110);
  r.fill(kPerp, *b1, Side::Buy, "50000", "0.1", "0.1", "0", "1", FeeAsset::Quote, 120);
  const Margin m = r.engine->context().margin(kPerpVenue);
  CHECK(m.initial == nt("1000.1"));  // 500 for the position, 500.1 for the resting sell
  CHECK(m.available == nt("-1.1"));  // less the fee
  // The position is long 0.1: a sell that reduces it passes on no margin, a buy does not.
  CHECK(r.send(kPerp, Side::Buy, "49990", "0.001").error() == RejectReason::BalanceShort);
  r.ack(kPerp, *s1, 125);
  REQUIRE(r.engine->cancel_order(*s1).has_value());
  CHECK(r.send(kPerp, Side::Sell, "50010", "0.05", /*reduce_only=*/true).has_value());
}

TEST_CASE("core.balance: an account-wide margin row takes over the venue's derivatives") {
  Rig r;
  r.balance(report(kPerpVenue, "USDT", "10", "0", 100));
  CHECK(r.send(kPerp, Side::Buy, "50000", "0.01").error() == RejectReason::BalanceShort);
  BalanceMsg acct = report(kPerpVenue, "USD", "20000", "0", 101, BalanceMsg::kAccount);
  r.balance(acct);
  const Margin m = r.engine->context().margin(kPerpVenue);
  CHECK(m.account);
  CHECK(m.asset.view() == "USD");
  CHECK(m.available == nt("20000"));
  CHECK(r.send(kPerp, Side::Buy, "50000", "0.01").has_value());
  CHECK(r.engine->context().margin(kPerpVenue).available == nt("19950"));
}

TEST_CASE("core.balance: check_balance false keeps the table and refuses nothing") {
  EngineConfig cfg = make_config();
  cfg.balance.check = false;
  Rig r(cfg);
  r.balance(report(kSpotVenue, "USDT", "1", "0", 100));
  CHECK(r.send(kSpot, Side::Buy, "50000", "1").has_value());
  CHECK(r.bal(kSpotVenue, "USDT").free.raw < 0);
}

TEST_CASE("core.balance: replacing an order checks only what it adds") {
  Rig r;
  r.balance(report(kSpotVenue, "USDT", "600", "0", 100));
  auto b1 = r.send(kSpot, Side::Buy, "50000", "0.01");  // holds 500.5
  REQUIRE(b1.has_value());
  r.ack(kSpot, *b1, 110);
  CHECK(r.engine->replace_order(*b1, px("50000"), qt("0.0119")).has_value());  // +95.095 fits
  auto b2 = r.send(kSpot, Side::Buy, "50000", "0.0001");
  REQUIRE(b2.has_value());
  r.ack(kSpot, *b2, 111);
  CHECK(r.engine->replace_order(*b2, px("50000"), qt("0.003")).error() ==
        RejectReason::BalanceShort);
}

// Found on the OKX demo: a report the venue sent while a post-only order was on its way held none
// of it, and the post-only's reject then released a hold the estimate no longer had (locked went to
// -1.69 USDT). An order the venue has not acknowledged keeps its hold on top of every report.
TEST_CASE("core.balance: an order in flight holds on top of a report until its ack") {
  Rig r;
  r.balance(report(kSpotVenue, "USDT", "1000", "0", 100));
  auto b1 = r.send(kSpot, Side::Buy, "50000", "0.01");  // holds 500.5, not acknowledged
  REQUIRE(b1.has_value());
  // A report the venue made before it took the order.
  r.balance(report(kSpotVenue, "USDT", "1000", "0", 105));
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("499.5"));
  CHECK(r.bal(kSpotVenue, "USDT").locked == nt("500.5"));
  // The venue refuses it (post-only would cross), stamped before the report: its hold goes all
  // the same, since no report had it.
  OrderRejectMsg rej{};
  init_header(rej, EventType::OrderReject, kSpot, kSpotVenue);
  rej.cl_ord_id = *b1;
  rej.reason = RejectReason::PostOnlyWouldCross;
  rej.hdr.exch_ts = Timestamp{104LL * 1'000'000};
  r.push(rej);
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("1000"));
  CHECK(r.bal(kSpotVenue, "USDT").locked.is_zero());

  // An ack stamped at or before a report that has the order gives the extra back.
  auto b2 = r.send(kSpot, Side::Buy, "50000", "0.01");
  REQUIRE(b2.has_value());
  r.balance(report(kSpotVenue, "USDT", "499.5", "500.5", 120));  // the venue has it
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("-1"));             // counted twice meanwhile
  r.ack(kSpot, *b2, 118);
  CHECK(r.bal(kSpotVenue, "USDT").free == nt("499.5"));
  CHECK(r.bal(kSpotVenue, "USDT").locked == nt("500.5"));
  // One stamped after the report keeps it: the report did not have the order.
  auto s1 = r.send(kSpot, Side::Sell, "51000", "0.001");
  REQUIRE(s1.has_value());
  r.balance(report(kSpotVenue, "BTC", "0.01", "0", 130));
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.009"));
  r.ack(kSpot, *s1, 131);
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.009"));
}

TEST_CASE("core.balance: an order sent from on_fill into the slot the fill freed keeps its hold") {
  Rig r;
  r.balance(report(kSpotVenue, "USDT", "1000", "0", 100));
  r.balance(report(kSpotVenue, "BTC", "0.01", "0", 100));
  auto b1 = r.send(kSpot, Side::Buy, "50000", "0.001");
  REQUIRE(b1.has_value());
  r.ack(kSpot, *b1, 110);
  r.strategy.sell_on_fill = true;
  // The last fill ends the buy and frees its slot; the strategy's sell takes it.
  r.fill(kSpot, *b1, Side::Buy, "50000", "0.001", "0.001", "0", "0", FeeAsset::Quote, 120);
  REQUIRE(r.strategy.sent.size() == 1);
  r.strategy.sell_on_fill = false;
  CHECK(r.bal(kSpotVenue, "BTC").locked == nt("0.001"));
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.01"));
  CHECK(r.bal(kSpotVenue, "USDT").locked.is_zero());
  // Acknowledged, then a report that has it: held once.
  r.ack(kSpot, r.strategy.sent[0], 130);
  r.balance(report(kSpotVenue, "BTC", "0.01", "0.001", 131));
  CHECK(r.bal(kSpotVenue, "BTC").locked == nt("0.001"));
  // Its cancel releases it.
  REQUIRE(r.engine->cancel_order(r.strategy.sent[0]).has_value());
  r.cancel_ack(kSpot, r.strategy.sent[0], 140);
  CHECK(r.bal(kSpotVenue, "BTC").locked.is_zero());
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.011"));
}

namespace {

// One ask of `qty` at `price`.
DesiredQuotes ask(const char* price, const char* qty) {
  DesiredQuotes q;
  REQUIRE(q.ask(px(price), qt(qty)));
  return q;
}

std::vector<const OutNewOrderMsg*> news_after(const Transport& t, std::size_t from) {
  const auto all = t.news();
  return {all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())), all.end()};
}

std::size_t cancels(const Transport& t) {
  std::size_t n = 0;
  for (const auto& b : t.out) {
    if (reinterpret_cast<const EventHeader*>(b.data())->type == EventType::OutCancel) ++n;
  }
  return n;
}

}  // namespace

// Found live (Binance spot, supports_replace = false): 0.00059 BTC, a resting ask of 0.0005. A
// requote cancels the ask and sends its New once the cancel's ack has released the hold; the
// balance check sees the old order's hold gone and passes. No BalanceShort.
TEST_CASE("core.balance: a requote on a balance-limited side waits for the cancel's ack") {
  Rig r;
  r.balance(report(kSpotVenue, "USDT", "1000", "0", 100));
  r.balance(report(kSpotVenue, "BTC", "0.00059", "0", 100));
  REQUIRE(r.engine->set_quotes(kSpot, ask("50100", "0.0005")));
  REQUIRE(r.transport.news().size() == 1);
  const ClientOrderId first = r.transport.news()[0]->cl_ord_id;
  r.ack(kSpot, first, 110);
  r.balance(report(kSpotVenue, "BTC", "0.00009", "0.0005", 110));
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.00009"));

  // The price moves: the cancel goes now, the New waits for its ack.
  r.clock.set(r.clock.now() + milliseconds(100));
  REQUIRE(r.engine->set_quotes(kSpot, ask("50200", "0.0005")));
  CHECK(cancels(r.transport) == 1);
  CHECK(r.transport.news().size() == 1);
  r.cancel_ack(kSpot, first, 120);
  const auto sent = news_after(r.transport, 1);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0]->price == px("50200"));
  CHECK(sent[0]->qty == qt("0.0005"));
  CHECK(r.engine->stats().risk_rejects_by_reason[RejectReason::BalanceShort] == 0);
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.00009"));
  CHECK(r.bal(kSpotVenue, "BTC").locked == nt("0.0005"));
  // The cancel's report, then the New's ack and report: the estimate stays where the venue is.
  r.balance(report(kSpotVenue, "BTC", "0.00059", "0", 120));
  r.ack(kSpot, sent[0]->cl_ord_id, 121);
  r.balance(report(kSpotVenue, "BTC", "0.00009", "0.0005", 121));
  CHECK(r.bal(kSpotVenue, "BTC").free == nt("0.00009"));
  CHECK(r.bal(kSpotVenue, "BTC").locked == nt("0.0005"));
}

// The incident's cause: the venue's report of the account came before the order's ack. An ack
// stamped with the venue's time of the placement says the report had the order, and the estimate
// gives the second hold back; the next requote then fits. Unstamped (Binance's WS API acks before
// the fix), the order stayed held twice until the next report, and the New after the cancel was
// refused.
TEST_CASE("core.balance: a report before the ack, then a requote: a stamped ack keeps it right") {
  for (const bool stamped : {true, false}) {
    CAPTURE(stamped);
    Rig r;
    r.balance(report(kSpotVenue, "BTC", "0.00059", "0", 100));
    REQUIRE(r.engine->set_quotes(kSpot, ask("50100", "0.0005")));
    const ClientOrderId first = r.transport.news().at(0)->cl_ord_id;
    r.balance(report(kSpotVenue, "BTC", "0.00009", "0.0005", 110));  // has the order
    r.ack(kSpot, first, stamped ? 110 : 0);
    CHECK(r.bal(kSpotVenue, "BTC").locked == (stamped ? nt("0.0005") : nt("0.001")));
    r.clock.set(r.clock.now() + milliseconds(100));
    REQUIRE(r.engine->set_quotes(kSpot, ask("50200", "0.0005")));
    r.cancel_ack(kSpot, first, 120);
    CHECK(news_after(r.transport, 1).size() == (stamped ? 1U : 0U));
    CHECK(r.engine->quote_manager().stats().kept_balance == (stamped ? 0U : 1U));
    CHECK(r.engine->stats().risk_rejects_by_reason[RejectReason::BalanceShort] == 0);
  }
}

// The cancel lost the race: the ask filled. The New recorded for after the cancel was decided
// against BTC the fill took, so it is withheld (not a risk reject) and the strategy's next quotes
// decide; the cancel's reject changes nothing. With BTC to spare it goes out.
TEST_CASE("core.balance: a requote whose old order filled instead re-checks the balance") {
  for (const char* btc : {"0.00059", "0.002"}) {
    CAPTURE(btc);
    Rig r;
    r.balance(report(kSpotVenue, "USDT", "1000", "0", 100));
    r.balance(report(kSpotVenue, "BTC", btc, "0", 100));
    REQUIRE(r.engine->set_quotes(kSpot, ask("50100", "0.0005")));
    const ClientOrderId first = r.transport.news().at(0)->cl_ord_id;
    r.ack(kSpot, first, 110);
    r.clock.set(r.clock.now() + milliseconds(100));
    REQUIRE(r.engine->set_quotes(kSpot, ask("50200", "0.0005")));
    REQUIRE(cancels(r.transport) == 1);
    r.fill(kSpot, first, Side::Sell, "50100", "0.0005", "0.0005", "0", "0", FeeAsset::Quote, 115);
    OrderCancelRejectMsg rej{};
    init_header(rej, EventType::OrderCancelReject, kSpot, kSpotVenue);
    rej.cl_ord_id = first;
    rej.reason = RejectReason::VenueUnknownOrder;
    r.push(rej);
    const bool fits = std::string_view(btc) == "0.002";
    const auto sent = news_after(r.transport, 1);
    CHECK(sent.size() == (fits ? 1U : 0U));
    if (fits && !sent.empty()) CHECK(sent[0]->price == px("50200"));
    CHECK(r.engine->quote_manager().stats().kept_balance == (fits ? 0U : 1U));
    CHECK(r.engine->runner_stats().balance_withheld == (fits ? 0U : 1U));  // status, metrics
    CHECK(r.engine->stats().risk_rejects_by_reason[RejectReason::BalanceShort] == 0);
    CHECK(r.bal(kSpotVenue, "BTC").free == (fits ? nt("0.001") : nt("0.00009")));
  }
}
