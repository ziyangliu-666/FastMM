# 7. Backtest and replay from the command line

## Backtest

<!-- snippet: scripts/docs/tutorial.sh#cli-backtest -->
```bash
"$BIN"/tutorial-backtest --config configs/backtest-example.toml --data synthetic \
  --strategy first_mm --param edge_bps=0.002 --seed 7 --duration 60 \
  --out runs/tutorial/backtest --journal-out runs/tutorial/backtest.fmj
```

- `--config configs/backtest-example.toml` supplies the synthetic market, fees, risk limits and the fill model. Its strategy is `basic_mm`; `--strategy first_mm` replaces it and ignores its `[strategy.params]`.
- `--param edge_bps=0.002` sets a parameter; repeat `--param` for more. An unknown name is an error ([exit codes](../../reference/cli.md#fastmm-backtest)).
- `--seed 7 --duration 60` fixes the market and runs 60 s of simulated time.

```text
tutorial-backtest: note: ignoring [strategy.params] of 'basic_mm' for --strategy first_mm
backtest first_mm  seed=7  md_events=15148  steps=16808  wall=0.03s
  net pnl                        1.2964
  fills (maker / taker)          471 (471 / 0)
  ...
  outbound messages / sha256     1189 / 6ccab4815434470ef46161a79f32f6bf18ef422229d26be328adacc73df80812
results written to runs/tutorial/backtest/{equity,fills,orders}.csv and summary.json
session journal: runs/tutorial/backtest.fmj
```

| File | Content |
|---|---|
| `summary.json` | the metrics of the summary table |
| `equity.csv` | one row per 1 s bar: realised and unrealised PnL, fees, position, mid, which sides were quoted |
| `fills.csv` | one row per fill: time, side, price, quantity, fee, liquidity, the mid at the fill |
| `orders.csv` | one row per order message sent: new, cancel or replace |

Prices and quantities in the CSV files are raw fixed-point integers (divide by 1e8).

## Replay

<!-- snippet: scripts/docs/tutorial.sh#cli-replay -->
```bash
"$BIN"/tutorial-replay --journal runs/tutorial/backtest.fmj --verify
```

```text
journal  runs/tutorial/backtest.fmj: format v2, ... seed 7, strategy 'first_mm'
config   embedded in the journal (hash d9b752508c6d3bab)
replay   strategy=first_mm events=16814
recorded outbound 1189 msgs sha256 6ccab4815434470ef46161a79f32f6bf18ef422229d26be328adacc73df80812
replayed outbound 1189 msgs sha256 6ccab4815434470ef46161a79f32f6bf18ef422229d26be328adacc73df80812
replay MATCH
```

The journal holds the events the engine consumed and the configuration after the command-line overrides, so the replay needs no configuration file. `--verify` compares each order message the replayed engine sends with the recorded copy and exits with code 1 on the first difference, which it prints. A mismatch with the same binary means the strategy used something that is not an engine input ([Determinism](../../explanation/determinism.md)). The replayed strategy's logs go to stderr.

Next: [8. Trade on the simulated exchange](08-sim-exchange.md)
