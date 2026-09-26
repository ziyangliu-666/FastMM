// Accounting across settlement currencies ([accounting], core/fx.hpp): the plan, the converted
// totals of PositionTracker, the FX gate of RiskEngine and the engine putting them together.
#include "fastmm/core/fx.hpp"

#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/risk.hpp"

#include <memory>
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

constexpr InstrumentId kInverse{0};  // BTCUSD, inverse: settles in BTC, venue a
constexpr InstrumentId kLinear{1};   // ETHUSDT: settles in USDT, venue a
constexpr InstrumentId kSource{2};   // BTCUSDT: prices BTC in USDT, venue b

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
  inv.contract_multiplier = qt("100");  // USD per contract
  REQUIRE(t.add(inv));
  REQUIRE(t.add(make("ETHUSDT", "ETH", "USDT", 0)));
  REQUIRE(t.add(make("BTCUSDT", "BTC", "USDT", 1)));
  return t;
}

const std::vector<std::string> kVenues{"a", "b"};

AccountingSpec usdt_spec() {
  AccountingSpec s;
  s.reporting_currency = "USDT";
  s.fx["BTC"] = "b:BTCUSDT";
  return s;
}

FxPlan usdt_plan(const InstrumentTable& t) {
  auto p = build_fx_plan(t, usdt_spec(), kVenues);
  REQUIRE_MESSAGE(p.has_value(), p.error());
  return *p;
}

}  // namespace

TEST_CASE("core.fx: a rate converts either way round") {
  const FxRate direct = FxRate::from_mid(px("50000"), false);     // BTCUSDT: USDT per BTC
  const FxRate inverted = FxRate::from_mid(px("0.00002"), true);  // USDTBTC: BTC per USDT
  CHECK(convert(nt("0.02"), direct) == nt("1000"));
  CHECK(convert(nt("0.02"), inverted) == nt("1000"));
  CHECK(convert(nt("-0.005"), direct) == nt("-250"));
  CHECK(direct.price() == px("50000"));
  CHECK(inverted.price() == px("50000"));
  CHECK(convert(nt("123.45"), FxRate::identity()) == nt("123.45"));
  CHECK_FALSE(FxRate::from_mid(Price{}, false).known());
  CHECK(convert(nt("7"), FxRate{}).is_zero());  // an unknown rate counts nothing
}

TEST_CASE("core.fx: the plan gives each instrument its currency and each currency its source") {
  const InstrumentTable t = make_table();
  const FxPlan p = usdt_plan(t);
  CHECK(p.active());
  CHECK(p.count == 2);
  CHECK(p.reporting() == "USDT");
  CHECK(p.names[1].view() == "BTC");
  CHECK(p.ccy[kInverse.value] == 1);
  CHECK(p.ccy[kLinear.value] == 0);
  CHECK(p.ccy[kSource.value] == 0);
  CHECK(p.sources[1].instrument == kSource);
  CHECK_FALSE(p.sources[1].invert);
  CHECK(p.priced_by(kSource) == 1);
  CHECK(p.priced_by(kLinear) == -1);

  SUBCASE("a pair quoted the other way round is inverted") {
    InstrumentTable u = make_table();
    REQUIRE(u.add(make("USDTBTC", "USDT", "BTC", 1)));
    AccountingSpec s = usdt_spec();
    s.fx["BTC"] = "b:USDTBTC";
    const auto q = build_fx_plan(u, s, kVenues);
    REQUIRE(q.has_value());
    CHECK(q->sources[1].instrument == InstrumentId{3});
    CHECK(q->sources[1].invert);
  }
  SUBCASE("a settlement currency without a source is refused, naming it") {
    AccountingSpec s = usdt_spec();
    s.fx.clear();
    const auto q = build_fx_plan(t, s, kVenues);
    REQUIRE_FALSE(q.has_value());
    CHECK(q.error().find("BTCUSD settles in BTC") != std::string::npos);
    CHECK(q.error().find("no source for BTC") != std::string::npos);
  }
  SUBCASE("a source outside the table is refused") {
    AccountingSpec s = usdt_spec();
    s.fx["BTC"] = "a:BTCUSDT";  // it is on venue b
    const auto q = build_fx_plan(t, s, kVenues);
    REQUIRE_FALSE(q.has_value());
    CHECK(q.error().find("is not an instrument of venue a") != std::string::npos);
  }
  SUBCASE("a source that does not price the currency in the reporting one is refused") {
    AccountingSpec s = usdt_spec();
    s.fx["BTC"] = "a:ETHUSDT";
    const auto q = build_fx_plan(t, s, kVenues);
    REQUIRE_FALSE(q.has_value());
    CHECK(q.error().find("a source prices BTC in USDT") != std::string::npos);
  }
  SUBCASE("a disabled instrument needs no source, unless every instrument counts") {
    InstrumentTable u = make_table();
    Instrument sol = make("SOLBTC", "SOL", "EUR", 0);
    sol.flags = 0;
    REQUIRE(u.add(sol));
    const auto q = build_fx_plan(u, usdt_spec(), kVenues);
    REQUIRE(q.has_value());
    CHECK(q->count == 3);
    CHECK_FALSE(q->sources[2].instrument.valid());  // its rate is never known
    CHECK_FALSE(build_fx_plan(u, usdt_spec(), kVenues, /*all_instruments=*/true).has_value());
  }
  SUBCASE("no [accounting]: nothing converts") {
    const auto q = build_fx_plan(t, AccountingSpec{}, kVenues);
    REQUIRE(q.has_value());
    CHECK_FALSE(q->active());
  }
  SUBCASE("uncovered: an error with a limit reading the totals, a warning without") {
    AccountingSpec s = usdt_spec();
    s.fx.clear();
    std::string warning;
    CHECK_FALSE(session_fx_plan(t, s, kVenues, false, /*guarded=*/true, &warning).has_value());
    CHECK(warning.empty());
    const auto q = session_fx_plan(t, s, kVenues, false, /*guarded=*/false, &warning);
    REQUIRE(q.has_value());
    CHECK_FALSE(q->active());
    CHECK(warning.find("not converted") != std::string::npos);
  }
  SUBCASE("one settlement currency: nothing to convert") {
    InstrumentTable u;
    REQUIRE(u.add(make("ETHUSDT", "ETH", "USDT", 0)));
    REQUIRE(u.add(make("BTCUSDT", "BTC", "USDT", 1)));
    const auto q = build_fx_plan(u, usdt_spec(), kVenues);
    REQUIRE(q.has_value());
    CHECK_FALSE(q->active());
  }
}

TEST_CASE("core.fx: the tracker's totals are in the reporting currency, linear and inverse") {
  const InstrumentTable t = make_table();
  const FxPlan plan = usdt_plan(t);
  PositionTracker pos;
  pos.set_accounting(plan);
  REQUIRE(pos.converting());
  // ETHUSDT: +100 realized, 2 in fees, in USDT.
  pos.on_fill(kLinear, Side::Buy, px("3000"), qt("1"), nt("1"), t[kLinear]);
  pos.on_fill(kLinear, Side::Sell, px("3100"), qt("1"), nt("1"), t[kLinear]);
  // BTCUSD: 10 contracts of 100 USD bought at 50000, a fee of 0.00002 BTC, marked at 40000:
  // 1000 * (1/50000 - 1/40000) = -0.005 BTC, and 1000 / 40000 = 0.025 BTC of exposure.
  pos.on_fill(kInverse, Side::Buy, px("50000"), qt("10"), nt("0.00002"), t[kInverse]);
  pos.mark(kInverse, px("40000"), t[kInverse]);
  CHECK(pos.get(kInverse).unrealized == nt("-0.005"));
  CHECK(pos.native(1).unrealized == nt("-0.005"));
  CHECK(pos.native(1).gross == nt("0.025"));
  CHECK(pos.native(0).realized == nt("100"));

  // No BTC rate yet: the BTC totals are not counted.
  CHECK(pos.total_realized() == nt("100"));
  CHECK(pos.total_unrealized().is_zero());
  CHECK(pos.total_fees() == nt("2"));
  CHECK(pos.gross_exposure().is_zero());

  pos.set_rate(1, FxRate::from_mid(px("40000"), false));
  CHECK(pos.total_unrealized() == nt("-200"));
  CHECK(pos.total_fees() == nt("2.8"));
  CHECK(pos.gross_exposure() == nt("1000"));
  CHECK(pos.net_exposure() == nt("1000"));
  CHECK(pos.net_pnl() == nt("-102.8"));
  CHECK(pos.portfolio().net == nt("-102.8"));

  // The rate moves: everything in BTC is converted again.
  pos.set_rate(1, FxRate::from_mid(px("50000"), false));
  CHECK(pos.total_unrealized() == nt("-250"));
  CHECK(pos.gross_exposure() == nt("1250"));
  // Closing at 40000 realizes the BTC loss, in BTC.
  pos.on_fill(kInverse, Side::Sell, px("40000"), qt("10"), Notional{}, t[kInverse]);
  CHECK(pos.native(1).realized == nt("-0.005"));
  CHECK(pos.total_realized() == nt("-150"));  // 100 - 250
  CHECK(pos.total_unrealized().is_zero());
  CHECK(pos.gross_exposure().is_zero());

  // Without [accounting] the same fills add unrelated numbers, as before.
  PositionTracker raw;
  raw.on_fill(kLinear, Side::Buy, px("3000"), qt("1"), nt("1"), t[kLinear]);
  raw.on_fill(kLinear, Side::Sell, px("3100"), qt("1"), nt("1"), t[kLinear]);
  raw.on_fill(kInverse, Side::Buy, px("50000"), qt("10"), nt("0.00002"), t[kInverse]);
  raw.mark(kInverse, px("40000"), t[kInverse]);
  CHECK_FALSE(raw.converting());
  CHECK(raw.total_unrealized() == nt("-0.005"));
  CHECK(raw.total_fees() == nt("2.00002"));
}

TEST_CASE("core.fx: an unknown or stale rate refuses exposure in its currency, not a reduction") {
  const InstrumentTable t = make_table();
  const Instrument& inv = t[kInverse];
  const Timestamp now{seconds(100).ns};
  RiskLimits l;
  l.stale_md = milliseconds(500);
  l.max_gross_notional = nt("3000");  // USDT
  RiskEngine risk(l, now);
  risk.set_fx(usdt_plan(t));
  risk.on_book(kInverse, px("50000"), now);
  Position flat{};
  RiskInputs in{};
  in.now = now;
  in.position = &flat;
  OrderIntent buy{kInverse, inv.venue, Side::Buy, OrderType::Limit, px("50000"), qt("10")};
  OrderIntent sell = buy;
  sell.side = Side::Sell;

  // Never known: nothing adds to BTC exposure, in either direction.
  CHECK(risk.check_new(buy, inv, in) == RejectReason::FxRateUnknown);
  CHECK(risk.check_new(sell, inv, in) == RejectReason::FxRateUnknown);
  // A position is reduced whatever the rate.
  Position longp{};
  longp.qty = qt("10");
  RiskInputs reducing = in;
  reducing.position = &longp;
  CHECK(risk.check_new(sell, inv, reducing) == RejectReason::None);
  // A flatten (no position input) is not gated either.
  RiskInputs flatten = in;
  flatten.position = nullptr;
  CHECK(risk.check_new(buy, inv, flatten) == RejectReason::None);
  // USDT instruments need no rate.
  const OrderIntent eth{kLinear, VenueId{0}, Side::Buy, OrderType::Limit, px("100"), qt("1")};
  risk.on_book(kLinear, px("100"), now);
  CHECK(risk.check_new(eth, t[kLinear], in) == RejectReason::None);

  // Known: the order's 0.02 BTC is 1000 USDT against the cap; 0.08 BTC is 4000, over it.
  risk.on_book(kSource, px("50000"), now);
  risk.on_fx_book(1, true);
  CHECK(risk.fx_rate(1, now).price() == px("50000"));
  CHECK(risk.check_new(buy, inv, in) == RejectReason::None);
  OrderIntent big = buy;
  big.qty = qt("40");
  CHECK(risk.check_new(big, inv, in) == RejectReason::MaxGrossNotional);

  // Stale: the source's book is older than stale_md (the instrument's own is fresh).
  const Timestamp later = now + milliseconds(600);
  risk.on_book(kInverse, px("50000"), later);
  in.now = later;
  reducing.now = later;
  CHECK(risk.check_new(buy, inv, in) == RejectReason::FxRateUnknown);
  CHECK(risk.check_new(sell, inv, reducing) == RejectReason::None);
  risk.on_book(kSource, px("50000"), later);
  CHECK(risk.check_new(buy, inv, in) == RejectReason::None);

  // The source's book is gone (disconnected, crossed).
  risk.on_fx_book(1, false);
  CHECK(risk.check_new(buy, inv, in) == RejectReason::FxRateUnknown);
  CHECK(risk.check_new(sell, inv, reducing) == RejectReason::None);

  // Nothing reads the totals: the rate does not matter.
  RiskLimits none;
  risk.set_limits(none, later);
  CHECK(risk.check_new(buy, inv, in) == RejectReason::None);
  none.max_loss = nt("100");
  risk.set_limits(none, later);
  CHECK(risk.check_new(buy, inv, in) == RejectReason::FxRateUnknown);
}

TEST_CASE("core.fx: max_loss trips on a BTC loss that only crosses the USDT budget converted") {
  const InstrumentTable t = make_table();
  PositionTracker pos;
  pos.set_accounting(usdt_plan(t));
  RiskLimits l;
  l.max_loss = nt("150");
  RiskEngine risk(l, Timestamp{});
  pos.on_fill(kInverse, Side::Buy, px("50000"), qt("10"), Notional{}, t[kInverse]);
  pos.mark(kInverse, px("40000"), t[kInverse]);       // -0.005 BTC
  CHECK_FALSE(risk.on_pnl(pos.native(1).net_pnl()));  // the BTC number alone: no
  CHECK_FALSE(risk.on_pnl(pos.net_pnl()));            // no rate yet: nothing counted
  pos.set_rate(1, FxRate::from_mid(px("20000"), false));
  CHECK(pos.net_pnl() == nt("-100"));
  CHECK_FALSE(risk.on_pnl(pos.net_pnl()));
  pos.set_rate(1, FxRate::from_mid(px("30000"), false));  // BTC rallies: the loss is 150 USDT
  CHECK(risk.on_pnl(pos.net_pnl()));
  CHECK(risk.killed());
}

// ---- the engine --------------------------------------------------------------------------------

namespace {

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
static_assert(verify_strategy<Idle>());

struct Rig {
  using E = Engine<Idle, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Idle strategy;
  std::unique_ptr<E> engine;
  std::uint32_t execs = 0;

  explicit Rig(const EngineConfig& base) {
    EngineConfig cfg = base;
    cfg.fx = usdt_plan(table);
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy);
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
  RejectReason order(InstrumentId id, Side side, const char* price, const char* qty) {
    NewOrderRequest r{};
    r.instrument = id;
    r.side = side;
    r.price = px(price);
    r.qty = qt(qty);
    const auto res = engine->context().send(r);
    return res ? RejectReason::None : res.error();
  }
};

}  // namespace

TEST_CASE("core.fx: the engine's orders, PnL totals and max_loss use converted values") {
  EngineConfig cfg;
  cfg.risk.max_loss = nt("150");
  cfg.risk.max_gross_notional = nt("3000");
  cfg.risk.stale_md = seconds(5);
  Rig r(cfg);
  r.book(kInverse, "49999.5", "50000.5");

  // No BTC rate yet: BTC exposure is refused; USDT trades on.
  CHECK(r.order(kInverse, Side::Buy, "49000", "10") == RejectReason::FxRateUnknown);
  r.book(kLinear, "2999.99", "3000.01");
  CHECK(r.order(kLinear, Side::Buy, "2990", "0.1") == RejectReason::None);

  // BTCUSDT's book prices BTC: 10 contracts at 49000 are 0.0204 BTC, 1020 USDT; 40 are 4081.
  r.book(kSource, "49999.99", "50000.01");
  CHECK(r.order(kInverse, Side::Buy, "49000", "10") == RejectReason::None);
  CHECK(r.order(kInverse, Side::Buy, "49000", "40") == RejectReason::MaxGrossNotional);

  // A position: 10 contracts long, and USDT PnL of +100 less 2 in fees.
  r.fill(kInverse, Side::Buy, "50000", "10", "0");
  r.fill(kLinear, Side::Buy, "3000", "1", "1");
  r.fill(kLinear, Side::Sell, "3100", "1", "1");
  CHECK(r.engine->positions().total_realized() == nt("100"));
  CHECK(r.engine->positions().total_fees() == nt("2"));

  // BTCUSDT's venue drops its market data: adding is refused, reducing passes.
  ConnectionStateMsg down{};
  init_header(down, EventType::ConnectionState, InstrumentId::invalid(), VenueId{1});
  down.state = ConnState::Disconnected;
  down.channel = 0;
  r.push(down);
  CHECK(r.order(kInverse, Side::Buy, "49000", "1") == RejectReason::FxRateUnknown);
  CHECK(r.order(kInverse, Side::Sell, "51000", "1") == RejectReason::None);
  r.book(kSource, "49999.99", "50000.01");
  CHECK(r.order(kInverse, Side::Buy, "49000", "1") == RejectReason::None);

  // Stale: BTCUSD's own book stays fresh, BTCUSDT's does not.
  r.clock.advance(seconds(6));
  r.book(kInverse, "49999.5", "50000.5");
  CHECK(r.order(kInverse, Side::Buy, "49000", "1") == RejectReason::FxRateUnknown);
  CHECK(r.order(kInverse, Side::Sell, "51000", "1") == RejectReason::None);
  r.book(kSource, "49999.99", "50000.01");
  CHECK(r.order(kInverse, Side::Buy, "49000", "1") == RejectReason::None);

  // BTCUSD falls to 45000: -0.00222222 BTC, -111.11 USDT at 50000; net -13.11, inside the budget.
  r.book(kInverse, "44999.5", "45000.5");
  CHECK(r.engine->position(kInverse).unrealized == nt("-0.00222222"));
  CHECK(r.engine->positions().total_unrealized() == nt("-111.111"));
  CHECK(r.engine->net_pnl() == nt("-13.111"));
  CHECK(r.engine->kill_reason() == KillReason::None);
  // BTC rallies against USD elsewhere: the same BTC loss is 266.67 USDT and the budget is spent.
  r.book(kSource, "119999.99", "120000.01");
  CHECK(r.engine->positions().total_unrealized() == nt("-266.6664"));
  CHECK(r.engine->kill_reason() == KillReason::MaxLoss);
  CHECK(r.engine->risk().killed());
  // The runner's totals, which the status file and the logs show, are converted too.
  const RunnerStats rs = r.engine->runner_stats();
  CHECK(rs.realized_pnl_raw == nt("100").raw);
  CHECK(rs.unrealized_pnl_raw == nt("-266.6664").raw);
}
