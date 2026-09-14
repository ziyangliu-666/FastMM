// OptionsMM against a fake strategy context: model price and spread, delta and inventory skew,
// delta / vega / position limits, vega widening, contract multipliers, inverse futures delta, own
// smoothed IV, expiry filter, stale pulls and requote gating. Deterministic (no clock, no I/O).
#include "fastmm/strategies/options_mm.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/registry.hpp"

#include <array>
#include <cmath>
#include <vector>

using namespace fastmm;
using namespace fastmm::options;

namespace {

constexpr std::int64_t kNow = 1'789'344'931'096LL * 1'000'000;  // 2026-09-13T22:15:31Z
constexpr std::int64_t kDay = 86'400LL * 1'000'000'000;

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

struct FakeBook {
  Price bid{};
  Price ask{};
  bool valid = false;
  [[nodiscard]] bool is_valid() const { return valid; }
  [[nodiscard]] Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  [[nodiscard]] Level best_bid() const { return Level{bid, qt("1")}; }
  [[nodiscard]] Level best_ask() const { return Level{ask, qt("1")}; }
  [[nodiscard]] Timestamp last_update() const { return Timestamp{}; }
};
struct FakePosition {
  Qty qty{};
};

// Ids: 0 BTC call K=77000 (inverse), 1 BTC put K=77000 (inverse), 2 BTC-PERPETUAL (inverse,
// 10 USD contracts), 3 linear call K=100 with a multiplier of 10.
struct Ctx {
  InstrumentTable table;
  std::vector<FakeBook> books = std::vector<FakeBook>(8);
  std::array<Qty, 8> pos{};
  Timestamp t{kNow};
  std::vector<std::pair<InstrumentId, DesiredQuotes>> sets;
  std::vector<InstrumentId> pulls;

  explicit Ctx(std::int64_t expiry_ns = kNow + 30 * kDay) {
    auto option =
        [&](const char* sym, OptionType type, const char* strike, bool inverse, const char* mult) {
          Instrument i{};
          i.symbol = sym;
          i.venue = VenueId{0};
          i.asset_class = AssetClass::Option;
          i.option_type = type;
          i.strike = px(strike);
          i.expiry_ns = expiry_ns;
          i.flags = static_cast<std::uint8_t>(Instrument::kEnabled |
                                              (inverse ? Instrument::kInverse : 0));
          i.tick = inverse ? px("0.0001") : px("0.01");
          i.lot = qt("0.1");
          i.contract_multiplier = qt(mult);
          REQUIRE(table.add(i));
        };
    option("BTC-C", OptionType::Call, "77000", true, "1");
    option("BTC-P", OptionType::Put, "77000", true, "1");
    Instrument perp{};
    perp.symbol = "BTC-PERPETUAL";
    perp.venue = VenueId{0};
    perp.asset_class = AssetClass::Perpetual;
    perp.flags = Instrument::kEnabled | Instrument::kInverse;
    perp.tick = px("0.5");
    perp.lot = qt("1");
    perp.contract_multiplier = qt("10");
    REQUIRE(table.add(perp));
    option("LIN-C", OptionType::Call, "100", false, "10");
    // Two-sided books far from the model prices, so the touch clamp never moves a quote.
    books[0] = FakeBook{px("0.0001"), px("0.9"), true};
    books[1] = FakeBook{px("0.0001"), px("0.9"), true};
    books[2] = FakeBook{px("76899.5"), px("76900.5"), true};
    books[3] = FakeBook{px("0.01"), px("1000"), true};
  }
  [[nodiscard]] const InstrumentTable& instruments() const { return table; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table.get(id); }
  [[nodiscard]] const FakeBook& book(InstrumentId id) const { return books[id.value]; }
  [[nodiscard]] FakePosition position(InstrumentId id) const { return FakePosition{pos[id.value]}; }
  [[nodiscard]] Timestamp now() const { return t; }
  void set_quotes(InstrumentId id, const DesiredQuotes& q) { sets.emplace_back(id, q); }
  void pull_quotes(InstrumentId id) { pulls.push_back(id); }
  TimerId add_timer(Duration, bool, std::uint64_t) { return TimerId{1}; }
  [[nodiscard]] const DesiredQuotes* last(InstrumentId id) const {
    for (auto it = sets.rbegin(); it != sets.rend(); ++it) {
      if (it->first == id) return &it->second;
    }
    return nullptr;
  }
};

OptionTickerMsg ticker(InstrumentId id, const char* underlying, double iv, double rate = 0.0) {
  OptionTickerMsg m{};
  init_header(m, EventType::OptionTicker, id, VenueId{0});
  m.underlying_price = px(underlying);
  m.mark_iv = iv;
  m.bid_iv = std::nan("");
  m.ask_iv = std::nan("");
  m.interest_rate = rate;
  return m;
}

OptionsMM make(const ParamMap& extra) {
  ParamMap p{{"half_spread_vol", "1"},
             {"min_half_spread_ticks", "1"},
             {"quote_qty", "0.1"},
             {"max_position", "0"},
             {"max_delta", "0"},
             {"max_vega", "0"},
             {"delta_skew_ticks", "0"},
             {"inventory_skew_ticks", "0"},
             {"vega_widen", "0"},
             {"min_expiry_s", "3600"},
             {"requote_threshold_ticks", "1"},
             {"pull_on_stale_ms", "0"}};
  for (const auto& [k, v] : extra) p[k] = v;
  OptionsMM s;
  REQUIRE_FALSE(s.configure(p));
  return s;
}

double years() {
  return year_fraction(kNow + 30 * kDay, kNow);
}

}  // namespace

TEST_CASE("strategies.options_mm: theo, vega spread and passive rounding (inverse call)") {
  Ctx ctx;
  OptionsMM s = make({});
  s.on_start(ctx);
  s.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  const DesiredQuotes* q = ctx.last(InstrumentId{0});
  REQUIRE(q != nullptr);
  REQUIRE(q->bids.size() == 1);
  REQUIRE(q->asks.size() == 1);
  const Greeks g = black76(CallPut::Call, 76904.4, 77000.0, years(), 0.5);
  const double theo = g.price / 76904.4;
  const double half = g.vega / 100.0 / 76904.4;  // one vol point in BTC
  CHECK(q->bids[0].price ==
        Price::from_raw(static_cast<std::int64_t>(std::floor((theo - half) / 0.0001)) * 10'000));
  CHECK(q->asks[0].price ==
        Price::from_raw(static_cast<std::int64_t>(std::ceil((theo + half) / 0.0001)) * 10'000));
  CHECK(q->bids[0].qty == qt("0.1"));
  const OptionsMM::State& st = s.state(InstrumentId{0});
  CHECK(st.theo == doctest::Approx(theo));
  CHECK(st.delta == doctest::Approx(g.delta - theo));  // premium-adjusted coin delta
  CHECK(st.vega == doctest::Approx(g.vega / 100.0));
  // Same inputs: gated, no new set_quotes; a theo move of at least a tick requotes.
  s.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  CHECK(ctx.sets.size() == 1);
  s.on_option_ticker(ctx, ticker(InstrumentId{0}, "77500", 0.5));
  CHECK(ctx.sets.size() == 2);
}

TEST_CASE("strategies.options_mm: portfolio delta skews calls down and puts up") {
  const auto run = [](const char* call_position) {
    Ctx ctx;
    OptionsMM s = make({{"max_delta", "1"}, {"delta_skew_ticks", "20"}});
    s.on_start(ctx);
    ctx.pos[0] = qt(call_position);
    s.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
    s.on_option_ticker(ctx, ticker(InstrumentId{1}, "76904.4", 0.5));
    s.on_fill(ctx, OmsUpdate{}, OrderFillMsg{});  // exposure now includes the call's greeks
    return std::make_pair(*ctx.last(InstrumentId{0}), *ctx.last(InstrumentId{1}));
  };
  const auto [flat_call, flat_put] = run("0");
  const auto [long_call, long_put] = run("1");  // ~+0.47 BTC of delta
  REQUIRE_FALSE(long_call.asks.empty());
  REQUIRE_FALSE(flat_call.asks.empty());
  CHECK(long_call.asks[0].price < flat_call.asks[0].price);
  REQUIRE_FALSE(long_put.bids.empty());
  REQUIRE_FALSE(flat_put.bids.empty());
  CHECK(long_put.bids[0].price > flat_put.bids[0].price);
}

TEST_CASE("strategies.options_mm: inventory skew and per-option position cap") {
  Ctx ctx;
  OptionsMM s = make({{"inventory_skew_ticks", "5"}, {"max_position", "0.2"}});
  s.on_start(ctx);
  s.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  const DesiredQuotes flat = *ctx.last(InstrumentId{0});
  ctx.pos[0] = qt("0.2");  // two quote units long, at the cap
  s.on_fill(ctx, OmsUpdate{}, OrderFillMsg{});
  const DesiredQuotes& longq = *ctx.last(InstrumentId{0});
  CHECK(longq.bids.empty());  // buying 0.1 more would exceed max_position
  REQUIRE_FALSE(longq.asks.empty());
  CHECK(longq.asks[0].price == flat.asks[0].price - px("0.001"));  // 2 units * 5 ticks lower
}

TEST_CASE("strategies.options_mm: max delta suppresses the side that adds delta") {
  Ctx ctx;
  OptionsMM s = make({{"max_delta", "0.3"}});
  s.on_start(ctx);
  ctx.pos[0] = qt("1");  // long call: ~0.47 BTC delta > 0.3
  s.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  s.on_option_ticker(ctx, ticker(InstrumentId{1}, "76904.4", 0.5));
  s.on_fill(ctx, OmsUpdate{}, OrderFillMsg{});
  const OptionsMM::Exposure e = s.exposure(ctx);
  CHECK(e.delta == doctest::Approx(s.state(InstrumentId{0}).delta));
  const DesiredQuotes& call = *ctx.last(InstrumentId{0});
  const DesiredQuotes& put = *ctx.last(InstrumentId{1});
  CHECK(call.bids.empty());        // buying calls adds delta
  CHECK_FALSE(call.asks.empty());  // selling calls reduces it
  CHECK_FALSE(put.bids.empty());   // buying puts reduces it
  CHECK(put.asks.empty());         // selling puts adds delta
}

TEST_CASE("strategies.options_mm: vega exposure widens the spread and caps buying") {
  Ctx ctx;
  OptionsMM base = make({{"max_vega", "1000"}, {"vega_widen", "1"}});
  base.on_start(ctx);
  base.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  const DesiredQuotes flat = *ctx.last(InstrumentId{0});
  const double contract_vega = base.state(InstrumentId{0}).vega;  // USD per vol point
  REQUIRE(contract_vega > 10.0);

  Ctx ctx2;
  ctx2.pos[1] = Qty::from_double(1000.0 / contract_vega * 0.5);  // half the vega limit in puts
  OptionsMM s = make({{"max_vega", "1000"}, {"vega_widen", "1"}});
  s.on_start(ctx2);
  s.on_option_ticker(ctx2, ticker(InstrumentId{1}, "76904.4", 0.5));
  s.on_option_ticker(ctx2, ticker(InstrumentId{0}, "76904.4", 0.5));
  s.on_fill(ctx2, OmsUpdate{}, OrderFillMsg{});
  const DesiredQuotes& wide = *ctx2.last(InstrumentId{0});
  REQUIRE_FALSE(wide.bids.empty());
  REQUIRE_FALSE(wide.asks.empty());
  CHECK((wide.asks[0].price - wide.bids[0].price) > (flat.asks[0].price - flat.bids[0].price));

  Ctx ctx3;
  ctx3.pos[1] = Qty::from_double(1000.0 / contract_vega);  // at the vega limit
  OptionsMM capped = make({{"max_vega", "1000"}});
  capped.on_start(ctx3);
  capped.on_option_ticker(ctx3, ticker(InstrumentId{1}, "76904.4", 0.5));
  capped.on_option_ticker(ctx3, ticker(InstrumentId{0}, "76904.4", 0.5));
  capped.on_fill(ctx3, OmsUpdate{}, OrderFillMsg{});
  CHECK(ctx3.last(InstrumentId{0})->bids.empty());  // buying options adds vega
  CHECK_FALSE(ctx3.last(InstrumentId{0})->asks.empty());
}

TEST_CASE("strategies.options_mm: contract multipliers and inverse futures delta") {
  Ctx ctx;
  OptionsMM s = make({});
  s.on_start(ctx);
  s.on_option_ticker(ctx, ticker(InstrumentId{3}, "100", 0.4));
  const OptionsMM::State& lin = s.state(InstrumentId{3});
  const Greeks g = black76(CallPut::Call, 100.0, 100.0, years(), 0.4);
  CHECK(lin.theo == doctest::Approx(g.price));          // linear: quote currency, no /F
  CHECK(lin.delta == doctest::Approx(10.0 * g.delta));  // multiplier 10
  CHECK(lin.vega == doctest::Approx(10.0 * g.vega / 100.0));
  ctx.pos[3] = qt("2");
  ctx.pos[2] = qt("-7690");  // short 76,900 USD of BTC-PERPETUAL (10 USD contracts)
  ctx.books[2] = FakeBook{px("76899.5"), px("76900.5"), true};
  const OptionsMM::Exposure e = s.exposure(ctx);
  CHECK(e.delta == doctest::Approx(2.0 * 10.0 * g.delta - 1.0));  // -7690 * 10 / 76900 = -1 BTC
  CHECK(e.vega == doctest::Approx(2.0 * 10.0 * g.vega / 100.0));
}

TEST_CASE("strategies.options_mm: own smoothed implied vol from book mids") {
  Ctx ctx;
  OptionsMM s = make({{"use_venue_iv", "false"}, {"iv_halflife_s", "10"}});
  s.on_start(ctx);
  s.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", std::nan("")));  // forward only
  CHECK(ctx.sets.empty());                                                    // no vol yet
  const auto mid_at = [&](double vol) {
    const double coin =
        black76_price(
            CallPut::Call, 76904.4, 77000.0, year_fraction(kNow + 30 * kDay, ctx.t.ns), vol) /
        76904.4;
    return Price::from_double(coin);
  };
  const Price m1 = mid_at(0.5);
  ctx.books[0] = FakeBook{m1 - px("0.00005"), m1 + px("0.00005"), true};
  s.on_book(ctx, InstrumentId{0}, ctx.books[0]);
  CHECK(s.state(InstrumentId{0}).own_iv == doctest::Approx(0.5).epsilon(1e-3));
  CHECK(ctx.last(InstrumentId{0}) != nullptr);
  ctx.t = Timestamp{kNow + 10'000'000'000};  // one half-life later, the market is at 70 %
  const Price m2 = mid_at(0.7);
  ctx.books[0] = FakeBook{m2 - px("0.00005"), m2 + px("0.00005"), true};
  s.on_book(ctx, InstrumentId{0}, ctx.books[0]);
  CHECK(s.state(InstrumentId{0}).own_iv == doctest::Approx(0.6).epsilon(2e-3));
  // An invalid book pulls that option.
  ctx.books[0].valid = false;
  s.on_book(ctx, InstrumentId{0}, ctx.books[0]);
  REQUIRE_FALSE(ctx.pulls.empty());
  CHECK(ctx.pulls.back() == InstrumentId{0});
}

TEST_CASE("strategies.options_mm: expiry filter, stale pulls and connection resets") {
  Ctx near(kNow + 1'800'000'000'000);  // 30 minutes to expiry
  OptionsMM s = make({{"pull_on_stale_ms", "1000"}});
  s.on_start(near);
  s.on_option_ticker(near, ticker(InstrumentId{0}, "76904.4", 0.5));
  CHECK(near.sets.empty());  // inside min_expiry_s (1 h)

  Ctx ctx;
  OptionsMM t = make({{"pull_on_stale_ms", "1000"}});
  t.on_start(ctx);
  t.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  REQUIRE(ctx.sets.size() == 1);
  ctx.t = Timestamp{kNow + 500'000'000};
  t.on_timer(ctx, TimerId{1}, OptionsMM::kStaleTimer);
  CHECK(ctx.pulls.empty());
  ctx.t = Timestamp{kNow + 1'500'000'000};
  t.on_timer(ctx, TimerId{1}, OptionsMM::kStaleTimer);
  REQUIRE(ctx.pulls.size() == 1);
  CHECK(ctx.pulls[0] == InstrumentId{0});

  t.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  const std::size_t before = ctx.sets.size();
  ConnectionStateMsg live{};
  init_header(live, EventType::ConnectionState, InstrumentId::invalid(), VenueId{0});
  live.state = ConnState::Live;
  t.on_connection(ctx, live);
  CHECK(ctx.sets.size() > before);  // requoted at once
}

TEST_CASE("strategies.options_mm: quotes wait for a two-sided book") {
  Ctx ctx;
  ctx.books[0].valid = false;
  OptionsMM s = make({});
  s.on_start(ctx);
  s.on_option_ticker(ctx, ticker(InstrumentId{0}, "76904.4", 0.5));
  CHECK(ctx.sets.empty());
  CHECK(s.state(InstrumentId{0}).priced);  // the greeks still count towards the portfolio
  ctx.books[0].valid = true;
  s.on_book(ctx, InstrumentId{0}, ctx.books[0]);  // the book became two-sided: quote now
  CHECK(ctx.sets.size() == 1);
  s.on_book(ctx, InstrumentId{0}, ctx.books[0]);  // already quoted: book updates do not requote
  CHECK(ctx.sets.size() == 1);
  ctx.books[0].valid = false;
  s.on_book(ctx, InstrumentId{0}, ctx.books[0]);
  REQUIRE(ctx.pulls.size() == 1);
  ctx.books[0].valid = true;
  s.on_book(ctx, InstrumentId{0}, ctx.books[0]);
  CHECK(ctx.sets.size() == 2);
}

TEST_CASE("strategies.options_mm: name, schema and parameter validation") {
  CHECK(std::string(OptionsMM::name()) == "options_mm");
  CHECK(OptionsMM::schema().find("max_delta") != nullptr);
  CHECK(OptionsMM::schema().find("max_vega") != nullptr);
  CHECK(OptionsMM::schema().find("use_venue_iv") != nullptr);
  OptionsMM s;
  CHECK(s.configure({{"max_delta", "-1"}}).has_value());
  CHECK(s.configure({{"nope", "1"}}).has_value());
  CHECK_FALSE(s.configure({{"half_spread_vol", "2.5"}}).has_value());
}
