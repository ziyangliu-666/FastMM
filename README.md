# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)
[![docs](https://github.com/ziyangliu-666/FastMM/actions/workflows/docs.yml/badge.svg)](https://ziy.bio/FastMM/)

Documentation: <https://ziy.bio/FastMM/>

FastMM is a market-making engine in C++20.

- Backtests, replay and live trading run the same strategy code.
- Strategies are written in C++ or Python. Python quoting hooks are compiled with Numba and called on the trading thread; model code runs as ordinary Python on another thread.
- Every session is recorded, and replaying a recording sends the same orders again.
- Orders pass pre-trade risk limits before they are sent, and reaching the loss limit cancels all orders.
- The trading thread does not allocate memory or wait on network I/O. In simulation a market-data update becomes an order in about 120 ns (p50, one core of a desktop; [bench/README.md](bench/README.md) says what the measurement includes).

## Quick start

```bash
pip install "fastmm-engine[hot]"                         # Linux x86-64, CPython 3.10+
fastmm init my-mm && cd my-mm && python backtest.py      # a strategy, a config and a backtest
```

The C++ programs ship as a release tarball and as `ghcr.io/ziyangliu-666/fastmm` ([Deploy a release](docs/how-to/operations/deploy.md)). Building them needs Linux, gcc 13+ or clang 16+, CMake 3.25+, Ninja and OpenSSL 3:

```bash
git clone https://github.com/ziyangliu-666/FastMM && cd FastMM
./scripts/bootstrap.sh                                   # checks toolchain, configures the release preset
cmake --build --preset release -j && ctest --preset release -j"$(nproc)"
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic --out runs/first
python3 tools/report.py runs/first                       # -> runs/first/report.html, open it
./scripts/run-sim.sh --duration 30s                      # sim exchange + live engine, ends with a report
```

On real data, a day of Binance BTCUSDT perpetual at the venue's own fees:

```bash
python3 python/fastmm/data/__main__.py fetch --symbol BTCUSDT --date 2024-03-27
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data binance:BTCUSDT,2024-03-27
```

What that result means, and what it cannot: [Backtest on real BTCUSDT data](docs/how-to/backtesting/binance-public-data.md).

## Write a strategy

<!-- snippet: examples/quickstart/my_mm.hpp#strategy -->
```cpp
#include "fastmm/strategy.hpp"

using namespace fastmm;

struct MyParams {
  FASTMM_PARAMS(MyParams)
  FASTMM_PARAM_BPS(half_spread_bps, 0.005_bps, 0_bps, 1000_bps, "half spread around the mid, bps")
  FASTMM_PARAM(Qty, quote_qty, 0.001_qty, 0_qty, 1000_qty, "quantity per side, base units")
};

struct MyMM : StrategyBase<MyParams> {
  static constexpr std::string_view name() noexcept { return "my_mm"; }

  void on_book(auto& ctx, InstrumentId id, const auto& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);  // empty or crossed
    const Instrument& inst = ctx.instrument(id);
    const Price half = book.mid() * params().half_spread_bps;
    DesiredQuotes q;
    q.bid(inst.round_price(book.mid() - half, Side::Buy), inst.round_qty(params().quote_qty));
    q.ask(inst.round_price(book.mid() + half, Side::Sell), inst.round_qty(params().quote_qty));
    ctx.set_quotes(id, q);  // diffed against resting orders
  }
};
```

Next: [tutorial](docs/tutorials/first-strategy/README.md), or [a strategy in Python](docs/how-to/strategies/python-live.md).

## Limitations

- Exchanges: Binance Spot, Binance USDⓈ-M perpetuals, Bybit spot and linear perpetuals, OKX USDT-margined swaps, Deribit, and Nasdaq ITCH market data.
- Linux on x86-64 only.
- The shipped strategies are reference implementations, not an edge: the example backtest is profitable only because it is configured with a maker rebate ([Economics](docs/explanation/economics.md)).
- One account per venue. Several strategies share it through `fastmm-gateway`, each on its own instruments ([Run behind a gateway](docs/how-to/operations/run-behind-a-gateway.md)).
- Monitoring is a status file to pull from (`fastmm-top`, or its Prometheus endpoint), with no alerting. A venue-side dead man's switch exists only on Deribit, Binance USDⓈ-M, OKX and Bybit, where the account has it ([Running this in production](docs/how-to/operations/running-in-production.md)).

[Documentation](https://ziy.bio/FastMM/) · [Performance](bench/README.md) · [Architecture](docs/explanation/architecture.md) · [MIT license](LICENSE)
