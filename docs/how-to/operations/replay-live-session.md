# Replay a live session from a checkpoint

Start a backtest inside a live session's state, so it matches the session and can test another configuration from the same point. Without one, a backtest over the session's journal lacks the session's positions, balances and learnt strategy state, and parts from the session at its first order. Why each step is there: [Backtesting](../../explanation/backtesting.md#starting-inside-a-live-session).

## Record checkpoints

Add to the session's configuration:

```toml
[engine]
checkpoint_s = 300
```

Every 300 s of engine time the journal gets a checkpoint: the strategy's `state()` if it keeps state, the positions and the open orders ([Journal format](../../reference/journal-format.md#strategy-state)). Besides the market, a backtest also reads back from the journal:

- the session's own orders, with their send, venue and receive times;
- the balances its venues reported;
- money moved in or out of its accounts outside the engine ([Balances](../../reference/venues/connector-behaviour.md#balances));
- the parameter changes it received.

## Backtest from a checkpoint

Copy the configuration the session ran with and add:

```toml
[backtest]
fill_model = "l2_queue"
md_arrival = "recorded"
initial_state = "journal"
start_at = "2026-10-20T08:00:00Z"
params_from_journal = true
balances_from_journal = true
latency_from_journal = true
```

Run it over the journal from a part well before `start_at`. The run reads the data from that part on and starts trading at the last checkpoint at or before `start_at`.

```bash
./build/release/bin/fastmm-backtest --config replay.toml --data "journal:runs/mm.12.fmj,strip_own=1" --journal-out runs/replay/session.fmj --out runs/replay
```

- The log names the checkpoint it started from (`journal: started from the checkpoint of <ns> ns`); a `start_at` before the first checkpoint stops the run with exit code 3.
- Give a part early enough for the books and the strategy's averages to settle before the checkpoint.
- Keys and limits: [Configuration](../../reference/configuration.md#backtest).

## Compare with the live session

Write both sessions' own traffic as CSV and lay them side by side ([Compare a backtest with the live session](journals-replay-pnl.md#compare-a-backtest-with-the-live-session)):

```bash
./build/release/bin/fastmm-data events runs/mm.12.fmj runs/mm.13.fmj --from-ns 1792483200000000000 --out live.csv
./build/release/bin/fastmm-data events runs/replay/session.fmj --out replay.csv
```

The run's journal replays bit for bit: `fastmm-replay --journal runs/replay/session.fmj --verify` exits 0.

## Compare two configurations from a checkpoint

Add a branch test to the backtest's configuration ([Run an A/B test](../strategies/ab-test.md)):

```toml
[ab]
name = "spread-wide"
assignment = "branch"

[[ab.arms]]
name = "control"

[[ab.arms]]
name = "wide"
params = { half_spread_bps = 8 }
```

```bash
./build/release/bin/fastmm-backtest --config branch.toml --data "journal:runs/mm.12.fmj,strip_own=1" --out runs/branch-1020
```

Each arm runs from the same checkpoint over the same data and seed. Its results and journal go to `runs/branch-1020/<arm>/`, followed by the `fastmm-data ab` readout.

An hour's window turns on a few fills. Run one branch per checkpoint over several days, each with its own `start_at` and `--out`, and read them out together; each branch is one cluster of the resampling:

```bash
./build/release/bin/fastmm-data ab runs/branch-*/*/session.fmj
```
