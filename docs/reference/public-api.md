# Public API

Which headers and CMake targets a project outside FastMM may use, and what may change. The list of headers is [`docs/api/public-headers.txt`](../api/public-headers.txt).

## Tiers

| Tier | Contents | Promise |
|---|---|---|
| 1 | Writing, testing, registering and running strategies: `fastmm/strategy.hpp`, `strategies/{strategy,hooks,params,param_publisher,quoting,registry,module,factories,factory_*}.hpp`, `testing/strategy_harness.hpp`, the core types a strategy sees (`core/{strategy_context,fixed_point,instrument,enums,strong_id,time,result,rng,messages,order,oms,position,quote_manager,engine_config,log}.hpp`, `core/book/{book_view,l2_book}.hpp`, `core/options/black76.hpp`), backtests and replay (`backtest/*`, `sim/param_schedule.hpp`, `cli/backtest.hpp`, `cli/replay.hpp`) and live trading (`cli/live.hpp`, `live/session.hpp`, `live/live_backend.hpp`) | Stable within a minor version from 1.0. Before 1.0, breaking changes are listed under **Breaking** in the [CHANGELOG](../../CHANGELOG.md) |
| 2 | Extending FastMM: configuration (`config/*`), venue connectors (`venues/*.hpp` outside the venue subdirectories), `core/book/book_syncer.hpp`, storage backends (`core/record_stream.hpp`, `store/*.hpp`), signal research (`research/*.hpp`), networking (`net/{reactor,connection,crypto,backoff,url,ws_client,http_client,udp_socket,datagram_source,kernel_datagram_source}.hpp`) and `codecs/codec.hpp` | Documented; may change in any release before 1.0 |
| internal | Everything else: engine internals (`core/engine.hpp`, rings, containers, the journal writer, timers, latency, the status segment), the simulator (`sim/*`), the reference connectors (`venues/binance/`, `venues/bybit/`, `venues/deribit/`), protocol-specific codec headers and the built-in strategies | No promise; may change in any release |

- Use the built-in strategies (`strategies/basic_mm.hpp`, `avellaneda_stoikov.hpp`, `options_mm.hpp`, `lead_mm.hpp`) by name through the registry; their parameters and behaviour may change.
- A tier 1 header may include internal headers; only the names the reference pages document are public. `Engine<...>` is internal even though `StrategyHarness::engine()` returns it; use it for inspection in tests.
- Everything is in `namespace fastmm` (backtests in `fastmm::bt`, the harness in `fastmm::sim`, command lines in `fastmm::cli`); `detail` namespaces are internal.

## CMake targets

Installed FastMM exports these targets through `find_package(fastmm)`; `add_subdirectory` and `FetchContent` create the same names.

| Target | Contents | Needs |
|---|---|---|
| `fastmm::core` | engine, books, OMS, risk, journal, configuration | fmt |
| `fastmm::sim` | matching engine, simulated transport, the strategy harness | `fastmm::core` |
| `fastmm::strategies` | the built-in strategies and their registration | `fastmm::sim` |
| `fastmm::backtest` | backtests, sweeps, replay, `cli::backtest`, `cli::replay` | `fastmm::strategies` |
| `fastmm::research` | the feature and forward-markout extractor, and signal evaluation ([Judging a signal](../explanation/signal-research.md)) | `fastmm::backtest` |
| `fastmm::store` | storage backends and the SQLite one ([Storage](storage.md)) | `fastmm::core` |
| `fastmm::codecs` | FIX, ITCH, OUCH, SoupBinTCP, MoldUDP64, MDP 3.0 | `fastmm::core` |
| `fastmm::net` | reactor, TLS, WebSocket, HTTP, UDP multicast receive | OpenSSL (`FASTMM_BUILD_NET=ON`) |
| `fastmm::venues` | venue connectors | `fastmm::net`, simdjson |
| `fastmm::live` | `run_live`, `cli::live` | `fastmm::venues`, `fastmm::strategies` |
| `fastmm::lowlatency` | FastMM's code generation flags (link privately where you instantiate engines) | |

`find_package(fastmm CONFIG REQUIRED COMPONENTS live)` fails with a message when the install was built without networking. Build your project with the same compiler as FastMM: a release install contains GCC LTO objects. Not provided: installed programs and configurations, other compilers, a version compatibility policy.

## Checks

- Every header in the manifest compiles on its own in its own translation unit (ctest `docs.public_headers`, label `docs`, not built under sanitizers).
- [`tests/docs/strategy_api_doc_test.cpp`](../../tests/docs/strategy_api_doc_test.cpp) pins the documented strategy API ([Strategy API](strategy-api.md)).
- `examples/external-project/` builds against an install in CI (`scripts/ci-external-project.sh`).

When you add a header that projects should use, add it to the manifest with its tier; when you remove or rename a tier 1 name, record it under **Breaking** in the CHANGELOG.
