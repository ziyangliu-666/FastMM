// Net position per underlying ([risk.underlying], core/underlying.hpp): the plan, the conversion to
// base units, RiskEngine's check and the engine gathering positions and open orders across venues.
#include "fastmm/core/underlying.hpp"

#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
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

constexpr InstrumentId kUsdm{0};     // BTCUSDT on venue 0, 1 BTC per contract
constexpr InstrumentId kBybit{1};    // BTCUSDT on venue 1
constexpr InstrumentId kOkx{2};      // BTC-USDT-SWAP on venue 2, 0.01 BTC per contract
constexpr InstrumentId kInverse{3};  // BTCUSD on venue 3, inverse, 100 USD per contract
constexpr InstrumentId kEth{4};      // ETHUSDT on venue 0
constexpr InstrumentId kOption{5};   // a BTC option on venue 3, premium in BTC

Instrument make(const char* symbol, const char* base, std::uint8_t venue) {
  Instrument i{};
  i.symbol = symbol;
  i.base = base;
  i.quote = "USDT";
  i.venue = VenueId{venue};
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = px("0.1");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  return i;
}

InstrumentTable make_table() {
  InstrumentTable t;
  REQUIRE(t.add(make("BTCUSDT", "BTC", 0)));
  REQUIRE(t.add(make("BTCUSDT", "btc", 1)));  // names compare without case
  Instrument okx = make("BTC-USDT-SWAP", "BTC", 2);
  okx.lot = qt("1");
  okx.min_qty = qt("1");
  okx.contract_multiplier = qt("0.01");
  REQUIRE(t.add(okx));
  Instrument inv = make("BTCUSD", "BTC", 3);
  inv.quote = "USD";
  inv.flags = Instrument::kEnabled | Instrument::kInverse;
  inv.tick = px("0.5");
  inv.lot = qt("1");
  inv.min_qty = qt("1");
  inv.contract_multiplier = qt("100");
  REQUIRE(t.add(inv));
  REQUIRE(t.add(make("ETHUSDT", "ETH", 0)));
  Instrument opt = make("BTC-27DEC26-60000-C", "BTC", 3);
  opt.asset_class = AssetClass::Option;
  opt.flags = Instrument::kEnabled | Instrument::kCoinQuoted;
  opt.tick = px("0.0005");
  opt.lot = qt("0.1");
  opt.min_qty = qt("0.1");
  REQUIRE(t.add(opt));
  return t;
}

UnderlyingSpec spec(const char* btc) {
  UnderlyingSpec s;
  s.max_net["BTC"] = btc;
  return s;
}

UnderlyingPlan plan_of(const InstrumentTable& t, const UnderlyingSpec& s) {
  auto p = build_underlying_plan(t, s);
  REQUIRE_MESSAGE(p.has_value(), p.error());
  return *p;
}

}  // namespace

TEST_CASE("core.underlying: the plan groups the instruments of each base asset, options excluded") {
  const InstrumentTable t = make_table();
  UnderlyingSpec s = spec("0.5");
  s.max_net["eth"] = "0";
  const UnderlyingPlan p = plan_of(t, s);
  REQUIRE(p.count == 2);
  const int btc = p.find("btc");
  const int eth = p.find("ETH");
  REQUIRE(btc >= 0);
  REQUIRE(eth >= 0);
  CHECK(p.max_net[static_cast<std::size_t>(btc)] == qt("0.5"));
  CHECK(p.max_net[static_cast<std::size_t>(eth)].is_zero());
  const auto members = p.instruments(static_cast<std::size_t>(btc));
  CHECK(std::vector<InstrumentId>(members.begin(), members.end()) ==
        std::vector<InstrumentId>{kUsdm, kBybit, kOkx, kInverse});
  CHECK(p.underlying_of(kOption) == -1);
  CHECK(p.underlying_of(kEth) == eth);
  CHECK(p.underlying_of(kBybit) == btc);

  CHECK_FALSE(build_underlying_plan(t, UnderlyingSpec{})->active());

  UnderlyingSpec none;
  none.max_net["SOL"] = "10";
  const auto e1 = build_underlying_plan(t, none, "gateway");
  REQUIRE_FALSE(e1.has_value());
  CHECK(e1.error() == "[gateway.underlying.SOL]: no instrument of this session has base SOL");

  InstrumentTable options;
  Instrument opt = t[kOption];
  REQUIRE(options.add(opt));
  const auto e2 = build_underlying_plan(options, spec("1"));
  REQUIRE_FALSE(e2.has_value());
  CHECK(e2.error().find("other than options, which do not count") != std::string::npos);

  const auto e3 = build_underlying_plan(t, spec("-1"));
  REQUIRE_FALSE(e3.has_value());
  CHECK(e3.error().find("not a non-negative decimal") != std::string::npos);
}

TEST_CASE("core.underlying: contracts convert to base units, an inverse one at the mark") {
  const InstrumentTable t = make_table();
  std::int64_t b = 0;
  CHECK(to_base_units(t[kUsdm], qt("0.3").raw, Price{}, b));
  CHECK(b == qt("0.3").raw);
  // 30 OKX contracts of 0.01 BTC.
  CHECK(to_base_units(t[kOkx], qt("-30").raw, Price{}, b));
  CHECK(b == qt("-0.3").raw);
  // 150 contracts of 100 USD at 50000 USD per BTC: 0.3 BTC. None without a mark.
  CHECK(to_base_units(t[kInverse], qt("150").raw, px("50000"), b));
  CHECK(b == qt("0.3").raw);
  CHECK_FALSE(to_base_units(t[kInverse], qt("150").raw, Price{}, b));
  CHECK(to_base_units(t[kInverse], 0, Price{}, b));  // nothing held needs no mark
  CHECK(b == 0);

  // Refused only past the limit and further from zero than now.
  const std::int64_t max = qt("0.5").raw;
  CHECK_FALSE(underlying_exceeds(max, 0, qt("0.5").raw));
  CHECK(underlying_exceeds(max, 0, qt("-0.6").raw));
  CHECK_FALSE(underlying_exceeds(max, qt("0.8").raw, qt("0.6").raw));
  CHECK(underlying_exceeds(max, qt("0.8").raw, qt("-0.9").raw));
  CHECK_FALSE(underlying_exceeds(0, 0, qt("100").raw));
}

TEST_CASE("core.underlying: RiskEngine counts open orders, excludes a replaced order's leaves") {
  const InstrumentTable t = make_table();
  const Timestamp now{seconds(100).ns};
  RiskEngine risk(RiskLimits{}, now);
  risk.set_underlying(plan_of(t, spec("0.5")));
  CHECK(risk.underlying_on());
  Position flat{};
  const auto inputs =
      [&](InstrumentId id, const std::vector<Qty>& pos, const std::vector<Qty>& open) {
        return risk.underlying_inputs(
            id,
            t,
            now,
            [&](InstrumentId j) { return pos[j.value]; },
            [&](InstrumentId j) { return open[j.value]; });
      };
  const std::vector<Qty> none(6, Qty{});
  RiskInputs in{};
  in.now = now;
  in.position = &flat;
  in.underlying = inputs(kOkx, none, none);
  CHECK(in.underlying.limited);
  OrderIntent buy{kOkx, VenueId{2}, Side::Buy, OrderType::Limit, px("50000"), qt("50")};
  CHECK(risk.check_new(buy, t[kOkx], in) == RejectReason::None);  // 0.5 BTC
  buy.qty = qt("51");
  CHECK(risk.check_new(buy, t[kOkx], in) == RejectReason::MaxUnderlyingNet);

  // 0.3 BTC of buys working on USD-M: 21 OKX contracts more is 0.51.
  std::vector<Qty> open = none;
  open[kUsdm.value] = qt("0.3");
  in.underlying = inputs(kOkx, none, open);
  buy.qty = qt("20");
  CHECK(risk.check_new(buy, t[kOkx], in) == RejectReason::None);
  buy.qty = qt("21");
  CHECK(risk.check_new(buy, t[kOkx], in) == RejectReason::MaxUnderlyingNet);
  // A replace of a working 10-contract OKX buy (counted in `open`) to 30 contracts: 0.3 + 0.1 +
  // 0.3 - 0.1 = 0.6 with it counted twice; 0.5 without.
  open[kOkx.value] = qt("10");
  in.underlying = inputs(kOkx, none, open);
  Order working{};
  working.qty = qt("10");
  buy.qty = qt("20");
  CHECK(risk.check_replace(buy, working, t[kOkx], in) == RejectReason::None);
  buy.qty = qt("21");
  CHECK(risk.check_replace(buy, working, t[kOkx], in) == RejectReason::MaxUnderlyingNet);

  // A flatten passes no position: not checked.
  RiskInputs flatten = in;
  flatten.position = nullptr;
  buy.qty = qt("1000");
  CHECK(risk.check_new(buy, t[kOkx], flatten) == RejectReason::None);

  // The inverse contract: without a mark its own orders cannot be measured; with no position or
  // orders it does not stop the others.
  OrderIntent inv{kInverse, VenueId{3}, Side::Sell, OrderType::Limit, px("50000"), qt("100")};
  in.underlying = inputs(kInverse, none, none);
  CHECK(risk.check_new(inv, t[kInverse], in) == RejectReason::UnderlyingMarkUnknown);
  in.underlying = inputs(kUsdm, none, none);
  OrderIntent usdm{kUsdm, VenueId{0}, Side::Buy, OrderType::Limit, px("50000"), qt("0.1")};
  CHECK(risk.check_new(usdm, t[kUsdm], in) == RejectReason::None);
  // Holding 150 contracts short (-0.3 BTC at 50000) with no mark: nothing on BTC can be measured.
  std::vector<Qty> pos = none;
  pos[kInverse.value] = qt("-150");
  in.underlying = inputs(kUsdm, pos, none);
  CHECK_FALSE(in.underlying.known);
  CHECK(risk.check_new(usdm, t[kUsdm], in) == RejectReason::UnderlyingMarkUnknown);
  risk.on_book(kInverse, px("50000"), now);
  in.underlying = inputs(kUsdm, pos, none);
  CHECK(in.underlying.net == qt("-0.3").raw);
  usdm.qty = qt("0.8");  // -0.3 + 0.8 = 0.5
  CHECK(risk.check_new(usdm, t[kUsdm], in) == RejectReason::None);
  usdm.qty = qt("0.801");
  CHECK(risk.check_new(usdm, t[kUsdm], in) == RejectReason::MaxUnderlyingNet);
  // A stale mark is no mark.
  RiskLimits stale;
  stale.stale_md = milliseconds(500);
  risk.set_limits(stale, now);
  risk.on_book(kUsdm, px("50000"), now + milliseconds(600));
  const Timestamp later = now + milliseconds(600);
  in.now = later;
  in.underlying = risk.underlying_inputs(
      kUsdm,
      t,
      later,
      [&](InstrumentId j) { return pos[j.value]; },
      [&](InstrumentId j) { return none[j.value]; });
  CHECK(risk.check_new(usdm, t[kUsdm], in) == RejectReason::UnderlyingMarkUnknown);

  // Other base assets and options are not limited.
  in.underlying = inputs(kEth, none, none);
  CHECK_FALSE(in.underlying.limited);
  in.underlying = inputs(kOption, none, none);
  CHECK_FALSE(in.underlying.limited);

  // The limit changes at run time; 0 lifts it.
  CHECK(risk.set_underlying_limit(0, Qty{}));
  CHECK_FALSE(risk.underlying_on());
  CHECK(risk.set_underlying_limit(0, qt("0.2")));
  CHECK(risk.underlying_limit(0) == qt("0.2"));
  CHECK_FALSE(risk.set_underlying_limit(1, qt("1")));
  CHECK_FALSE(risk.set_underlying_limit(0, qt("-1")));
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

  explicit Rig(const EngineConfig& base, const UnderlyingSpec& s) {
    EngineConfig cfg = base;
    cfg.underlying = plan_of(table, s);
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
  void fill(InstrumentId id, Side side, const char* price, const char* qty) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, id, table.get(id).venue);
    m.cl_ord_id = ClientOrderId{0xF00D};
    m.side = side;
    m.price = px(price);
    m.qty = qt(qty);
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
  EngineLiveStats::Underlying published(std::size_t u) {
    ControlMsg flush{};
    init_header(flush, EventType::Control);
    flush.command = ControlCommand::FlushStats;
    push(flush);
    return engine->live_stats().underlyings[u];
  }
};

}  // namespace

TEST_CASE("core.underlying: the engine nets BTC across venues and contract sizes") {
  EngineConfig cfg;
  Rig r(cfg, spec("0.5"));
  for (const InstrumentId id : {kUsdm, kBybit, kOkx}) r.book(id, "49999.9", "50000.1");

  // Long 0.3 on USD-M, short 0.3 on Bybit: flat in BTC, so the whole 0.5 is available on OKX.
  r.fill(kUsdm, Side::Buy, "50000", "0.3");
  r.fill(kBybit, Side::Sell, "50000", "0.3");
  CHECK(r.published(0).net_raw == 0);
  CHECK(r.order(kOkx, Side::Buy, "49000", "51") == RejectReason::MaxUnderlyingNet);
  CHECK(r.order(kOkx, Side::Buy, "49000", "50") == RejectReason::None);
  // The 50 contracts (0.5 BTC) now work on the buy side, on every venue's account.
  CHECK(r.order(kUsdm, Side::Buy, "49000", "0.001") == RejectReason::MaxUnderlyingNet);
  // Sells are measured against the sells working: none.
  CHECK(r.order(kBybit, Side::Sell, "51000", "0.5") == RejectReason::None);
  CHECK(r.order(kBybit, Side::Sell, "51000", "0.001") == RejectReason::MaxUnderlyingNet);
  // ETH has no limit.
  r.book(kEth, "2999.9", "3000.1");
  CHECK(r.order(kEth, Side::Buy, "2990", "100") == RejectReason::None);

  // Fills are booked whatever the limit: 0.3 - 0.3 + 0.6 + 20 OKX contracts is 0.8 BTC.
  r.fill(kUsdm, Side::Buy, "50000", "0.6");
  r.fill(kOkx, Side::Buy, "50000", "20");
  CHECK(r.published(0).net_raw == qt("0.8").raw);
  CHECK(r.engine->stats().risk_rejects_by_reason[RejectReason::MaxUnderlyingNet] == 3);
}

TEST_CASE("core.underlying: an order that reduces the net passes, over the limit or not") {
  EngineConfig cfg;
  Rig r(cfg, spec("0.5"));
  for (const InstrumentId id : {kUsdm, kBybit}) r.book(id, "49999.9", "50000.1");
  r.fill(kUsdm, Side::Buy, "50000", "0.8");
  CHECK(r.order(kBybit, Side::Buy, "49000", "0.001") == RejectReason::MaxUnderlyingNet);
  CHECK(r.order(kBybit, Side::Sell, "51000", "0.2") == RejectReason::None);  // 0.6: closer
  CHECK(r.order(kBybit, Side::Sell, "51000", "0.9") == RejectReason::None);  // -0.3
  // With every sell working filled: -0.8 is over the limit but no further from zero than the
  // position is; -0.801 is.
  CHECK(r.order(kBybit, Side::Sell, "51000", "0.5") == RejectReason::None);
  CHECK(r.order(kBybit, Side::Sell, "51000", "0.001") == RejectReason::MaxUnderlyingNet);
}

TEST_CASE("core.underlying: an inverse contract counts at the mark and fails closed without one") {
  EngineConfig cfg;
  cfg.risk.stale_md = seconds(5);
  Rig r(cfg, spec("0.5"));
  r.book(kUsdm, "49999.9", "50000.1");
  // No book on BTCUSD yet, and nothing held or working there: the rest trade on.
  CHECK(r.order(kUsdm, Side::Buy, "49000", "0.1") == RejectReason::None);

  r.book(kInverse, "49999.5", "50000.5");
  // Short 150 contracts of 100 USD at a 50000 mark: -0.3 BTC. With 0.1 BTC of USD-M buys working,
  // 0.7 more is 0.5.
  r.fill(kInverse, Side::Sell, "50000", "150");
  const auto u = r.published(0);
  CHECK(u.known);
  CHECK(u.net_raw == qt("-0.3").raw);
  CHECK(r.order(kUsdm, Side::Buy, "49000", "0.701") == RejectReason::MaxUnderlyingNet);
  CHECK(r.order(kUsdm, Side::Buy, "49000", "0.7") == RejectReason::None);
  // The mark moves: at 40000 the same contracts are -0.375 BTC.
  r.book(kInverse, "39999.5", "40000.5");
  CHECK(r.published(0).net_raw == qt("-0.375").raw);

  // BTCUSD's book goes stale: nothing on BTC can be measured, on any venue.
  r.clock.advance(seconds(6));
  r.book(kUsdm, "49999.9", "50000.1");
  CHECK_FALSE(r.published(0).known);
  CHECK(r.order(kUsdm, Side::Sell, "51000", "0.001") == RejectReason::UnderlyingMarkUnknown);
  r.book(kInverse, "39999.5", "40000.5");
  CHECK(r.order(kUsdm, Side::Sell, "51000", "0.001") == RejectReason::None);
}

TEST_CASE("core.underlying: SetUnderlyingLimit changes the limit at run time") {
  EngineConfig cfg;
  Rig r(cfg, spec("0"));  // tracked, no limit
  r.book(kUsdm, "49999.9", "50000.1");
  CHECK_FALSE(r.engine->risk().underlying_on());
  CHECK(r.order(kUsdm, Side::Buy, "49000", "2") == RejectReason::None);
  CHECK(r.published(0).max_net_raw == 0);

  ControlUnderlyingMsg m{};
  init_header(m, EventType::Control);
  m.command = ControlCommand::SetUnderlyingLimit;
  m.underlying = 0;
  m.name = UnderlyingName("BTC");
  m.arg = static_cast<std::uint64_t>(qt("1").raw);
  r.push(m);
  CHECK(r.engine->risk().underlying_limit(0) == qt("1"));
  CHECK(r.published(0).max_net_raw == qt("1").raw);
  CHECK(r.order(kUsdm, Side::Buy, "49000", "0.001") == RejectReason::MaxUnderlyingNet);
  // An index the session does not have changes nothing.
  m.underlying = 3;
  m.arg = 0;
  r.push(m);
  CHECK(r.engine->risk().underlying_limit(0) == qt("1"));
}

TEST_CASE("core.underlying: without a plan the engine checks nothing more") {
  EngineConfig cfg;
  Rig r(cfg, UnderlyingSpec{});
  CHECK_FALSE(r.engine->risk().underlying_on());
  r.book(kUsdm, "49999.9", "50000.1");
  CHECK(r.order(kUsdm, Side::Buy, "49000", "100") == RejectReason::None);
  CHECK(r.engine->risk_headroom(kUsdm).underlying_buy_qty == Qty::max());
  static_cast<void>(nt);
}

TEST_CASE("core.underlying: risk_headroom reports what the net limit admits, in contracts") {
  EngineConfig cfg;
  cfg.risk.stale_md = seconds(5);
  Rig r(cfg, spec("0.5"));
  for (const InstrumentId id : {kUsdm, kOkx}) r.book(id, "49999.9", "50000.1");
  CHECK(r.engine->risk_headroom(kEth).underlying_buy_qty == Qty::max());  // ETH has no limit

  // Long 0.2 BTC on USD-M: 0.3 more, which on OKX is 30 contracts of 0.01; selling may go through
  // zero to -0.5, 0.7 BTC or 70 contracts.
  r.fill(kUsdm, Side::Buy, "50000", "0.2");
  RiskHeadroom h = r.engine->risk_headroom(kOkx);
  CHECK(h.underlying_buy_qty == qt("30"));
  CHECK(h.underlying_sell_qty == qt("70"));
  CHECK(r.order(kOkx, Side::Buy, "49000", "31") == RejectReason::MaxUnderlyingNet);
  CHECK(r.order(kOkx, Side::Buy, "49000", "30") == RejectReason::None);
  // The 30 contracts now work on the buy side.
  h = r.engine->risk_headroom(kUsdm);
  CHECK(h.underlying_buy_qty == Qty{});
  CHECK(h.underlying_sell_qty == qt("0.7"));

  // An inverse contract at 50000: 0.7 BTC is 350 contracts of 100 USD; none without its mark.
  CHECK(r.engine->risk_headroom(kInverse).underlying_sell_qty == Qty{});
  r.book(kInverse, "49999.5", "50000.5");
  CHECK(r.engine->risk_headroom(kInverse).underlying_sell_qty == qt("350"));
  CHECK(r.order(kInverse, Side::Sell, "50000", "351") == RejectReason::MaxUnderlyingNet);
  CHECK(r.order(kInverse, Side::Sell, "50000", "350") == RejectReason::None);
}
