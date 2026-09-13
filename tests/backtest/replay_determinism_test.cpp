// Replay proof (5.11): a journal recorded by a sim run, replayed through JournalFeed +
// ReplayTransport, reproduces the recorded outbound stream bit for bit; plus the golden
// fixture tests/fixtures/journals/sample_1000.{fmj,sha256}.
//
// Regenerating the golden files (only when the journal format, the message layouts, the
// synthetic generator or BasicMM change on purpose):
//
//   FASTMM_REGEN_GOLDEN=1 ./build/<preset>/tests/fastmm_backtest_tests
//       --test-case='backtest.golden*'
//
// rewrites sample_1000.fmj (the first 1000 market-data events of SyntheticSource seed 42 with
// the generator / instrument of configs/backtest-example.toml, written with
// JournalFileWriter) and sample_1000.sha256 (the outbound SHA-256 of basic_mm backtested on
// that journal under configs/backtest-example.toml). Commit both files together;
// `fastmm-replay --journal tests/fixtures/journals/sample_1000.fmj --verify` checks the same.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/synthetic_source.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {
std::string tmp_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

void check_replay(const BacktestResult& rec, const std::string& path, const BacktestConfig& cfg) {
  const JournalInfo info = inspect_journal(path);
  CHECK(info.strategy == rec.strategy);
  CHECK(info.outbound_messages == rec.outbound_messages);
  CHECK(info.market_data_messages == rec.md_events);
  const ReplayResult rp = replay_journal(path, cfg);
  CHECK(rp.strategy == rec.strategy);
  CHECK(rp.recorded_messages == rec.outbound_messages);
  CHECK(rp.recorded_sha256 == rec.outbound_sha256);
  CHECK(rp.outbound_messages == rec.outbound_messages);
  CHECK(rp.outbound_sha256 == rec.outbound_sha256);
  CHECK(rp.first_mismatch == -1);
  CHECK(rp.ok());
  CHECK(rp.events > rec.md_events);  // market data plus acks / fills / cancels
}

std::filesystem::path repo_root() {
  return std::filesystem::path(FASTMM_FIXTURES_DIR).parent_path().parent_path();
}
}  // namespace

TEST_CASE("backtest.replay: BasicMM coupled sim run replays to the identical outbound hash") {
  BacktestConfig cfg = synthetic_config(11, seconds(15));
  cfg.strategy = "basic_mm";
  cfg.journal_out = tmp_journal("replay_basic_mm.fmj");
  const BacktestResult rec = run_backtest(cfg, "basic_mm");
  REQUIRE(rec.metrics.fills > 0);
  REQUIRE(rec.outbound_messages > 20);
  check_replay(rec, cfg.journal_out, cfg);

  // what-if: another parameter set diverges and the replay says so
  BacktestConfig what_if = cfg;
  what_if.params["half_spread_bps"] = "1.5";
  const ReplayResult diff = replay_journal(cfg.journal_out, what_if);
  CHECK_FALSE(diff.ok());
  CHECK(diff.first_mismatch >= 0);
}

TEST_CASE("backtest.replay: timer-driven quote pulls and the L2 queue model replay exactly") {
  BacktestConfig cfg = synthetic_config(12, seconds(10));
  cfg.strategy = "basic_mm";
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 5000;
  cfg.params["pull_on_stale_ms"] = "20";  // 100 ms timer: pulls between 100 ms depth batches
  cfg.journal_out = tmp_journal("replay_timers.fmj");
  const BacktestResult rec = run_backtest(cfg, "basic_mm");
  REQUIRE(rec.engine.timers_fired > 0);
  REQUIRE(rec.metrics.cancels > 0);
  check_replay(rec, cfg.journal_out, cfg);
}

TEST_CASE("backtest.replay: AvellanedaStoikov replays exactly") {
  BacktestConfig cfg = synthetic_config(13, seconds(10));
  cfg.strategy = "avellaneda_stoikov";
  cfg.params = {
      {"gamma", "0.1"}, {"kappa", "100"}, {"quote_qty", "0.002"}, {"max_inventory", "0.02"}};
  cfg.journal_out = tmp_journal("replay_as.fmj");
  const BacktestResult rec = run_backtest(cfg, "avellaneda_stoikov");
  REQUIRE(rec.outbound_messages > 10);
  check_replay(rec, cfg.journal_out, cfg);
}

TEST_CASE("backtest.golden: sample_1000 journal gives the committed BasicMM outbound hash") {
  const auto dir = std::filesystem::path(FASTMM_FIXTURES_DIR) / "journals";
  const auto fmj = dir / "sample_1000.fmj";
  const auto sha = dir / "sample_1000.sha256";
  BacktestConfig cfg = BacktestConfig::from_config(
      Config::load((repo_root() / "configs" / "backtest-example.toml").string()));
  cfg.measure_wall_clock = false;
  REQUIRE(cfg.strategy == "basic_mm");

  const char* regen = std::getenv("FASTMM_REGEN_GOLDEN");
  if (regen != nullptr && std::string(regen) == "1") {
    SyntheticSourceConfig sc;
    sc.generator = cfg.generator;
    sc.md = cfg.transport.md;
    sc.seed = 42;
    sc.duration = seconds(600);
    SyntheticSource synth(sc);
    std::filesystem::create_directories(dir);
    REQUIRE(write_md_journal(synth, fmj.string(), cfg.instruments, 42, "synthetic-seed42", 1000) ==
            1000);
    JournalSource js(fmj.string());
    const BacktestResult r = run_backtest(cfg, cfg.strategy, &js);
    std::ofstream(sha, std::ios::trunc) << r.outbound_sha256 << "\n";
    MESSAGE("regenerated " << fmj.string() << " and " << sha.string() << ": " << r.outbound_sha256);
  }

  REQUIRE(std::filesystem::exists(fmj));
  REQUIRE(std::filesystem::exists(sha));
  const std::string expected = fastmm::test::read_file(sha).substr(0, 64);
  JournalSource js(fmj.string());
  CHECK(js.md_events() == 1000);
  CHECK(js.reader().header().rng_seed == 42);
  const BacktestResult r = run_backtest(cfg, cfg.strategy, &js);
  CHECK(r.md_events == 1000);
  CHECK(r.outbound_messages > 0);
  CHECK(r.outbound_sha256 == expected);

  // the same run, journaled, replays to the same hash
  cfg.journal_out = tmp_journal("golden_session.fmj");
  js.reset();
  const BacktestResult rec = run_backtest(cfg, cfg.strategy, &js);
  CHECK(rec.outbound_sha256 == expected);
  check_replay(rec, cfg.journal_out, cfg);
}
