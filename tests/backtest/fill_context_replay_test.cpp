// A fill's decision record (FillRecord from placed_ts on: its order's trail and the strategy's
// quote context) is made from the engine's own state and clock, so a replay of the journal makes
// the same records the recorded session handed its store.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/record_stream.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;

namespace {

std::filesystem::path repo_root() {
  return std::filesystem::path(FASTMM_FIXTURES_DIR).parent_path().parent_path();
}

std::vector<FillRecord> fills_in(MsgRing& ring, std::size_t& names) {
  std::vector<FillRecord> out;
  while (const std::byte* p = ring.try_peek()) {
    const auto* h = reinterpret_cast<const RecordHeader*>(p);
    if (h->type == RecordType::Fill) {
      FillRecord r;
      std::memcpy(&r, p, sizeof r);
      out.push_back(r);
    } else if (h->type == RecordType::ContextNames) {
      ++names;
    }
    ring.release();
  }
  return out;
}

}  // namespace

TEST_CASE("backtest.replay: a replay of the journal makes the same fill decision records") {
  BacktestConfig cfg = BacktestConfig::from_config(
      Config::parse(fastmm::test::read_file(repo_root() / "configs" / "backtest-example.toml")));
  cfg.measure_wall_clock = false;
  cfg.duration = seconds(60);
  const auto journal = fastmm::test::tmp_dir() / "fill_context_replay.fmj";
  std::filesystem::remove(journal);
  cfg.journal_out = journal.string();
  MsgRing recorded_ring(std::size_t{64} << 20);
  cfg.record_ring = &recorded_ring;
  const BacktestResult rec = run_backtest(cfg, cfg.strategy);
  std::size_t recorded_names = 0;
  const std::vector<FillRecord> recorded = fills_in(recorded_ring, recorded_names);
  REQUIRE_FALSE(rec.fills.ts.empty());
  REQUIRE(recorded.size() == rec.fills.ts.size());
  CHECK(recorded_names == 1);

  MsgRing replayed_ring(std::size_t{64} << 20);
  ReplayOptions opt;
  opt.record_ring = &replayed_ring;
  const ReplayResult rp = replay_journal(journal.string(), opt);
  REQUIRE(rp.ok());
  std::size_t replayed_names = 0;
  const std::vector<FillRecord> replayed = fills_in(replayed_ring, replayed_names);
  CHECK(replayed_names == 1);
  REQUIRE(replayed.size() == recorded.size());

  std::size_t with_trail = 0;
  std::size_t in_flight = 0;
  for (std::size_t i = 0; i < recorded.size(); ++i) {
    const FillRecord& a = recorded[i];
    const FillRecord& b = replayed[i];
    CAPTURE(i);
    CHECK(a.hdr.engine_ts == b.hdr.engine_ts);
    CHECK(a.cl_ord_id == b.cl_ord_id);
    // Everything past the header, the decision record included, byte for byte.
    CHECK(std::memcmp(reinterpret_cast<const std::byte*>(&a) + sizeof(RecordHeader),
                      reinterpret_cast<const std::byte*>(&b) + sizeof(RecordHeader),
                      sizeof(FillRecord) - sizeof(RecordHeader)) == 0);
    with_trail += a.trail;
    in_flight += a.pending_ts.valid() ? 1U : 0U;
    // basic_mm publishes the mid and the centre for every quote it sends.
    if (a.trail != 0) CHECK(a.order_context.v[0] > 0.0);
    CHECK(a.fill_context.v[0] > 0.0);
  }
  CHECK(with_trail == recorded.size());
  MESSAGE(recorded.size() << " fills, " << in_flight << " with a cancel or replace in flight");
}
