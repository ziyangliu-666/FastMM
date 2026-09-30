# FastMM

FastMM is a low-latency market-making engine in C++20: it quotes on one venue and hedges every fill on another, with strategies in C++ or Python. Backtests, replays and live sessions run the same strategy code.

## Quickstart

```bash
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

The wheel needs Linux x86-64 and CPython 3.10 or later, and no keys. To build the C++ programs, see [Install](getting-started/install.md).

## Where to go next

- [Quickstart](getting-started/quickstart.md): a strategy, a backtest and a live session against the local exchange.
- [Your first market maker](tutorials/first-strategy/README.md): a C++ strategy from its header to Binance Demo, in nine steps.
- [Quote on one venue, hedge on another](how-to/strategies/xmm.md): run `xmm` on Binance Demo.
- [Run on a testnet or Binance Demo](how-to/operations/run-on-testnet.md): keys, a dry run and a first keyed session.
- [How FastMM works](explanation/how-it-works.md): the engine thread, the path of one event and what it guarantees.
