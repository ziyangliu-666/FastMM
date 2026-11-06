# Run a live A/B by instrument

Run two parameter sets of one strategy at once, each on part of the instruments, and swap them every day. The engine assigns the instruments, publishes each arm's parameters, tags every order and fill with its arm, and `fastmm-pnl ab` compares the arms from the store with intervals over whole days.

## Check the strategy

The strategy must keep a parameter set per instrument and read it with `params(id)` ([Strategy API](../../reference/strategy-api.md#parameters-per-instrument)). `basic_mm`, `avellaneda_stoikov` and every Python strategy do. A strategy with one set refuses per-instrument values at start with exit code 3.

## Set values per instrument

Without a test, `[strategy.overrides]` gives an instrument, or a group from `[strategy.groups]`, its own values on top of `[strategy.params]`. An instrument's own table wins over its groups'.

```toml
[strategy.params]
half_spread_bps = 5

[strategy.groups]
majors = ["BTCUSDT", "ETHUSDT"]

[strategy.overrides.majors]
half_spread_bps = 3

[strategy.overrides.ETHUSDT]
levels = 2
```

The session publishes them before its first event, and the journal records them, so a replay or a backtest of the journal runs them too. At run time, from the control socket:

| Command | Does |
|---|---|
| `fastmm-ctl --name mm param half_spread_bps=4 --instrument SOLUSDT` | sets SOLUSDT's own value |
| `fastmm-ctl --name mm param half_spread_bps=4 --group majors` | sets it for each instrument of the group |
| `fastmm-ctl --name mm param --reset half_spread_bps --instrument SOLUSDT` | SOLUSDT takes the shared value again |
| `fastmm-ctl --name mm param half_spread_bps=6` | changes the shared value; instruments with their own keep theirs |
| `fastmm-ctl --name mm params --instrument SOLUSDT` | the instrument's values, and `own=` the ones it holds as its own |

`fastmm-top` lists each instrument with an arm or values of its own under `params`.

## Define the experiment

An `[ab]` section names the arms and how instruments are assigned to them ([Configuration](../../reference/configuration.md#ab)). This one compares a wider spread with the configured one, stratified by daily volume, and swaps the arms on each instrument every UTC day:

```toml
[ab]
name = "spread-nov"
assignment = "crossover"
block_s = 86400
seed = 7

[ab.weights]
BTCUSDT = 2.1e9
ETHUSDT = 9.5e8
SOLUSDT = 3.2e8
XRPUSDT = 2.7e8

[[ab.arms]]
name = "control"

[[ab.arms]]
name = "wide"
params = { half_spread_bps = 8 }

[engine]
markout_ms = [1000, 10000, 60000]
```

Pick the assignment and the schedule from these keys:

| You want | Set |
|---|---|
| a random split | no `[ab.weights]` and no `[ab.assign]`: the split is drawn from `seed` |
| a split stratified by volume | `[ab.weights]`: instruments of similar weight go to different arms |
| your own list | `[ab.assign]`, `"SYMBOL" = "arm"`; instruments not listed stay out of the test |
| the same arm for good | `assignment = "instrument"` |
| a switchback | `assignment = "crossover"` with `block_s`: each instrument changes arm every block |
| days that start at another hour | `block_offset_s`, e.g. 79200 to start each block at 22:00 UTC |

`[engine] markout_ms` makes the engine record the mid at each fill and at those horizons after it, which the readout needs. A parameter an arm names belongs to the test: `[strategy.overrides]` may not set it, and `fastmm-ctl param` refuses it while the test runs.

Try the configuration on a recorded session before it trades ([Run an A/B test](ab-test.md#try-it-on-a-recorded-session)).

## Run it

```bash
fastmm-live --config mm-ab.toml
fastmm-ctl --name mm ab
```

`fastmm-ctl ab` prints the test's state, the current block and each instrument's arm. The assignment depends only on the configuration and the clock, so a restart resumes where the clock is. End the test with `fastmm-ctl --name mm ab stop`, which puts every instrument back on the shared values, or with `ab promote wide`, which keeps that arm's.

## Read it out

```bash
fastmm-pnl --engine mm ab --since 2026-11-01
```

Three days of a two-instrument test in the store of `tests/store/ab_readout_test.cpp`:

```text
test     t: 1 session(s), 3 UTC day(s) with time in every arm

arm              inst_hours  days    fills  maker%         notional         fees  params
control               72.00     3        3   100.0           300.00       0.0000  (configured)
wide                  72.00     3        3   100.0           300.00       0.0000  half_spread_bps=8

arm               horizon   marked  markout_bps            pnl     pnl/hour
control                1s        3        0.100          0.003  4.16667e-05
control                1m        3        0.200          0.006  8.33333e-05
wide                   1s        3        1.300          0.039  0.000541667
wide                   1m        3        2.600          0.078   0.00108333

against control: 95% intervals from 2000 resamples of whole UTC days
arm              metric                        arm        control           diff  95% interval
wide             pnl/hour 1s           0.000541667    4.16667e-05        +0.0005  [+0.000416667, +0.000666667]
wide             pnl/hour 1m            0.00108333    8.33333e-05         +0.001  [+0.000833333, +0.00133333]
wide             markout bps 1s                1.3            0.1           +1.2  [+1, +1.6]
wide             markout bps 1m                2.6            0.2           +2.4  [+2, +3.2]
wide             notional/hour             4.16667        4.16667             +0  [+0, +0]
wide             fills/hour              0.0416667      0.0416667             +0  [+0, +0]
```

| Column | Meaning |
|---|---|
| `inst_hours`, `days` | the time instruments spent in the arm, from the arm assignments |
| `markout_bps` | mid at the horizon against the fill price, over the marked fills' notional |
| `pnl` | each fill marked at the horizon, side × (mid − price) × quantity × contract size, less its fee, in the settlement currency |
| `95% interval` | of the difference with arm 0, from resampling whole UTC days |

A fill belongs to the arm its order was sent under. Resampling whole days counts a market move that hit every instrument on one day once. `--horizons-ms` keeps some of the recorded horizons, `--test` names a test other than the newest session's, and `--csv` writes one row per arm and day.
