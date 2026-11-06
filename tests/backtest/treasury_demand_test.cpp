// A pool treasury with targets = "demand" and reserve = true in a backtest: the strategy declares
// what its instrument's bids want resting, the engine sums it by home, and the treasury funds the
// home from the account with the most to give. Each payout is held in the engine before it is sent,
// the records reach the engine and the journal, and the journal replays to the same stream. The
// simulated venue refuses transfers and loses answers when told to, and the treasury recovers.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/strategies/basic_mm.hpp"

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

// basic_mm that says its instrument's bids want 20000 USDT resting.
class FundedMM : public BasicMM {
 public:
  static constexpr std::string_view name() noexcept { return "funded_mm"; }
  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    BasicMM::on_start(ctx);
    ctx.set_funding_need(InstrumentId{0}, Notional::from_decimal("20000").value());
  }
};

sim::SimAccountConfig account(VenueId v, const char* usdt) {
  return sim::SimAccountConfig{v,
                               {sim::SimBalance{FixedString<8>("BTC"), nt("0")},
                                sim::SimBalance{FixedString<8>("USDT"), nt(usdt)}}};
}

std::string tmp_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

// Three accounts; the instrument's home is pinned to the third, which starts without quote.
BacktestConfig pool_config(TreasuryTargets targets, bool reserve) {
  BacktestConfig cfg = synthetic_config(11, seconds(600));
  Instrument i = cfg.instruments.get(InstrumentId{0});
  i.base = "BTC";
  i.quote = "USDT";
  InstrumentTable t;
  REQUIRE(t.add(i));
  cfg.instruments = t;
  cfg.strategy = "funded_mm";
  REQUIRE(cfg.transport.pools.add(VenueId{1}, VenueId{0}));
  REQUIRE(cfg.transport.pools.add(VenueId{2}, VenueId{0}));
  cfg.engine.pools = cfg.transport.pools;
  cfg.transport.accounts = {
      account(VenueId{0}, "100000"), account(VenueId{1}, "60000"), account(VenueId{2}, "0")};
  cfg.engine.risk.max_position = qt("0.5");
  PoolPlacement& p = cfg.engine.homes.pool[0];
  p.configured = true;
  p.placement = Placement::Home;
  p.plan_interval = Duration{};
  cfg.engine.homes.pin[0] = VenueId{2};
  TreasuryConfig c;
  c.enabled = true;
  c.asset = "USDT";
  c.targets = targets;
  c.reserve = reserve;
  c.members = cfg.transport.pools.members(VenueId{0});
  c.names[0] = "a";
  c.names[1] = "b";
  c.names[2] = "c";
  c.interval_ns = seconds(1).ns;
  c.min_interval_ns = seconds(5).ns;
  c.max_per_hour = 0;
  c.cooldown_ns = seconds(20).ns;
  c.timeout_ns = seconds(30).ns;
  cfg.treasuries.push_back(c);
  cfg.transfer_latency = milliseconds(300);
  return cfg;
}

struct Run {
  BacktestResult result;
  Notional home_usdt;   // the home's USDT at the end, venue side
  PoolFunding funding;  // the engine's view at the end
  std::size_t held_records = 0;
};

Run run(const BacktestConfig& cfg) {
  BacktestSession session(cfg, nullptr, &FundedMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<FundedMM>(session.deps());
  Run out;
  out.result = session.run(session.backend().hooks, runner.get(), "funded_mm");
  out.home_usdt = session.backend().transport.accounts()->total(VenueId{2}, "USDT");
  REQUIRE(runner->pool_funding(VenueId{0}, "USDT", out.funding));
  if (cfg.journal_out.empty()) return out;
  JournalReader jr;
  REQUIRE(jr.open(cfg.journal_out));
  jr.for_each([&](const EventHeader* e) {
    if ((e->flags & EventHeader::kOutbound) != 0 || e->type != EventType::Control) return;
    const auto& m = *reinterpret_cast<const ControlTransferMsg*>(e);
    if (m.command == ControlCommand::Transfer && (m.flags & ControlTransferMsg::kHeld) != 0)
      ++out.held_records;
  });
  return out;
}

ReplayStrategy funded_replay() {
  ReplayStrategy s;
  s.name = "funded_mm";
  s.schema = &FundedMM::schema();
  s.make = [](RunnerDeps& deps) {
    return static_cast<sim::ReplayBackend*>(deps.backend)->make_runner<FundedMM>(deps);
  };
  return s;
}

}  // namespace

TEST_CASE("backtest.treasury_demand: the home is funded for its instruments' need") {
  const Run d = run(pool_config(TreasuryTargets::Demand, false));
  // The need with 15% headroom, from the account with the most to give.
  CHECK(d.funding.need_of(VenueId{2}) == nt("20000"));
  CHECK(d.funding.need_of(VenueId{0}) == Notional{});
  REQUIRE_FALSE(d.result.treasury.transfers.empty());
  const sim::SimTransferRecord& first = d.result.treasury.transfers.front();
  CHECK(first.from == VenueId{0});
  CHECK(first.to == VenueId{2});
  CHECK(first.amount == nt("23000"));
  CHECK(d.result.metrics.fills > 0);

  // By weights the same pool levels the free balances: a third of it each.
  const Run w = run(pool_config(TreasuryTargets::Weights, false));
  REQUIRE_FALSE(w.result.treasury.transfers.empty());
  CHECK(w.result.treasury.transfers.front().amount > nt("40000"));
}

TEST_CASE("backtest.treasury_demand: a held payout reaches the engine and replays exactly") {
  BacktestConfig cfg = pool_config(TreasuryTargets::Demand, true);
  cfg.journal_out = tmp_journal("treasury_demand.fmj");
  const Run r = run(cfg);
  REQUIRE(r.result.treasury.done >= 1);
  CHECK(r.result.treasury.failed == 0);
  CHECK(r.result.treasury.transfers.front().amount == nt("23000"));
  CHECK(r.held_records >= 3);      // reserve, sent, done
  CHECK(r.funding.hold_seq == 0);  // nothing left held
  CHECK(r.home_usdt > Notional{});
  const ReplayResult rp = replay_journal(cfg.journal_out, cfg, {}, funded_replay());
  CHECK(rp.first_mismatch == -1);
  CHECK(rp.ok());
  cfg.journal_out = tmp_journal("treasury_demand2.fmj");
  CHECK(run(cfg).result.outbound_sha256 == r.result.outbound_sha256);
}

TEST_CASE("backtest.treasury_demand: refused and lost transfers are survived") {
  BacktestConfig cfg = pool_config(TreasuryTargets::Demand, true);
  cfg.treasuries[0].max_amount = nt("5000");  // several transfers to fund the home
  cfg.transfer_p_fail = 0.3;
  cfg.transfer_p_unknown = 0.3;
  cfg.transfer_hang = seconds(45);  // past timeout_s: times out, then resolves
  cfg.journal_out = tmp_journal("treasury_faults.fmj");
  const Run r = run(cfg);
  CHECK(r.result.treasury.failed >= 1);
  CHECK(r.result.treasury.timed_out >= 1);
  CHECK(r.result.treasury.done >= 1);
  // Nothing moved for a refusal, every lost one landed: the pool's quote is all still there.
  CHECK(r.result.treasury.moved("USDT") > 0);
  CHECK(r.funding.hold_seq == 0);
  CHECK(r.result.transport.rejects_balance == 0);
  const ReplayResult rp = replay_journal(cfg.journal_out, cfg, {}, funded_replay());
  CHECK(rp.ok());
  cfg.journal_out = tmp_journal("treasury_faults2.fmj");
  CHECK(run(cfg).result.outbound_sha256 == r.result.outbound_sha256);
}

TEST_CASE(
    "backtest.treasury_demand: a recorded session's held payouts move a later run's estimate") {
  BacktestConfig cfg = pool_config(TreasuryTargets::Demand, true);
  cfg.journal_out = tmp_journal("treasury_held_rec.fmj");
  const Run rec = run(cfg);
  REQUIRE(rec.held_records >= 3);

  // The recorded journal as the data, its balances followed, no treasury of its own: the session's
  // held payouts reach the engine at their times, and its journal has them again.
  BacktestConfig next = pool_config(TreasuryTargets::Demand, true);
  next.treasuries.clear();
  next.balances_from_journal = true;
  next.journal_out = tmp_journal("treasury_held_next.fmj");
  JournalSource src(cfg.journal_out);
  BacktestSession session(next, &src, &FundedMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<FundedMM>(session.deps());
  static_cast<void>(session.run(session.backend().hooks, runner.get(), "funded_mm"));
  std::size_t held = 0;
  JournalReader jr;
  REQUIRE(jr.open(next.journal_out));
  jr.for_each([&](const EventHeader* e) {
    if ((e->flags & EventHeader::kOutbound) != 0 || e->type != EventType::Control) return;
    const auto& m = *reinterpret_cast<const ControlTransferMsg*>(e);
    if (m.command == ControlCommand::Transfer && (m.flags & ControlTransferMsg::kHeld) != 0) ++held;
  });
  CHECK(held == rec.held_records);
  PoolFunding f;
  REQUIRE(runner->pool_funding(VenueId{0}, "USDT", f));
  CHECK(f.hold_seq == 0);
}
