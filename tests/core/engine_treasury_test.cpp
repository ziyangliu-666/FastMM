// What the engine does for a pool treasury: the strategy's funding needs summed by home
// (pool_funding), and a payout held out of the payer's balance by a ControlTransferMsg with kHeld
// until it resolves: no order spends it, a failure gives it back, a transfer done moves it.
#include "fastmm/core/engine.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
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

constexpr InstrumentId kBtc{0};  // BTCUSDT spot on the pool of venue 0
constexpr InstrumentId kEth{1};  // ETHUSDT spot on the same pool
constexpr InstrumentId kEur{2};  // BTCEUR spot on the same pool: another quote asset
constexpr VenueId kA{0};         // the primary
constexpr VenueId kB{1};
constexpr VenueId kC{2};

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument s{};
  s.symbol = "BTCUSDT";
  s.base = "BTC";
  s.quote = "USDT";
  s.venue = kA;
  s.asset_class = AssetClass::Spot;
  s.flags = Instrument::kEnabled;
  s.tick = px("0.01");
  s.lot = qt("0.0001");
  s.min_qty = qt("0.0001");
  REQUIRE(t.add(s));
  Instrument e = s;
  e.symbol = "ETHUSDT";
  e.base = "ETH";
  REQUIRE(t.add(e));
  Instrument u = s;
  u.symbol = "BTCEUR";
  u.quote = "EUR";
  REQUIRE(t.add(u));
  return t;
}

struct Transport {
  std::size_t sent = 0;
  bool send(const EventHeader&) noexcept {
    ++sent;
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    sent += batch.size();
    return batch.size();
  }
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return true; }
};

struct Mute {
  static std::string_view name() noexcept { return "mute"; }
};

struct Rig {
  using E = Engine<Mute, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Mute strategy;
  std::unique_ptr<E> engine;

  Rig() {
    EngineConfig cfg;
    cfg.fees = FeeTable::from_bps(0, 0);
    cfg.risk.max_open_orders = 100;
    REQUIRE(cfg.pools.add(kB, kA));
    REQUIRE(cfg.pools.add(kC, kA));
    PoolPlacement& p = cfg.homes.pool[kA.value];
    p.configured = true;
    p.placement = Placement::Home;
    p.plan_interval = Duration{};
    cfg.homes.pin[kBtc.value] = kB;
    cfg.homes.pin[kEur.value] = kC;
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy, nullptr);
    engine->warm_up();
    engine->start();
    REQUIRE(engine->context().set_home(kEth, kB));
    book(kBtc, "49990", "50010");
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
  // One account's USDT as the venue reports it at venue time `at_s` (0: not stamped).
  void usdt(VenueId v, const char* free, std::int64_t at_s = 0) {
    BalanceMsg m{};
    init_header(m, EventType::Balance, InstrumentId::invalid(), v);
    m.asset.assign("USDT");
    m.free = nt(free);
    m.total = m.free;
    m.equity = m.total;
    if (at_s != 0) m.hdr.exch_ts = Timestamp{seconds(at_s).ns};
    push(m);
  }
  void transfer(TreasuryEventKind k,
                std::uint64_t seq,
                const char* amount,
                bool held = true,
                VenueId from = kA,
                VenueId to = kB) {
    ControlTransferMsg m{};
    init_header(m, EventType::Control, InstrumentId::invalid(), kA);
    m.command = ControlCommand::Transfer;
    m.event = static_cast<std::uint8_t>(k);
    m.flags = held ? ControlTransferMsg::kHeld : std::uint8_t{0};
    m.seq = seq;
    m.from = from;
    m.to = to;
    m.arg = static_cast<std::uint64_t>(nt(amount).raw);
    m.asset.assign("USDT");
    push(m);
  }
  [[nodiscard]] Notional free(VenueId v) { return engine->context().balance(v, "USDT").free; }
  [[nodiscard]] PoolFunding funding() const {
    PoolFunding f;
    engine->pool_funding(kA, "USDT", f);
    return f;
  }
};

}  // namespace

TEST_CASE("core.engine treasury: funding needs are summed by home in the treasury's asset") {
  Rig r;
  auto& ctx = r.engine->context();
  CHECK_FALSE(r.funding().declared);
  ctx.set_funding_need(kBtc, nt("300"));
  ctx.set_funding_need(kEth, nt("200"));
  ctx.set_funding_need(kEur, nt("900"));  // quoted in EUR: not the USDT treasury's
  CHECK(ctx.funding_need(kBtc) == nt("300"));
  const PoolFunding f = r.funding();
  CHECK(f.declared);
  CHECK(f.members.size() == 3);
  CHECK(f.need_of(kA) == Notional{});
  CHECK(f.need_of(kB) == nt("500"));
  CHECK(f.need_of(kC) == Notional{});
  PoolFunding eur;
  r.engine->pool_funding(kA, "EUR", eur);
  CHECK(eur.need_of(kC) == nt("900"));
  // A home change moves the need with it; a need of zero or less is none.
  REQUIRE(ctx.set_home(kEth, kC));
  CHECK(r.funding().need_of(kC) == nt("200"));
  ctx.set_funding_need(kBtc, nt("-5"));
  CHECK(ctx.funding_need(kBtc) == Notional{});
  CHECK(r.funding().need_of(kB) == Notional{});
}

TEST_CASE("core.engine treasury: a held payout is out of the payer's free balance") {
  Rig r;
  r.usdt(kA, "1000");
  r.usdt(kB, "0");
  r.transfer(TreasuryEventKind::Reserve, 7, "600");
  CHECK(r.free(kA) == nt("400"));
  PoolFunding f = r.funding();
  CHECK(f.hold_seq == 7);
  CHECK(f.held == nt("600"));
  // An order on the payer cannot spend it.
  NewOrderRequest buy = NewOrderRequest::limit(kBtc, Side::Buy, px("50000"), qt("0.01"));
  buy.account = kA;
  CHECK_FALSE(r.engine->send_order(buy));
  buy.qty = qt("0.007");  // 350 of the 400 left
  CHECK(r.engine->send_order(buy));
  // A report replaces the estimate; the hold stays on top of it.
  r.usdt(kA, "1000");
  CHECK(r.free(kA) == nt("50"));  // 1000 - 600 held - 350 the bid holds
  // The send can only lower it.
  r.transfer(TreasuryEventKind::Sent, 7, "500");
  CHECK(r.free(kA) == nt("150"));
  CHECK(r.funding().held == nt("500"));
  // Refused: it is given back.
  r.transfer(TreasuryEventKind::Failed, 7, "500");
  CHECK(r.free(kA) == nt("650"));
  CHECK(r.funding().hold_seq == 0);
}

TEST_CASE("core.engine treasury: the engine holds no more than the payer has free") {
  Rig r;
  r.usdt(kA, "1000");
  r.transfer(TreasuryEventKind::Reserve, 3, "1500");
  CHECK(r.funding().held == nt("1000"));
  CHECK(r.free(kA) == Notional{});
  r.transfer(TreasuryEventKind::Released, 3, "1500");
  CHECK(r.free(kA) == nt("1000"));
  // Another transfer's record leaves this one's hold alone.
  r.transfer(TreasuryEventKind::Reserve, 4, "100");
  r.transfer(TreasuryEventKind::Failed, 9, "100");
  CHECK(r.funding().hold_seq == 4);
  CHECK(r.free(kA) == nt("900"));
}

TEST_CASE("core.engine treasury: a done transfer moves the amount until the reports have it") {
  Rig r;
  r.usdt(kA, "1000", 1100);
  r.usdt(kB, "0", 1100);
  r.clock.set(Timestamp{seconds(1200).ns});
  r.transfer(TreasuryEventKind::Reserve, 1, "600");
  r.transfer(TreasuryEventKind::Sent, 1, "600");
  r.clock.set(Timestamp{seconds(1201).ns});
  r.transfer(TreasuryEventKind::Done, 1, "600");
  CHECK(r.free(kA) == nt("400"));
  CHECK(r.free(kB) == nt("600"));
  CHECK(r.funding().hold_seq == 0);
  // A report older than the transfer does not have it: the move stays.
  r.usdt(kA, "1000", 1150);
  r.usdt(kB, "0", 1150);
  CHECK(r.free(kA) == nt("400"));
  CHECK(r.free(kB) == nt("600"));
  // One after it does.
  r.usdt(kA, "400", 1300);
  r.usdt(kB, "600", 1300);
  CHECK(r.free(kA) == nt("400"));
  CHECK(r.free(kB) == nt("600"));
}

TEST_CASE("core.engine treasury: a record without kHeld changes no balance") {
  Rig r;
  r.usdt(kA, "1000");
  r.transfer(TreasuryEventKind::Reserve, 1, "600", false);
  r.transfer(TreasuryEventKind::Done, 1, "600", false);
  CHECK(r.free(kA) == nt("1000"));
  CHECK(r.free(kB) == Notional{});
  CHECK(r.engine->stats().treasury_records == 2);
}
