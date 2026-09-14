# FastMM

[![CI](https://github.com/ziyangliu-666/FastMM/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)

FastMM is a market-making engine in C++20. One engine thread owns all trading state, every input is
journaled, and the same strategy code runs in backtests, against a simulated exchange, in replay and
on venues. Connectors: Binance Spot (testnet and Demo Mode), Bybit v5 spot and Deribit. The FIX 4.4,
Nasdaq ITCH/OUCH and CME MDP 3.0 codecs are not connected to a venue. Version 0.1: the configs
default to testnets and Binance Demo, APIs may change between 0.x releases, and live trading is at
your own risk.

## Latency

An 8-core desktop under WSL2, gcc 13.3, `-O3 -march=native` with LTO, one pinned core, median of 5
runs unless marked p50; network and venue latency are excluded ([method](bench/README.md)).
`tests/hotpath/noalloc_test.cpp` fails on any heap allocation in the hot paths after warm-up.

| Hot-path operation | Time |
|---|---:|
| L2 book: change the quantity of one of the top 4 levels | 2.0 ns |
| Pre-trade risk check (15 checks) | 8.1 ns |
| OMS submit → ack → fill lifecycle | 91.7 ns |
| SPSC ping-pong between two cores (round trip, two hops) | 129.4 ns |
| Binance depth diff JSON → normalised event (20 levels) | 630.1 ns |
| Tick-to-order in simulation, p50 of ticks that sent orders (book delta in → order serialised) | 639.0 ns |

## Quick start

Requires gcc 13+ or clang 16+, CMake 3.25+, Ninja, OpenSSL 3 and zlib headers; CPM fetches the rest.
Without a toolchain, `docker compose up --build` runs the simulated exchange and the engine ([Install](docs/getting-started/install.md)).

```bash
git clone https://github.com/ziyangliu-666/FastMM && cd FastMM
./scripts/bootstrap.sh                                   # checks toolchain, configures the release preset
cmake --build --preset release -j && ctest --preset release
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic
./scripts/run-sim.sh --duration 30s                      # sim exchange + live engine on localhost
```

## Write a strategy

A strategy is one header; this one is built and run in CI. Next: [Tutorial](docs/tutorials/first-strategy/README.md), [Strategy API](docs/reference/strategy-api.md).

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

## Limitations

- Linux on x86-64 only (epoll or io_uring, `rdtsc`); one process on one host; no kernel-bypass transport.
- The connectors have run against testnets, Binance Demo and the simulated exchange only.
- Replay feeds recorded venue responses back; it cannot show how a venue would answer other orders.
- Python strategies run in backtests only, and the Python package is not on PyPI.
- `git clone` and the quick start's `FetchContent` need read access to the GitHub repository.

## Documentation

[`docs/README.md`](docs/README.md). Before a keyed session: [Go-live checklist](docs/how-to/operations/go-live-checklist.md).

## License

MIT. See [`LICENSE`](LICENSE).
