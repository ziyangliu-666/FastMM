# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)

FastMM is a fast market-making engine in C++20.

## Design

- One engine thread owns the books, orders, positions and risk state. Venue connections run on their own threads and pass events to it through single-producer single-consumer ring buffers.
- The engine is a template over its clock, transport and event feed. Backtests, replay and live sessions compile the same strategy code with different policies, and the strategy itself is a template parameter.
- The engine journals every input it consumes. `fastmm-replay --verify` runs a journal through the engine again and compares the SHA-256 of the orders it sends with the recorded ones.
- Prices and quantities are 64-bit integers with 8 decimal places.
- The hot path does not allocate after warm-up; `tests/hotpath/noalloc_test.cpp` fails if it does.
- Strategy hooks are checked at compile time. A hook with the wrong signature is a build error.

## Latency

| Operation | Time |
|---|---:|
| L2 book: change one of the top 4 levels | 2.0 ns |
| Pre-trade risk check, 15 checks | 8.1 ns |
| OMS submit, ack and fill | 91.7 ns |
| SPSC round trip between two cores | 129.4 ns |
| Binance depth diff JSON to event, 20 levels | 630.1 ns |
| Book delta in to order serialised, p50 | 639.0 ns |

Medians unless marked; hardware and method in [bench/README.md](bench/README.md).

## Quick start

Requires gcc 13+ or clang 16+, CMake 3.25+, Ninja, OpenSSL 3 and zlib. CPM fetches the other dependencies. [Install](docs/getting-started/install.md) covers Docker.

```bash
git clone https://github.com/ziyangliu-666/FastMM && cd FastMM
./scripts/bootstrap.sh                                   # checks toolchain, configures the release preset
cmake --build --preset release -j && ctest --preset release
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic
./scripts/run-sim.sh --duration 30s                      # sim exchange + live engine on localhost
```

## Write a strategy

A strategy is one header. More in the [tutorial](docs/tutorials/first-strategy/README.md) and the [strategy API](docs/reference/strategy-api.md).

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

## Status

Version 0.1; APIs may change between 0.x releases. Connectors: Binance Spot, Bybit v5 spot and Deribit. The shipped configs point at their testnets and Binance Demo.

## Limitations

- Linux on x86-64 only.
- One process on one host; no kernel-bypass networking.
- The connectors have run against testnets, Binance Demo and the simulated exchange, not with real funds.
- The FIX 4.4, Nasdaq ITCH/OUCH and CME MDP 3.0 codecs are not connected to any venue.
- Replay returns the recorded venue responses, so it cannot show how a venue would respond to different orders.
- Python strategies run in backtests only. The Python package is not on PyPI.

## Documentation

[docs/README.md](docs/README.md). Read the [go-live checklist](docs/how-to/operations/go-live-checklist.md) before trading with API keys.

## License

MIT. See [LICENSE](LICENSE).
