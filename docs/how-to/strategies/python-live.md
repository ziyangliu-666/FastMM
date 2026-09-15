# Run a Python strategy live

A class with hot hooks runs against venues inside the Python process, through `python -m fastmm run` or `fastmm.run_live`. Reference: [Live sessions](../../reference/python-api.md#live-sessions). Before a keyed session, work through the [Go-live checklist](../operations/go-live-checklist.md).

## Install

Install numba and the live runtime (CPython 3.10 or later):

```bash
pip install "fastmm-engine[hot,live]"
```

## Run against the simulated exchange

Start the simulated exchange:

```bash
./build/release/bin/fastmm-sim-exchange --config configs/sim.toml
```

Copy `configs/sim-local.toml` to `sim-py.toml` and set `name = "py:BasicMMHot"` in its `[strategy]` table, so its `[strategy.params]` apply to the class. In another terminal:

```bash
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret PYTHONPATH=examples/python/strategies python -m fastmm run basic_mm_hot:BasicMMHot --config sim-py.toml --duration 60s
```

The hooks compile and run once on scratch data before the session connects; then the log is the one `fastmm-live` writes. Ctrl-C stops the session early: the kill switch trips, open orders are cancelled, and the process exits with code 0 after `fastmm-live: shutdown took <n> ms (cancel_all ok)` ([Kill switch and shutdown](../operations/kill-switch-and-shutdown.md)).

From a script, `run_live` returns the exit code:

```python
import fastmm
from basic_mm_hot import BasicMMHot
raise SystemExit(fastmm.run_live(BasicMMHot, "sim-py.toml", duration="60s"))
```

## Run slow methods live

A class with slow methods ([Run slow methods beside hot hooks](python-slow-methods.md)) runs the same way. `on_start` runs before the session connects, and the session does not quote before the first publish:

```bash
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret PYTHONPATH=examples/python/strategies python -m fastmm run hot_slow_mm:HotSlowMM --config configs/sim-local.toml --duration 60s
```

`[strategy.params]` of `basic_mm` in `configs/sim-local.toml` do not apply to the class (stderr says `note: ignoring [strategy.params]`). The session stops with exit code 7 when a slow method raises, a call runs past its `timeout`, the fills ring is full or the slow thread ends ([Slow methods live](../../reference/python-api.md#slow-methods-live)).

1. Set each `timeout` above the longest wall time the method can take; `run_backtest` reports it in `result.slow_methods`.
2. Keep `max_param_age_ms` below the timeouts, so a stalled model pulls the quotes before the session stops: the log shows `no parameter update for <n> ms (max_param_age_ms): quotes pulled until the next`.
3. Make `on_stop` return: `python -m fastmm run` waits `--slow-tier-timeout-ms` (10000 ms by default) for it after the session stops, then exits.

## Keep other threads off the engine's core

1. Pin the engine thread with `[engine] cpu` (and `net_cpus` for the venue threads). Before `on_start`, the calling thread moves to the other CPUs, so the slow thread and the threads `on_start` starts inherit them; when the session starts, every other thread of the process moves too, and the log says `thread affinity: <n> thread(s) of this process moved to CPUs <list>`.
2. Set thread-count variables before Python starts, for example `OPENBLAS_NUM_THREADS=2 python my_session.py`; numpy reads them when it loads. `python -m fastmm run` sets `OPENBLAS_NUM_THREADS`, `OMP_NUM_THREADS` and `MKL_NUM_THREADS` to 1 unless they are set.
3. Start worker processes with `multiprocessing.set_start_method("forkserver")` or `"spawn"`: a child forked during a session cannot run a session or publish parameters.

## Check the journal

The journal header records the class, a hash of the hot-hook source and the package versions:

```bash
python -c "import fastmm; print(fastmm.inspect_journal('runs/sim-local-<session id>.fmj')['strategy_meta'])"
```

Replay a Python strategy's journal with its class; slow methods do not run, the parameter updates come from the journal ([Replay](../../reference/python-api.md#replay)):

```bash
PYTHONPATH=examples/python/strategies python -c "import fastmm, hot_slow_mm; print(fastmm.replay('runs/sim-local-<session id>.fmj', hot_slow_mm.HotSlowMM))"
```

`BasicMMHot` sends the orders C++ `basic_mm` sends, so the C++ strategy also replays its journal. Pass a configuration that names `basic_mm` with the same parameters; `fastmm-replay` warns that the configuration hash differs and still compares every message:

```bash
./build/release/bin/fastmm-replay --journal runs/sim-local-<session id>.fmj --config configs/sim-local.toml --strategy basic_mm --verify
```
