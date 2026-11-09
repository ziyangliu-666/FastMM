// Home placement in a backtest: a pool of three simulated accounts with balances of their own.
// With placement = "home" every buy lands on the instrument's home; the planner's moves reach the
// engine as journaled controls, and the journal replays to the identical outbound stream. The
// observe mode keeps the balance routing. With orders_credit_fills a fill gives its order's
// window slot back.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

sim::SimAccountConfig account(VenueId v, const char* btc, const char* usdt) {
  return sim::SimAccountConfig{v,
                               {sim::SimBalance{FixedString<8>("BTC"), nt(btc)},
                                sim::SimBalance{FixedString<8>("USDT"), nt(usdt)}}};
}

std::string tmp_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

BacktestConfig pool_config(Placement placement, std::int64_t plan_s) {
  BacktestConfig cfg = synthetic_config(11, seconds(120));
  Instrument i = cfg.instruments.get(InstrumentId{0});
  i.base = "BTC";
  i.quote = "USDT";
  InstrumentTable t;
  REQUIRE(t.add(i));
  cfg.instruments = t;
  cfg.strategy = "basic_mm";
  REQUIRE(cfg.transport.pools.add(VenueId{1}, VenueId{0}));
  REQUIRE(cfg.transport.pools.add(VenueId{2}, VenueId{0}));
  cfg.engine.pools = cfg.transport.pools;
  cfg.transport.accounts = {account(VenueId{0}, "0", "1000000"),
                            account(VenueId{1}, "0", "1000000"),
                            account(VenueId{2}, "0", "1000000")};
  cfg.engine.risk.max_position = qt("0.5");
  PoolPlacement& p = cfg.engine.homes.pool[0];
  p.configured = true;
  p.placement = placement;
  p.plan_interval = seconds(plan_s);
  p.plan_lookback = seconds(60);
  return cfg;
}

struct Run {
  BacktestResult result;
  std::array<std::size_t, 3> fills{};  // per account
  std::size_t planner_homes = 0;       // PoolHome inputs from the planner
  std::size_t engine_homes = 0;        // ... the engine's own records
};

Run run(const BacktestConfig& cfg) {
  BacktestSession session(cfg, nullptr, &BasicMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<BasicMM>(session.deps());
  Run out;
  out.result = session.run(session.backend().hooks, runner.get(), "basic_mm");
  if (cfg.journal_out.empty()) return out;
  JournalReader jr;
  REQUIRE(jr.open(cfg.journal_out));
  jr.for_each([&](const EventHeader* e) {
    if ((e->flags & EventHeader::kOutbound) != 0) return;
    if (e->type == EventType::OrderFill && e->venue.value < 3) ++out.fills[e->venue.value];
    if (e->type == EventType::Control &&
        reinterpret_cast<const ControlMsg*>(e)->command == ControlCommand::PoolHome) {
      ((e->flags & EventHeader::kSynthetic) != 0 ? out.engine_homes : out.planner_homes) += 1;
    }
  });
  return out;
}

}  // namespace

TEST_CASE("backtest.pool_home: buys land on the home, and the planner's moves replay exactly") {
  BacktestConfig cfg = pool_config(Placement::Home, 0);
  cfg.journal_out = tmp_journal("pool_home.fmj");
  const Run r = run(cfg);
  REQUIRE(r.result.metrics.fills > 0);
  // One home and nothing to move it: every fill is on one account.
  std::size_t accounts = 0;
  for (const std::size_t n : r.fills) accounts += n > 0 ? 1 : 0;
  CHECK(accounts == 1);
  CHECK(r.engine_homes == 1);  // the first order's home
  CHECK(r.result.transport.rejects_balance == 0);

  BacktestConfig planned = pool_config(Placement::Home, 30);
  planned.journal_out = tmp_journal("pool_home_planned.fmj");
  const Run p = run(planned);
  REQUIRE(p.result.metrics.fills > 0);
  const ReplayResult rp = replay_journal(planned.journal_out, planned);
  CHECK(rp.first_mismatch == -1);
  CHECK(rp.ok());
  // The same run twice: the same stream.
  planned.journal_out = tmp_journal("pool_home_planned2.fmj");
  CHECK(run(planned).result.outbound_sha256 == p.result.outbound_sha256);
}

TEST_CASE("backtest.pool_home: the observe mode keeps the balance routing") {
  BacktestConfig cfg = pool_config(Placement::Balance, 30);
  cfg.journal_out = tmp_journal("pool_home_observe.fmj");
  const Run plain = run(cfg);
  REQUIRE(plain.result.metrics.fills > 0);
  CHECK(plain.planner_homes >= 1);  // the planner gives homes all the same
  CHECK(replay_journal(cfg.journal_out, cfg).ok());
  // A window the routing reads and never reaches: accounts with the same room take turns. (A
  // replay sees no venue budget, so this run is not one to replay.)
  cfg.transport.orders_10s = 100000;
  const Run r = run(cfg);
  std::size_t accounts = 0;
  for (const std::size_t n : r.fills) accounts += n > 0 ? 1 : 0;
  CHECK(accounts > 1);
}

TEST_CASE("backtest.pool_home: orders_credit_fills gives a filled order's window slot back") {
  BacktestConfig cfg = pool_config(Placement::Home, 0);
  cfg.transport.orders_10s = 20;
  const Run plain = run(cfg);
  cfg.transport.orders_credit_fills = true;
  const Run credited = run(cfg);
  // The engine reads the simulated window and holds back what it would refuse.
  const auto limited = [](const Run& x) {
    return x.result.engine.risk_rejects_by_reason[RejectReason::RateLimit] +
           x.result.transport.rejects_rate_limit;
  };
  REQUIRE(limited(plain) > 0);
  CHECK(limited(credited) < limited(plain));
}

TEST_CASE("backtest.pool_home: spill busy takes the orders a full home window would refuse") {
  BacktestConfig cfg = pool_config(Placement::Home, 0);
  cfg.transport.orders_10s = 20;
  const Run home = run(cfg);
  cfg.engine.homes.pool[0].spill = HomeSpill::Busy;
  const Run busy = run(cfg);
  const auto limited = [](const Run& x) {
    return x.result.engine.risk_rejects_by_reason[RejectReason::RateLimit] +
           x.result.transport.rejects_rate_limit;
  };
  REQUIRE(limited(home) > 0);
  CHECK(limited(busy) < limited(home));
  CHECK(busy.result.engine.orders_sent > home.result.engine.orders_sent);
}
