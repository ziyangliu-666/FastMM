// [start:main]
#include "my_mm.hpp"

#include "fastmm/backtest/backtest_runner.hpp"

#include <cstdio>

int main(int argc, char** argv) {
  auto cfg = bt::BacktestConfig::single_instrument("BTCUSDT", 0.01_px, 0.00001_qty);
  cfg.generator.market_qty_median_lots = 1500;  // synthetic market: larger taker orders
  cfg.transport.fees = sim::FeeModel::from_bps(10.0, 10.0);  // Binance spot VIP 0: 0.1 % both sides
  auto source = bt::open_data(argc > 1 ? argv[1] : "synthetic");  // .fmj, .csv or synthetic
  std::fputs(bt::run_backtest<MyMM>(cfg, source.get()).summary_table().c_str(), stdout);
}
// [end:main]
