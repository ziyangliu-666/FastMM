// [venues.<x>] fetch_fees in a whole fastmm-live session against the in-process simulator: the
// account's rates replace the configured ones, the journal's embedded configuration carries them,
// and a replay of the journal reports and books the same rates. A refused request stops the start.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

std::string fresh(const std::string& name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

Config fees_config(const ServerFixture& fx, const std::string& name) {
  static const bool registered = [] {
    register_builtin_strategies(StrategyRegistry::instance());
    return true;
  }();
  static_cast<void>(registered);
  Config cfg = sim_local_config(fx, false);
  cfg.engine.name = name;
  cfg.engine.epoch_file = fresh(name + ".epoch");
  cfg.engine.kill_file = fresh(name + ".kill");
  cfg.venues[0].fees.maker_bps = 1.0;
  cfg.venues[0].fees.taker_bps = 4.0;
  cfg.venues[0].extra["fetch_fees"] = "true";
  return cfg;
}

live::LiveOptions options(const std::string& name) {
  live::LiveOptions o;
  o.duration_ns = seconds(2).ns;
  o.journal_path = fresh(name + ".fmj");
  o.program = "fetch-fees-test";
  return o;
}

}  // namespace

TEST_CASE("sim_exchange fetch_fees: the account's rates are used, journaled and replayed") {
  sim::server::SimServerConfig sc = test_server_config();
  sc.maker_bps = -0.25;
  sc.taker_bps = 7.5;
  ServerFixture fx(std::move(sc));
  const Config cfg = fees_config(fx, "fetch-fees");
  const live::LiveOptions o = options("fetch-fees");
  REQUIRE(live::run_live(cfg, o) == live::kExitOk);

  const bt::JournalInfo info = bt::inspect_journal(o.journal_path);
  CHECK(info.config_toml.find("maker_bps = -0.25") != std::string::npos);
  CHECK(info.config_toml.find("taker_bps = 7.5") != std::string::npos);
  const bt::BacktestConfig replayed = bt::journal_config(o.journal_path);
  CHECK(replayed.engine.fees.schedule(InstrumentId{0}) == FeeRates::from_bps(-0.25, 7.5));
  CHECK(replayed.transport.fees.schedule(InstrumentId{0}) == FeeRates::from_bps(-0.25, 7.5));
  const bt::ReplayResult r = bt::replay_journal(o.journal_path);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.ok());
}

TEST_CASE("sim_exchange fetch_fees: a refused request stops the session before it trades") {
  ServerFixture fx;
  Config cfg = fees_config(fx, "fetch-fees-refused");
  cfg.venues[0].api_secret = "wrong-secret";
  live::LiveOptions o = options("fetch-fees-refused");
  CHECK(live::run_live(cfg, o) == live::kExitVenue);
  CHECK(fx.server.stats().orders_accepted == 0);
}
