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
#include "fastmm/core/crc32c.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

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

// Rewrites a v2 journal as format version 1: no engine-time records or flags, no session
// settings or embedded config in the header.
void downgrade_to_v1(const std::string& in, const std::string& out) {
  JournalReader r;
  REQUIRE(r.open(in));
  JournalFileHeader h = r.header();
  h.version = 1;
  h.header_bytes = static_cast<std::uint32_t>(sizeof(JournalFileHeader) +
                                              h.instrument_count * sizeof(Instrument));
  h.session_epoch = 0;
  h.quoting_enabled = 0;
  h.header_flags = 0;
  h.config_bytes = 0;
  h.replace_venues = 0;
  h.config_crc32c = 0;
  h.crc32c = crc32c(&h, offsetof(JournalFileHeader, crc32c));
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(&h), sizeof h);
  f.write(reinterpret_cast<const char*>(r.instruments().data()),
          static_cast<std::streamsize>(r.instruments().size_bytes()));
  std::vector<char> payload;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  std::uint32_t count = 0;
  auto flush = [&](std::uint32_t flags) {
    JournalBlockHeader b{};
    std::memcpy(b.magic, "FMJB", 4);
    b.byte_len = static_cast<std::uint32_t>(payload.size());
    b.seq_first = first;
    b.seq_last = last;
    b.count = count;
    b.crc32c = crc32c(payload.data(), payload.size());
    b.flags = flags;
    f.write(reinterpret_cast<const char*>(&b), sizeof b);
    f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    payload.clear();
    count = 0;
  };
  std::size_t engine_time_records = 0;
  r.for_each([&](const EventHeader* e) {
    if (e->type == EventType::EngineTime) {
      ++engine_time_records;
      return;
    }
    REQUIRE((e->flags & EventHeader::kDropped) == 0);
    if (payload.size() + e->len > kJournalBlockBytes) flush(0);
    if (count == 0) first = e->seq;
    last = e->seq;
    ++count;
    const std::size_t at = payload.size();
    payload.resize(at + e->len);
    std::memcpy(payload.data() + at, e, e->len);
    auto* copy = reinterpret_cast<EventHeader*>(payload.data() + at);
    copy->flags = static_cast<std::uint8_t>(copy->flags & ~EventHeader::kEngineTime);
    copy->reserved0 = 0;
  });
  CHECK(engine_time_records >= 2);  // start and finish
  if (count > 0) flush(0);
  flush(kBlockFlagTrailer);
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

TEST_CASE("backtest.replay: a version 1 session journal still replays on receive times") {
  BacktestConfig cfg = synthetic_config(21, seconds(10));
  cfg.strategy = "basic_mm";
  cfg.params["pull_on_stale_ms"] = "20";  // timers too
  cfg.journal_out = tmp_journal("replay_v2_source.fmj");
  const BacktestResult rec = run_backtest(cfg, "basic_mm");
  REQUIRE(rec.engine.timers_fired > 0);
  check_replay(rec, cfg.journal_out, cfg);

  const std::string v1 = tmp_journal("replay_v1.fmj");
  downgrade_to_v1(cfg.journal_out, v1);
  JournalReader r;
  REQUIRE(r.open(v1));
  CHECK(r.version() == 1);
  CHECK_FALSE(r.has_session());
  r.for_each([](const EventHeader* e) { CHECK((e->flags & EventHeader::kEngineTime) == 0); });
  check_replay(rec, v1, cfg);
}
