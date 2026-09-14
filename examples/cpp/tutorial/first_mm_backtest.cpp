// A C++ backtest of FirstMM on the synthetic market: no registration, no configuration file.
// It prints the summary and fails (exit code 1) if the strategy did not trade, broke its position
// limit, or two runs with the same seed sent different orders.
#include "first_mm.hpp"

#include "fastmm/backtest/backtest_runner.hpp"

#include <cstdio>

int main() {
  using namespace fastmm;
  // [start:config]
  auto cfg = bt::BacktestConfig::single_instrument("BTCUSDT", 0.01_px, 0.00001_qty);
  cfg.duration = seconds(60);            // of simulated time
  cfg.set_seed(7);                       // the synthetic market and the latency model
  cfg.generator.limit_rate_per_s = 400;  // a busier market than the defaults
  cfg.generator.market_rate_per_s = 30;
  cfg.generator.market_qty_median_lots = 1500;
  cfg.transport.fees = sim::FeeModel::from_bps(-0.5, 3.0);  // maker rebate 0.5 bps, taker fee 3 bps
  cfg.params = {{"edge_bps", "0.002"}, {"max_position", "0.004"}, {"report_ms", "0"}};
  // [end:config]

  // [start:run]
  const bt::BacktestResult first = bt::run_backtest<tutorial::FirstMM>(cfg);
  const bt::BacktestResult second = bt::run_backtest<tutorial::FirstMM>(cfg);
  std::fputs(first.summary_table().c_str(), stdout);

  int failures = 0;
  const auto expect = [&failures](bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok    " : "FAILED", what);
    failures += ok ? 0 : 1;
  };
  expect(first.metrics.fills > 0, "the strategy traded");
  // Largest |position| at the end of a 1 s bar, base units.
  expect(first.metrics.inventory_max <= 0.004, "the position stayed within max_position");
  expect(first.outbound_sha256 == second.outbound_sha256, "two runs sent the same orders");
  return failures == 0 ? 0 : 1;
  // [end:run]
}
