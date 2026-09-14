# Quick start

Requires a FastMM build ([Install](install.md)). The code is in `examples/quickstart/`; CI builds
and runs it (ctest `examples.quickstart`).

## 1. The strategy

`MyMM` quotes one bid and one ask `half_spread_bps` away from the mid on every book update:

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

- `FASTMM_PARAM_BPS` and `FASTMM_PARAM` declare typed parameters with a default, a range and a
  description. `Ratio` (basis points) and `Qty` are 64-bit fixed-point values.
- `on_book` is a hook: the engine calls it after each book update. Other hooks cover trades,
  fills, timers and connection changes ([Strategy API](../reference/strategy-api.md)).
- `set_quotes` sets the desired quotes; the engine sends new, cancel and replace messages for the
  difference.

## 2. The backtest

<!-- snippet: examples/quickstart/main.cpp#main -->
```cpp
#include "my_mm.hpp"

#include "fastmm/backtest/backtest_runner.hpp"

#include <cstdio>

int main(int argc, char** argv) {
  auto cfg = bt::BacktestConfig::single_instrument("BTCUSDT", 0.01_px, 0.00001_qty);
  cfg.generator.market_qty_median_lots = 1500;  // synthetic market: larger taker orders
  auto source = bt::open_data(argc > 1 ? argv[1] : "synthetic");  // .fmj, .csv or synthetic
  std::fputs(bt::run_backtest<MyMM>(cfg, source.get()).summary_table().c_str(), stdout);
}
```

`run_backtest<MyMM>` builds the engine with your strategy, a simulated venue with a matching
engine and latency, and runs it for 60 s of simulated time.

## 3. Build and run

In the FastMM build tree:

```bash
cmake --build --preset release --target my_mm_backtest
./build/release/examples/quickstart/my_mm_backtest
```

In your own project, copy `examples/quickstart/`. Its `CMakeLists.txt` fetches FastMM:

<!-- snippet: examples/quickstart/CMakeLists.txt#cmake -->
```cmake
cmake_minimum_required(VERSION 3.25)
project(my_mm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

if(NOT TARGET fastmm::backtest)
  include(FetchContent)
  set(FASTMM_BUILD_NET OFF CACHE BOOL "backtests need no networking")
  FetchContent_Declare(fastmm GIT_REPOSITORY https://github.com/ziyangliu-666/FastMM GIT_TAG main)
  FetchContent_MakeAvailable(fastmm)
endif()

add_executable(my_mm_backtest main.cpp)
target_link_libraries(my_mm_backtest PRIVATE fastmm::backtest)
```

```bash
cmake -S examples/quickstart -B build/quickstart -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/quickstart && ./build/quickstart/my_mm_backtest
```

Add `-DFETCHCONTENT_SOURCE_DIR_FASTMM=$PWD` to the first command to build against this checkout
instead of downloading FastMM.

## 4. Read the result

Output (shortened):

```text
backtest my_mm  seed=1  md_events=7129  steps=7694  wall=0.01s
  net pnl                        0.0012
  realized / unrealized / fees   0.0011 / 0.0001 / 0.0000
  fills (maker / taker)          37 (37 / 0)
  orders / cancels / replaces    283 / 245 / 0
  inventory mean / |mean| / max  ...
  outbound messages / sha256     ... / ...
```

- Money is in the quote currency (USDT), quantities in the base currency (BTC).
- `fills (maker / taker)`: quotes are post-only, so taker is 0.
- `outbound messages / sha256`: a hash of the order messages sent; it is identical on every run
  ([Determinism](../explanation/determinism.md)).

## Next

- [Tutorial: your first market maker](../tutorials/first-strategy/README.md) adds unit tests,
  inventory limits, registration, the command-line tools, the simulated exchange and Binance Demo.
- [Strategy API](../reference/strategy-api.md) lists the hooks, context methods and helpers.
