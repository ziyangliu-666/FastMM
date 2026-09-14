# ADR-0012: Strategy developer experience

Status: accepted (2026-09)

FastMM is pre-1.0 with no external users, so API changes are clean breaks with no deprecated shims.

## Context

- Hooks fail silently. The engine detects hooks with `requires`, so a wrong signature is never
  called and never reported. `docs/adding-a-strategy.md` lists wrong signatures (`on_trade`,
  `on_book_ticker`) and leaves out `on_connection` and eight context methods.
- Instrument hooks are inconsistent. `on_book` gets `(ctx, id, book)`; `on_trade`,
  `on_book_ticker` and `on_option_ticker` get `(ctx, msg)` and may receive instruments that are not
  in the table, so strategies repeat `ctx.instruments().contains(id)`.
- Nothing tells a strategy that quoting is possible again. `set_quotes` is ignored while quoting
  is disabled (control pull, kill, reconcile), and strategies still record the mid as quoted.
  ResumeQuotes, ResetKill and Reconcile End only flip flags.
- Fills are awkward. `on_fill(ctx, OmsUpdate, OrderFillMsg)`; every strategy repeats
  `u.known ? u.order.instrument : InstrumentId{}`.
- Fixed-point math is verbose. Strategies convert doubles in `on_start`, keep centi-bps
  members and write `Int128` casts by hand. The example calls `Qty::from_double` per event.
- Registration is spread over four places (`apps/fastmm-live/runner_<s>.cpp`,
  `live_runners.hpp`, `live_runners.cpp`, `src/backtest/registrations.cpp`). `FASTMM_REGISTER_STRATEGY`
  relies on static initialisers, which static archives drop.
- External projects cannot trade live without forking. The install exports core, sim, backtest,
  codecs, net and venues, but not the live session code or a strategies library.
- There is no test harness for strategies, and the no-registration path
  `bt::run_backtest<S>(cfg, source)` is not documented.
- Python can run backtests of C++ strategies but cannot define strategies.
- Docs drift. `adding-a-venue.md` describes a registration scheme and feed signatures that do
  not exist; `schema.hpp` lists a `binance_futures` venue kind that has no connector.

Two engine defects surfaced during review and are fixed before any API work:

- Reconcile holes. Reconcile Begin pulls quotes; a requote that meets orders still waiting for
  their cancel ack is skipped without remembering the target, and `reconcile_open_order` clears an
  in-flight PendingCancel. Orders a strategy sends on channel `Live` before the open-orders snapshot
  are marked Canceled by `reconcile_end` and their ack is then ignored, leaving an order at the venue
  that the OMS no longer tracks. `reconcile_end` updates reach the strategy but not the QuoteManager.
- Late fills are not booked. For a recently terminal order the OMS reports `known` without an
  order snapshot, so the engine books the fill on an invalid instrument and it never reaches
  position, fees or PnL.

Smaller defects: `StrategyContext::pull_all_quotes` skips `mark_decision()`; direct orders may use a
`user_tag` in the QuoteManager tag range; `clamp_to_touch` exists twice with different behaviour, and
the level-0 uncross fix is copied into all three strategies; the journal `config_hash` hashes the
config file, not the effective configuration.

Constraints that do not change: no virtual calls and no allocation on the hot path (ADR-0009);
one engine template for live, sim and replay; replay hashes stay the determinism proof
(ADR-0010); static libraries (ADR-0006).

## Decision

### 1. Hooks: plain member functions, checked at compile time

A new `include/fastmm/strategies/hooks.hpp`
holds one X-macro table (name, expected call, message) that drives dispatch, the checks and the
reference table in the docs.

| Hook | Signature |
|---|---|
| `on_start`, `on_stop` | `(auto& ctx)` |
| `on_book` | `(auto& ctx, InstrumentId id, const auto& book)` |
| `on_book_ticker` | `(auto& ctx, InstrumentId id, const BookTickerMsg& m)` |
| `on_trade` | `(auto& ctx, InstrumentId id, const TradeMsg& m)` |
| `on_option_ticker` | `(auto& ctx, InstrumentId id, const OptionTickerMsg& m)` |
| `on_fill` | `(auto& ctx, const Fill& fill)` |
| `on_order_update` | `(auto& ctx, const OmsUpdate& u)` |
| `on_timer` | `(auto& ctx, TimerId id, std::uint64_t tag)` |
| `on_connection` | `(auto& ctx, const ConnectionStateMsg& m)` |
| `on_quoting` | `(auto& ctx, bool enabled)` (new) |

Rules:

- All hooks return `void`; `template <class Ctx>` spelling is equivalent to `auto&`. `noexcept` is
  recommended, not enforced (hooks already run inside `noexcept` engine code).
- Instrument-scoped hooks fire only for instruments in the table, so `ctx.book(id)` is valid.
- `on_fill` fires after position and fees are updated and before the `on_order_update` of the same
  execution, for every fill on an instrument in the table, including late fills and fills for
  unknown orders.
- `on_connection` fires for every state change. For non-`Live` states the engine first pulls that
  venue's quotes and, on the market-data channel, clears its books.
- `on_quoting(enabled)` reports changes of `ctx.quoting_enabled()`. The engine compares the value
  before and after each dispatched event, fired timer and `start()`, and calls the hook after the
  triggering hook has returned and all flags are final (for reconcile, after `reconciling_` is
  cleared and the quotes paused at Begin are resumed, so the strategy may replace them). It never
  fires from inside a context call, so a kill switch tripped by `set_quotes` or
  `send` cannot re-enter the strategy. It does not fire at start: `on_start` reads
  `ctx.quoting_enabled()` (dry-run starts disabled). Connection pulls do not change
  `quoting_enabled()`; `on_connection` covers them.
- `on_stop` does not pull quotes; shutdown's cancel-all does.

`Fill` is a view built on the stack by the engine. Its pointers are valid only during the call; do
not store them.

```cpp
struct Fill {
  InstrumentId instrument;   // the order's instrument, else the fill message's
  Side side; Price price; Qty qty;   // this execution
  Qty position_delta;        // signed change of the position (differs from qty when commission is
                             // charged in the base asset)
  Notional fee;              // quote currency as booked
  bool fee_converted = true; // false: commission in a third asset, not booked (counted in stats)
  Liquidity liquidity;
  bool known = false;        // an order snapshot is available (`update->order` is populated)
  bool late = false;         // the order was already terminal when the fill arrived
  bool order_done = false;   // the order reached a terminal state with this fill
  const OmsUpdate* update = nullptr;   // non-null when known
  const OrderFillMsg* msg = nullptr;   // always set
};
```

Checking (verified on gcc 13.3 and clang 18.1):

- A name probe detects any member with a hook's name (function, template, data member, static,
  inherited, private). If the name exists and the engine's call does not compile, `static_assert`
  fails with `fastmm: on_trade has the wrong signature or is not public; expected void
  on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)`. Each hook has its own assert, so one
  build reports all of them.
- A near-miss list (`on_fills`, `on_trades`, `on_order_book`, `on_tick`, `on_bbo`, `on_order`,
  `on_execution`, `on_disconnect`, `onBook`, ...) produces a warning through a `[[deprecated]]`
  marker ("on_fills is not a hook; did you mean on_fill?"). It is a warning because a private helper
  can legitimately use such a name; `FASTMM_WERROR` builds turn it into an error, and a strategy can
  silence it with `static constexpr bool fastmm_allow_near_miss_names = true;`.
- The check runs in the `Engine` constructor. Strategy headers may add
  `static_assert(fastmm::verify_strategy<MyMM>());`, which uses stand-in context and book types.
- `Engine::start()` logs the implemented set once: `strategy basic_mm hooks: start book fill timer
  connection quoting`.
- Documented limits: a `final` class loses the name probe (the call check remains); a hook that is
  ambiguous across two bases is reported as a wrong signature; an `auto`-returning hook must not be
  checked with `verify_strategy` (the stand-in types instantiate its body); private hooks are
  rejected, also with `friend Engine` (see [Deviations](#deviations-from-the-decision)); implicit conversions are
  accepted (`on_timer(auto&, TimerId, int)` compiles).

Rejected: a CRTP base with no-op defaults (typos compile and call the default; the engine can no
longer skip empty hooks), one overloaded `on(ctx, Event)` (overloads cannot be enumerated, a generic
overload swallows everything), a single `on_event` visitor (users write the dispatch), an explicit
`HookSet` declaration (repeats what the code already says).

### 2. Context API

| Group | API |
|---|---|
| Time, reference data | `now()`, `instrument(id)`, `instruments()`, `contains(id)` |
| Market data | `book(id)` |
| Portfolio | `position(id)`, `portfolio()` (realized, unrealized, fees, net) |
| Quoting | `set_quotes(id, q) -> bool` (false: suppressed while quoting is disabled), `pull_quotes(id)`, `pull_all_quotes()`, `working_quote(id, side, level) -> const Order*` |
| Direct orders | `send(req) -> Result<ClientOrderId, RejectReason>`, `cancel(id)`, `replace(id, px, qty)`, `order(id) -> const Order*`, `open_qty(id, side)`, `oms()` |
| Timers | `every(period, tag) -> TimerId`, `once(delay, tag) -> TimerId`, `cancel_timer(id)` |
| Control | `quoting_enabled()`, `killed()`, `request_stop()` |
| Randomness | `rng()` (seeded, replay-deterministic) |

- `NewOrderRequest::limit(id, side, px, qty)` with `.post_only()`, `.reduce_only()`, `.ioc()`.
- `ctx.send` rejects a `user_tag` inside the QuoteManager range with `RejectReason::InvalidTag`. The
  check lives in the context path, not in `submit_new`, which quotes also use. `InvalidTag` is
  appended to the OMS block of `RejectReason`; existing values keep their numbers (they are
  journaled).
- Every order-API method, including `pull_all_quotes`, calls `mark_decision()`.
- Removed: `add_timer(period, bool, tag)` and `instrument_count()`.
- Logging stays the `FASTMM_LOG_*` macros. Logs are not journaled; strategy logic must not depend
  on them.

### 3. Fixed-point helpers

In `core/fixed_point.hpp` and a new `strategies/quoting.hpp`, `constexpr` and integer-only on the hot
path:

- `Ratio = Fixed<RatioTag>` (1.0 = 1e8 raw, 1 bp = 10'000 raw). `Fixed<T> * Ratio` multiplies through
  `Int128` with one truncation toward zero. `ratio(num, den)`, `Ratio::from_bps(double)` (startup
  only), `to_bps()` (diagnostics).
- Exact literals in `fastmm::literals`: `100.25_px`, `0.01_qty` (up to 8 decimals), `5_bps`,
  `0.25_bps` (up to 4 decimals). Too many decimals is a compile error.
- `mid(bid, ask)`, `microprice(Level bid, Level ask)`, `spread_ratio(bid, ask)`,
  `away_from(ref, side, dist)`, `inventory_allows(side, position, qty, limit)`, `Instrument::ticks(n)`.
- `DesiredQuotes::bid(px, qty)` / `ask(px, qty)` (drop non-positive values, return `bool`),
  `DesiredQuotes::uncross(tick)`, `keep_passive(q, best_bid, best_ask, tick)` (BasicMM's whole-ladder
  shift). OptionsMM keeps its own level-0 clamp, which is a different rule, and uses `uncross`.

A new umbrella header `include/fastmm/strategy.hpp` includes what a strategy needs: fixed point and
literals, instrument, quotes and helpers, params, hooks, log macros.

The BasicMM formula port is bit-identical: `mid.raw * (cbps * 100) / 1e8` truncates exactly like the
old `mid.raw * cbps / 1e6`, including the skew sign. Intended differences, each re-baselined with an
explanation in its change: bps configs with more than two decimals gain precision; a non-positive ask
is dropped instead of rejected; `on_fill` now reaches strategies for late and unknown fills; late
fills now move positions and therefore skew.

### 4. Parameters

`FASTMM_PARAM` stays the single declaration (field, default, range, doc). It gains:

- `Price`, `Qty` and `Notional` fields, parsed exactly.
- `FASTMM_PARAM_BPS(name, ...)` (field `Ratio`, configured in bps) and `FASTMM_PARAM_MS(name, ...)`
  (field `Duration`, configured in ms). Config key names and values stay as they are.
- Exact parsing accepts exponent notation, because TOML floats reach the parser through fmt
  (`2e-05`) and Python through `repr`. A value is rejected only if more than 8 decimals (4 for bps)
  remain after applying the exponent.
- `ParamDesc::parse(void*, string_view) -> optional<string>` and `format(const void*)` replace
  `set(double)` and `get`. `double def/min/max` stay for display and Python.
- Schema type names: `int`, `double`, `bool`, `decimal` (Price, Qty, Notional), `bps`, `ms`. Python
  maps `decimal` and `bps` to float and `ms` to int; `python/tests/test_smoke.py` is updated.
- An optional `std::optional<std::string> validate() const` on the params struct, run by
  `configure()` after all keys are applied (cross-field checks such as `quote_qty <= max_inventory`).
- `StrategyBase::params()` is const-only and `params_` becomes private.

Rejected: Boost.PFR (dependency, no docs or ranges), descriptor tuples (names written twice), schema
files (two sources of truth).

### 5. Registration: one function per strategy library

```cpp
// mm/strategies.hpp
namespace mm { void register_strategies(fastmm::StrategyRegistry& r); }

// strategies.cpp: the only registration file an author writes
#include "fastmm/strategies/factories.hpp"
#include "mm/microprice_mm.hpp"
#include "mm/strategies.hpp"
void mm::register_strategies(fastmm::StrategyRegistry& r) {
  fastmm::register_strategy<mm::MicropriceMM>(r);   // Sim + Replay + Live
}
```

- `strategies/module.hpp` declares `StrategyModule`, `register_strategy<S>(r)`, the factory templates
  `sim_factory<S>`, `replay_factory<S>`, `live_factory<S>` and the `FASTMM_INSTANTIATE_STRATEGY(S, kind)`
  macro. It never instantiates `Engine`. The factories are declared `template <class S>` (not
  `template <StrategyLike S>`) and check `StrategyLike<S>` with `static_assert` in their definitions:
  GCC and Clang mangle constrained templates differently.
- `factories.hpp` includes `factory_sim.hpp`, `factory_replay.hpp` and `factory_live.hpp`, so the
  file above instantiates all three engines. Taking a factory's address only emits a reference, so a
  missing instantiation is a link error naming the factory.
- The app references the registration function, so the linker pulls it and its instantiations out
  of static archives.
- `StrategyRegistry::try_add` returns `Added`, `AlreadyPresent` (same schema and factory),
  `Conflict` or `Invalid`. `register_strategy` throws on `Conflict`; CLIs exit 3 with the message.
  Tests that register fakes use a local `StrategyRegistry`, not the global one.
- Built-in strategies are always registered; a module list adds to them. Names stay flat with
  conflict detection. Only a strategy's owner instantiates it; others call the owner's function.
- `FASTMM_REGISTER_STRATEGY` is removed.
- Built-ins use an internal CMake function that generates one file per (strategy, transport) so the
  nine engine instantiations compile in parallel. It is not installed or documented until an
  external project needs it; a strategy that should not run live is not registered by that
  project's live app.

Rejected: static self-registration (dropped by archives, fails at runtime), X-macro strategy lists
(external projects would edit FastMM), `cli::live<A, B>()` variadics (all engines in `main.cpp`,
not shareable with Python), `dlopen` plugins (ADR-0006, ABI coupling, arbitrary code in a trading
process).

### 6. Runtime libraries and entry points

```
fastmm::core ─ fastmm::sim ─ fastmm::strategies (built-ins) ─ fastmm::backtest (+ cli::backtest, cli::replay)
fastmm::net ─ fastmm::venues ─ fastmm::live (run_live + cli::live) ── links fastmm::strategies
```

```cpp
namespace fastmm::cli {
int live(int argc, char argv, std::initializer_list<StrategyModule> modules = {});
int backtest(int argc, char argv, std::initializer_list<StrategyModule> modules = {});
int replay(int argc, char argv, std::initializer_list<StrategyModule> modules = {});
}
// apps/fastmm-live/main.cpp
int main(int argc, char argv) { return fastmm::cli::live(argc, argv); }
```

- `apps/fastmm-live/{live_backend,main}.cpp` move to `src/live/{session,cli}.cpp`; `LiveBackend`
  and `make_live_runner` move to `include/fastmm/live/live_backend.hpp`. Live factories need only
  `fastmm::core`. `runner_*.cpp`, `live_runners.*` and `src/backtest/registrations.cpp` are deleted.
- The backtest and replay CLI bodies move into `fastmm::backtest`. All three apps become 3-line mains.
- `fastmm-live` gains `--strategy <name>` and `--param k=v`; changing the strategy clears
  `[strategy.params]` (as `fastmm-backtest` does). The journal `config_hash` hashes the effective
  configuration after overrides. Error prefixes use the program name; log tags stay `fastmm-live:`.
- `cli::live` installs process-wide SIGINT/SIGTERM handlers. Deferred: a `LiveSession` class without
  signal handling, to replace the integration tests' own wiring.
- `fastmm::strategies` and `fastmm::live` join the install export. Supported consumers use the same
  compiler as the FastMM build: `find_package(fastmm)` against an install prefix, or
  `add_subdirectory`/FetchContent. Installing binaries and configs, cross-compiler consumers,
  fat LTO objects and version-compatibility policy are deferred until the repository is public.
- `examples/external-project/` replaces `tests/install-consumer` and is a copyable template: one
  strategy header, `strategies.cpp`, live/backtest/replay mains, a unit test using the harness
  (section 8). CI builds it against the install prefix, lists strategies, runs a synthetic backtest,
  runs its live app for 15 s against the build tree's `fastmm-sim-exchange`, and replays the journal.

### 7. Python strategies (v1)

- Model. A `fastmm.Strategy` subclass with the same hook names and argument order as C++
  (`on_book(ctx, inst, book)`, `on_trade(ctx, inst, trade)`, `on_fill(ctx, fill)`,
  `on_quoting(ctx, enabled)`, ...) and `Param(default, min=, max=, doc=)` descriptors. Undefined
  hooks are never called.
- Bridge. One C++ adapter, `PyStrategy`, compiled only into `_core` (`python/src/`). It is a
  normal `StrategyLike` class, so the engine, risk, OMS, QuoteManager, journal and outbound hash are
  unchanged. It caches the bound functions and a hook bitmask, reuses one view object per instrument
  and event type, and calls with vectorcall. A view read outside its callback raises
  `StaleViewError` (one integer compare).
- Numbers. Prices and quantities are available as floats (`book.mid`) and exact raw int64
  (`book.mid_raw`). `set_quotes` rounds floats to tick and lot (bids down, asks up);
  `set_quotes_raw` takes raw ints. No `Decimal`.
- Context. Mirrors section 2 in snake_case (`now_ns`, `every`, `once`, `random()` from the engine
  RNG).
- GIL. Held for the whole run; released briefly every 1,024 hooks, where
  `PyErr_CheckSignals()` also runs, so Ctrl-C works on the main thread.
- Errors. A raising hook stores the exception, pulls all quotes and requests stop.
  `EngineHooks` gains a nullable `stopped` callback honoured by `SimDriver` only (this also makes
  `request_stop()` work in C++ backtests). `ReplayDriver` always drains the journal, because stopping
  early would change the replay hash; a failed adapter goes inert instead. `run_backtest` raises
  `fastmm.StrategyError` with the traceback as `__cause__` and the partial result as `.result`.
- Entry point. `run_backtest(cfg, data=None, strategy=None, params=None)` where `strategy` is a
  registered name, a `Strategy` subclass or an unused instance. Backward compatible.
- Scope. In-process backtests only. Deferred: replay of Python strategies, process-pool sweeps,
  `fastmm.fixed`/`fastmm.testing` helpers, research series (`record`), a C++ skeleton generator,
  and any live use (first a hybrid where Python publishes slow targets to a C++ strategy through
  shared memory, then an opt-in `fastmm-live-py` limited to the sim exchange and testnets).
  `fastmm-live` never links libpython and the wheel stays net-free.
- Custom C++ strategies in Python. No runtime loading into the prebuilt wheel (ABI coupling).
  Build `_core` from source with `FASTMM_PYTHON_STRATEGY_DIRS=<dir;...>` (deferred with the rest).
- Proof. `examples/python/strategies/basic_mm_exact.py`, a raw-int port of BasicMM, reproduces
  `tests/fixtures/journals/sample_1000.sha256` in CI.

### 8. Quick start, test harness and documentation

Quick start. The shortest path is one strategy header plus a `main` that calls
`bt::run_backtest<MyMM>(cfg, source)`, built with FetchContent: no registration, under 30 lines. It is
the first page of the docs and is compiled and run in CI.

Test harness. `include/fastmm/testing/strategy_harness.hpp` (tier 1, in `fastmm::sim`):
`StrategyHarness<S>` wraps a real `Engine<S, SimClock, SimTransport, InlineFeed>` with one instrument
table and exposes `book(...)`, `trade(...)`, `fill(...)`, `disconnect()`/`reconnect()`,
`pull_quotes()`/`resume_quotes()`, `advance(duration)`, and `working_orders()`. Authors test hooks
without writing fake contexts; the docs' strategy API test and the external project use it.

Documentation. Markdown rendered on GitHub is canonical (the repository is private; a site
generator and Doxygen are deferred until it is public).

- Layout: `docs/getting-started/`, `docs/tutorials/`, `docs/how-to/{strategies,venues,operations}/`,
  `docs/reference/`, `docs/explanation/`, `docs/contributing/`, `docs/adr/`.
- Tutorial "Your first market maker" in `examples/cpp/tutorial/`: write the strategy, unit-test it
  with the harness, backtest, register, backtest and replay from the CLI, trade on the sim exchange
  with a disconnect fault, then Binance Demo (`--dry-run`, then a short keyed run with tight risk
  limits, Ctrl-C and `cancel_all ok`). Shell steps live in `scripts/docs/tutorial.sh`; CI runs the
  steps up to the sim exchange.
- Reference: strategy API (hooks, context, `Fill`, messages, params, helpers, harness), fixed point,
  public headers (`docs/api/public-headers.txt`; tier 1 strategy, testing and backtest API, tier 2
  venue and codec extension API, everything else internal), CLI, configuration, journal format,
  status file, glossary, Python API. `reference/cli.md` and `reference/configuration.md` are
  generated from `--help` and `schema.hpp` by tools with a `--check` mode.
- How-to: register, test, patterns (inventory limits, timers, stale books, `on_quoting`,
  `set_quotes` vs `send`), recorded-data backtests, sweeps; add a venue (rewritten from the real
  connectors, with a conformance checklist), add a binary protocol, record fixtures; run on testnets,
  go-live checklist, kill switch and shutdown, journals and PnL reconciliation (the Demo session's
  report becomes `tools/pnl_report.py`), latency tuning, troubleshooting keyed by log text.
- Explanation: event flow, determinism, risk model.
- Style: one page type per page, second person, present tense, British spelling (as today),
  commands copy-pasteable from the repo root, explicit units, no hand-copied code longer than three
  lines.

Docs checks in CI:

1. `tools/doc_snippets.py --check`: fenced blocks marked `<!-- snippet: path#region -->` match their
   source region (the tool rewrites them, so GitHub shows real code).
2. `tests/docs/strategy_api_doc_test.cpp`: an all-hooks strategy with the documented signatures runs
   in the harness and asserts every hook fired; `static_assert`s pin each documented context
   method's type. The reference tables are snippets of this file.
3. Generated `cli.md` and `configuration.md` are current; every `configs/*.toml` loads without
   warnings.
4. Every header in the public manifest compiles alone.
5. The `examples` and `tutorial` ctest labels run the C++ examples, the quick start and the external
   project; the tutorial script runs after the gcc-release build.
6. Relative links and anchors resolve (offline).

## Consequences

- Strategies, test fakes and strategy docs break once. Golden outbound hashes are recorded on
  `main` before the change; each step keeps them or re-baselines them with the reason in the change.
- A wrong hook signature becomes a readable compile error and a missing instantiation a link error.
- External projects get a supported path to live trading without forking, with a tier 1 API that
  must stay stable within a minor version.
- Compile-fail tests (`tests/compile_fail/`, label `compile_fail`) join both compiler presets; each
  case has a control build that must compile and matches the expected message.
- Python hooks are slower than C++: the integer BasicMM port adds about 3.1 µs per `on_book` or
  `on_fill` call, and a synthetic backtest runs 1.31 M events/s against 2.02 M for C++ `basic_mm`
  ([Python API](../reference/python-api.md#performance)). That is acceptable for research and is why
  Python strategies stay out of live trading.
- `binance_futures` is removed from `schema.hpp` until a connector exists.

## Implementation

Implemented in 2026-09 in this order: golden hashes, engine fixes, hooks and context, fixed point and
parameters, registration and runtime, Python strategies, docs.

## Deviations from the decision

- `NewOrderRequest::limit` returns a `LimitOrder` builder (also with `.tag(n)`) that converts to `NewOrderRequest`, because `post_only` and `reduce_only` are data members.
- The hook call check runs in `hooks.hpp`, so the engine and `verify_strategy` agree and a private hook is rejected even with `friend Engine`.
- Near-miss warnings are emitted inside `hooks.hpp`, so a consumer that includes FastMM as a system header does not see them; wrong-signature errors are unaffected.
- `set_quotes` also returns false for an instrument outside the table; fills for such instruments are counted in `EngineStats::unknown_instrument_fills`.
- A late fill gets the order's terminal record: `Fill::known` is true and `update->order` carries only id, instrument, side, state and filled quantity.
- Exponents are parsed by a new `Fixed::parse`; `Fixed::from_decimal` stays strict for venue strings.
- `fastmm::literals` is an inline namespace; `Ratio * Ratio` is defined, and `from_bps`/`to_bps` are `Fixed` members constrained to the `Ratio` tag.
- `DesiredQuotes::bid/ask` also drop a non-positive quantity and levels beyond `kMaxQuoteLevels`, so a zero-quantity level (`quote_qty` below the lot) is no longer sent.
- `configure()` applies keys to a copy and replaces the parameters only after `validate()` passes.
- AvellanedaStoikov's seconds-valued parameters and OptionsMM's `max_delta` and `max_vega` stay `double`; no built-in strategy has a `validate()`.
- The `basic_mm/coupled` and `basic_mm/l2_queue` golden hashes were re-baselined: their test market's `half_spread_bps = 0.003` had rounded to 0 in the centi-bps code.
- `register_strategy<S>(r, Transports)` exists for FastMM's own tests; the documented API is `register_strategy<S>(r)`.
- `EngineConfig` moved to `core/engine_config.hpp` so registration headers do not include `engine.hpp`, and `StrategyRegistry::add` was removed.
- Each command line lists only the strategies of its own transport; `--list-strategies --format json` was added.
- The external project's unit test uses plain checks because the install does not export doctest; `-DFASTMM_SOURCE_DIR=` switches it to `add_subdirectory`.
- Python hooks receive an `Instrument` value; sides and liquidity are integers, order states and reject reasons strings, and timers take nanoseconds.
- Python `send` and `replace` raise `OrderRejected`; `cancel` returns `False` for an order that already ended.
- Python `KeyboardInterrupt` and `SystemExit` propagate with `.result` instead of being wrapped in `StrategyError`, and signals are also checked every 4,096 driver steps.
- A Python strategy's journal name is `py:<QualName>`, truncated to 31 characters.
- The quick start sets one synthetic-market value, because the default market gives no fills to a quote 0.005 bps from the mid.
- `[backtest]` and `[sim]` in `reference/configuration.md` stay hand-written because the schema treats them as free-form.
- Deferred from section 8: how-to pages for testing, patterns, recorded-data backtests, sweeps, binary protocols, fixtures and latency tuning; a log catalogue check; Markdown linting; a scheduled Binance Demo dry run.
