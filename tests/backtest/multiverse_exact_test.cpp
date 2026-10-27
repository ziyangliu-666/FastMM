// The multiverse against run_backtest(): every built-in strategy, both queue fill models, many
// variants per pass that differ in their parameters, latency models (fixed, jittered, a cancel or
// fill path of their own, an order service time, market data that arrives late or with jitter),
// their feed showing their orders or not, loss limits that stop them early, replace or cancel and
// new. Each variant's result must equal its own backtest's in everything bt::result_difference
// compares: every fill, order, equity bar, count and the outbound hash.
#include "backtest_test_util.hpp"
#include "itch_session.hpp"
#include "options_market.hpp"
#include "random_market.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/multiverse.hpp"
#include "fastmm/backtest/result.hpp"
#include "fastmm/backtest/sweep.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/rng.hpp"

#include <fmt/format.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// Runs `cfgs` in multiverses (with `threads` workers) and alone, and checks every result.
void check_exact(const std::vector<BacktestConfig>& cfgs,
                 const std::string& strategy,
                 const SourceFactory& factory,
                 int threads = 1) {
  const std::vector<BacktestResult> mv = run_multiverse(cfgs, strategy, factory, threads);
  REQUIRE(mv.size() == cfgs.size());
  std::uint64_t fills = 0;
  for (std::size_t i = 0; i < cfgs.size(); ++i) {
    CAPTURE(i);
    const std::unique_ptr<MdSource> source = factory();
    const BacktestResult alone = run_backtest(cfgs[i], strategy, source.get());
    CHECK(result_difference(alone, mv[i]) == "");
    fills += alone.fills.size();
  }
  CHECK(fills > 0);  // the variants traded
}

// The axes a variant can differ on besides its strategy's parameters, picked by `rng`.
void vary_run(BacktestConfig& c, Xoshiro256ss& rng, bool md_jitter) {
  c.transport.seed = 1 + rng.uniform(1000);
  const std::int64_t lat = 20 + static_cast<std::int64_t>(rng.uniform(400));
  c.transport.order_out.fixed = microseconds(lat);
  c.transport.order_out.jitter =
      microseconds(rng.uniform(3) == 0 ? 0 : static_cast<std::int64_t>(rng.uniform(200)));
  c.transport.ack_in.fixed = microseconds(20 + static_cast<std::int64_t>(rng.uniform(400)));
  c.transport.ack_in.jitter = microseconds(static_cast<std::int64_t>(rng.uniform(100)));
  switch (rng.uniform(4)) {
    case 0:
      c.transport.md_in = {};
      break;
    case 1:
      c.transport.md_in.fixed = microseconds(static_cast<std::int64_t>(rng.uniform(3000)));
      break;
    case 2:
      c.transport.md_in.fixed = milliseconds(1);
      c.transport.md_in.jitter = md_jitter ? microseconds(300) : Duration{};
      break;
    default:
      c.transport.md_in.fixed = microseconds(500);
      break;
  }
  if (rng.uniform(4) == 0) {
    c.transport.cancel_latency = true;
    c.transport.cancel_out.fixed = microseconds(10);
  }
  if (rng.uniform(4) == 0) {
    c.transport.fill_latency = true;
    c.transport.fill_in.fixed = microseconds(700);
    c.transport.fill_in.jitter = microseconds(200);
  }
  if (rng.uniform(5) == 0) c.transport.order_service = microseconds(30);
  c.transport.own_orders_in_feed = rng.uniform(3) == 0;
  c.transport.queue_conservatism_bps = static_cast<std::int64_t>(rng.uniform(3)) * 5'000;
  if (rng.uniform(4) == 0)
    c.engine.risk.max_loss = Notional::from_raw(rng.uniform(2) == 0 ? 1 : 50'000'000'000);
  c.markout_horizons = {milliseconds(100), seconds(1)};
}

// `n` variants of `base`: each one point of `grid` (cycled) and its own run settings.
std::vector<BacktestConfig> variants(const BacktestConfig& base,
                                     const ParamGrid& grid,
                                     std::size_t n,
                                     std::uint64_t seed,
                                     bool md_jitter = true) {
  const std::vector<ParamMap> points = expand_grid(grid);
  Xoshiro256ss rng(seed);
  std::vector<BacktestConfig> out;
  for (std::size_t k = 0; k < n; ++k) {
    BacktestConfig c = base;
    for (const auto& [key, v] : points[k % points.size()]) c.params[key] = v;
    if (k > 0) vary_run(c, rng, md_jitter);
    out.push_back(std::move(c));
  }
  return out;
}

BacktestConfig config_of(const std::string& toml) {
  BacktestConfig c = BacktestConfig::from_config(Config::parse(toml));
  c.measure_wall_clock = false;
  c.output_dir.clear();
  return c;
}

std::string instruments(const std::vector<std::string>& venues) {
  std::string s;
  for (std::size_t i = 0; i < venues.size(); ++i) {
    s += fmt::format(R"(
[[instruments]]
venue = "{}"
symbol = "S{}"
base = "B{}"
quote = "USD"
tick = "0.01"
lot = "1"
min_qty = "1"
)",
                     venues[i],
                     i,
                     i);
  }
  return s;
}

constexpr const char* kEngine = R"(
[engine]
rng_seed = 3
min_requote_ticks = 1
min_requote_interval_ms = 0
min_qty_bps = 8000
post_only = true

[venues.a]
kind = "sim"
supports_replace = true
[venues.a.fees]
maker_bps = -0.5
taker_bps = 3.0
)";

constexpr const char* kRisk = R"(
[risk]
max_order_qty = "100"
max_order_notional = "100000"
max_position = "50"
max_open_orders = 32
stp = true
stale_md_ms = 0
)";

SourceFactory random_market(RandomMarketConfig rc) {
  return [rc]() -> std::unique_ptr<MdSource> { return std::make_unique<RandomMarket>(rc); };
}

std::string backtest(const char* model) {
  return fmt::format(R"(
[backtest]
fill_model = "{}"
latency_fixed_us = 100
latency_jitter_us = 0
equity_bar_s = 1
)",
                     model);
}

}  // namespace

std::uint64_t stress_seed(std::uint64_t base) {
  const char* e = std::getenv("MV_SEED");
  return e == nullptr ? base : base + std::strtoull(e, nullptr, 10) * 1000;
}

TEST_CASE("backtest.multiverse.exact: basic_mm under l2_queue, two instruments, 40 variants") {
  const BacktestConfig base = config_of(std::string(kEngine) + instruments({"a", "a"}) + R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 2
skew_bps_per_unit = 0.5
quote_qty = 1
max_inventory = 10
requote_threshold_ticks = 1
pull_on_stale_ms = 0
)" + kRisk + backtest("l2_queue"));
  const ParamGrid grid = {{"half_spread_bps", {"0.5", "2", "6"}},
                          {"quote_qty", {"1", "3"}},
                          {"requote_threshold_ticks", {"0", "1", "2"}},
                          {"levels", {"1", "2"}}};
  RandomMarketConfig rc;
  rc.seed = stress_seed(11);
  rc.venues = {VenueId{0}, VenueId{0}};
  check_exact(variants(base, grid, 40, stress_seed(101)), "basic_mm", random_market(rc));
  // The same in passes of several threads.
  check_exact(variants(base, grid, 12, stress_seed(102)), "basic_mm", random_market(rc), 3);
}

TEST_CASE("backtest.multiverse.exact: avellaneda_stoikov under l2_queue") {
  const BacktestConfig base = config_of(std::string(kEngine) + instruments({"a"}) + R"(
[strategy]
name = "avellaneda_stoikov"
[strategy.params]
gamma = 0.1
kappa = 50
quote_qty = 1
max_inventory = 10
sigma_window_s = 5
)" + kRisk + backtest("l2_queue"));
  const ParamGrid grid = {{"gamma", {"0.01", "0.1", "1"}},
                          {"estimate_kappa", {"false", "true"}},
                          {"requote_threshold_ticks", {"0", "1"}}};
  RandomMarketConfig rc;
  rc.seed = stress_seed(12);
  check_exact(variants(base, grid, 24, stress_seed(103)), "avellaneda_stoikov", random_market(rc));
}

TEST_CASE("backtest.multiverse.exact: lead_mm quoting one instrument off another's tickers") {
  const BacktestConfig base = config_of(std::string(kEngine) + instruments({"a", "a", "a"}) + R"(
[strategy]
name = "lead_mm"
[strategy.params]
target = 0
leader = 1
fx = -1
edge_min_bps = 0.5
quote_qty = 1
max_inventory = 10
max_leader_age_ms = 500
)" + kRisk + backtest("l2_queue"));
  const ParamGrid grid = {{"edge_min_bps", {"0.1", "1", "5"}},
                          {"hysteresis_bps", {"0", "0.5"}},
                          {"skew_bps_per_unit", {"0", "1"}}};
  RandomMarketConfig rc;
  rc.seed = stress_seed(13);
  rc.venues = {VenueId{0}, VenueId{0}, VenueId{0}};
  check_exact(variants(base, grid, 24, stress_seed(104)), "lead_mm", random_market(rc));
}

TEST_CASE(
    "backtest.multiverse.exact: our orders in the feed and jittered market data, deep books") {
  // Books 256 levels deep whose deepest levels come and go, every run's orders shown in its feed
  // (most of them) and half of the runs on one seed with jittered market data: those share one
  // feed, the rest each their own seed's.
  const ParamGrid grid = {{"half_spread_bps", {"0.2", "1", "4"}},
                          {"levels", {"1", "3"}},
                          {"requote_threshold_ticks", {"0", "2"}}};
  const auto deep = [](std::vector<BacktestConfig> cfgs) {
    for (std::size_t k = 0; k < cfgs.size(); ++k) {
      cfgs[k].transport.own_orders_in_feed = k % 5 != 4;
      if (k % 2 == 0) {
        cfgs[k].transport.seed = 7;
        cfgs[k].transport.md_in = sim::LatencyParams{microseconds(300), microseconds(200)};
      }
    }
    return cfgs;
  };
  RandomMarketConfig rc;
  rc.seed = stress_seed(15);
  rc.depth = 300;
  rc.venues = {VenueId{0}, VenueId{0}};
  const BacktestConfig mm = config_of(std::string(kEngine) + instruments({"a", "a"}) + R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 1
skew_bps_per_unit = 0.5
quote_qty = 1
max_inventory = 10
requote_threshold_ticks = 1
pull_on_stale_ms = 0
)" + kRisk + backtest("l2_queue"));
  check_exact(deep(variants(mm, grid, 32, stress_seed(111))), "basic_mm", random_market(rc));
  check_exact(deep(variants(mm, grid, 12, stress_seed(112))), "basic_mm", random_market(rc), 3);
  const BacktestConfig as = config_of(std::string(kEngine) + instruments({"a", "a"}) + R"(
[strategy]
name = "avellaneda_stoikov"
[strategy.params]
gamma = 0.1
kappa = 50
quote_qty = 1
max_inventory = 10
sigma_window_s = 5
)" + kRisk + backtest("l2_queue"));
  check_exact(deep(variants(as, {{"gamma", {"0.01", "0.1", "1"}}}, 12, stress_seed(113))),
              "avellaneda_stoikov",
              random_market(rc));
}

TEST_CASE(
    "backtest.multiverse.exact: a point that keeps no order rows has its backtest's metrics") {
  // The metrics count the orders as the run goes (OrderStats), so a sweep's points, which keep no
  // order rows, report what their backtests report from theirs.
  const BacktestConfig base = config_of(std::string(kEngine) + instruments({"a", "a"}) + R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 1
skew_bps_per_unit = 0.5
quote_qty = 1
max_inventory = 10
requote_threshold_ticks = 0
pull_on_stale_ms = 0
)" + kRisk + backtest("l2_queue"));
  std::vector<BacktestConfig> cfgs = variants(
      base, {{"half_spread_bps", {"0.2", "1"}}, {"levels", {"1", "3"}}}, 12, stress_seed(114));
  for (BacktestConfig& c : cfgs) c.keep_orders = false;
  RandomMarketConfig rc;
  rc.seed = stress_seed(16);
  rc.venues = {VenueId{0}, VenueId{0}};
  const SourceFactory factory = random_market(rc);
  const std::vector<BacktestResult> mv = run_multiverse(cfgs, "basic_mm", factory, 2);
  for (std::size_t i = 0; i < cfgs.size(); ++i) {
    CAPTURE(i);
    BacktestConfig c = cfgs[i];
    c.keep_orders = true;
    const std::unique_ptr<MdSource> source = factory();
    const BacktestResult alone = run_backtest(c, "basic_mm", source.get());
    REQUIRE(alone.orders.size() > 0);
    CHECK(mv[i].orders.size() == 0);
    CHECK(result_difference(alone, mv[i]) == "");
    // Every metric of the summary; the wall time is the only line that may differ.
    const auto summary = [](const BacktestResult& r) {
      std::string s = r.summary_json();
      const std::size_t w = s.find("\"wall_seconds\"");
      if (w != std::string::npos) s.erase(w, s.find('\n', w) - w);
      return s;
    };
    CHECK(summary(alone) == summary(mv[i]));
    CHECK(alone.metrics.fill_quality.quotes_filled > 0);
  }
}

TEST_CASE("backtest.multiverse.exact: xmm quoting one venue and hedging on another") {
  const BacktestConfig base = config_of(R"(
[engine]
rng_seed = 5
min_requote_interval_ms = 0

[venues.a]
kind = "sim"
[venues.b]
kind = "sim"
)" + instruments({"a", "b"}) + R"(
[strategy]
name = "xmm"
[strategy.params]
quote_qty = 1
edge_bps = 0.5
slippage_bps = 0
hedge_tolerance_bps = 50
basis_halflife_s = 0
max_unhedged = 5
requote_threshold_ticks = 0
)" + kRisk + backtest("l2_queue"));
  const ParamGrid grid = {{"edge_bps", {"0.2", "1", "3"}},
                          {"quote_qty", {"1", "2"}},
                          {"requote_threshold_ticks", {"0", "1"}}};
  RandomMarketConfig rc;
  rc.seed = stress_seed(14);
  rc.venues = {VenueId{0}, VenueId{1}};
  check_exact(variants(base, grid, 24, stress_seed(105)), "xmm", random_market(rc));
}

namespace {

std::string itch_config(const std::string& strategy) {
  return std::string(R"(
[engine]
rng_seed = 1
min_requote_ticks = 1
min_requote_interval_ms = 0
min_qty_bps = 8000
post_only = true

[venues.nq]
kind = "sim"
supports_replace = true
[venues.nq.fees]
maker_bps = -0.2
taker_bps = 0.3

[[instruments]]
venue = "nq"
symbol = "AAAA"
asset_class = "equity"
tick = "0.01"
lot = "1"
min_qty = "1"

[[instruments]]
venue = "nq"
symbol = "BBBB"
asset_class = "equity"
tick = "0.01"
lot = "1"
min_qty = "1"
)") + strategy +
         R"(
[risk]
max_order_qty = "10000"
max_order_notional = "10000000"
max_position = "100000"
max_open_orders = 64
stp = true

[backtest]
fill_model = "l3_queue"
latency_fixed_us = 50
latency_jitter_us = 0
equity_bar_s = 1
)";
}

SourceFactory itch_session(std::uint64_t seed, const BacktestConfig& cfg) {
  const std::uint64_t from = 34'200ULL * 1'000'000'000;  // 09:30 New York
  SessionWriter w(seed);
  const std::string path = w.write("multiverse_exact_" + std::to_string(seed) + "_07302019.itch",
                                   from,
                                   from + 60ULL * 1'000'000'000);
  const InstrumentTable* table = &cfg.instruments;
  return [path, table]() { return open_data("itch:" + path + ",date=2019-07-30", table); };
}

}  // namespace

TEST_CASE("backtest.multiverse.exact: basic_mm under l3_queue on a random ITCH session") {
  const BacktestConfig base = config_of(itch_config(R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 1
skew_bps_per_unit = 0.5
quote_qty = 100
max_inventory = 300
requote_threshold_ticks = 1
pull_on_stale_ms = 0
)"));
  const ParamGrid grid = {{"half_spread_bps", {"0.5", "1", "3", "8"}},
                          {"quote_qty", {"100", "200"}},
                          {"requote_threshold_ticks", {"0", "1", "2"}}};
  check_exact(
      variants(base, grid, 48, stress_seed(106)), "basic_mm", itch_session(stress_seed(21), base));
}

TEST_CASE(
    "backtest.multiverse.exact: walk-forward folds under l3_queue start on the resting orders") {
  const BacktestConfig base = config_of(itch_config(R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 1
skew_bps_per_unit = 0.5
quote_qty = 100
max_inventory = 300
requote_threshold_ticks = 1
pull_on_stale_ms = 0
)"));
  const ParamGrid grid = {{"half_spread_bps", {"0.5", "3"}}, {"quote_qty", {"100", "200"}}};
  const SourceFactory data = itch_session(stress_seed(23), base);
  const WalkForwardReport rep = walk_forward_by_name(base, "basic_mm", grid, data, 3, "net_pnl", 2);
  REQUIRE(rep.folds.size() == 3);
  const BacktestConfig start = shared_start(base);
  std::uint64_t fills = 0;
  for (const WalkForwardFold& f : rep.folds) {
    for (const SweepPoint& p : f.points) {
      BacktestConfig c = start;
      for (const auto& [k, v] : p.params) c.params[k] = v;
      const std::unique_ptr<MdSource> fold =
          fold_source(base, data(), Timestamp{f.start_ts}, Timestamp{f.end_ts});
      CAPTURE(f.start_ts);
      CHECK(result_difference(run_backtest(c, "basic_mm", fold.get()), p.result).empty());
      fills += p.result.fills.size();
    }
  }
  // The folds after the first fill orders that joined queues of orders resting before them.
  CHECK(fills > 0);
}

TEST_CASE("backtest.multiverse.exact: avellaneda_stoikov and lead_mm under l3_queue") {
  const BacktestConfig as = config_of(itch_config(R"(
[strategy]
name = "avellaneda_stoikov"
[strategy.params]
gamma = 0.1
kappa = 20
quote_qty = 100
max_inventory = 500
sigma_window_s = 5
)"));
  check_exact(variants(as, {{"gamma", {"0.01", "0.1", "1"}}}, 16, stress_seed(107)),
              "avellaneda_stoikov",
              itch_session(stress_seed(22), as));
  const BacktestConfig lead = config_of(itch_config(R"(
[strategy]
name = "lead_mm"
[strategy.params]
target = 0
leader = 1
fx = -1
edge_min_bps = 0.5
quote_qty = 100
max_inventory = 500
max_leader_age_ms = 2000
)"));
  check_exact(variants(lead, {{"edge_min_bps", {"0.1", "1", "4"}}}, 16, stress_seed(108)),
              "lead_mm",
              itch_session(stress_seed(23), lead));
}

TEST_CASE("backtest.multiverse.exact: options_mm on a scripted options market") {
  static const InstrumentTable table = option_table();
  BacktestConfig base;
  base.instruments = table;
  base.set_seed(24);
  base.transport.fill_model = sim::FillModel::L2Queue;
  base.transport.fees = sim::FeeModel::from_bps(0.0, 3.0);
  base.engine.quotes.min_requote_interval = milliseconds(50);
  base.engine.risk.max_open_orders = 8;
  base.params = {{"quote_qty", "0.1"},
                 {"max_position", "0.5"},
                 {"max_delta", "0.3"},
                 {"delta_skew_ticks", "5"},
                 {"inventory_skew_ticks", "1"},
                 {"max_vega", "0"},
                 {"pull_on_stale_ms", "1000"}};
  base.measure_wall_clock = false;
  const SourceFactory factory = []() -> std::unique_ptr<MdSource> {
    return std::make_unique<ScriptedOptionsSource>(table);
  };
  check_exact(variants(base,
                       {{"delta_skew_ticks", {"0", "5"}}, {"quote_qty", {"0.1", "0.2"}}},
                       16,
                       stress_seed(109)),
              "options_mm",
              factory);
}

TEST_CASE("backtest.multiverse.exact: l3_queue in passes on several threads, loss limits") {
  BacktestConfig base = config_of(itch_config(R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 0.5
skew_bps_per_unit = 0.5
quote_qty = 100
max_inventory = 500
requote_threshold_ticks = 1
pull_on_stale_ms = 0
)"));
  std::vector<BacktestConfig> cfgs =
      variants(base,
               {{"half_spread_bps", {"0.25", "0.5", "2"}}, {"skew_bps_per_unit", {"0", "1"}}},
               24,
               stress_seed(110));
  // Budgets that run out at different times: the max_loss bands must wake each engine exactly
  // where its loss reaches the limit.
  for (std::size_t k = 0; k < cfgs.size(); ++k)
    cfgs[k].engine.risk.max_loss = Notional::from_raw(static_cast<std::int64_t>(k) * 20'000'000);
  check_exact(cfgs, "basic_mm", itch_session(stress_seed(24), base), 3);
}

namespace {

// A random market that fails part of the way through.
class FailingMarket final : public MdSource {
 public:
  FailingMarket(const RandomMarketConfig& rc, std::size_t at) : inner_(rc), at_(at) {}
  const EventHeader* next() override {
    if (++n_ == at_) throw std::runtime_error("bad data");
    return inner_.next();
  }
  void reset() override {}
  [[nodiscard]] Timestamp start_ts() const override { return inner_.start_ts(); }

 private:
  RandomMarket inner_;
  std::size_t at_;
  std::size_t n_ = 0;
};

}  // namespace

TEST_CASE("backtest.multiverse: a failure in the data or in a pass stops every thread") {
  const BacktestConfig base = config_of(std::string(kEngine) + instruments({"a"}) + R"(
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 2
quote_qty = 1
)" + kRisk + backtest("l2_queue"));
  const std::vector<BacktestConfig> cfgs(4, base);
  RandomMarketConfig rc;
  rc.seed = stress_seed(13);
  rc.events = 40'000;
  // The data fails while the passes wait for more of it.
  const SourceFactory failing = [&]() -> std::unique_ptr<MdSource> {
    return std::make_unique<FailingMarket>(rc, 30'000);
  };
  CHECK_THROWS_WITH_AS(static_cast<void>(run_multiverse(cfgs, "basic_mm", failing, 3)),
                       "bad data",
                       std::runtime_error);
  // A run cannot be built while the data is read for the others.
  std::vector<BacktestConfig> bad = cfgs;
  bad[2].params["quote_qty"] = "-1";
  CHECK_THROWS_AS(static_cast<void>(run_multiverse(bad, "basic_mm", random_market(rc), 3)),
                  std::invalid_argument);
}
