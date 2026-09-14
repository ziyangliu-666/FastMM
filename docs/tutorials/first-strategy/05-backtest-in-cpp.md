# 5. Backtest in C++

In this page you run `first_mm` for 60 s of simulated time on a synthetic market, from a C++
program, and check three properties of the result. The program is
`examples/cpp/tutorial/first_mm_backtest.cpp`; like the quick start, it needs no registration.

## The configuration

<!-- snippet: examples/cpp/tutorial/first_mm_backtest.cpp#config -->
```cpp
auto cfg = bt::BacktestConfig::single_instrument("BTCUSDT", 0.01_px, 0.00001_qty);
cfg.duration = seconds(60);            // of simulated time
cfg.set_seed(7);                       // the synthetic market and the latency model
cfg.generator.limit_rate_per_s = 400;  // a busier market than the defaults
cfg.generator.market_rate_per_s = 30;
cfg.generator.market_qty_median_lots = 1500;
cfg.transport.fees = sim::FeeModel::from_bps(-0.5, 3.0);  // maker rebate 0.5 bps, taker fee 3 bps
cfg.params = {{"edge_bps", "0.002"}, {"max_position", "0.004"}, {"report_ms", "0"}};
```

- `single_instrument` builds a configuration with one instrument on a simulated venue.
- The seed fixes the synthetic market and the latency model, so every run sees the same events.
- The synthetic market's spread is one tick (0.01 USDT) at 60,000 USDT, so the edge is tiny here:
  0.002 bps of 60,000 is 0.012 USDT, about one tick. On the simulated exchange (page 8) and on
  Binance Demo the edge is 5 bps.
- `cfg.params` takes the same strings as a configuration file.

## Run and check

<!-- snippet: examples/cpp/tutorial/first_mm_backtest.cpp#run -->
```cpp
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
```

`run_backtest` returns fills, orders, an equity curve and summary metrics. The program asserts:

- the strategy traded (`fills > 0`);
- the largest position at the end of each 1 s bar stayed within `max_position` (0.004 BTC);
- a second run with the same configuration sent exactly the same order messages, compared through
  the SHA-256 of the outbound stream.

<!-- snippet: scripts/docs/tutorial.sh#cpp-backtest -->
```bash
"$BIN"/first_mm_backtest
```

```text
backtest first_mm  seed=7  md_events=18478  steps=20023  wall=0.01s
  net pnl                        1.3575
  realized / unrealized / fees   0.0067 / 0.0000 / -1.3507
  fills (maker / taker)          467 (467 / 0)
  orders / cancels / replaces    762 / 316 / 0
  inventory mean / |mean| / max  -0.00025 / 0.00172 / 0.00387
  outbound messages / sha256     1078 / f29af3d82986f7e18291c51a7d46503cf3097cfff09a22f2068e1f60c29f5deb
ok    : the strategy traded
ok    : the position stayed within max_position
ok    : two runs sent the same orders
```

Negative fees are rebates: the configuration pays makers 0.5 bps. All amounts are in USDT, the
position in BTC.

A backtest is a model. The matching engine and the synthetic flow are simplifications, and the
result says little about real profitability; its value here is a fast, repeatable check that the
strategy behaves as designed.

Next: [6. Register it](06-register.md)
