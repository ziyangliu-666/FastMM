// [ab] in a backtest: a crossover test over four instruments puts the arms' parameters in at the
// block boundaries, the orders carry their arm, the journal replays to the recorded hash, and the
// readout (fastmm-data ab) pairs the arms per instrument and run of blocks.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/ab_report.hpp"
#include "fastmm/backtest/array_source.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;

namespace {

constexpr std::int64_t kSec = 1'000'000'000;
constexpr std::int64_t kStart = std::int64_t{1'792'681'200} * kSec;  // a block boundary
constexpr std::int64_t kRun = 480 * kSec;                            // 8 blocks of 60 s
constexpr std::uint32_t kInstruments = 4;

std::string config(std::string_view arms) {
  std::string out = R"(
[engine]
name = "ab-test"
journal = false

[venues.sim]
kind = "sim"
fees = { maker_bps = -0.5, taker_bps = 3 }
)";
  for (std::uint32_t i = 0; i < kInstruments; ++i) {
    out += "\n[[instruments]]\nvenue = \"sim\"\nsymbol = \"S" + std::to_string(i) +
           "\"\ntick = \"0.01\"\nlot = \"0.001\"\nmin_qty = \"0.001\"\n";
  }
  out += R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 2
skew_bps_per_unit = 0
quote_qty = 0.01
max_inventory = 1
requote_threshold_ticks = 1
pull_on_stale_ms = 0

[risk]
max_order_qty = "1"
max_position = "10"
max_open_orders = 8

[backtest]
fill_model = "l2_queue"
queue_conservatism = 0
latency_fixed_us = 100
latency_jitter_us = 0

[ab]
name = "spread-1"
assignment = "crossover"
block_s = 60
seed = 5
)";
  out += arms;
  return out;
}

constexpr std::string_view kArms = R"(
[[ab.arms]]
name = "control"

[[ab.arms]]
name = "wide"
params = { half_spread_bps = 9 }
)";

constexpr std::string_view kSameArms = R"(
[[ab.arms]]
name = "a"

[[ab.arms]]
name = "b"
)";

// Every 100 ms per instrument: a one-level snapshot 0.05 either side of a random-walk mid, its
// book ticker, and a trade 0.03 to 0.12 through the mid, alternately sold and bought.
struct Tape {
  std::vector<std::int64_t> ts;
  std::vector<std::uint8_t> type;
  std::vector<std::uint32_t> inst;
  std::vector<std::int8_t> side;
  std::vector<double> price;
  std::vector<double> qty;

  Tape() {
    Xoshiro256ss rng(9);
    std::vector<double> mid(kInstruments, 100.0);
    std::int64_t n = 0;
    for (std::int64_t t = kStart; t < kStart + kRun; t += kSec / 10, ++n) {
      for (std::uint32_t i = 0; i < kInstruments; ++i) {
        mid[i] = std::round((mid[i] + (rng.uniform01() < 0.5 ? -0.01 : 0.01)) * 100) / 100;
        const double bid = mid[i] - 0.05;
        const double ask = mid[i] + 0.05;
        row(t, 0, i, 0, bid, 5);
        row(t, 0, i, 1, ask, 5);
        row(t, 3, i, 0, bid, 5);
        row(t, 3, i, 1, ask, 5);
      }
      for (std::uint32_t i = 0; i < kInstruments; ++i) {
        const bool sell = (n + i) % 2 == 0;
        const double through = 0.03 + 0.01 * std::floor(rng.uniform01() * 10);
        row(t + 1, 2, i, sell ? 1 : 0, sell ? mid[i] - through : mid[i] + through, 50);
      }
    }
  }
  void row(std::int64_t t, std::uint8_t ty, std::uint32_t i, std::int8_t s, double p, double q) {
    ts.push_back(t);
    type.push_back(ty);
    inst.push_back(i);
    side.push_back(s);
    price.push_back(p);
    qty.push_back(q);
  }
  [[nodiscard]] ArrayColumns columns() const {
    ArrayColumns c;
    c.ts = ts;
    c.type = type;
    c.inst = inst;
    c.side = side;
    c.price_f64 = price;
    c.qty_f64 = qty;
    return c;
  }
};

std::string tmp_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

BacktestConfig backtest_config(std::string_view arms, const std::string& journal) {
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(config(arms)));
  cfg.journal_out = journal;
  cfg.measure_wall_clock = false;
  return cfg;
}

}  // namespace

TEST_CASE("backtest.ab: a crossover test runs, replays, and reads out paired by instrument") {
  const Tape tape;
  const std::string path = tmp_journal("ab_crossover.fmj");
  const BacktestConfig cfg = backtest_config(kArms, path);
  ArraySource source(tape.columns());
  const BacktestResult res = run_backtest(cfg, "basic_mm", &source);
  REQUIRE(res.fills.size() > 100);

  // Each instrument changes arm within each run of two blocks, and between runs when the draw
  // says so: 4 to 8 updates per instrument, each to the other arm.
  JournalReader reader;
  REQUIRE(reader.open(path));
  std::map<std::uint32_t, std::vector<int>> arms;
  std::map<int, std::size_t> orders_by_arm;
  reader.for_each([&](const EventHeader* h) {
    if (h->type == EventType::ParamUpdate) {
      const auto& m = msg_cast<ParamUpdateMsg>(h);
      CHECK(m.origin == ParamUpdateMsg::Origin::Experiment);
      CHECK_FALSE(m.all_instruments());
      arms[m.hdr.instrument.value].push_back(m.arm - 1);
    } else if (h->type == EventType::OutNewOrder && (h->flags & EventHeader::kOutbound) != 0) {
      ++orders_by_arm[msg_cast<OutNewOrderMsg>(h).arm];
    }
  });
  REQUIRE(arms.size() == kInstruments);
  for (const auto& [inst, seq] : arms) {
    CHECK(seq.size() >= 4);
    CHECK(seq.size() <= 8);
    for (std::size_t k = 1; k < seq.size(); ++k) CHECK(seq[k] != seq[k - 1]);
  }
  CHECK(orders_by_arm[0] == 0);
  CHECK(orders_by_arm[1] > 0);
  CHECK(orders_by_arm[2] > 0);

  const ReplayResult rp = replay_journal(path, cfg);
  CHECK(rp.ok());
  CHECK(rp.outbound_sha256 == res.outbound_sha256);

  // A second run is the same run.
  BacktestConfig again = cfg;
  again.journal_out.clear();
  source.reset();
  CHECK(run_backtest(again, "basic_mm", &source).outbound_sha256 == res.outbound_sha256);

  const std::vector<std::string> journals{path};
  const AbReport r = ab_report(journals, {});
  CHECK(r.test == "spread-1");
  CHECK(r.assignment == "crossover");
  CHECK_FALSE(r.aa);
  REQUIRE(r.arms.size() == 2);
  CHECK(r.arms[1].params == "half_spread_bps=9");
  CHECK(r.outside_fills == 0);
  CHECK(r.pairs == kInstruments * 4);
  CHECK(r.incomplete_pairs == 0);
  CHECK(r.clusters == 4);
  CHECK(r.instruments.size() == kInstruments);
  CHECK(r.cells.size() == kInstruments * 8);
  // Every instrument spends half the run in each arm.
  CHECK(std::llabs(r.arms[0].ns - r.arms[1].ns) < kSec);
  CHECK(std::llabs(r.arms[0].ns + r.arms[1].ns - kInstruments * kRun) < kInstruments * kSec);
  CHECK(r.arms[0].fills + r.arms[1].fills == res.fills.size());
  // The wider arm trades less, and the interval says so.
  CHECK(r.arms[1].fills < r.arms[0].fills);
  const auto fills = std::find_if(r.effects.begin(), r.effects.end(), [](const AbEffect& e) {
    return e.metric == "fills/hour";
  });
  REQUIRE(fills != r.effects.end());
  CHECK(fills->diff < 0);
  CHECK(fills->hi < 0);
  CHECK(fills->lo <= fills->diff);
  CHECK(r.effects.size() == 2 * r.horizons_ns.size() + 2);
  const std::string text = format_ab_report(r);
  CHECK(text.find("test     spread-1: crossover, blocks of 60 s, seed 5, 2 arms") !=
        std::string::npos);
  CHECK(text.find("16 pair(s) (instrument x run of blocks) in 4 cluster(s)") != std::string::npos);
  CHECK(ab_report_json(r).find("\"metric\": \"fills/hour\"") != std::string::npos);
  CHECK(ab_cells_csv(r).find("sim:S0,control,") != std::string::npos);

  // An A/A run of the same market: its intervals are the noise floor beside each metric.
  const std::string aa_path = tmp_journal("ab_aa.fmj");
  source.reset();
  static_cast<void>(run_backtest(backtest_config(kSameArms, aa_path), "basic_mm", &source));
  const std::vector<std::string> aa_journals{aa_path};
  const AbReport aa = ab_report(aa_journals, {});
  CHECK(aa.aa);
  AbReport with_floor = r;
  add_noise_floor(with_floor, aa);
  CHECK(std::all_of(with_floor.effects.begin(), with_floor.effects.end(), [](const AbEffect& e) {
    return e.noise;
  }));
  CHECK(format_ab_report(with_floor).find("A/A +-") != std::string::npos);
  // Journals of different tests are refused together.
  const std::vector<std::string> mixed{path, aa_path};
  CHECK_THROWS_WITH_AS(static_cast<void>(ab_report(mixed, {})),
                       doctest::Contains("is not the one of the journals"),
                       std::runtime_error);
  std::filesystem::remove(path);
  std::filesystem::remove(aa_path);
}

TEST_CASE("backtest.ab: arms the strategy cannot run are refused before the run") {
  BacktestConfig cfg = backtest_config(kArms, "");
  cfg.ab.arms[1].params["half_spread_bps"] = "-1";
  CHECK_THROWS_WITH_AS(static_cast<void>(run_backtest(cfg, "basic_mm")),
                       doctest::Contains("[ab] arm 'wide': parameter 'half_spread_bps'"),
                       std::invalid_argument);
  BacktestConfig both = backtest_config(kArms, "");
  both.params_from_journal = true;
  CHECK_THROWS_WITH_AS(static_cast<void>(run_backtest(both, "basic_mm")),
                       doctest::Contains("params_from_journal"),
                       std::invalid_argument);
  BacktestConfig shared = backtest_config(kArms, "");
  shared.strategy = "lead_mm";
  shared.params.clear();
  shared.ab.arms[1].params = {{"enabled", "false"}};
  CHECK_THROWS(static_cast<void>(run_backtest(shared, "lead_mm")));
}
