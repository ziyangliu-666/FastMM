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

## Keep other threads off the engine's core

1. Pin the engine thread with `[engine] cpu` (and `net_cpus` for the venue threads). When the session starts, every thread of the process moves to the other CPUs, and the log says `thread affinity: <n> thread(s) of this process moved to CPUs <list>`.
2. Set thread-count variables before Python starts, for example `OPENBLAS_NUM_THREADS=2 python my_session.py`; numpy reads them when it loads. `python -m fastmm run` sets `OPENBLAS_NUM_THREADS`, `OMP_NUM_THREADS` and `MKL_NUM_THREADS` to 1 unless they are set.
3. Start worker processes with `multiprocessing.set_start_method("forkserver")` or `"spawn"`: a child forked during a session cannot run a session or publish parameters.

## Check the journal

The journal header records the class, a hash of the hot-hook source and the package versions:

```bash
python -c "import fastmm; print(fastmm.inspect_journal('runs/sim-local-<session id>.fmj')['strategy_meta'])"
```

`BasicMMHot` sends the orders C++ `basic_mm` sends, so the C++ strategy replays its journal. Pass a configuration that names `basic_mm` with the same parameters; `fastmm-replay` warns that the configuration hash differs and still compares every message:

```bash
./build/release/bin/fastmm-replay --journal runs/sim-local-<session id>.fmj --config configs/sim-local.toml --strategy basic_mm --verify
```
