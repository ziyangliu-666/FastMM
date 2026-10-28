# Run an A/B test

An `[ab]` section runs two or more parameter sets of one strategy, the arms, in one session. The same section runs in `fastmm-live` and `fastmm-backtest`, so you can try a test on a recorded session before it goes live.

## Choose the design

| `assignment` | Each instrument | Compares | Strategy |
|---|---|---|---|
| `instrument` | stays in one arm | instruments of similar weight with each other | a parameter set per instrument |
| `time` | is in the arm every instrument is in, which changes each block | each instrument with itself in the neighbouring block | any that takes parameter updates |
| `crossover` | changes arm each block, the instruments split between the arms in every block | each instrument with itself, and the arms within each block | a parameter set per instrument |
| `branch` | is in one arm per run: `fastmm-backtest` runs the configuration once per arm over the same data and seed | each instrument with itself over the same market | any that takes parameter updates; a backtest only |

![The instrument, time and crossover assignments of two arms over four instruments and six blocks](../../assets/diagrams/ab-designs.svg#only-light){ .fastmm-diagram }
![The instrument, time and crossover assignments of two arms over four instruments and six blocks](../../assets/diagrams/ab-designs-dark.svg#only-dark){ .fastmm-diagram }

Start from `crossover`: it removes both the difference between instruments and the difference between moments. `time` suits a strategy that keeps one parameter set. `instrument` suits a parameter whose effect outlasts a block, such as the inventory a wider spread lets build up.

Blocks are `block_s` long and start at multiples of it since 1970-01-01 UTC, so a backtest and a live session over the same hours share them. Each run of as many blocks as there are arms gives each arm one block, in an order drawn from `seed`. Make a block longer than the parameter takes to show its effect, and the test several runs long.

`instrument` and `crossover` sort the instruments by `[ab.weights]`, such as a recent daily volume or a volatility. Each consecutive group of as many instruments as there are arms gets each arm once.

`branch` takes nothing from the market's noise: both arms see the same data, and the simulator's draws are the same until their orders differ. It tests what a change would have done over a recorded session, not how it does live, and `fastmm-live` refuses it. From a checkpoint of a live session, each arm starts in the session's state ([Replay a live session](../operations/replay-live-session.md#compare-two-configurations-from-a-checkpoint)).

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

Arm 0 is the control: the readout compares each other arm with it. An arm without `params` runs the configured `[strategy.params]`; an arm's `params` are set on top of them.

Each update carries every parameter some arm names, with the arm's value or the configured one. An instrument that leaves an arm gets the configured values back.

Two arms with no `params` are an A/A test. Run one first: its intervals are the noise floor of the comparison.

The arms are checked against the strategy at start. An unknown parameter, a value out of range or one `validate()` refuses stops the session or backtest with exit code 3.

## Try it on a recorded session

Backtest the configuration on the session's journal and read the result out:

```bash
fastmm-backtest --config mm-ab.toml --data runs/mm-1761091200.fmj --journal-out runs/ab-dry.fmj --out -
fastmm-data ab runs/ab-dry.fmj
```

The backtest publishes each arm's parameters at the block boundaries of the data's time, as the live session would.

## Run it live

1. Start the session. Each instrument is in its arm before the first event and moves at each block boundary.

    ```bash
    fastmm-live --config mm-ab.toml
    ```

2. See where the test is:

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

3. End the test, either way:

    | Command | Instruments go to |
    |---|---|
    | `fastmm-ctl --name mm ab stop` | the configured parameters, out of any arm |
    | `fastmm-ctl --name mm ab promote wide` | that arm's parameters |

While the test runs, `fastmm-ctl param` refuses the parameters the arms name; the others change as usual.

The assignment is a function of the configuration and the time, so a restart with `[ab]` still configured resumes where the clock is. A stop or promote lasts for the session. To keep a promoted arm, move its `params` into `[strategy.params]` and remove `[ab]`.

## Read it out

```bash
fastmm-data ab runs/mm-*.fmj --aa runs/aa-*.fmj --maker-rebate-bps 0.8 --csv ab-cells.csv
```

A backtest of four instruments in 60 s blocks, read out without `--aa`:

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

Pass all journals the test ran in, all parts of all sessions. They are read in time order and must carry the same `[ab]`. The configuration, the arm updates, the arm of each order and the BookTicker mids all come from the journals.

A fill belongs to the arm its order was sent under.

| Column | Meaning |
|---|---|
| `markout_bps` | mid at the horizon against the fill price, in bps of the notional marked |
| `fee_bps` | the rates of `[venues.<x>.fees]` and `[[instruments]]`, negative for a rebate, plus `--maker-rebate-bps` for a rebate paid apart from the fills |
| `net_bps` | markout less fees |
| `net/hour`, `notional/hour`, `fills/hour` | an arm's total over its instrument-hours in the pairs |
| `95% interval` | from resampling whole runs of blocks, or weight groups |
| `A/A +-` | with `--aa`, the half-width of the A/A test's interval for the same metric |

The comparison pairs what the design makes comparable: one instrument over one run of blocks (`time`, `crossover`), or the instruments of one weight group (`instrument`). A pair counts when it has time in all arms, which the test's first and last runs may not. A run cut short at start or stop counts for the time it has.

Resampling whole runs counts a market move that hit all instruments at once only once. A difference inside the `A/A +-` half-width is noise.

| Option | Writes |
|---|---|
| `--csv <file>` | one row per instrument, arm and block |
| `--json` | the whole report |

## What is recorded

| Where | What |
|---|---|
| journal | each arm change as a `ParamUpdate` with origin 2 and the arm; each `OutNewOrder` and `OutReplace` with its arm ([Journal format](../../reference/journal-format.md#parameter-updates)) |
| store | `ab_arms` names each session's arms; `fills`, `orders` and `param_updates` have an `arm` column ([Storage](../../reference/storage.md#ab_arms)) |

A replay reproduces the session with its arm changes. `fastmm-pnl fills` and `orders` print the arm.
