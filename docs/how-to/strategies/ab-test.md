# Run an A/B test

An `[ab]` section runs two or more parameter sets of one strategy, the arms, in the same session: the engine moves each instrument into its arm at the times the design says, tags every order with the arm it was sent under, and the journal and the store record both. `fastmm-data ab` reads the journals back into a comparison of the arms. The same section runs in `fastmm-live` and in `fastmm-backtest`, so a test can be tried on a recorded session before it goes live.

## Choose the design

| `assignment` | Each instrument | Compares | Strategy |
|---|---|---|---|
| `instrument` | stays in one arm | instruments of similar weight with each other | a parameter set per instrument |
| `time` | is in the arm every instrument is in, which changes each block | each instrument with itself in the neighbouring block | any that takes parameter updates |
| `crossover` | changes arm each block, the instruments split between the arms in every block | each instrument with itself, and the arms within each block | a parameter set per instrument |

- `crossover` removes both the difference between instruments and the difference between moments; it is the design to start from. `time` suits a strategy that keeps one parameter set. `instrument` suits a parameter whose effect outlasts a block, such as the inventory a wider spread lets build up.
- Blocks are `block_s` long and start at multiples of it since 1970-01-01 UTC, so a backtest and a live session over the same hours share them. Each run of as many blocks as there are arms gives every arm one block, in an order drawn from `seed`. Make a block longer than the time the parameter takes to show its effect, and the test several runs long.
- `instrument` and `crossover` sort the instruments by `[ab.weights]` (a recent daily volume, a volatility) and give each consecutive group of as many instruments as there are arms every arm once.

A strategy keeps a parameter set per instrument when it derives from `StrategyBase<Params, ParamScope::PerInstrument>` and reads `params(id)` ([Strategy API](../../reference/strategy-api.md#parameters-per-instrument)); `basic_mm` and `avellaneda_stoikov` do.

## Configure it

Add the section to the session's configuration ([Configuration](../../reference/configuration.md#ab)):

```toml
[ab]
name = "spread-1"
assignment = "crossover"
block_s = 3600
end = "2026-11-05T00:00:00Z"

[ab.weights]
BTCUSDT = 2.1e9
ETHUSDT = 9.5e8
SOLUSDT = 3.2e8
XRPUSDT = 2.7e8

[[ab.arms]]
name = "control"

[[ab.arms]]
name = "wide"
params = { half_spread_bps = 8, levels = 2 }
```

- Arm 0 is the control: the readout compares every other arm with it. An arm without `params` runs the configured `[strategy.params]`; an arm's `params` are set on top of them.
- Each update carries every parameter some arm names, the arm's value or the configured one, so an instrument that leaves an arm gets the configured values back.
- Two arms with no `params` are an A/A test: run one first, its intervals are the noise floor of the comparison.
- The arms are checked against the strategy when the session or backtest starts; an unknown parameter, a value out of range or one `validate()` refuses stops it with exit code 3.

## Try it on a recorded session

Backtest the configuration on the session's journal and read the result out:

```bash
fastmm-backtest --config mm-ab.toml --data runs/mm-1761091200.fmj --journal-out runs/ab-dry.fmj --out -
fastmm-data ab runs/ab-dry.fmj
```

The backtest publishes each arm's parameters at the block boundaries of the data's time, as the live session would.

## Run it live

`fastmm-live --config mm-ab.toml` puts every instrument in its arm before the first event and moves it at each block boundary. `fastmm-ctl ab` shows where the test is:

```bash
fastmm-ctl --name mm ab
```

```text
ok spread-1: running, assignment crossover, 2 arms, seed 7, block 3600 s
arm        0 control: the configured parameters; 2 instrument(s) now
arm        1 wide: half_spread_bps=8 levels=2; 2 instrument(s) now
block      497969 until 2026-10-22 18:00:00 UTC (in 58m 20s)
end        2026-11-05 00:00:00 UTC (in 13d 06h)
instrument binance:BTCUSDT wide
instrument binance:ETHUSDT control
...
```

- `fastmm-ctl --name mm ab stop` ends the test: every instrument goes back to the configured parameters, out of any arm. `ab promote wide` ends it with every instrument on that arm's parameters.
- While the test runs, `fastmm-ctl param` refuses the parameters the arms name; the others change as usual.
- The assignment is a function of the configuration and the time: a restart with `[ab]` still in the configuration resumes the test where the clock is. A stop or promote lasts for the session; to keep a promoted arm, move its `params` into `[strategy.params]` and remove `[ab]`.

## Read it out

```bash
fastmm-data ab runs/mm-*.fmj --aa runs/aa-*.fmj --maker-rebate-bps 0.8 --csv ab-cells.csv
```

A backtest of four instruments in 60 s blocks (`tests/backtest/ab_backtest_test.cpp`), read out without `--aa`:

```text
test     spread-1: crossover, blocks of 60 s, seed 5, 2 arms
read     1 journal(s), 2026-10-22 15:00:00 UTC to 2026-10-22 15:07:59 UTC
fills    12440 in the test, 0 of orders sent outside it, 0 repeated report(s) counted once
fees     the configured maker and taker rates

arm              inst_hours    fills  maker%         notional  capture_bps  params
control                0.27     8896   100.0          8890.67         0.95  (configured)
wide                   0.27     3544   100.0          3537.87         8.99  half_spread_bps=9

arm              horizon    marked markout_bps      buy     sell  fee_bps  net_bps       net/hour
control          1s           8877        0.89     0.89     0.89    -0.50     1.39         4.6386
...
wide             1m           3109        8.27     8.99     7.56    -0.50     8.77        10.2084

paired   against control: 16 pair(s) (instrument x run of blocks) in 4 cluster(s) (runs of blocks), 0 incomplete; 95% interval from 2000 resamples of the clusters
arm              metric                      arm        control           diff                   95% interval
wide             net/hour 1s             12.5528         4.6384        +7.9144             [+6.9959, +8.4289]
...
wide             net/hour 1m             10.2063         5.8063        +4.4000             [+2.3781, +6.4218]
wide             net bps 1m               8.7724         1.9926        +6.7798             [+5.8340, +7.5126]
wide             notional/hour        13269.6887     33346.9817    -20077.2929     [-20679.0299, -19662.7221]
wide             fills/hour           13292.6901     33367.0031    -20074.3130     [-20651.8130, -19666.1259]
```

- Pass every journal the test ran in, all parts of all sessions; they are read in time order and must carry the same `[ab]`. The configuration, the arm updates, the arm of each order and the BookTicker mids all come from the journals.
- A fill belongs to the arm its order was sent under. Markouts are the mid at the horizon against the fill price, in quote currency and in bps of the notional marked; fees are the rates `[venues.<x>.fees]` and `[[instruments]]` configure (negative: a rebate), and `--maker-rebate-bps` adds a rebate the venue pays apart from the fills. Net is markout less fees.
- The comparison pairs what the design makes comparable: one instrument over one run of blocks (time, crossover), or the instruments of one weight group (instrument). A pair counts when it has time in every arm; the test's first and last runs may not. `net/hour` and `notional/hour` are per instrument-hour.
- The interval resamples whole runs of blocks (or weight groups), so a market move that hit every instrument at once counts once. With `--aa`, the `A/A +-` column is the half-width of the A/A test's interval for the same metric: a difference inside it is noise.
- `--csv` writes one row per instrument, arm and block; `--json` prints the whole report.

## What is recorded

- The journal: each arm change is a `ParamUpdate` with origin 2 and the arm, and every `OutNewOrder` and `OutReplace` carries its arm ([Journal format](../../reference/journal-format.md#parameter-updates)). A replay reproduces the session with its arm changes.
- The store: `ab_arms` names each session's arms, and `fills`, `orders` and `param_updates` have an `arm` column ([Storage](../../reference/storage.md#ab_arms)). `fastmm-pnl fills` and `orders` print it.
