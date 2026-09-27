// A journal embeds the effective configuration, and a replay from the journal alone runs with it:
// the exposure caps and the net limit per underlying must be in that text, or the replayed engine
// sends orders the recorded one refused.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/config/config.hpp"

#include <filesystem>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;

namespace {

std::filesystem::path repo_root() {
  return std::filesystem::path(FASTMM_FIXTURES_DIR).parent_path().parent_path();
}

// configs/backtest-example.toml with `risk` added to [risk] and `extra` appended, 20 s.
BacktestConfig config_with(const std::string& risk, const std::string& extra = {}) {
  std::string text = fastmm::test::read_file(repo_root() / "configs" / "backtest-example.toml");
  const std::string anchor = "\nstp = true\n";
  const std::size_t at = text.find(anchor);
  REQUIRE(at != std::string::npos);
  text.insert(at + anchor.size(), risk + "\n");
  text += "\n" + extra;
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(text));
  cfg.measure_wall_clock = false;
  cfg.duration = seconds(20);
  return cfg;
}

std::string tmp_journal(const std::string& name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

// Records `capped` and checks that the cap changed what was sent (against the same run without it)
// and that the journal, replayed with the configuration it embeds, sends exactly that again.
void check_cap_replays(BacktestConfig capped, const std::string& name, const std::string& key) {
  const BacktestConfig plain = config_with("");
  const BacktestResult free_run = run_backtest(plain, plain.strategy);
  capped.journal_out = tmp_journal(name);
  const BacktestResult rec = run_backtest(capped, capped.strategy);
  REQUIRE(rec.outbound_messages > 0);
  CHECK(rec.outbound_sha256 != free_run.outbound_sha256);  // the cap refused something

  const JournalInfo info = inspect_journal(capped.journal_out);
  CHECK(info.config_toml.find(key) != std::string::npos);
  const ReplayResult rp = replay_journal(capped.journal_out);  // the embedded configuration
  CHECK(rp.recorded_sha256 == rec.outbound_sha256);
  CHECK(rp.outbound_sha256 == rec.outbound_sha256);
  CHECK(rp.first_mismatch == -1);
  CHECK(rp.ok());
}

}  // namespace

TEST_CASE("backtest.replay: max_gross_notional refusals replay from the journal's configuration") {
  check_cap_replays(
      config_with("max_gross_notional = \"150\""), "caps_gross.fmj", "max_gross_notional");
}

TEST_CASE("backtest.replay: max_net_notional refusals replay from the journal's configuration") {
  check_cap_replays(config_with("max_net_notional = \"150\""), "caps_net.fmj", "max_net_notional");
}

TEST_CASE("backtest.replay: [risk.underlying] refusals replay from the journal's configuration") {
  const BacktestConfig cfg = config_with("", "[risk.underlying.BTC]\nmax_net = \"0.003\"\n");
  REQUIRE(cfg.engine.underlying.active());
  check_cap_replays(cfg, "caps_underlying.fmj", "underlying");
}
