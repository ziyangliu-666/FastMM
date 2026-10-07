# FastMM

- **[Strategies in C++ or Python](how-to/strategies/python-hot-hooks.md).** Python hooks marked `@fastmm.hot` compile to machine code with Numba.
- **[One strategy file for backtest and live](explanation/determinism.md).** Replays are deterministic, and the simulated exchange models queue position.
- **[Account pools](reference/venues.md#account-pools).** Several sub-accounts trade behind one venue, and orders spread across them.
- **[Gateway](how-to/operations/run-behind-a-gateway.md).** One `fastmm-gateway` holds the exchange connections; several strategy processes attach to it and can stop or restart without dropping them.
- **[Live control](how-to/operations/operate-a-running-session.md).** `fastmm-ctl` changes a running session's parameters without a restart.
- **[Low-latency tuning](how-to/operations/running-in-production.md#host-tuning).** Host and network tuning built in, such as core pinning and kernel bypass.

<video class="fastmm-video" controls preload="none" poster="assets/intro-poster.jpg" src="https://github.com/ziyangliu-666/FastMM/releases/download/v0.5.1/fastmm-intro.mp4"></video>

## Quickstart

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

The wheel needs Linux x86-64 and CPython 3.10 or later, and no keys. On Debian and Ubuntu, `sudo apt install python3-venv` first. To build the C++ programs, see [Install](getting-started/install.md).

## Where to go next

- [Quickstart](getting-started/quickstart.md): a strategy, a backtest and a live session against the local exchange.
- [Your first market maker](tutorials/first-strategy/README.md): a C++ strategy from its header to Binance Demo, in nine steps.
- [Quote on one venue, hedge on another](how-to/strategies/xmm.md): run `xmm` on Binance Demo.
- [Run on a testnet or Binance Demo](how-to/operations/run-on-testnet.md): keys, a dry run and a first keyed session.
- [Account pools](reference/venues.md#account-pools): several accounts of one exchange behind one venue.
- [Run in production](how-to/operations/running-in-production.md): what to set before a keyed session runs unattended.
- [Low-latency TCP](how-to/operations/low-latency-tcp.md): busy polling and kernel bypass for order entry.
- [How FastMM works](explanation/how-it-works.md): the engine thread, the path of one event and what it guarantees.
- [How fast it is](explanation/how-fast.md): one production session's latency from market data to order, and its load.
- [Benchmarks](explanation/benchmarks.md): the engine alone, on one machine.
