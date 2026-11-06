# Run a live A/B by instrument

Run two parameter sets of one strategy at once, each on part of the instruments, and swap them every day. The engine assigns the instruments, publishes each arm's parameters, tags every order and fill with its arm, and `fastmm-pnl ab` compares the arms from the store with intervals over whole days.

## Check the strategy

The strategy must keep a parameter set per instrument and read it with `params(id)` ([Strategy API](../../reference/strategy-api.md#parameters-per-instrument)). `basic_mm`, `avellaneda_stoikov` and every Python strategy do; a Python strategy runs the same configuration with `fastmm.run_live` ([Python API](../../reference/python-api.md#live-sessions)). A strategy with one set refuses per-instrument values at start with exit code 3.

## Set values per instrument

Without a test, `[strategy.overrides]` gives an instrument, or a group from `[strategy.groups]`, its own values on top of `[strategy.params]`:

```toml
[strategy.params]
quote_qty = 0.01

[strategy.groups]
majors = ["BTCUSDT", "ETHUSDT"]

[strategy.overrides.majors]
quote_qty = 0.05

[strategy.overrides.ETHUSDT]
levels = 2
```

An instrument's own table wins over its groups'. Two groups that give one instrument different values, or a group named like a symbol, stop the session with exit code 3. The session publishes the values before its first event and the journal records them, so a replay or a backtest of the journal runs them too.

At run time, from the control socket:

| Command | Does |
|---|---|
| `fastmm-ctl --name mm param quote_qty=0.02 --instrument SOLUSDT` | sets SOLUSDT's own value |
| `fastmm-ctl --name mm param quote_qty=0.02 --group majors` | sets it for each instrument of the group |
| `fastmm-ctl --name mm param --reset quote_qty --instrument SOLUSDT` | SOLUSDT takes the shared value again |
| `fastmm-ctl --name mm param quote_qty=0.03` | changes the shared value; instruments with their own keep theirs |
| `fastmm-ctl --name mm params --instrument SOLUSDT` | the instrument's values, and `own=` the ones it holds as its own |

`fastmm-top` lists each instrument with an arm or values of its own under `params` ([Monitor with fastmm-top](../operations/monitor-with-fastmm-top.md)).

## Define the experiment

An `[ab]` section names the arms and how instruments are assigned to them ([Configuration](../../reference/configuration.md#ab)). This one compares a wider spread with the configured one, stratified by daily volume, swaps the arms on each instrument every UTC day, and ends on a date:

```toml
[ab]
name = "spread-nov"
assignment = "crossover"
block_s = 86400
seed = 7
end = "2026-12-01"

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
| a split stratified by volume | `[ab.weights]` for every instrument: instruments of similar weight go to different arms |
| your own list | `[ab.assign]`, `"SYMBOL" = "arm"`; instruments not listed stay out of the test |
| the same arm for good | `assignment = "instrument"` |
| a switchback | `assignment = "crossover"` with `block_s`: each instrument changes arm every block; with `[ab.assign]` the listed arm is its arm in one block of each pair |
| days that start at another hour | `block_offset_s`, e.g. 79200 to start each block at 22:00 UTC |

`[engine] markout_ms` makes the engine record the mid at each fill and at those horizons after it, which the readout needs. The engine holds up to 4096 fills per horizon waiting for their mid; fills past that are not marked at it.

A parameter an arm names belongs to the test: `[strategy.overrides]` may not set it, and `fastmm-ctl param` refuses it while the test runs. To test a parameter some instruments override, remove those overrides first, or list only the other instruments in `[ab.assign]`.

The split depends on the instrument list and the weights. Keep both fixed while the test runs, or name the arms in `[ab.assign]`: a change re-draws the split.

Try the configuration on a recorded session before it trades ([Run an A/B test](ab-test.md#try-it-on-a-recorded-session)). Run an A/A test, two arms with no `params`, to see how wide the intervals are with no difference.

## Run it

```bash
fastmm-live --config mm-ab.toml
fastmm-ctl --name mm ab
```

`fastmm-ctl ab` prints the test's state, the current block and each instrument's arm. The assignment depends only on the configuration and the clock, so a restart resumes where the clock is.

End the test with `fastmm-ctl --name mm ab stop`, which puts every instrument back on the shared values, or with `ab promote wide`, which keeps that arm's. Either lasts until the session stops: a restart with `[ab]` still configured runs the test again. Set `end`, or move the chosen arm's `params` into `[strategy.params]` and remove `[ab]`, before the next restart.

## Read it out

```bash
fastmm-pnl --engine mm ab --since 2026-11-01
```

Three days of a two-instrument test, the toy store of `tests/store/ab_readout_test.cpp`:

```text
test     t: 1 session(s), 3 UTC day(s) with time in every arm

arm              inst_hours  days    fills  maker%         notional         fees  params
control               72.00     3        3   100.0           300.00       0.0000  (configured)
wide                  72.00     3        3   100.0           300.00       0.0000  half_spread_bps=8

arm               horizon   marked  markout_bps    markout_pnl     per hour
control                1s        3        0.100       0.003000     0.000042
control                1m        3        0.200       0.006000     0.000083
wide                   1s        3        1.300       0.039000     0.000542
wide                   1m        3        2.600       0.078000     0.001083

against control, each instrument with itself: 95% intervals from 2000 resamples of whole UTC days
arm              metric                        arm        control           diff  95% interval
wide             markout pnl/hour 1s       0.000521       0.000042      +0.000479  [+0.000417, +0.000542]
wide             markout pnl/hour 1m       0.001042       0.000083      +0.000958  [+0.000833, +0.001083]
wide             markout bps 1s           1.250000       0.100000      +1.150000  [+1.000000, +1.300000]
wide             markout bps 1m           2.500000       0.200000      +2.300000  [+2.000000, +2.600000]
wide             notional/hour            4.166667       4.166667      +0.000000  [+0.000000, +0.000000]
wide             fills/hour               0.041667       0.041667      +0.000000  [+0.000000, +0.000000]
```

| Column | Meaning |
|---|---|
| `inst_hours`, `days` | the time instruments spent in the arm, from the arm assignments |
| `markout_bps` | mid at the horizon against the fill price, over the marked fills' notional; positive is in the strategy's favour |
| `markout_pnl` | each fill marked at the horizon, side × (mid − price) × quantity × contract size, less its fee, in the settlement currency |
| `95% interval` | of the difference with arm 0, from resampling whole UTC days |

Where instruments ran in both arms, as in a switchback, each instrument's rate in an arm is compared with its own in arm 0, and the differences are averaged weighted by hours. Resampling whole days counts a market move that hit every instrument on one day once, so the interval narrows with the number of days, not of fills.

`markout_pnl` is what the fills made by the horizon. The inventory a parameter leaves behind is not in it; `fastmm-pnl pnl` gives the realised PnL by day and instrument, which a daily switchback puts in one arm per day.

A fill belongs to the arm its order was sent under. `--store` names the store when it is not `runs/<engine>.db`, `--maker-rebate-bps` takes a rebate paid apart from the fills off the fees, `--horizons-ms` keeps some of the recorded horizons, `--test` names a test other than the newest session's, `--until` and `--instrument` narrow it, and `--csv` prints one row per arm and day. The readout warns when sessions run arms of one name with different parameters.

`fastmm-data ab` reads the same test out of the journals instead, with the configured fees and an A/A test's half-widths beside the intervals ([Run an A/B test](ab-test.md#read-it-out)). The store keeps the arms after the journals are deleted.
