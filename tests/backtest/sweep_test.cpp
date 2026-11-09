// A pool's sweep in a backtest: five simulated accounts, home placement with spill = "busy" under a
// tight order window, so spilled buys make pieces and the sweep moves them home through the
// simulated venue's transfers. With and without the venue's faults, at the end of each run: every
// account's estimate agrees with the venue, the pool's base moved only by its fills, every transfer
// the engine asked for resolved or is still held, none beyond the budget, and the run is
// deterministic.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

constexpr std::size_t kAccounts = 5;

BacktestConfig sweep_config(bool sweep) {
  BacktestConfig cfg = synthetic_config(23, seconds(900));
  Instrument i = cfg.instruments.get(InstrumentId{0});
  i.base = "BTC";
  i.quote = "USDT";
  InstrumentTable t;
  REQUIRE(t.add(i));
  cfg.instruments = t;
  cfg.strategy = "basic_mm";
  cfg.transport.accounts.clear();
  for (std::uint8_t v = 0; v < kAccounts; ++v) {
    if (v > 0) REQUIRE(cfg.transport.pools.add(VenueId{v}, VenueId{0}));
    cfg.transport.accounts.push_back(
        sim::SimAccountConfig{VenueId{v},
                              {sim::SimBalance{FixedString<8>("BTC"), nt("0.2")},
                               sim::SimBalance{FixedString<8>("USDT"), nt("50000")}}});
  }
  cfg.engine.pools = cfg.transport.pools;
  cfg.engine.risk.max_position = qt("2");
  cfg.transport.orders_10s = 12;
  cfg.transport.orders_credit_fills = true;
  cfg.transfer_latency = milliseconds(300);
  PoolPlacement& p = cfg.engine.homes.pool[0];
  p.configured = true;
  p.placement = Placement::Home;
  p.spill = HomeSpill::Busy;
  p.plan_interval = Duration{};
  LaneConfig& l = cfg.engine.homes.lane[0];
  l.sweep = sweep ? SweepMode::On : SweepMode::Off;
  l.per_minute = 6;
  l.timeout = seconds(60);
  l.status_interval = seconds(10);
  return cfg;
}

using SimEngine = Engine<BasicMM, SimClock, sim::SimTransport, InlineFeed>;

struct Run {
  BacktestResult result;
  std::array<std::int64_t, kAccounts> venue_btc{};   // the simulated venue's totals at the end
  std::array<std::int64_t, kAccounts> engine_btc{};  // ... and the engine's estimate
  std::array<std::int64_t, kAccounts> venue_free{};
  std::array<std::int64_t, kAccounts> engine_free{};
  std::uint32_t in_flight = 0;
};

Run run(const BacktestConfig& cfg) {
  BacktestSession session(cfg, nullptr, &BasicMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<BasicMM>(session.deps());
  Run out;
  out.result = session.run(session.backend().hooks, runner.get(), "basic_mm");
  auto* er = static_cast<EngineRunner<SimEngine, BasicMM>*>(runner.get());
  const SimEngine& e = er->engine();
  const sim::SimAccounts* acc = session.backend().transport.accounts();
  REQUIRE(acc != nullptr);
  for (std::uint8_t v = 0; v < kAccounts; ++v) {
    out.venue_btc[v] = acc->total(VenueId{v}, "BTC").raw;
    out.engine_btc[v] = e.balance(VenueId{v}, "BTC").total.raw;
    out.venue_free[v] = acc->free(VenueId{v}, "BTC").raw;
    out.engine_free[v] = e.balance(VenueId{v}, "BTC").free.raw;
  }
  if (const TransferLane* lane = e.lane(VenueId{0})) out.in_flight = lane->stats().in_flight;
  return out;
}

// The pool's base moved by its fills alone: what the accounts hold less what they started with.
void check_position(const Run& r) {
  std::int64_t held = 0;
  for (const std::int64_t b : r.venue_btc) held += b;
  std::int64_t filled = 0;
  for (std::size_t k = 0; k < r.result.fills.size(); ++k) {
    const std::int64_t q = r.result.fills.qty[k];
    filled += r.result.fills.side[k] == 0 ? q : -q;
  }
  CHECK(held - static_cast<std::int64_t>(kAccounts) * nt("0.2").raw == filled);
}

// No two transfers sent closer than the per-minute budget lets them, in any minute.
void check_budget(const Run& r, std::uint32_t per_minute) {
  std::vector<std::int64_t> ts;
  for (const sim::SimTransferRecord& t : r.result.treasury.transfers) ts.push_back(t.ts.ns);
  std::sort(ts.begin(), ts.end());
  for (std::size_t k = per_minute; k < ts.size(); ++k)
    CHECK(ts[k] - ts[k - per_minute] > 60'000'000'000 - 1'000'000'000);  // carried out 300 ms on
}

}  // namespace

TEST_CASE("backtest.sweep: spilled pieces go home and the accounts stay consistent") {
  const Run off = run(sweep_config(false));
  const Run on = run(sweep_config(true));
  REQUIRE(on.result.engine.home_spills > 0);
  const RunnerStats& s = on.result.engine;
  CHECK(s.lane_requested > 0);
  CHECK(s.lane_failed == 0);
  CHECK(s.lane_done + on.in_flight == s.lane_requested);
  CHECK(s.lane_done_by_flows == s.lane_done);
  CHECK(off.result.engine.lane_requested == 0);
  check_position(on);
  check_position(off);
  check_budget(on, 6);
  for (std::size_t v = 0; v < kAccounts; ++v) {
    CHECK(on.engine_btc[v] == on.venue_btc[v]);
    if (on.in_flight == 0) CHECK(on.engine_free[v] == on.venue_free[v]);
  }
  // No order was refused for a balance a transfer took.
  CHECK(on.result.transport.rejects_balance == 0);
  // The same run twice: the same stream.
  CHECK(run(sweep_config(true)).result.outbound_sha256 == on.result.outbound_sha256);
}

TEST_CASE("backtest.sweep: refused and lost transfers resolve, and the accounts stay consistent") {
  BacktestConfig cfg = sweep_config(true);
  cfg.transfer_p_fail = 0.05;
  cfg.transfer_p_unknown = 0.05;
  cfg.transfer_hang = milliseconds(90'000);
  cfg.engine.homes.lane[0].per_minute = 20;
  const Run r = run(cfg);
  const RunnerStats& s = r.result.engine;
  REQUIRE(s.lane_requested > 10);
  CHECK(s.lane_done + s.lane_failed + r.in_flight == s.lane_requested);
  check_position(r);
  for (std::size_t v = 0; v < kAccounts; ++v) CHECK(r.engine_btc[v] == r.venue_btc[v]);
  CHECK(r.result.transport.rejects_balance == 0);
  MESSAGE("requested " << s.lane_requested << " done " << s.lane_done << " failed " << s.lane_failed
                       << " unknown " << s.lane_unknown << " late " << s.lane_late << " trips "
                       << s.lane_trips << " in flight " << r.in_flight);
}
