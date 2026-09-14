# 6. Register it

`tutorial-backtest`, `tutorial-replay` and `tutorial-live` are FastMM's `fastmm-backtest`, `fastmm-replay` and `fastmm-live` with the tutorial's strategies added.

## The registration function

A strategy library exports one registration function:

<!-- snippet: examples/cpp/tutorial/strategies.cpp#register -->
```cpp
#include "strategies.hpp"

#include "first_mm.hpp"

#include "fastmm/strategies/factories.hpp"

void tutorial::register_strategies(fastmm::StrategyRegistry& r) {
  fastmm::register_strategy<FirstMM>(r);  // Sim + Replay + Live
}
```

- `fastmm/strategies/factories.hpp` compiles the strategy's three engines (simulated, replay and live) into this file.
- Nothing registers itself: each program passes the function to the command line it runs. If you register a strategy but omit `factories.hpp`, the link fails and names the missing factory.

Each program is a three-line `main`, here the live one:

<!-- snippet: examples/cpp/tutorial/tutorial_live.cpp#main -->
```cpp
#include "strategies.hpp"

#include "fastmm/cli/live.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::live(argc, argv, {tutorial::register_strategies});
}
```

The built-in strategies are always registered first, so `tutorial-live` also runs `basic_mm`.

## Build it: in FastMM's tree

`examples/cpp/tutorial/CMakeLists.txt`:

<!-- snippet: examples/cpp/tutorial/CMakeLists.txt#cmake -->
```cmake
# The strategy library: the strategy header and its registration file.
add_library(tutorial_strategies STATIC strategies.cpp)
target_include_directories(tutorial_strategies PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tutorial_strategies PUBLIC fastmm::sim)

# Unit test and C++ backtest: no registration needed.
add_executable(first_mm_test first_mm_test.cpp)
target_link_libraries(first_mm_test PRIVATE tutorial_strategies)
add_executable(first_mm_backtest first_mm_backtest.cpp)
target_link_libraries(first_mm_backtest PRIVATE tutorial_strategies fastmm::backtest)

# FastMM's command lines with the tutorial's strategies registered.
add_executable(tutorial-backtest tutorial_backtest.cpp)
target_link_libraries(tutorial-backtest PRIVATE tutorial_strategies fastmm::backtest)
add_executable(tutorial-replay tutorial_replay.cpp)
target_link_libraries(tutorial-replay PRIVATE tutorial_strategies fastmm::backtest)
if(TARGET fastmm::live)
  add_executable(tutorial-live tutorial_live.cpp)
  target_link_libraries(tutorial-live PRIVATE tutorial_strategies fastmm::live)
endif()
```

## Build it: in your own project

For your own repository, copy [`examples/external-project/`](../../../examples/external-project/). It has the same pieces (a strategy header, `examples/external-project/src/strategies.cpp`, live, backtest and replay mains and a harness test) and builds against an installed FastMM with `find_package(fastmm)` or a source tree with `add_subdirectory`; [Register a strategy](../../how-to/strategies/register-a-strategy.md) walks through it. To ship a strategy with FastMM itself, add it to the `STRATEGIES` list in `src/strategies/CMakeLists.txt`, which registers it in every FastMM program.

## Check

<!-- snippet: scripts/docs/tutorial.sh#list-registered -->
```bash
"$BIN"/tutorial-backtest --list-strategies --format json | grep -o '"name": "first_mm", "transports": [^]]*]'
```

```text
"name": "first_mm", "transports": ["sim", "replay", "live"]
```

Next: [7. Backtest and replay from the command line](07-cli-backtest-and-replay.md)
