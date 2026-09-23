// [start:main]
#include "my_mm.hpp"

#include "fastmm/backtest/backtest_runner.hpp"

#include <cstdio>

int main(int argc, char** argv) {
  auto cfg = bt::BacktestConfig::single_instrument("BTCUSDT", 0.01_px, 0.00001_qty);
  cfg.generator.market_qty_median_lots = 1500;  // synthetic market: larger taker orders
  cfg.transport.fees = sim::FeeModel::from_bps(10.0, 10.0);  // Binance spot VIP 0: 0.1 % both sides
  auto source = bt::open_data(argc > 1 ? argv[1] : "synthetic");  // .fmj, .csv or synthetic
  const bt::BacktestResult result = bt::run_backtest<MyMM>(cfg, source.get());
  std::fputs(result.summary_table().c_str(), stdout);

  const char* out = argc > 2 ? argv[2] : "runs/quickstart";  // equity, fills, orders, summary
  if (!result.write_all(out)) {
    std::fprintf(stderr, "cannot write the results to %s\n", out);
    return 1;
  }
  std::printf("\nresults in %s/ -- for the HTML report: fastmm report %s\n", out, out);
}
// [end:main]
