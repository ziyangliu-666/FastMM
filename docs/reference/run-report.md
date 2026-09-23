# Run report

`fastmm report` turns what a run left behind into one HTML file: equity and inventory over time, where the PnL came from, markouts, fill quality, the counts, and the configuration that produced them. Code: `python/fastmm/report.py`.

The file is self-contained — inline CSS, charts as inline SVG and CSS bars, no JavaScript and no network at all — so it opens from a `file://` URL, keeps working when it is copied somewhere else, follows the system dark mode and prints.

## Write one

```bash
fastmm report runs/backtest                          # -> runs/backtest/report.html
fastmm report runs/20260101-101500/session.fmj       # -> the same directory
fastmm report runs/backtest -o /tmp/run.html
python3 tools/report.py runs/backtest                # the same, from a checkout, nothing installed
```

The command prints the path it wrote and nothing else.

| Input | Where it comes from |
|---|---|
| a backtest output directory | `fastmm-backtest --out <dir>` or `BacktestResult::write_all`: `summary.json`, `equity.csv`, `fills.csv` |
| a session journal (`.fmj`) | `fastmm-live --journal <path>`, `scripts/run-sim.sh` |
| a `BacktestResult` in memory | `fastmm.write_report(result, "report.html")` |

`fastmm` is the console script of the Python package ([Python](../python.md)); `python -m fastmm report` is the same command. `tools/report.py` loads the same module out of the source tree and needs only the standard library, which is how `scripts/run-sim.sh` writes its report.

The report names the instrument's assets and lists the strategy parameters when it can find the configuration: the one embedded in a session journal, a `.toml` file in the run directory, or `--config <file.toml>`.

## What it shows

| Panel | What is in it |
|---|---|
| header | strategy, kind of run, instrument, window, and the net PnL in the quote currency |
| key figures | fills and their maker/taker split, traded notional, fees paid, quote uptime, max drawdown, Sharpe per bar |
| equity and inventory | equity (realized + unrealized, less fees) and the net position on one time axis, with the worst drawdown marked, the inventory limit drawn as a threshold, and a strip showing when both sides were quoted |
| where the PnL came from | the `pnl_decomposition` of `summary.json`: gross spread capture, mid drift after the fills, fees, rebates, and what is left unexplained ([Economics](../explanation/economics.md)) |
| markouts | the venue mid at each horizon against the fill price, in bps of notional, per horizon and split buy/sell, with the fills each horizon excluded ([Backtesting](../explanation/backtesting.md)) |
| fill quality | at the touch / behind it / through it, time from quote to fill (p50, p90, p99), realized spread, quotes placed and filled, queue position at the fill |
| quotes, rejects and fills | orders, cancels, replaces, rejects, outbound messages, market-data events, tick-to-order latency, and when the fills arrived |
| configuration | the strategy parameters, the run settings (seed, window, markout horizons, outbound hash) and the effective configuration |

Every number is one the run itself reported; the report computes nothing a backtest already measured. A missing input removes its panel rather than filling it with a substitute.

## A session report is smaller

A live session journal records the fills, not the venue's book at each fill, so a session report has no markouts, no spread capture and no fill quality; it says so instead of guessing. It marks inventory at the last book ticker the venue published (or the last trade print), and splits the PnL into realized, unrealized and fees only.

For the full set of panels, backtest the journal — `fastmm-backtest --data session.fmj --out runs/replayed` — and report on that. Those numbers are a simulation over the session's market data, not the session ([Backtesting](../explanation/backtesting.md)).

## From Python

```python
import fastmm

result = fastmm.run_backtest(cfg, data="synthetic", strategy=MyMM)
print(fastmm.write_report(result, "report.html"))
```

`fastmm.write_report(source, out=None, config=None)` takes a result, a run directory or a journal path ([Python API](../api/python.md)).
