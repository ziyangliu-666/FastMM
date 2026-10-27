# Register a strategy

A strategy runs in backtests, replay and live trading once a registration function adds it to the strategy registry. Your apps call that function; nothing registers itself.

## Out of tree: your own project

Copy [`examples/external-project/`](../../../examples/external-project/).

| File | Role |
|---|---|
| `include/mm/microprice_mm.hpp` | the strategy |
| `include/mm/strategies.hpp` | declares `mm::register_strategies` |
| `src/strategies.cpp` | the only registration file |
| `apps/live.cpp`, `apps/backtest.cpp`, `apps/replay.cpp` | the FastMM command lines with your strategies |
| `tests/microprice_mm_test.cpp` | a hook test with `fastmm::sim::StrategyHarness` |
| `CMakeLists.txt` | `find_package(fastmm)` or `add_subdirectory` |

`src/strategies.cpp` exports `void register_strategies(fastmm::StrategyRegistry& r)`, which calls `fastmm::register_strategy<S>(r)` once per strategy. It includes `fastmm/strategies/factories.hpp`, so it compiles each strategy's engine for the simulator, replay and live trading:

```cpp
void mm::register_strategies(fastmm::StrategyRegistry& r) {
  fastmm::register_strategy<MicropriceMM>(r);  // Sim + Replay + Live
}
```

Each app is one line, here the live one:

```cpp
int main(int argc, char** argv) { return fastmm::cli::live(argc, argv, {mm::register_strategies}); }
```

The other entry points are `fastmm::cli::backtest` and `fastmm::cli::replay`. The apps accept the flags of `fastmm-live`, `fastmm-backtest` and `fastmm-replay`, and their messages start with your program's name. `fastmm::cli::live` installs process-wide SIGINT and SIGTERM handlers.

A list built at runtime can be passed as a `std::span<const fastmm::StrategyModule>`.

### Build against an installed FastMM

```bash
cmake --preset release -B build/sdk -DFASTMM_BUILD_TESTS=OFF -DFASTMM_BUILD_BENCH=OFF \
    -DFASTMM_BUILD_EXAMPLES=OFF
cmake --build build/sdk && cmake --install build/sdk --prefix build/install
cmake -S examples/external-project -B build/mm -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/mm && ctest --test-dir build/mm
```

| Target | Links |
|---|---|
| strategy library | `fastmm::sim` (the Live factory needs only core types), and `fastmm::lowlatency` privately so the engines get FastMM's code generation flags |
| live app | `fastmm::live` |
| backtest and replay apps | `fastmm::backtest` |

Build your project with the compiler that built FastMM, because a Release install contains GCC LTO objects. The install needs only the libraries; leaving out FastMM's tests, benchmarks and examples builds a third of the targets.

An install built with `FASTMM_BUILD_NET=OFF` has no networking, and `find_package(fastmm CONFIG REQUIRED COMPONENTS live)` fails:

```text
component 'live' (fastmm::live) is not available: this FastMM was built with FASTMM_BUILD_NET=OFF
```

A backtest-only project links `fastmm::backtest` and asks for no component.

### Build together with a FastMM source tree

Pass `-DFASTMM_SOURCE_DIR=<FastMM checkout>` to the template. It calls `add_subdirectory` instead of `find_package`, and the same `fastmm::` targets exist. `FetchContent_Declare(fastmm GIT_REPOSITORY <FastMM repository> GIT_TAG <tag>)` followed by `FetchContent_MakeAvailable(fastmm)` works the same way.

As a subproject, FastMM builds no tests, benchmarks, apps or examples and installs nothing. It downloads its dependencies through CPM; set `CPM_SOURCE_CACHE` to reuse them.

### Run it

```bash
./build/mm/mm-backtest --list-strategies --format json
./build/mm/mm-backtest --config configs/backtest-example.toml --data synthetic \
    --strategy microprice_mm --param edge_ticks=1
./build/release/bin/fastmm-sim-exchange --config configs/sim.toml &
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret ./build/mm/mm-live \
    --config configs/sim-local.toml --strategy microprice_mm --duration 60s \
    --journal runs/mm.fmj
./build/mm/mm-replay --journal runs/mm.fmj --verify
```

`--strategy` with a strategy other than the config's ignores `[strategy.params]` and says so. Give the new strategy's values with `--param key=value`; an unknown parameter exits with code 3 before any venue is contacted.

The journal embeds the configuration after these overrides, so `mm-replay` needs no `--config`. Replay a journal with an app that registers the same strategies. [`scripts/ci-external-project.sh`](../../../scripts/ci-external-project.sh) runs these steps in CI.

### Compile the engines in parallel

`src/strategies.cpp` compiles three engines per strategy in one file. To compile them in parallel, put each engine in its own file with `FASTMM_INSTANTIATE_STRATEGY(S, kind)`, where `kind` is `sim`, `replay` or `live` and the file includes `fastmm/strategies/factory_<kind>.hpp`:

```cpp
#include "fastmm/strategies/factory_live.hpp"
#include "mm/microprice_mm.hpp"
FASTMM_INSTANTIATE_STRATEGY(mm::MicropriceMM, live);
```

The registration file then includes only `fastmm/strategies/module.hpp`.

## In tree: a strategy shipped with FastMM

1. Put the header in `include/fastmm/strategies/`, in `namespace fastmm`.
2. Add the type and header to the `STRATEGIES` list in [`src/strategies/CMakeLists.txt`](../../../src/strategies/CMakeLists.txt), for example `fastmm::MyQuoter fastmm/strategies/my_quoter.hpp`. The build generates one file per runtime and adds the strategy to `fastmm::register_builtin_strategies`.
3. Every app, `bt::run_backtest(cfg, "my_quoter")`, sweeps, replays and `fastmm.strategies()` in Python then find it by name.

The built-in strategies are `basic_mm`, `avellaneda_stoikov`, `options_mm`, `lead_mm` and `xmm`. Every app registers them before your own.

## Check a registration

`--list-strategies --format json` lists `"transports": ["sim", "replay", "live"]` for the strategy.

| Symptom | Cause |
|---|---|
| link error such as `undefined reference to fastmm::live_factory<mm::MicropriceMM>(...)` | the strategy is registered, but no file that includes `fastmm/strategies/factories.hpp` (or the matching `factory_<kind>.hpp`) compiles its engine |
| `fastmm::StrategyConflict`, exit code 3 | a name that different code already registered; registering the same strategy twice does nothing |
| the strategy is missing from an app | the app does not call the registration function; there are no static initialisers or whole-archive flags ([ADR-0012](../../adr/0012-strategy-developer-experience.md)) |
| a strategy from another library is missing | only its owner compiles its engines; call that library's registration function |
| a C++ strategy is missing in Python | the prebuilt wheel registers the built-in strategies only ([ADR-0012](../../adr/0012-strategy-developer-experience.md), section 7) |

Tests that register strategies use a local `fastmm::StrategyRegistry` instead of `StrategyRegistry::instance()`.
