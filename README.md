# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)

FastMM is a fast market-making engine in C++20. A strategy is one C++ header, and the same header runs in backtests and live on Binance, Bybit and Deribit.

## Quick start

Requires Linux, gcc 13+ or clang 16+, CMake 3.25+, Ninja, OpenSSL 3 and zlib.

```bash
git clone https://github.com/ziyangliu-666/FastMM && cd FastMM
./scripts/bootstrap.sh                                   # checks toolchain, configures the release preset
cmake --build --preset release -j && ctest --preset release
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

Next: [tutorial](docs/tutorials/first-strategy/README.md).

## Limitations

- Version 0.1; the API may change.
- Linux on x86-64 only.
- Not yet used with real money.

[Documentation](docs/README.md) · [Performance](bench/README.md) · [Architecture](docs/explanation/architecture.md) · [MIT license](LICENSE)
