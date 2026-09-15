// Parameter updates in backtests and replay (ADR-0013): updates scheduled at simulated times are
// journaled and replay to the recorded outbound hash, a replay that skips them does not, and
// max_param_age pulls the quotes in a gap between updates identically in replay.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

using Values = std::vector<ParamPublisher::ParamValue>;
using Updates = std::vector<std::pair<Duration, Values>>;

std::string tmp_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

// Runs BasicMM on the synthetic market with `updates` delivered at start + their offset. The first
// goes through the schedule's sink with a delay, the others are placed at their times.
BacktestResult run_with_updates(const BacktestConfig& cfg, const Updates& updates) {
  BacktestSession session(cfg, nullptr, &BasicMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<BasicMM>(session.deps());
  BasicMM configured;
  REQUIRE_FALSE(configured.configure(cfg.params).has_value());
  sim::ParamSchedule schedule;
  const Timestamp start = session.backend().clock.now();
  for (std::size_t i = 0; i < updates.size(); ++i) {
    const auto& [offset, values] = updates[i];
    if (i == 0) {
      schedule.set_delay(offset);
      ParamPublisher pub(schedule.sink(session.backend().clock), configured.params());
      REQUIRE(pub.publish(values));
      continue;
    }
    const ParamPublisher pub(ParamSink{}, configured.params());
    ParamUpdateMsg m{};
    REQUIRE_FALSE(pub.build(values, ParamPublisher::kAllInstruments, m).has_value());
    schedule.at(start + offset, m);
  }
  REQUIRE(schedule.size() == updates.size());
  session.set_param_schedule(&schedule);
  BacktestResult r = session.run(session.backend().hooks, runner.get(), BasicMM::name());
  CHECK(schedule.empty());
  return r;
}

std::size_t param_records(const std::string& path) {
  JournalReader reader;
  REQUIRE(reader.open(path));
  CHECK(reader.version() == kJournalVersion);
  CHECK(reader.params().size() == BasicMM::schema().size());
  std::size_t n = 0;
  reader.for_each([&](const EventHeader* h) {
    if (h->type != EventType::ParamUpdate) return;
    ++n;
    CHECK((h->flags & EventHeader::kEngineTime) != 0);
  });
  return n;
}

std::size_t new_orders(const BacktestResult& r, Duration from, Duration to) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < r.orders.size(); ++i) {
    const std::int64_t t = r.orders.ts[i] - r.start_ts;
    if (r.orders.kind[i] == kOrderKindNew && t >= from.ns && t < to.ns) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE(
    "backtest.params: scheduled parameter updates replay to the recorded hash, and not when "
    "skipped") {
  BacktestConfig cfg = synthetic_config(21, seconds(10));
  cfg.strategy = "basic_mm";
  cfg.journal_out = tmp_journal("params_basic_mm.fmj");
  const Updates updates{{seconds(2), {{"half_spread_bps", "0.01"}}},
                        {seconds(5), {{"half_spread_bps", "0.02"}, {"quote_qty", "0.003"}}}};
  const BacktestResult rec = run_with_updates(cfg, updates);
  REQUIRE(rec.outbound_messages > 20);
  CHECK(param_records(cfg.journal_out) == 2);

  const ReplayResult rp = replay_journal(cfg.journal_out, cfg);
  CHECK(rp.ok());
  CHECK(rp.outbound_sha256 == rec.outbound_sha256);
  CHECK(rp.outbound_messages == rec.outbound_messages);

  ReplayOptions skip;
  skip.param_updates = false;
  const ReplayResult without = replay_journal(cfg.journal_out, cfg, skip);
  CHECK_FALSE(without.ok());
  CHECK(without.first_mismatch >= 0);

  BacktestConfig plain = cfg;
  plain.journal_out.clear();
  CHECK(run_backtest(plain, "basic_mm").outbound_sha256 != rec.outbound_sha256);
}

TEST_CASE("backtest.params: max_param_age pulls the quotes between updates and replays exactly") {
  BacktestConfig cfg = synthetic_config(22, seconds(10));
  cfg.strategy = "basic_mm";
  cfg.engine.max_param_age = milliseconds(1500);
  cfg.journal_out = tmp_journal("params_max_age.fmj");
  const Updates updates{{seconds(1), {}},
                        {seconds(2), {{"half_spread_bps", "0.004"}}},
                        {seconds(6), {}},
                        {seconds(7), {}},
                        {seconds(8), {}}};
  const BacktestResult rec = run_with_updates(cfg, updates);
  CHECK(param_records(cfg.journal_out) == 5);
  CHECK(new_orders(rec, Duration{}, seconds(1)) == 0);  // before the first update
  CHECK(new_orders(rec, seconds(1), milliseconds(3500)) > 0);
  CHECK(new_orders(rec, milliseconds(3500) + milliseconds(1), seconds(6)) == 0);  // stale
  CHECK(new_orders(rec, seconds(6), seconds(10)) > 0);

  const ReplayResult rp = replay_journal(cfg.journal_out, cfg);
  CHECK(rp.ok());
  CHECK(rp.outbound_sha256 == rec.outbound_sha256);
}

TEST_CASE("backtest.params: max_param_age_ms is read from [strategy]") {
  const std::string base = R"(
[engine]
name = "t"
[venues.sim]
kind = "sim"
[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
tick = "0.01"
lot = "0.001"
[strategy]
name = "basic_mm"
)";
  const Config c = Config::parse(base + "max_param_age_ms = 1500\n");
  CHECK(c.strategy.max_param_age_ms == 1500);
  CHECK(BacktestConfig::from_config(c).engine.max_param_age == milliseconds(1500));
  CHECK(c.effective_toml().find("max_param_age_ms = 1500") != std::string::npos);
  const Config off = Config::parse(base);
  CHECK(off.strategy.max_param_age_ms == 0);
  CHECK(off.effective_toml().find("max_param_age_ms") == std::string::npos);
  CHECK_THROWS_AS(static_cast<void>(Config::parse(base + "max_param_age_ms = -1\n")), ConfigError);
}
