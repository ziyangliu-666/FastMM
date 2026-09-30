// xmm's behaviour pinned over two long seeded sessions, recorded before its hedging moved into
// HedgeExecutor (strategies/hedge_executor.hpp). A change of either hash is a change of behaviour.
//
// 1. A fake context: every call xmm makes (quotes set and pulled, orders sent or refused) is folded
//    into one FNV-1a hash, over 20000 random events: book moves on both instruments, a hedge book
//    that goes invalid and comes back, maker fills of every size, hedge ends of every kind (filled,
//    partial, expired, rejected, refused by the engine, unacknowledged and cancelled by the ack
//    timeout, dropped by reconciliation, generic venue rejects, late fills after them),
//    reconciliations, the hedge venue's order channel dropping, balance shortfalls on both venues,
//    quoting disabled and enabled, halts and restarts.
// 2. The real engine (StrategyHarness): the SHA-256 of the outbound stream over 3000 random steps
//    of book moves, maker fills, hedge liquidity that is there or not, and hedge venue drops.
//
// FASTMM_PRINT_GOLDEN=1 prints the current values.
#include "test_support.hpp"

#include "fastmm/strategies/xmm.hpp"
#include "fastmm/testing/strategy_harness.hpp"

#include <array>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

constexpr std::int64_t kT0 = 1'789'344'931'096LL * 1'000'000;

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

bool print_golden() {
  const char* p = std::getenv("FASTMM_PRINT_GOLDEN");
  return p != nullptr && std::string(p) == "1";
}

struct FakeBook {
  Price bid{};
  Price ask{};
  bool valid = false;
  Timestamp updated{};
  [[nodiscard]] bool is_valid() const { return valid; }
  [[nodiscard]] Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  [[nodiscard]] Level best_bid() const { return Level{bid, Qty::from_int(1)}; }
  [[nodiscard]] Level best_ask() const { return Level{ask, Qty::from_int(1)}; }
  [[nodiscard]] Timestamp last_update() const { return updated; }
};
struct FakePosition {
  Qty qty{};
};

Instrument linear(const char* sym, std::uint8_t venue, const char* mult, const char* lot) {
  Instrument i{};
  i.symbol = sym;
  i.venue = VenueId{venue};
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = px("0.1");
  i.lot = qt(lot);
  i.min_qty = i.lot;
  i.contract_multiplier = qt(mult);
  return i;
}

// Instrument 0 quotes on venue 0, instrument 1 hedges on venue 1.
struct TraceCtx {
  struct Sent {
    NewOrderRequest req;
    ClientOrderId id;
    bool open = true;
  };
  InstrumentTable table;
  std::array<FakeBook, 2> books{};
  std::array<Qty, 2> pos{};
  Timestamp t{kT0};
  std::deque<Sent> sent;  // stable references: a hook can send while the test holds one
  bool refuse = false;
  bool quoting = true;
  bool reconciling_now = false;
  std::array<FeeRates, 2> fee_rates{FeeRates{0, 0}, FeeRates{0, 400}};
  std::array<std::array<Qty, 2>, 2> room{{{Qty::max(), Qty::max()}, {Qty::max(), Qty::max()}}};
  std::uint64_t trace = 14695981039346656037ULL;
  std::uint64_t calls = 0;

  void mix(std::int64_t v) {
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
      trace ^= u & 0xffU;
      trace *= 1099511628211ULL;
      u >>= 8;
    }
    ++calls;
  }

  [[nodiscard]] const InstrumentTable& instruments() const { return table; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table.get(id); }
  [[nodiscard]] const FakeBook& book(InstrumentId id) const { return books[id.value]; }
  [[nodiscard]] FakePosition position(InstrumentId id) const { return FakePosition{pos[id.value]}; }
  [[nodiscard]] Timestamp now() const { return t; }
  [[nodiscard]] const FeeRates& fees(InstrumentId id) const { return fee_rates[id.value]; }
  [[nodiscard]] bool reconciling() const { return reconciling_now; }
  [[nodiscard]] bool venue_killed(VenueId) const { return false; }
  [[nodiscard]] VenueHealthView venue_health(VenueId) const { return VenueHealthView{}; }
  bool set_quotes(InstrumentId id, const DesiredQuotes& q) {
    mix(1);
    mix(id.value);
    if (!quoting) return false;
    for (const Level& l : q.bids) {
      mix(l.price.raw);
      mix(l.qty.raw);
    }
    mix(-1);
    for (const Level& l : q.asks) {
      mix(l.price.raw);
      mix(l.qty.raw);
    }
    return true;
  }
  void pull_quotes(InstrumentId id) {
    mix(2);
    mix(id.value);
  }
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
  Result<ClientOrderId, RejectReason> send(const NewOrderRequest& r) {
    mix(refuse ? 4 : 3);
    mix(r.instrument.value);
    mix(static_cast<std::int64_t>(r.side));
    mix(static_cast<std::int64_t>(r.tif));
    mix(r.reduce_only ? 1 : 0);
    mix(r.price.raw);
    mix(r.qty.raw);
    mix(r.user_tag);
    if (refuse) return fail(RejectReason::MaxPosition);
    const ClientOrderId id{0x0001'0000'0000ULL + sent.size() + 1};
    sent.push_back(Sent{r, id, true});
    return id;
  }
  [[nodiscard]] RefPrice mark(InstrumentId) const { return RefPrice{}; }
  [[nodiscard]] RefPrice index(InstrumentId) const { return RefPrice{}; }
  [[nodiscard]] FundingView funding(InstrumentId) const { return FundingView{}; }
  [[nodiscard]] Qty balance_room(InstrumentId id, Side side, Price) const {
    return room[id.value][static_cast<std::size_t>(side)];
  }
  [[nodiscard]] Qty open_qty(InstrumentId id, Side side) const {
    Qty q{};
    for (const Sent& s : sent) {
      if (s.open && s.req.instrument == id && s.req.side == side) q += s.req.qty;
    }
    return q;
  }
};

struct Rng {
  std::mt19937_64 g;
  explicit Rng(std::uint64_t seed) : g(seed) {}
  std::uint64_t below(std::uint64_t n) { return g() % n; }
  bool chance(std::uint64_t pct) { return below(100) < pct; }
};

ConnectionStateMsg connection(std::uint8_t venue, std::uint8_t channel, ConnState st) {
  ConnectionStateMsg m{};
  init_header(m, EventType::ConnectionState, InstrumentId{}, VenueId{venue});
  m.state = st;
  m.channel = channel;
  return m;
}

BalanceMsg balance_on(std::uint8_t venue) {
  BalanceMsg m{};
  init_header(m, EventType::Balance, InstrumentId{}, VenueId{venue});
  return m;
}

struct TraceResult {
  std::uint64_t trace = 0;
  std::uint64_t calls = 0;
  Xmm::Stats stats{};
};

// One seeded session against the fake context; `mult` and `lot` shape the hedge instrument.
TraceResult trace_session(std::uint64_t seed, const char* mult, const char* lot, const char* maxq) {
  TraceCtx c;
  REQUIRE(c.table.add(linear("BTCUSDT", 0, "1", "0.001")));
  Instrument hi = linear("BTC-USDT-SWAP", 1, mult, lot);
  hi.max_qty = qt(maxq);
  hi.min_notional = Notional::from_int(5);
  REQUIRE(c.table.add(hi));
  Xmm s;
  REQUIRE_FALSE(s.configure({{"quote_qty", "0.01"},
                             {"edge_bps", "2"},
                             {"slippage_bps", "1"},
                             {"hedge_tolerance_bps", "5"},
                             {"basis_halflife_s", "20"},
                             {"max_unhedged", "0.05"},
                             {"requote_threshold_ticks", "2"},
                             {"hedge_retry_ms", "50"},
                             {"max_hedge_failures", "4"},
                             {"failure_window_ms", "1500"},
                             {"uncertain_hold_ms", "400"}}));
  Rng r(seed);
  std::int64_t qmid = 1'000'000;  // tenths
  std::int64_t hmid = 1'000'000;
  auto set_books = [&](bool q, bool h) {
    if (q) {
      c.books[0] = FakeBook{Price::from_raw((qmid - 100) * (kFixedScale / 10)),
                            Price::from_raw((qmid + 100) * (kFixedScale / 10)),
                            true,
                            c.t};
    }
    if (h) {
      const bool valid = c.books[1].valid || c.books[1].bid.is_zero();
      c.books[1] = FakeBook{Price::from_raw((hmid - 1) * (kFixedScale / 10)),
                            Price::from_raw((hmid + 1) * (kFixedScale / 10)),
                            valid,
                            c.t};
    }
  };
  set_books(true, true);
  s.on_start(c);
  REQUIRE(s.ready());
  s.on_book(c, InstrumentId{0}, c.books[0]);
  s.on_book(c, InstrumentId{1}, c.books[1]);
  int restart = 0;
  auto open_hedge = [&]() -> TraceCtx::Sent* {
    for (auto& o : c.sent) {
      if (o.open) return &o;
    }
    return nullptr;
  };
  auto end_hedge = [&](TraceCtx::Sent& o,
                       OrderState st,
                       Qty cum,
                       bool acked,
                       Qty unresolved,
                       RejectReason reason) {
    o.open = false;
    OmsUpdate u;
    u.known = true;
    u.changed = true;
    u.terminal = true;
    u.order.cl_ord_id = o.id;
    u.order.instrument = o.req.instrument;
    u.order.side = o.req.side;
    u.order.qty = o.req.qty;
    u.order.cum_qty = cum;
    u.order.state = st;
    u.order.reject_reason = reason;
    if (acked) u.order.venue_order_id.assign("1");
    u.unresolved_qty = unresolved;
    s.on_order_update(c, u);
  };
  auto hedge_fill = [&](TraceCtx::Sent& o, Qty q, bool done, bool late) {
    c.pos[1] += o.req.side == Side::Buy ? q : -q;
    if (done) o.open = false;
    Fill f;
    f.instrument = InstrumentId{1};
    f.side = o.req.side;
    f.qty = q;
    f.order_done = done;
    f.late = late;
    s.on_fill(c, f);
  };
  for (int step = 0; step < 20000; ++step) {
    switch (r.below(12)) {
      case 0:
      case 1: {
        c.t = c.t + milliseconds(static_cast<std::int64_t>(1 + r.below(120)));
        c.books[0].updated = c.t;
        c.books[1].updated = c.t;
        s.on_timer(c, TimerId{1}, Xmm::kTimer);
        break;
      }
      case 2:
        qmid += static_cast<std::int64_t>(r.below(41)) - 20;
        set_books(true, false);
        s.on_book(c, InstrumentId{0}, c.books[0]);
        break;
      case 3:
        hmid += static_cast<std::int64_t>(r.below(41)) - 20;
        set_books(false, true);
        if (r.chance(4)) c.books[1].valid = !c.books[1].valid;
        s.on_book(c, InstrumentId{1}, c.books[1]);
        break;
      case 4:
      case 5: {
        const Side side = r.chance(50) ? Side::Buy : Side::Sell;
        const Qty q =
            Qty::from_raw(static_cast<std::int64_t>(1 + r.below(20)) * (kFixedScale / 1000));
        c.pos[0] += side == Side::Buy ? q : -q;
        Fill f;
        f.instrument = InstrumentId{0};
        f.side = side;
        f.qty = q;
        s.on_fill(c, f);
        break;
      }
      case 6:
      case 7: {
        TraceCtx::Sent* o = open_hedge();
        if (o == nullptr) break;
        const Qty all = o->req.qty;
        const Qty lotq = c.table.get(InstrumentId{1}).lot;
        switch (r.below(9)) {
          case 0:
            hedge_fill(*o, all, true, false);
            end_hedge(*o, OrderState::Filled, all, true, Qty{}, RejectReason::None);
            break;
          case 1: {
            const std::int64_t lots = all.raw / lotq.raw;
            const Qty part = Qty::from_raw(
                lotq.raw * static_cast<std::int64_t>(r.below(static_cast<std::uint64_t>(lots))));
            if (part.is_positive()) hedge_fill(*o, part, false, false);
            end_hedge(*o, OrderState::Expired, part, true, Qty{}, RejectReason::None);
            break;
          }
          case 2:
            end_hedge(*o, OrderState::Expired, Qty{}, true, Qty{}, RejectReason::None);
            break;
          case 3:
            end_hedge(*o, OrderState::Rejected, Qty{}, true, Qty{}, RejectReason::None);
            break;
          case 4: {
            // Cancelled by the ack timeout; sometimes it had filled and the fill comes late.
            TraceCtx::Sent copy = *o;
            end_hedge(*o, OrderState::Canceled, Qty{}, false, Qty{}, RejectReason::None);
            if (r.chance(50)) {
              c.t = c.t + milliseconds(static_cast<std::int64_t>(r.below(300)));
              hedge_fill(copy, copy.req.qty, true, true);
            }
            break;
          }
          case 5:
            end_hedge(*o, OrderState::Canceled, Qty{}, true, all, RejectReason::None);
            break;
          case 6:
            end_hedge(*o, OrderState::Rejected, Qty{}, false, Qty{}, RejectReason::VenueReject);
            break;
          default:
            if (lotq < all) hedge_fill(*o, lotq, false, false);
            break;
        }
        break;
      }
      case 8: {
        const ConnState st = r.chance(50) ? ConnState::Disconnected : ConnState::Live;
        const auto venue = static_cast<std::uint8_t>(r.below(2));
        const auto channel = static_cast<std::uint8_t>(r.below(2));
        s.on_connection(c, connection(venue, channel, st));
        break;
      }
      case 9: {
        const std::uint64_t k = r.below(4);
        if (k == 0) {
          c.reconciling_now = !c.reconciling_now;
          c.quoting = !c.reconciling_now;
          s.on_quoting(c, c.quoting);
        } else if (k == 1) {
          c.refuse = r.chance(10);
        } else {
          const std::size_t v = r.below(2);
          const std::size_t side = r.below(2);
          c.room[v][side] =
              r.chance(50)
                  ? Qty::max()
                  : Qty::from_raw(static_cast<std::int64_t>(r.below(3)) * kFixedScale / 100);
          s.on_balance(c, balance_on(static_cast<std::uint8_t>(v)));
        }
        break;
      }
      case 10:
        if (s.halted()) {
          REQUIRE_FALSE(s.configure({{"restart", std::to_string(++restart)}}));
          s.on_params(c);
        }
        break;
      default:
        s.on_perp_state(c, InstrumentId{static_cast<std::uint32_t>(r.below(2))}, PerpStateMsg{});
        break;
    }
    c.mix(s.unhedged(c).raw);
    c.mix(s.halted() ? 1 : 0);
    c.mix(s.hedge_held() ? 1 : 0);
  }
  return TraceResult{c.trace, c.calls, s.stats()};
}

void check_trace(const char* name, const TraceResult& t, std::uint64_t expected) {
  if (print_golden()) {
    MESSAGE(std::string(name) << " trace " << t.trace << " calls " << t.calls << " sent "
                              << t.stats.hedges_sent << " failures " << t.stats.hedge_failures
                              << " uncertain " << t.stats.uncertain_ends << " halts "
                              << t.stats.halts << " held " << t.stats.hedges_held);
  }
  CAPTURE(name);
  CHECK(t.trace == expected);
}

}  // namespace

TEST_CASE("strategies.xmm.equivalence: fake context traces over seeded sessions") {
  check_trace(
      "linear 1, lot 0.001", trace_session(1, "1", "0.001", "0.012"), 11987839591907772369ULL);
  check_trace("0.01 contracts, lot 1", trace_session(2, "0.01", "1", "3"), 797411967461634741ULL);
  check_trace(
      "0.01 contracts, lot 0.01", trace_session(3, "0.01", "0.01", "100"), 7317237200756849893ULL);
}

namespace {

using fastmm::sim::HarnessOptions;
using fastmm::sim::StrategyHarness;

HarnessOptions two_venues() {
  HarnessOptions o;
  o.instruments.clear();
  REQUIRE(o.instruments.add(linear("BTCUSDT", 0, "1", "0.001")));
  REQUIRE(o.instruments.add(linear("BTC-USDT-SWAP", 1, "0.01", "1")));
  return o;
}

}  // namespace

TEST_CASE("strategies.xmm.equivalence: engine outbound hash over a seeded session") {
  const ParamMap p{{"quote_qty", "0.03"},
                   {"basis_halflife_s", "5"},
                   {"max_unhedged", "0.1"},
                   {"hedge_retry_ms", "100"},
                   {"max_hedge_failures", "8"},
                   {"failure_window_ms", "500"},
                   {"hedge_tolerance_bps", "2"},
                   {"requote_threshold_ticks", "3"}};
  StrategyHarness<Xmm> h(p, two_venues());
  const InstrumentId q{0};
  const InstrumentId hid{1};
  Rng r(99);
  std::int64_t qmid = 1'000'000;
  std::int64_t hmid = 1'000'000;
  std::uint64_t seq = 5000;
  auto tenth = [](std::int64_t v) { return Price::from_raw(v * (kFixedScale / 10)); };
  h.book(tenth(qmid - 100), tenth(qmid + 100), Qty::from_int(1), q);
  h.book(tenth(hmid - 1), tenth(hmid + 1), Qty::from_int(1), hid);
  bool hedge_down = false;
  for (int step = 0; step < 3000; ++step) {
    switch (r.below(8)) {
      case 0:
        qmid += static_cast<std::int64_t>(r.below(41)) - 20;
        h.book(tenth(qmid - 100), tenth(qmid + 100), Qty::from_int(1), q);
        break;
      case 1:
        hmid += static_cast<std::int64_t>(r.below(41)) - 20;
        h.book(tenth(hmid - 1), tenth(hmid + 1), Qty::from_int(1), hid);
        break;
      case 2: {
        // Liquidity for the hedges at the venue, near or away from the touch.
        sim::NewOrder o;
        o.account = 9;
        o.cl_ord_id = ClientOrderId{++seq};
        o.instrument = hid;
        o.side = r.chance(50) ? Side::Buy : Side::Sell;
        const std::int64_t off = r.chance(70) ? 1 : 400;
        o.price = tenth(o.side == Side::Buy ? hmid - off : hmid + off);
        o.qty = Qty::from_int(static_cast<std::int64_t>(1 + r.below(4)));
        static_cast<void>(h.venue_transport().matching_engine().submit(o, h.now()));
        break;
      }
      case 3: {
        const Side side = r.chance(50) ? Side::Buy : Side::Sell;
        const Qty qty = Qty::from_raw(static_cast<std::int64_t>(r.below(4)) * (kFixedScale / 100));
        static_cast<void>(h.fill(side, qty, q));
        break;
      }
      case 4:
        if (r.chance(15)) {
          if (hedge_down) {
            h.reconnect(1, VenueId{1});
          } else {
            h.disconnect(1, VenueId{1});
          }
          hedge_down = !hedge_down;
        }
        break;
      default:
        h.advance(milliseconds(static_cast<std::int64_t>(1 + r.below(60))));
        break;
    }
  }
  h.advance(seconds(2));
  const std::string hash = h.venue_transport().outbound_hash().hex();
  const Xmm::Stats& st = h.strategy().stats();
  if (print_golden()) {
    MESSAGE("engine outbound_sha256 "
            << hash << " messages " << h.venue_transport().outbound_hash().count() << " sent "
            << st.hedges_sent << " failures " << st.hedge_failures << " halts " << st.halts);
  }
  CHECK(hash == "08028b5f5f7a1e6a34363805ba4aa68e7d6f725c02ea8e58bff295e29700bd0b");
}
