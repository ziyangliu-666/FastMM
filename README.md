# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)

FastMM is a fast market-making engine in C++20.

- Backtests, replay and live trading run the same strategy code.
- Strategies are written in C++ or Python. Python quoting hooks are compiled with Numba and called on the trading thread; model code runs as ordinary Python on another thread.
- Every session is recorded, and replaying a recording sends the same orders again.
- Orders pass pre-trade risk limits before they are sent, and reaching the loss limit cancels all orders.
- The trading thread does not allocate memory or wait on network I/O; in simulation a market-data update becomes an order in about 120 nanoseconds (p50 on one core of a desktop; [bench/README.md](bench/README.md) says what that measurement contains and what it leaves out).

## Quick start

Requires Linux, gcc 13+ or clang 16+, CMake 3.25+, Ninja, OpenSSL 3 and zlib.

```bash
git clone https://github.com/ziyangliu-666/FastMM && cd FastMM
./scripts/bootstrap.sh                                   # checks toolchain, configures the release preset
cmake --build --preset release -j && ctest --preset release -j"$(nproc)"
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic
./scripts/run-sim.sh --duration 30s                      # sim exchange + live engine on localhost
```

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

- Version 0.1; the API may change.
- Exchanges: Binance Spot, Binance USDⓈ-M perpetuals, Bybit spot, Deribit, and Nasdaq ITCH market data.
- Linux on x86-64 only.
- Not yet used with real money.

[Documentation](docs/README.md) · [Performance](bench/README.md) · [Architecture](docs/explanation/architecture.md) · [MIT license](LICENSE)
