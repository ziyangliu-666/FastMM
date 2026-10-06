// Calibration (backtest/calibrate.hpp): the queue_conservatism the fill check picks, the latency
// fitted to a journal's round trips, and a backtest compared with the session it came from. The
// journals are synthetic with a known answer; write_session() also builds the committed fixtures
// of the fastmm-data calibrate command test (FASTMM_REGEN_GOLDEN=1 rewrites them).
#include "fastmm/backtest/calibrate.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/config/config.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

constexpr std::array<double, 5> kGrid{0.0, 0.25, 0.5, 0.75, 1.0};
constexpr std::int64_t kUs = 1'000;
constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kT0 = 1'700'000'000'000'000'000;

// One order that joins behind 2 at its price; 1.5 of the 2 are cancelled, then a sell of 1.3
// trades there. The share of the cancel that was ahead of the order is the conservatism: at
// c = 0.5 1.25 are left ahead and the order fills 0.05, which is what the venue did. At c <= 0.5
// the model fills it, above not. A second order improves the touch and is cancelled untouched.
//
// Order latency: the venue stamps the ack `to_venue_us` after the send, the session receives it
// `round_trip_us` after the send (whole milliseconds when `ms`).
struct SessionSpec {
  int orders = 6;
  std::int64_t to_venue_us = 400;
  std::int64_t round_trip_us = 1500;
  bool ms = false;
};

void write_session(const std::string& path, const SessionSpec& spec) {
  JournalBuilder j(path);
  std::int64_t t = kT0;
  auto md = [&](std::int64_t dt) {
    t += dt;
    j.now = t;
    j.venue = t;
  };
  md(0);
  j.book(true, {{"99.98", "4"}}, {{"100.02", "4"}});
  std::uint64_t id = 1;
  std::uint64_t trade = 1;
  for (int i = 0; i < spec.orders; ++i) {
    md(10 * kMs);
    j.book(false, {{"100.00", "2"}}, {});
    // The queued order, and one improving the ask that nothing reaches.
    const std::uint64_t q = id++;
    const std::uint64_t idle = id++;
    md(kMs);
    const std::int64_t sent = t;
    j.out_new(q, Side::Buy, "100.00", "1");
    j.out_new(idle, Side::Sell, "100.01", "1");
    std::int64_t venue_ack = sent + spec.to_venue_us * kUs;
    if (spec.ms) venue_ack -= venue_ack % kMs;
    j.now = sent + spec.round_trip_us * kUs;
    j.venue = venue_ack;
    j.ack(q);
    j.ack(idle);
    t = j.now;
    md(kMs);
    j.book(false, {{"100.00", "0.5"}}, {});
    md(kMs);
    j.trade("100.00", "1.3", Side::Sell, trade++);
    j.venue = t;
    j.fill(q, "100.00", "0.05", "0.95", std::to_string(trade - 1).c_str());
    md(kMs);
    j.out_cancel(q);
    j.out_cancel(idle);
    j.cancel_ack(q);
    j.cancel_ack(idle);
    md(kMs);
    j.book(false, {{"100.00", "0"}}, {});
  }
}

FillScore score(
    std::uint64_t both, std::uint64_t live, std::uint64_t model, const char* lq, const char* mq) {
  FillScore s;
  s.both = both;
  s.live_only = live;
  s.model_only = model;
  s.live_qty = qt(lq);
  s.model_qty = qt(mq);
  return s;
}

}  // namespace

TEST_CASE(
    "backtest.calibrate: the pick is the lowest error, then model/live nearest 1, then the larger "
    "value") {
  const std::vector<double> c{0.0, 0.5, 1.0};
  // Error first: 1/3 at c = 0, 0 at 0.5 and 1.
  std::vector<FillScore> g{
      score(2, 0, 1, "2", "3"), score(2, 0, 0, "2", "2.5"), score(2, 0, 0, "2", "2")};
  CHECK(pick_conservatism(c, g) == 2);
  // Equal errors: the quantity ratio decides.
  g[2].model_qty = qt("1");
  CHECK(pick_conservatism(c, g) == 1);
  // Everything equal: the most conservative.
  g = {score(1, 1, 0, "2", "1"), score(1, 1, 0, "2", "1"), score(1, 1, 0, "2", "1")};
  CHECK(pick_conservatism(c, g) == 2);
  CHECK(g[0].error() == doctest::Approx(0.5));
  CHECK(g[0].hit_rate() == doctest::Approx(0.5));
  CHECK(g[0].miss_rate() == doctest::Approx(0.5));
}

TEST_CASE("backtest.calibrate: fills made at conservatism 0.5 fit 0.5 in every fold") {
  const std::vector<std::string> paths{tmp_path("calibrate_fit_a.fmj"),
                                       tmp_path("calibrate_fit_b.fmj")};
  write_session(paths[0], {.orders = 6});
  write_session(paths[1], {.orders = 4});
  const Calibration c = calibrate(paths, kGrid);
  REQUIRE(c.sessions.size() == 2);
  CHECK(c.identified);
  CHECK(c.conservatism[c.pick] == 0.5);
  const SessionCalibration& a = c.sessions[0];
  REQUIRE(a.grid.size() == kGrid.size());
  // Two orders per round: one fills live, one never; the model fills the first up to c = 0.5.
  for (std::size_t k = 0; k < kGrid.size(); ++k) {
    CAPTURE(kGrid[k]);
    CHECK(a.grid[k].live() == 6);
    CHECK(a.grid[k].neither == 6);
    if (kGrid[k] <= 0.5) {
      CHECK(a.grid[k].both == 6);
    } else {
      CHECK(a.grid[k].live_only == 6);
    }
  }
  CHECK(a.grid[2].qty_ratio() == doctest::Approx(1.0));
  REQUIRE(c.folds.size() == 2);
  for (const CalibrationFold& f : c.folds) {
    CHECK(c.conservatism[f.pick] == 0.5);
    CHECK(f.test_score.error() == 0.0);
    CHECK(f.test_best_pick == f.pick);
  }
  // No ticker in these journals: the inputs make no difference.
  CHECK(a.no_touch[2].both == 6);
  CHECK(a.no_both[2].both == 6);
  const std::string report = format_calibration(c);
  INFO(report);
  CHECK(report.find("queue_conservatism 0.50, fitted on 2 sessions\n") != std::string::npos);
  const std::string snippet = calibration_snippet(c);
  CHECK(snippet.find("queue_conservatism = 0.50\n") != std::string::npos);
  CHECK(snippet.find("fill_model = \"l2_queue\"\n") != std::string::npos);
  CHECK(calibration_csv(c).find("calibrate_fit_a.fmj,all,0.5000,6,0,0,6,") != std::string::npos);
}

TEST_CASE("backtest.calibrate: the latency model fitted to the journal's round trips") {
  SUBCASE("microsecond venue times: each leg as measured") {
    const std::string path = tmp_path("calibrate_latency_us.fmj");
    write_session(path, {.orders = 3, .to_venue_us = 400, .round_trip_us = 1500});
    JournalReader reader;
    REQUIRE(reader.open(path));
    const std::vector<VenueLatency> l = measure_latency(reader);
    REQUIRE(l.size() == 1);
    CHECK(l[0].name == "venue0");
    CHECK_FALSE(l[0].ms_venue_times);
    CHECK(l[0].round_trip_us.size() == 6);
    CHECK(l[0].fixed_us == 400);
    CHECK(l[0].jitter_us == 0);
    CHECK(l[0].ack_us == 1100);
    CHECK(l[0].ack_jitter_us == 0);
  }
  SUBCASE("millisecond venue times: the one-way leg from the stamps' intervals") {
    const std::string path = tmp_path("calibrate_latency_ms.fmj");
    write_session(path, {.orders = 3, .to_venue_us = 1300, .round_trip_us = 2500, .ms = true});
    JournalReader reader;
    REQUIRE(reader.open(path));
    const std::vector<VenueLatency> l = measure_latency(reader);
    REQUIRE(l.size() == 1);
    CHECK(l[0].ms_venue_times);
    // The first order goes out on a whole millisecond (1.3 ms is stamped 1 ms later: the latency
    // is in [1, 2) ms), the others half a millisecond into one (stamped 0.5 ms later: [0.5, 1.5)
    // ms). Both hold for [1, 1.5) ms; the fit takes the end nearest the middle of the stamps.
    CHECK(l[0].fixed_us == 1480);
    CHECK(l[0].jitter_us == 0);
    CHECK(l[0].ack_us == 1020);
  }
  SUBCASE("a spread: fixed and jitter through the 5th and 50th percentiles") {
    // 101 round trips whose 5th percentile is 687.7 us and median 1182.5 us: those of 300 us fixed
    // and a lognormal excess of mean 1000 us.
    VenueLatency v;
    for (int i = 0; i <= 100; ++i) {
      v.round_trip_us.push_back(i < 5     ? 600.0
                                : i == 5  ? 687.7
                                : i < 50  ? 1000.0
                                : i == 50 ? 1182.5
                                          : 5000.0);
    }
    v.to_venue_us.assign(101, 100.0);
    v.ms_venue_times = true;
    v.fit();
    CHECK(v.fixed_us == 100);
    CHECK(v.jitter_us == 0);
    CHECK(v.ack_us == 200);
    CHECK(v.ack_jitter_us == 1000);
  }
}

TEST_CASE("backtest.calibrate: a grid that scores the same everywhere keeps conservatism 1") {
  // Only the order that improves the touch: no queue, nothing for the conservatism to act on.
  const std::string path = tmp_path("calibrate_flat.fmj");
  {
    JournalBuilder j(path);
    j.book(true, {{"99.98", "4"}}, {{"100.02", "4"}});
    j.now += kMs;
    j.out_new(1, Side::Sell, "100.01", "1");
    j.now += kMs;
    j.ack(1);
    j.now += kMs;
    j.trade("100.01", "1", Side::Buy, 1);
    j.fill(1, "100.01", "1", "0", "1");
    j.now += kMs;
  }
  const std::vector<std::string> paths{path};
  const Calibration c = calibrate(paths, kGrid);
  CHECK_FALSE(c.identified);
  CHECK(c.conservatism[c.pick] == 1.0);
  CHECK(c.folds.empty());
  CHECK(format_calibration(c).find("not identified") != std::string::npos);
}

TEST_CASE("backtest.calibrate: a backtest's own journal compared with a re-run of it") {
  // The recorded session is itself an l2_queue backtest; its journal embeds the configuration,
  // and the re-run with it sends and fills the same.
  const char* toml = R"(
[engine]
name = "calibrate-test"
min_requote_interval_ms = 50
post_only = true
supports_replace = false
[venues.sim]
kind = "sim"
[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
tick = "0.01"
lot = "0.00001"
min_qty = "0.00001"
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 0.003
skew_bps_per_unit = 0
requote_threshold_ticks = 1
quote_qty = 0.002
max_inventory = 0.01
pull_on_stale_ms = 0
[risk]
max_order_qty = "0.01"
max_position = "0.05"
max_open_orders = 8
price_collar_bps = 200
stale_md_ms = 0
[backtest]
fill_model = "l2_queue"
duration_s = 90
seed = 41
[sim]
start_mid = "60000"
limit_rate_per_s = 300
market_rate_per_s = 40
mid_step_rate_per_s = 5
market_qty_median_lots = 300
)";
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(toml));
  cfg.measure_wall_clock = false;
  cfg.output_dir.clear();
  cfg.journal_out = tmp_path("calibrate_session.fmj");
  const BacktestResult rec = run_backtest(cfg, "basic_mm");
  REQUIRE(rec.metrics.fills > 5);
  const std::vector<std::string> paths{cfg.journal_out};
  const Calibration c = calibrate(paths, kGrid);
  CHECK(c.sessions[0].grid[c.pick].live() > 0);
  const std::vector<BacktestGap> gaps = compare_backtests(c, {});
  REQUIRE(gaps.size() == 1);
  const BacktestGap& g = gaps[0];
  CHECK(g.live.fills == rec.metrics.fills);
  CHECK(g.live.orders == rec.metrics.orders);
  INFO(format_backtest_gaps(gaps));
  CHECK(g.before.fills == g.live.fills);
  CHECK(g.before.orders == g.live.orders);
  CHECK(g.before.net == doctest::Approx(g.live.net));
  CHECK(g.after.fills > 0);
  CHECK(g.horizons_ns.size() == g.live.markout_bps.size());
  CHECK(format_backtest_gaps(gaps).find("backtest vs live: calibrate_session.fmj") !=
        std::string::npos);
}

TEST_CASE("backtest.calibrate: the command's fixtures are what write_session writes") {
  const auto dir = std::filesystem::path(FASTMM_FIXTURES_DIR) / "journals";
  const std::vector<std::string> fixtures{(dir / "calibrate_a.fmj").string(),
                                          (dir / "calibrate_b.fmj").string()};
  const std::vector<std::string> fresh{tmp_path("calibrate_a.fmj"), tmp_path("calibrate_b.fmj")};
  const SessionSpec a{.orders = 6};
  const SessionSpec b{.orders = 4};
  if (const char* regen = std::getenv("FASTMM_REGEN_GOLDEN");
      regen != nullptr && std::string(regen) == "1") {
    write_session(fixtures[0], a);
    write_session(fixtures[1], b);
  }
  write_session(fresh[0], a);
  write_session(fresh[1], b);
  const Calibration want = calibrate(fresh, kGrid);
  const Calibration got = calibrate(fixtures, kGrid);
  CHECK(calibration_csv(got) == calibration_csv(want));
  CHECK(calibration_snippet(got) == calibration_snippet(want));
  CHECK(calibration_snippet(got).find("latency_fixed_us = 400\n") != std::string::npos);
  CHECK(calibration_snippet(got).find("latency_ack_us = 1100\n") != std::string::npos);
}

TEST_CASE("backtest.calibrate: a cancel path and the intake's service time from bursts") {
  // New orders alone take 400 us to the venue, cancels alone 150 us; bursts of five cancels sent
  // together are taken in 200 us apart.
  VenueLatency v;
  std::int64_t t = kT0;
  for (int i = 0; i < 30; ++i) {
    t += 10 * kMs;
    v.messages.push_back({t, t + 400 * kUs, false});
    v.round_trip_us.push_back(1500.0);
    t += 10 * kMs;
    v.messages.push_back({t, t + 150 * kUs, true});
  }
  for (int b = 0; b < 10; ++b) {
    t += 10 * kMs;
    for (int k = 0; k < 5; ++k) v.messages.push_back({t, t + (150 + 200 * k) * kUs, true});
  }
  v.fit();
  CHECK(v.fixed_us == 400);
  CHECK(v.cancel_fitted);
  CHECK(v.cancel_fixed_us == 150);
  CHECK(v.cancel_jitter_us == 0);
  CHECK(v.burst_messages == 40);
  CHECK(v.service_us == 200);
  CHECK(v.burst_err_fit_us == doctest::Approx(0.0));
  CHECK(v.burst_err_us == doctest::Approx(500.0));  // (200 + 400 + 600 + 800) / 4 off
  Calibration c;
  c.conservatism = {1.0};
  c.latency.push_back(v);
  const auto keys = c.backtest_keys();
  const auto has = [&](const char* k, const char* value) {
    return std::find(keys.begin(), keys.end(), std::pair<std::string, std::string>{k, value}) !=
           keys.end();
  };
  CHECK(has("latency_cancel_us", "150"));
  CHECK(has("order_service_us", "200"));
  CHECK(calibration_snippet(c).find("order_service_us = 200\n") != std::string::npos);
  CHECK(format_calibration(c).find("intake: 200 us per message") != std::string::npos);
}
