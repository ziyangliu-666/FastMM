// A backtest that starts trading inside a recorded session ([backtest] start_at): the strategy
// warms up on the data before, the engine refuses orders until every venue has reconciled, and
// with initial_state = "journal" the strategy restores the journal's checkpoint as at a warm
// standby's take-over. From a checkpoint, a strategy whose orders depend on its state and the
// market sends the orders the run that wrote the checkpoint sent; the run is deterministic and its
// journal replays bit for bit.
#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/journal_state.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

BacktestConfig parse(const std::string& backtest) {
  const std::string text =
      "[[instruments]]\nvenue = \"sim\"\nsymbol = \"BTCUSDT\"\ntick = \"0.01\"\nlot = "
      "\"0.001\"\n\n[venues.sim]\nkind = \"sim\"\n\n[strategy]\nname = \"basic_mm\"\n\n"
      "[backtest]\n" +
      backtest;
  return BacktestConfig::from_config(Config::parse(text));
}

// Every 100 ms of data: the position, and a post-only bid far below the touch (cancelled at the
// next look), with whether the engine let it go.
class Probe : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "probe"; }
  struct Look {
    std::int64_t ts;
    Qty position;
    bool sent;
    RejectReason why;
  };
  static inline std::vector<Look> looks;
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    const std::int64_t now = ctx.now().ns;
    if (now < next_ || !b.is_valid()) return;
    next_ = now + 100'000'000;
    if (open_.valid()) static_cast<void>(ctx.cancel(open_));
    open_ = ClientOrderId{};
    const Price px = b.best_bid().price - ctx.instrument(id).ticks(100);
    const auto r = ctx.send(NewOrderRequest::limit(id, Side::Buy, px, qt("0.001")).post_only());
    if (r) open_ = *r;
    looks.push_back(
        Look{now, ctx.position(id).qty, r.has_value(), r ? RejectReason::None : r.error()});
  }

 private:
  std::int64_t next_ = 0;
  ClientOrderId open_{};
};

// Keeps an average of the mid and a count of books as its state; at every 4th book an IOC bid
// 40 ticks under the average, which never fills. What it sends depends on its state and the market
// alone.
class Ewma : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "ewma"; }
  static inline std::vector<std::string> restored;
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    if (!b.is_valid()) return;
    const std::int64_t mid = (b.best_bid().price.raw + b.best_ask().price.raw) / 2;
    s_.avg = s_.books == 0 ? mid : s_.avg + (mid - s_.avg) / 16;
    if (++s_.books % 4 != 0) return;
    const std::int64_t tick = ctx.instrument(id).tick.raw;
    const Price px = Price::from_raw(s_.avg / tick * tick) - ctx.instrument(id).ticks(40);
    static_cast<void>(ctx.send(NewOrderRequest::limit(id, Side::Buy, px, qt("0.001")).ioc()));
  }
  std::string_view state() noexcept {
    std::memcpy(buf_, &s_, sizeof s_);
    return {buf_, sizeof s_};
  }
  bool restore(std::string_view b) {
    restored.emplace_back(b);
    if (b.size() != sizeof s_) return false;
    std::memcpy(&s_, b.data(), sizeof s_);
    return true;
  }

 private:
  struct S {
    std::int64_t avg = 0;
    std::int64_t books = 0;
  };
  S s_;
  char buf_[sizeof(S)] = {};
};

// The new orders a journal records: send time, price, quantity, side.
using Sent = std::tuple<std::int64_t, std::int64_t, std::int64_t, int>;
std::vector<Sent> sent(const std::string& path) {
  JournalReader r;
  REQUIRE(r.open(path));
  std::vector<Sent> out;
  r.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) == 0 || h->type != EventType::OutNewOrder) return;
    const auto& m = msg_cast<OutNewOrderMsg>(h);
    out.emplace_back(h->recv_ts.ns, m.price.raw, m.qty.raw, static_cast<int>(m.side));
  });
  return out;
}

BacktestConfig ewma_config() {
  BacktestConfig c = synthetic_config(21, seconds(12));
  c.transport.fill_model = sim::FillModel::L2Queue;
  // A fixed latency: no draws, so a run from a checkpoint is in step with the one that wrote it.
  for (sim::LatencyParams* p : {&c.transport.order_out, &c.transport.ack_in})
    *p = sim::LatencyParams{microseconds(300), Duration{}, 0.0};
  c.markout_horizons.clear();
  return c;
}

}  // namespace

TEST_CASE("backtest.start_at: [backtest] start_at") {
  CHECK(parse("start_at = 1700000002.5\n").start_at == Timestamp{1'700'000'002'500'000'000});
  CHECK(parse("start_at = \"2026-10-20T08:00:00Z\"\n").start_at ==
        Timestamp{1'792'483'200'000'000'000});
  CHECK_FALSE(parse("").start_at.valid());
  CHECK_THROWS_WITH_AS(static_cast<void>(parse("start_at = \"soon\"\n")),
                       doctest::Contains("neither unix seconds nor a UTC time"),
                       ConfigError);
}

TEST_CASE("backtest.start_at: the strategy warms up, then trades from the positions given") {
  BacktestConfig cfg = synthetic_config(5, seconds(4));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  const Timestamp from = cfg.start + seconds(2);
  for (const bool warm : {false, true}) {
    CAPTURE(warm);
    cfg.positions = {StartPosition{InstrumentId{0}, qt("0.003"), px("60000")}};
    cfg.start_at = warm ? from : Timestamp{};
    Probe::looks.clear();
    const BacktestResult r = run_backtest<Probe>(cfg);
    REQUIRE(Probe::looks.size() > 20);
    std::size_t before = 0;
    for (const Probe::Look& l : Probe::looks) {
      if (warm && l.ts < from.ns) {
        // Refused as before a live session's first reconciliation, and flat.
        CHECK_FALSE(l.sent);
        CHECK(l.why == RejectReason::NotReconciled);
        CHECK(l.position.is_zero());
        ++before;
      } else if (l.ts > from.ns + 10'000'000 || !warm) {
        CHECK(l.sent);
        CHECK(l.position == qt("0.003"));
      }
    }
    CHECK((before > 0) == warm);
    for (std::size_t i = 0; warm && i < r.orders.size(); ++i) CHECK(r.orders.ts[i] >= from.ns);
  }
}

TEST_CASE("backtest.start_at: each account starts from its balances as the venue reported them") {
  const std::string path = tmp_path("start_at_balances.fmj");
  constexpr std::int64_t kS = 1'000'000'000;
  {
    JournalBuilder j(path, true);
    j.venue = 1 * kS;
    j.book(true, {{"100.00", "5"}}, {{"100.02", "5"}});
    j.balance("USDT", "100", BalanceMsg::kSnapshot);
    j.balance("BTC", "1", BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd);
    j.venue = 2 * kS;
    j.balance("USDT", "80", 0);  // an update after a fill
    j.venue = 3 * kS;
    j.balance("USDT", "50", BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd);  // BTC: none
    j.venue = 4 * kS;
    j.book(false, {{"100.00", "4"}}, {});
  }
  JournalSource src(path);
  const sim::RecordedSession& s = *src.recorded();
  const auto total = [&](std::int64_t t, const char* asset) -> std::optional<Notional> {
    for (const sim::SimAccountConfig& a : s.balances_at(Timestamp{t})) {
      for (const sim::SimBalance& b : a.balances) {
        if (b.asset.view() == asset) return b.amount;
      }
    }
    return std::nullopt;
  };
  CHECK(total(kS / 2, "USDT") == Notional::from_decimal("100").value());  // before: the first
  CHECK(total(2 * kS, "USDT") == Notional::from_decimal("80").value());
  CHECK(total(2 * kS, "BTC") == Notional::from_decimal("1").value());
  CHECK(total(3 * kS, "USDT") == Notional::from_decimal("50").value());
  CHECK_FALSE(total(3 * kS, "BTC").has_value());  // the snapshot did not name it
  REQUIRE(s.balances_at().size() == 1);
  CHECK(s.balances_at()[0].balances.size() == 2);  // the first snapshot
  REQUIRE(s.refreshes().size() == 2);
  CHECK(s.refreshes()[1].second == Timestamp{3 * kS});
}

TEST_CASE(
    "backtest.start_at: from a checkpoint the strategy sends what the run that wrote it sent") {
  // The continuous run, writing a checkpoint every 2 s.
  const std::string first = tmp_path("start_at_continuous.fmj");
  BacktestConfig a = ewma_config();
  a.engine.checkpoint_interval = seconds(2);
  a.journal_out = first;
  static_cast<void>(run_backtest<Ewma>(a));
  const std::vector<Sent> all = sent(first);
  REQUIRE(all.size() > 20);

  // From its journal (its own orders taken out of the feed), trading from the checkpoint at or
  // before 5 s: the one of 4 s.
  BacktestConfig b = ewma_config();
  b.initial_state = InitialState::Journal;
  b.start_at = a.start + seconds(5);
  sim::RecordedSession states;
  {
    JournalReader r;
    REQUIRE(r.open(first));
    read_states(std::span<JournalReader>{&r, 1}, states);
  }
  const sim::SessionStart at = states.start(b.start_at);
  REQUIRE(at.has_state);
  CHECK(at.at == a.start + seconds(4));
  std::string hash;
  for (int k = 0; k < 2; ++k) {
    Ewma::restored.clear();
    b.journal_out = tmp_path(k == 0 ? "start_at_branch.fmj" : "start_at_branch2.fmj");
    JournalSource src(first, true);
    const BacktestResult r = run_backtest<Ewma>(b, &src);
    // Restored once, at the take-over, with the checkpoint's bytes.
    REQUIRE(Ewma::restored.size() == 1);
    CHECK(Ewma::restored[0] == at.strategy);
    if (k == 1) CHECK(r.outbound_sha256 == hash);
    hash = r.outbound_sha256;
  }
  std::vector<Sent> after;
  for (const Sent& s : all) {
    if (std::get<0>(s) > at.at.ns) after.push_back(s);
  }
  const std::vector<Sent> from = sent(b.journal_out);
  REQUIRE_FALSE(from.empty());
  CHECK(from == after);

  // The run's journal replays bit for bit: the take-over restores the checkpoint it recorded.
  ReplayStrategy ewma;
  ewma.name = "ewma";
  ewma.schema = &Ewma::schema();
  ewma.make = [](RunnerDeps& deps) {
    return static_cast<sim::ReplayBackend*>(deps.backend)->make_runner<Ewma>(deps);
  };
  const ReplayResult rr = replay_journal(b.journal_out, b, ReplayOptions{}, ewma);
  INFO("first mismatch ",
       rr.first_mismatch,
       ": expected ",
       rr.expected_message,
       ", got ",
       rr.actual_message,
       "; ",
       rr.outbound_messages,
       " of ",
       rr.recorded_messages);
  CHECK(rr.ok());
  CHECK(rr.outbound_messages > 0);

  // Without a checkpoint by then, initial_state = "journal" refuses to guess.
  b.start_at = a.start + seconds(1);
  JournalSource early(first, true);
  CHECK_THROWS_WITH_AS(static_cast<void>(run_backtest<Ewma>(b, &early)),
                       doctest::Contains("no checkpoint at or before start_at"),
                       std::invalid_argument);
}

namespace {
// Sends nothing.
class Idle : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "idle"; }
};
}  // namespace

TEST_CASE("backtest.start_at: the accounts follow the session's flows of money and its refreshes") {
  // A session's journal: the account's first snapshot, 25 USDT in at 2 s, 500 out at 3 s (more
  // than the run will have free), a refresh at 4 s, books until 6 s.
  const std::string path = tmp_path("start_at_flows.fmj");
  constexpr std::int64_t kT0 = 1'700'000'000'000'000'000;
  constexpr std::int64_t kS = 1'000'000'000;
  {
    JournalBuilder j(path, true);
    j.now = kT0;
    j.venue = kT0;
    j.book(true, {{"100.00", "5"}}, {{"100.02", "5"}});
    j.balance("BTC", "1", BalanceMsg::kSnapshot);
    j.balance("USDT", "100", BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd);
    for (int s = 1; s <= 6; ++s) {
      j.now = kT0 + s * kS;
      j.venue = j.now;
      if (s == 2) j.flow("USDT", "25");
      if (s == 3) j.flow("USDT", "-500");
      if (s == 4) {
        j.balance("BTC", "1", BalanceMsg::kSnapshot);
        j.balance("USDT", "0", BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd);
      }
      const std::string bid = std::to_string(4 + s);
      j.book(false, {{"100.00", bid.c_str()}}, {});
    }
  }
  BacktestConfig cfg = BacktestConfig::single_instrument("BTCUSDT", px("0.01"), qt("0.001"));
  Instrument i = cfg.instruments.get(InstrumentId{0});
  i.base = "BTC";
  i.quote = "USDT";
  InstrumentTable t;
  REQUIRE(t.add(i));
  cfg.instruments = t;
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.markout_horizons.clear();
  cfg.measure_wall_clock = false;
  cfg.balances_from_journal = true;
  cfg.journal_out = tmp_path("start_at_flows_out.fmj");
  JournalSource src(path);
  {
    BacktestSession session(cfg, &src, &Idle::schema());
    std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<Idle>(session.deps());
    static_cast<void>(session.run(session.backend().hooks, runner.get(), "idle"));
    const sim::SimAccounts* a = session.backend().transport.accounts();
    REQUIRE(a != nullptr);
    CHECK(a->total(VenueId{0}, "USDT").is_zero());  // 100 + 25, then all of it out
    CHECK(a->total(VenueId{0}, "BTC") == Notional::from_decimal("1").value());
  }
  // The run's own account reports: its first snapshot, the two flows' updates, the refresh.
  JournalReader r;
  REQUIRE(r.open(cfg.journal_out));
  std::vector<std::int64_t> snapshots;
  int updates = 0;
  r.for_each([&](const EventHeader* h) {
    if (h->type != EventType::Balance) return;
    const auto& m = msg_cast<BalanceMsg>(h);
    if ((m.flags & BalanceMsg::kSnapshotEnd) != 0) {
      snapshots.push_back(h->exch_ts.ns);
    } else if ((m.flags & BalanceMsg::kSnapshot) == 0) {
      ++updates;
    }
  });
  REQUIRE(snapshots.size() == 2);
  CHECK(snapshots[1] == kT0 + 4 * kS);
  CHECK(updates == 2);
}
