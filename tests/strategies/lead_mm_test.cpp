#include "fastmm/strategies/lead_mm.hpp"

#include "test_support.hpp"

#include "fastmm/core/order.hpp"

#include <array>
#include <random>
#include <string>

using namespace fastmm;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
Instrument inst(std::uint32_t id = 0) {
  Instrument i{};
  i.id = InstrumentId{id};
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.flags = Instrument::kEnabled;
  return i;
}

struct NoCtx {
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
};

// A LeadMM configured with `params` on top of the test defaults, started (roles resolved).
LeadMM make(const ParamMap& params = {}) {
  LeadMM s;
  ParamMap p{{"quote_qty", "0.05"}, {"max_inventory", "0.1"}, {"skew_bps_per_unit", "0"}};
  for (const auto& [k, v] : params) p[k] = v;
  REQUIRE_FALSE(s.configure(p));
  NoCtx ctx;
  s.on_start(ctx);
  return s;
}

// Target book 99.99 / 100.01 unless given; no resting quotes, flat.
DesiredQuotes quotes(const LeadMM& s,
                     const char* fair,
                     Qty position = Qty{},
                     const char* bid = "99.99",
                     const char* ask = "100.01",
                     Price own_bid = Price{},
                     Price own_ask = Price{}) {
  return s.compute_quotes(px(fair), px(bid), px(ask), own_bid, own_ask, position, inst());
}
}  // namespace

TEST_CASE(
    "strategies.lead_mm: fair value divides by the fx mid, multiplies inverted, or is the "
    "leader mid") {
  const LeadMM divide = make();
  CHECK(divide.fair_value(px("150.01"), px("0.9991")) == px("150.14513061"));  // truncated
  const LeadMM invert = make({{"fx_invert", "true"}});
  CHECK(invert.fair_value(px("150.01"), px("0.9991")) == px("149.874991"));
  const LeadMM none = make({{"fx", "-1"}});
  CHECK(none.fair_value(px("150.01"), px("0.9991")) == px("150.01"));
  CHECK(none.fair_value(px("150.01"), Price{}) == px("150.01"));
  CHECK(divide.fair_value(px("150.01"), Price{}) == Price{});  // no fx mid: no fair
  CHECK(std::string(LeadMM::name()) == "lead_mm");
}

TEST_CASE("strategies.lead_mm: a side is quoted at the touch only when its edge passes") {
  const LeadMM s = make({{"edge_min_bps", "1"}});
  // fair 100: both edges are 0.01 / 100 = 1.0001 / 0.9999 bps; only the bid passes 1 bp.
  DesiredQuotes q = quotes(s, "100");
  REQUIRE(q.bids.size() == 1);
  CHECK(q.bids[0] == Level{px("99.99"), qt("0.05")});
  CHECK(q.asks.empty());
  // fair 100.005: bid 1.5 bps, ask 0.5 bps.
  q = quotes(s, "100.005");
  CHECK(q.bids.size() == 1);
  CHECK(q.asks.empty());
  // fair 99.995: ask 1.5 bps, bid 0.5 bps.
  q = quotes(s, "99.995");
  CHECK(q.bids.empty());
  REQUIRE(q.asks.size() == 1);
  CHECK(q.asks[0] == Level{px("100.01"), qt("0.05")});
  // The edge is a fraction of the quote price, not of fair.
  CHECK(LeadMM::edge(Side::Sell, px("100"), px("100.01")) == ratio(px("0.01"), px("100.01")));
  CHECK(LeadMM::edge(Side::Buy, px("100"), px("99.99")) == ratio(px("0.01"), px("99.99")));
  // Both pass at 0.3 bps.
  const LeadMM loose = make();
  q = quotes(loose, "100");
  CHECK(q.bids.size() == 1);
  CHECK(q.asks.size() == 1);
  // No fair (zero) or an empty side: nothing.
  CHECK(quotes(loose, "0").empty());
  CHECK(quotes(loose, "100", Qty{}, "0", "100.01").empty());
}

TEST_CASE("strategies.lead_mm: inventory caps a side and skews both thresholds") {
  SUBCASE("cap") {
    const LeadMM s = make();
    DesiredQuotes q = quotes(s, "100", qt("0.1"));  // 0.1 + 0.05 > 0.1
    CHECK(q.bids.empty());
    CHECK(q.asks.size() == 1);
    q = quotes(s, "100", qt("-0.1"));
    CHECK(q.bids.size() == 1);
    CHECK(q.asks.empty());
    q = quotes(s, "100", qt("0.05"));  // 0.05 + 0.05 == 0.1: allowed
    CHECK(q.bids.size() == 1);
  }
  SUBCASE("skew per quote_qty unit, fractional") {
    const LeadMM s = make({{"skew_bps_per_unit", "0.5"}, {"max_inventory", "0"}});
    CHECK(s.inventory_skew(qt("0.05")) == 0.5_bps);
    CHECK(s.inventory_skew(qt("0.025")) == 0.25_bps);
    CHECK(s.inventory_skew(qt("-0.1")) == -1_bps);
    // fair 100: edges ~1 bp. Long 0.1 (2 units): buy needs 0.3 + 1 = 1.3 -> no bid; sell needs
    // 0.3 - 1 -> ask.
    DesiredQuotes q = quotes(s, "100", qt("0.1"));
    CHECK(q.bids.empty());
    CHECK(q.asks.size() == 1);
    // Long 0.05: buy needs 0.8 -> bid.
    q = quotes(s, "100", qt("0.05"));
    CHECK(q.bids.size() == 1);
    // Short 0.1: the mirror.
    q = quotes(s, "100", qt("-0.1"));
    CHECK(q.bids.size() == 1);
    CHECK(q.asks.empty());
    // Long 0.1, fair 100.02: the ask's edge is -1 bp; the sell threshold is -0.7 -> no ask.
    q = quotes(s, "100.02", qt("0.1"));
    CHECK(q.asks.empty());
    // Long 0.2: sell threshold -1.7 -> the ask at -1 bp is quoted to unwind.
    q = quotes(s, "100.02", qt("0.2"));
    CHECK(q.asks.size() == 1);
  }
}

TEST_CASE("strategies.lead_mm: quotes never cross the target book or each other") {
  SUBCASE("fair far outside the book") {
    const LeadMM s = make({{"improve", "true"}});
    DesiredQuotes q = quotes(s, "101");
    REQUIRE(q.bids.size() == 1);
    CHECK(q.bids[0].price == px("100.00"));  // improved one tick, still below the ask
    CHECK(q.asks.empty());
    q = quotes(s, "99");
    REQUIRE(q.asks.size() == 1);
    CHECK(q.asks[0].price == px("100.00"));
    CHECK(q.bids.empty());
  }
  SUBCASE("a one-tick spread is joined, not improved") {
    const LeadMM s = make({{"improve", "true"}});
    const DesiredQuotes q = quotes(s, "101", Qty{}, "99.99", "100.00");
    REQUIRE(q.bids.size() == 1);
    CHECK(q.bids[0].price == px("99.99"));
  }
  SUBCASE("both sides improving into the same tick join instead") {
    const LeadMM s = make({{"improve", "true"}, {"edge_min_bps", "0"}});
    const DesiredQuotes q = quotes(s, "100");
    REQUIRE(q.bids.size() == 1);
    REQUIRE(q.asks.size() == 1);
    CHECK(q.bids[0].price == px("99.99"));
    CHECK(q.asks[0].price == px("100.01"));
  }
  SUBCASE("a resting quote at or through the opposite touch is not kept") {
    const LeadMM s = make({{"improve", "true"}});
    // Our old bid at 100.01 is now the best ask's price: requote at the touch instead.
    const DesiredQuotes q = quotes(s, "101", Qty{}, "99.99", "100.01", px("100.01"));
    REQUIRE(q.bids.size() == 1);
    CHECK(q.bids[0].price == px("100.00"));
  }
  SUBCASE("random books, fairs, positions and resting quotes") {
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int> ticks(0, 400);
    std::uniform_int_distribution<int> width(1, 6);
    std::uniform_int_distribution<int> pos(-6, 6);
    const LeadMM s = make({{"improve", "true"},
                           {"edge_min_bps", "0"},
                           {"skew_bps_per_unit", "3"},
                           {"max_inventory", "0"}});
    const Instrument i = inst();
    for (int n = 0; n < 5000; ++n) {
      const Price bid = px("99") + i.ticks(ticks(rng));
      const Price ask = bid + i.ticks(width(rng));
      const Price fair = px("99") + i.ticks(ticks(rng));
      const Price own_bid = bid + i.ticks(width(rng) - 3);
      const Price own_ask = ask - i.ticks(width(rng) - 3);
      const DesiredQuotes q =
          s.compute_quotes(fair, bid, ask, own_bid, own_ask, qt("0.05") * pos(rng), i);
      CAPTURE(n);
      if (!q.bids.empty()) CHECK(q.bids[0].price < ask);
      if (!q.asks.empty()) CHECK(q.asks[0].price > bid);
      if (!q.bids.empty() && !q.asks.empty()) CHECK(q.bids[0].price < q.asks[0].price);
    }
  }
}

TEST_CASE(
    "strategies.lead_mm: hysteresis keeps a resting quote until the edge or the touch "
    "moves") {
  // Edges at fair 100 with a 99.99 bid: 1.0001 bps. Minimum 1.05, hysteresis 0.1 -> keep >= 0.95.
  const LeadMM s = make({{"edge_min_bps", "1.05"}, {"hysteresis_bps", "0.1"}});
  CHECK(quotes(s, "100").bids.empty());  // not resting: needs 1.05
  DesiredQuotes q = quotes(s, "100", Qty{}, "99.99", "100.01", px("99.99"));
  REQUIRE(q.bids.size() == 1);  // resting at the touch: 0.95 is enough
  CHECK(q.bids[0].price == px("99.99"));
  // Resting at 99.98 while the touch moved up to 99.99: the new price needs the full minimum.
  CHECK(quotes(s, "100", Qty{}, "99.99", "100.01", px("99.98")).bids.empty());
  // Edge 0.9 bps (fair 99.999): below the hysteresis band, dropped even while resting.
  CHECK(quotes(s, "99.999", Qty{}, "99.99", "100.01", px("99.99")).bids.empty());
  // The ask side mirrors it: 0.9999 bps is kept only while resting, 0.94 bps is dropped.
  CHECK(quotes(s, "100").asks.empty());
  CHECK(quotes(s, "100", Qty{}, "99.99", "100.01", Price{}, px("100.01")).asks.size() == 1);
  CHECK(quotes(s, "100.0006", Qty{}, "99.99", "100.01", Price{}, px("100.01")).asks.empty());
}

TEST_CASE("strategies.lead_mm: improve never steps over our own resting order") {
  const LeadMM s = make({{"improve", "true"}});
  // Live books include our order: resting at 100.00, which is now the best bid.
  DesiredQuotes q = quotes(s, "101", Qty{}, "100.00", "100.05", px("100.00"));
  REQUIRE(q.bids.size() == 1);
  CHECK(q.bids[0].price == px("100.00"));
  // Someone else's best bid: improve on it.
  q = quotes(s, "101", Qty{}, "100.00", "100.05");
  REQUIRE(q.bids.size() == 1);
  CHECK(q.bids[0].price == px("100.01"));
  // Improving would lose the edge: join.
  q = quotes(s, "100.01", Qty{}, "100.00", "100.05");
  REQUIRE(q.bids.size() == 1);
  CHECK(q.bids[0].price == px("100.00"));
}

namespace {
struct FakeBook {
  Price bid, ask;
  Timestamp updated{};
  std::uint64_t id = 0;
  bool valid = true;
  std::uint64_t seq() const { return id; }
  bool is_valid() const { return valid; }
  Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  Level best_bid() const { return Level{bid, qt("1")}; }
  Level best_ask() const { return Level{ask, qt("1")}; }
  Timestamp last_update() const { return updated; }
};
struct FakePosition {
  Qty qty{};
};
// Target 0 (SOLFDUSD-like), leader 1, fx 2.
struct LeadCtx {
  std::array<Instrument, 3> list{inst(0), inst(1), inst(2)};
  std::array<FakeBook, 3> books{FakeBook{px("150.10"), px("150.20")},
                                FakeBook{px("150.00"), px("150.02")},
                                FakeBook{px("0.9990"), px("0.9992")}};
  Timestamp t = Timestamp{1'000'000'000};
  Qty pos{};
  int set_calls = 0;
  int pulls = 0;
  DesiredQuotes last{};
  const std::array<Instrument, 3>& instruments() const { return list; }
  const Instrument& instrument(InstrumentId id) const { return list[id.value]; }
  const FakeBook& book(InstrumentId id) const { return books[id.value]; }
  Timestamp now() const { return t; }
  FakePosition position(InstrumentId) const { return {pos}; }
  const Order* working_quote(InstrumentId, Side) const { return nullptr; }
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
  bool set_quotes(InstrumentId id, const DesiredQuotes& q) {
    CHECK(id == InstrumentId{0});
    ++set_calls;
    last = q;
    return true;
  }
  void pull_quotes(InstrumentId id) {
    CHECK(id == InstrumentId{0});
    ++pulls;
  }
  void touch_all() {
    for (FakeBook& b : books) b.updated = t;
  }
};
}  // namespace

TEST_CASE("strategies.lead_mm: a stale or invalid leader or fx book pulls the target's quotes") {
  LeadMM s = make({{"max_leader_age_ms", "500"}, {"fx_max_age_ms", "60000"}});
  LeadCtx ctx;
  s.on_start(ctx);
  ctx.touch_all();
  s.on_book(ctx, InstrumentId{1}, ctx.books[1]);
  REQUIRE(ctx.set_calls == 1);
  // fair 150.1451: bid 150.10 has 3 bps, ask 150.20 has 3.6 bps.
  CHECK(ctx.last.bids.size() == 1);
  CHECK(ctx.last.asks.size() == 1);
  CHECK(ctx.pulls == 0);

  SUBCASE("the leader goes quiet: the timer pulls, once") {
    ctx.t = ctx.t + milliseconds(500);
    s.on_timer(ctx, TimerId{1}, LeadMM::kStaleTimer);
    CHECK(ctx.pulls == 0);  // exactly 500 ms old is still fresh
    ctx.t = ctx.t + milliseconds(1);
    s.on_timer(ctx, TimerId{1}, LeadMM::kStaleTimer);
    CHECK(ctx.pulls == 1);
    s.on_timer(ctx, TimerId{1}, LeadMM::kStaleTimer);
    CHECK(ctx.pulls == 1);
    // A target update does not requote on a stale fair.
    ctx.books[0].updated = ctx.t;
    s.on_book(ctx, InstrumentId{0}, ctx.books[0]);
    CHECK(ctx.set_calls == 1);
    // The leader is back: quotes again.
    ctx.books[1].updated = ctx.t;
    ctx.books[2].updated = ctx.t;
    s.on_book(ctx, InstrumentId{1}, ctx.books[1]);
    CHECK(ctx.set_calls == 2);
    CHECK(ctx.last.bids.size() == 1);
  }
  SUBCASE("the fx book ages against fx_max_age_ms, not the leader's limit") {
    ctx.t = ctx.t + milliseconds(60000);
    ctx.books[1].updated = ctx.t;
    s.on_book(ctx, InstrumentId{1}, ctx.books[1]);
    CHECK(ctx.pulls == 0);  // 60 s old fx: still fresh
    CHECK(ctx.set_calls == 2);
    ctx.t = ctx.t + milliseconds(1);
    ctx.books[1].updated = ctx.t;
    s.on_book(ctx, InstrumentId{1}, ctx.books[1]);
    CHECK(ctx.pulls == 1);
    CHECK(ctx.set_calls == 2);
  }
  SUBCASE("an invalid leader book pulls at once") {
    ctx.books[1].valid = false;
    s.on_book(ctx, InstrumentId{1}, ctx.books[1]);
    CHECK(ctx.pulls == 1);
  }
  SUBCASE("an invalid target book pulls") {
    ctx.books[0].valid = false;
    s.on_book(ctx, InstrumentId{0}, ctx.books[0]);
    CHECK(ctx.pulls == 1);
  }
  SUBCASE("without fx, only the leader's age counts") {
    LeadMM n = make({{"fx", "-1"}});
    LeadCtx c;
    n.on_start(c);
    c.books[1].updated = c.t;  // fx never updated
    n.on_book(c, InstrumentId{1}, c.books[1]);
    CHECK(c.pulls == 0);
    CHECK(c.set_calls == 1);
    CHECK(c.last.bids.empty());  // fair 150.01 is below the target's 150.10 bid
    REQUIRE(c.last.asks.size() == 1);
    CHECK(c.last.asks[0].price == px("150.20"));
  }
  SUBCASE("an update of an unrelated instrument is ignored") {
    LeadMM n = make({{"fx", "-1"}});
    LeadCtx c;
    n.on_start(c);
    c.touch_all();
    n.on_book(c, InstrumentId{2}, c.books[2]);
    CHECK(c.set_calls == 0);
  }
}

TEST_CASE("strategies.lead_mm: roles are checked against the instrument table") {
  LeadMM s;
  CHECK(s.configure({{"leader", "0"}}).value().find("different") != std::string::npos);
  CHECK(s.configure({{"fx", "1"}}).value().find("different") != std::string::npos);
  REQUIRE_FALSE(s.configure({{"fx", "-1"}, {"leader", "3"}}));
  InstrumentTable t;
  Instrument a = inst();
  a.symbol = Symbol("SOLFDUSD");
  REQUIRE(t.add(a));
  Instrument b = inst();
  b.symbol = Symbol("SOLUSDT");
  b.flags = 0;
  REQUIRE(t.add(b));
  CHECK(s.check_instruments(t).value().find("indices") != std::string::npos);  // leader 3
  REQUIRE_FALSE(s.configure({{"leader", "1"}}));
  CHECK_FALSE(s.check_instruments(t));
  REQUIRE_FALSE(s.configure({{"target", "1"}, {"leader", "0"}}));
  CHECK(s.check_instruments(t).value().find("disabled") != std::string::npos);
}

namespace {
BookTickerMsg ticker(std::uint32_t id,
                     const char* bid,
                     const char* ask,
                     std::uint64_t update_id,
                     Timestamp ts,
                     const char* qty = "1") {
  BookTickerMsg m{};
  m.hdr.instrument = InstrumentId{id};
  m.hdr.venue_seq = update_id;
  m.hdr.recv_ts = ts;  // JSON @bookTicker has no event time
  m.bid_px = px(bid);
  m.ask_px = px(ask);
  m.bid_qty = qt(qty);
  m.ask_qty = qt(qty);
  return m;
}

// Started and quoting both sides off the depth books: fair 150.1451, target 150.10 / 150.20.
struct TickerFixture {
  LeadMM s;
  LeadCtx ctx;
  explicit TickerFixture(const ParamMap& params = {}) : s(make(params)) {
    s.on_start(ctx);
    ctx.touch_all();
    ctx.books[0].id = 100;
    ctx.books[1].id = 100;
    ctx.books[2].id = 100;
    s.on_book(ctx, InstrumentId{1}, ctx.books[1]);
    REQUIRE(ctx.set_calls == 1);
    REQUIRE(ctx.last.bids.size() == 1);
    REQUIRE(ctx.last.asks.size() == 1);
  }
};
}  // namespace

TEST_CASE("strategies.lead_mm: a leader ticker move requotes before any depth update") {
  TickerFixture f;
  // SOLUSDT ticks down to 149.95 / 149.97: fair 150.0901, below the target's 150.10 bid.
  f.s.on_book_ticker(f.ctx, InstrumentId{1}, ticker(1, "149.95", "149.97", 101, f.ctx.t));
  CHECK(f.ctx.set_calls == 2);
  CHECK(f.ctx.last.bids.empty());
  CHECK(f.ctx.last.asks.size() == 1);
  // The depth update that follows (older id) does not bring the old fair back.
  f.s.on_book(f.ctx, InstrumentId{1}, f.ctx.books[1]);
  CHECK(f.ctx.last.bids.empty());
}

TEST_CASE("strategies.lead_mm: a target ticker moves the join price") {
  TickerFixture f;
  f.s.on_book_ticker(f.ctx, InstrumentId{0}, ticker(0, "150.11", "150.19", 101, f.ctx.t));
  CHECK(f.ctx.set_calls == 2);
  REQUIRE(f.ctx.last.bids.size() == 1);
  REQUIRE(f.ctx.last.asks.size() == 1);
  CHECK(f.ctx.last.bids[0].price == px("150.11"));
  CHECK(f.ctx.last.asks[0].price == px("150.19"));
  // An fx ticker is stored, not a trigger.
  f.s.on_book_ticker(f.ctx, InstrumentId{2}, ticker(2, "0.9990", "0.9992", 101, f.ctx.t));
  CHECK(f.ctx.set_calls == 2);
}

TEST_CASE("strategies.lead_mm: a ticker older than the depth book, crossed or empty is ignored") {
  SUBCASE("older update id") {
    TickerFixture f;
    f.s.on_book_ticker(f.ctx, InstrumentId{0}, ticker(0, "150.11", "150.19", 99, f.ctx.t));
    CHECK(f.ctx.last.bids[0].price == px("150.10"));
  }
  SUBCASE("by timestamp when update ids are not compared") {
    TickerFixture f({{"bbo_by_update_id", "false"}});
    const Timestamp before = f.ctx.t - milliseconds(1);
    f.s.on_book_ticker(f.ctx, InstrumentId{0}, ticker(0, "150.11", "150.19", 101, before));
    CHECK(f.ctx.last.bids[0].price == px("150.10"));
    f.s.on_book_ticker(
        f.ctx, InstrumentId{0}, ticker(0, "150.11", "150.19", 99, f.ctx.t + milliseconds(1)));
    CHECK(f.ctx.last.bids[0].price == px("150.11"));
  }
  SUBCASE("crossed or zero size") {
    TickerFixture f;
    f.s.on_book_ticker(f.ctx, InstrumentId{0}, ticker(0, "150.19", "150.11", 101, f.ctx.t));
    f.s.on_book_ticker(f.ctx, InstrumentId{0}, ticker(0, "150.11", "150.19", 101, f.ctx.t, "0"));
    CHECK(f.ctx.set_calls == 1);
    // A leader ticker then requotes against the depth top of the target.
    f.s.on_book_ticker(f.ctx, InstrumentId{1}, ticker(1, "150.00", "150.02", 101, f.ctx.t));
    REQUIRE(f.ctx.last.bids.size() == 1);
    CHECK(f.ctx.last.bids[0].price == px("150.10"));
  }
  SUBCASE("a fresh leader ticker keeps a quiet leader book from going stale") {
    TickerFixture f;
    f.ctx.t = f.ctx.t + milliseconds(800);
    f.s.on_book_ticker(f.ctx, InstrumentId{1}, ticker(1, "150.00", "150.02", 101, f.ctx.t));
    CHECK(f.ctx.pulls == 0);
    CHECK(f.ctx.set_calls == 2);
    f.s.on_timer(f.ctx, TimerId{1}, LeadMM::kStaleTimer);
    CHECK(f.ctx.pulls == 0);
  }
}
