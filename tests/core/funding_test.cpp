// Perpetual funding payments (FundingMsg): realized PnL of the instrument in its settlement
// currency, converted with [accounting], counted by max_loss, booked once per venue id and
// instrument, and recorded for the store.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/fx.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/record_stream.hpp"

#include <memory>
#include <string>
#include <utility>
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

constexpr InstrumentId kInverse{0};  // BTCUSD, inverse: settles in BTC, venue a
constexpr InstrumentId kLinear{1};   // ETHUSDT: settles in USDT, venue a
constexpr InstrumentId kSource{2};   // BTCUSDT: settles in USDT and prices BTC, venue b

Instrument make(const char* symbol, const char* base, const char* quote, std::uint8_t venue) {
  Instrument i{};
  i.symbol = symbol;
  i.base = base;
  i.quote = quote;
  i.venue = VenueId{venue};
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  return i;
}

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument inv = make("BTCUSD", "BTC", "USD", 0);
  inv.flags = Instrument::kEnabled | Instrument::kInverse;
  inv.tick = px("0.5");
  inv.lot = qt("1");
  inv.min_qty = qt("1");
  inv.contract_multiplier = qt("100");
  REQUIRE(t.add(inv));
  REQUIRE(t.add(make("ETHUSDT", "ETH", "USDT", 0)));
  REQUIRE(t.add(make("BTCUSDT", "BTC", "USDT", 1)));
  return t;
}

FxPlan usdt_plan(const InstrumentTable& t) {
  AccountingSpec s;
  s.reporting_currency = "USDT";
  s.fx["BTC"] = "b:BTCUSDT";
  const std::vector<std::string> venues{"a", "b"};
  auto p = build_fx_plan(t, s, venues);
  REQUIRE_MESSAGE(p.has_value(), p.error());
  return *p;
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
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return false; }
};

struct Idle {};

struct Rig {
  using E = Engine<Idle, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  MsgRing records{1 << 18};
  Idle strategy;
  std::unique_ptr<E> engine;
  std::uint32_t execs = 0;

  explicit Rig(EngineConfig cfg, bool accounting) {
    if (accounting) cfg.fx = usdt_plan(table);
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy, nullptr, &records);
    engine->warm_up();
    engine->start();
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
    d->last_update_id = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  void fill(InstrumentId id, Side side, const char* price, const char* qty, const char* fee) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, id, table.get(id).venue);
    m.cl_ord_id = ClientOrderId{0xF00D};
    m.side = side;
    m.price = px(price);
    m.qty = qt(qty);
    m.fee = nt(fee);
    m.fee_asset = FeeAsset::Quote;
    m.exec_id = FixedString<40>(std::to_string(++execs).c_str());
    push(m);
  }
  void funding(InstrumentId id,
               const char* amount,
               const char* asset,
               const char* funding_id,
               bool replayed = false) {
    FundingMsg m{};
    init_header(
        m, EventType::Funding, id, id.value < table.size() ? table.get(id).venue : VenueId{});
    m.amount = nt(amount);
    m.asset.assign(asset);
    m.funding_id.assign(funding_id);
    m.hdr.exch_ts = Timestamp{1'789'000'000'000'000'000};
    if (replayed) m.flags = FundingMsg::kReplayed;
    push(m);
  }
  // The store records of `type`, in the order they were written.
  template <class R>
  std::vector<R> take(RecordType type) {
    std::vector<R> out;
    while (const std::byte* p = records.try_peek()) {
      const auto* h = reinterpret_cast<const RecordHeader*>(p);
      if (h->type == type) {
        R r;
        std::memcpy(&r, p, sizeof r);
        out.push_back(r);
      }
      records.release();
    }
    return out;
  }
};

}  // namespace

TEST_CASE("core.funding: the tracker books a payment into realized and says how much is funding") {
  const InstrumentTable t = make_table();
  PositionTracker pos;
  pos.on_fill(kLinear, Side::Buy, px("3000"), qt("1"), nt("1"), t[kLinear]);
  pos.on_funding(kLinear, nt("-2.5"));
  pos.on_funding(kLinear, nt("0.75"));
  CHECK(pos.get(kLinear).realized == nt("-1.75"));
  CHECK(pos.get(kLinear).fees == nt("1"));  // not a fee
  CHECK(pos.get(kLinear).qty == qt("1"));   // nor a trade
  CHECK(pos.funding(kLinear) == nt("-1.75"));
  CHECK(pos.total_funding() == nt("-1.75"));
  CHECK(pos.total_realized() == nt("-1.75"));
  CHECK(pos.net_pnl() == nt("-2.75"));
  pos.reset(kLinear);
  CHECK(pos.funding(kLinear).is_zero());
  CHECK(pos.total_funding().is_zero());
  CHECK(pos.total_realized().is_zero());
}

TEST_CASE("core.funding: funding in BTC on an inverse perpetual counts in USDT at the rate") {
  const InstrumentTable t = make_table();
  PositionTracker pos;
  pos.set_accounting(usdt_plan(t));
  pos.on_funding(kInverse, nt("-0.001"));  // BTC
  pos.on_funding(kSource, nt("-5"));       // USDT
  CHECK(pos.native(1).realized == nt("-0.001"));
  CHECK(pos.native_funding(1) == nt("-0.001"));
  CHECK(pos.total_funding() == nt("-5"));  // no BTC rate yet: BTC counts as zero
  pos.set_rate(1, FxRate::from_mid(px("50000"), false));
  CHECK(pos.total_realized() == nt("-55"));
  CHECK(pos.total_funding() == nt("-55"));
  CHECK(pos.net_pnl() == nt("-55"));
  // Configured after the payments (as a tracker is when the plan comes later), the same.
  PositionTracker late;
  late.on_funding(kInverse, nt("-0.001"));
  late.set_accounting(usdt_plan(t));
  late.set_rate(1, FxRate::from_mid(px("50000"), false));
  CHECK(late.total_funding() == nt("-50"));
}

TEST_CASE("core.funding: the engine books a payment once per id and instrument, in its currency") {
  EngineConfig cfg;
  Rig r(cfg, /*accounting=*/false);
  r.fill(kLinear, Side::Buy, "3000", "1", "0");
  r.funding(kLinear, "-1.5", "USDT", "tran-1");
  CHECK(r.engine->position(kLinear).realized == nt("-1.5"));
  // Delivered again (the stream and the venue's history): booked once.
  r.funding(kLinear, "-1.5", "USDT", "tran-1", /*replayed=*/true);
  CHECK(r.engine->position(kLinear).realized == nt("-1.5"));
  CHECK(r.engine->funding_stats().duplicates == 1);
  // The same id on another instrument is another payment; a currency named in lower case is the
  // same currency.
  r.funding(kSource, "2", "usdt", "tran-1");
  CHECK(r.engine->position(kSource).realized == nt("2"));
  // A payment in an asset the instrument does not settle in cannot be valued: not booked.
  r.funding(kLinear, "-0.01", "BNB", "tran-2");
  CHECK(r.engine->position(kLinear).realized == nt("-1.5"));
  // Nor one on an instrument outside the table.
  r.funding(InstrumentId{7}, "-1", "USDT", "tran-3");
  CHECK(r.engine->funding_stats().unbooked == 2);
  CHECK(r.engine->funding_stats().payments == 2);
  CHECK(r.engine->positions().total_funding() == nt("0.5"));
  CHECK(r.engine->positions().total_realized() == nt("0.5"));
  CHECK(r.engine->positions().total_fees().is_zero());
  CHECK(r.engine->runner_stats().funding_raw == nt("0.5").raw);
  CHECK(r.engine->runner_stats().realized_pnl_raw == nt("0.5").raw);

  // What the store receives: one record per payment booked, then the position it changed.
  std::vector<FundingRecord> recs = r.take<FundingRecord>(RecordType::Funding);
  REQUIRE(recs.size() == 2);
  CHECK(recs[0].hdr.instrument == kLinear);
  CHECK(recs[0].amount == nt("-1.5"));
  CHECK(recs[0].asset.view() == "USDT");
  CHECK(recs[0].funding_id.view() == "tran-1");
  CHECK(recs[0].position_qty == qt("1"));
  CHECK(recs[0].position_realized == nt("-1.5"));
  CHECK(recs[0].position_funding == nt("-1.5"));
  CHECK(recs[0].total_funding == nt("-1.5"));
  CHECK(recs[0].hdr.exch_ts == Timestamp{1'789'000'000'000'000'000});
  CHECK((recs[0].hdr.flags & RecordHeader::kReplayed) == 0);
  CHECK(recs[1].hdr.instrument == kSource);
  CHECK(recs[1].total_funding == nt("0.5"));
}

TEST_CASE("core.funding: the record of a replayed payment and the position after it") {
  EngineConfig cfg;
  Rig r(cfg, /*accounting=*/false);
  r.funding(kLinear, "0.25", "USDT", "tran-9", /*replayed=*/true);
  std::vector<std::pair<RecordType, std::vector<std::byte>>> all;
  while (const std::byte* p = r.records.try_peek()) {
    const auto* h = reinterpret_cast<const RecordHeader*>(p);
    all.emplace_back(h->type, std::vector<std::byte>(p, p + h->len));
    r.records.release();
  }
  REQUIRE(all.size() == 2);
  REQUIRE(all[0].first == RecordType::Funding);
  CHECK((reinterpret_cast<const FundingRecord*>(all[0].second.data())->hdr.flags &
         RecordHeader::kReplayed) != 0);
  REQUIRE(all[1].first == RecordType::Position);
  const auto* pr = reinterpret_cast<const PositionRecord*>(all[1].second.data());
  CHECK(pr->pos.realized == nt("0.25"));
  CHECK(pr->funding == nt("0.25"));
  CHECK(pr->total_funding == nt("0.25"));
}

TEST_CASE("core.funding: with [accounting] a BTC payment counts in USDT and in max_loss") {
  EngineConfig cfg;
  cfg.risk.max_loss = nt("100");
  Rig r(cfg, /*accounting=*/true);
  r.book(kSource, "49999.99", "50000.01");  // BTC = 50000 USDT
  r.funding(kInverse, "-0.0015", "BTC", "f-btc-1");
  CHECK(r.engine->position(kInverse).realized == nt("-0.0015"));  // BTC, the instrument's own
  CHECK(r.engine->positions().total_funding() == nt("-75"));
  CHECK(r.engine->net_pnl() == nt("-75"));
  CHECK(r.engine->kill_reason() == KillReason::None);
  r.funding(kInverse, "-0.0006", "BTC", "f-btc-2");  // -105 USDT in all
  CHECK(r.engine->kill_reason() == KillReason::MaxLoss);
}

TEST_CASE("core.funding: max_loss trips on a funding loss the fills alone do not reach") {
  EngineConfig cfg;
  cfg.risk.max_loss = nt("100");
  Rig r(cfg, /*accounting=*/false);
  // Fills: -60 realized, and flat, so no mark can move it.
  r.fill(kLinear, Side::Buy, "3000", "1", "0");
  r.fill(kLinear, Side::Sell, "2940", "1", "0");
  CHECK(r.engine->net_pnl() == nt("-60"));
  CHECK(r.engine->kill_reason() == KillReason::None);
  CHECK_FALSE(r.engine->risk().killed());
  // Funding paid on the position before it was closed takes it past the budget.
  r.funding(kLinear, "-45", "USDT", "tran-45");
  CHECK(r.engine->net_pnl() == nt("-105"));
  CHECK(r.engine->kill_reason() == KillReason::MaxLoss);
  CHECK(r.engine->risk().killed());
}
