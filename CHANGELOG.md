# Changelog

All notable changes are recorded here (Keep a Changelog format).

## [Unreleased]

### Changed
- Parameter updates (ADR-0013). `EventType::ParamUpdate` (26) carries up to 32 (field index, raw
  value) pairs. `ParamPublisher` validates an update off the engine thread and pushes it into a
  feed ring (false when the ring is full); `sim::ParamSchedule` delivers updates at simulated
  times in backtests. The engine applies an update at one event, journals it and calls the new
  `on_params(ctx)` hook. `[strategy] max_param_age_ms` (default off) disables quoting before the
  first update and while none was applied for that long in engine time. Journal format version 3
  embeds the strategy's parameter table and replay matches fields by name; readers open versions
  1 to 3 and `sample_1000.sha256` is unchanged. `KillReason::StrategyError` (9) is appended.
  `ParamDesc` gains `get_raw` and `set_raw`; `TimerMsg` byte 68 marks the engine's own timer.
- `ctx.request_stop()` ends a backtest: `sim::EngineHooks` gains a nullable `stopped` callback that
  `SimDriver` checks after every engine step. `ReplayDriver` ignores it and always drains the
  journal. The golden hashes are unchanged.
- `fastmm.run_backtest` is a Python function with a `params=` argument applied on top of
  `config.params`; `strategy=` also takes a `fastmm.Strategy` subclass or instance. The existing
  call forms are unchanged.
- **`fastmm-live` exits after a kill switch it did not ask for.** New `[engine] on_kill = "exit" |
  "stay"` (default `"exit"`): after the engine trips the kill switch itself (`[risk] max_loss`, a
  full outbound or journal ring, every venue killed) the session runs the normal shutdown (quotes
  pulled, REST cancel-all on every venue, summary, status file, journal trailer) and exits with the
  new exit code 6 (5 if a cancel-all failed). `"stay"` keeps the previous behaviour and logs an
  ERROR line every 10 s while killed. `fastmm-live --help` lists every exit code; a test checks the
  list against docs/reference/cli.md.
- Status segment version 3 (kill reasons, per-venue kill flags, `venue_kills`); `fastmm-top` shows
  `KILLED (<reason>)` or `VENUE KILLED` next to the state and a `kill` column per venue. A
  `fastmm-top` of another build refuses the file.
- **Breaking, strategy registration (ADR-0012).** A strategy library exports one registration
  function that calls `fastmm::register_strategy<S>(r)` per strategy; that call adds the Sim,
  Replay and Live factories, whose templates are declared in `strategies/module.hpp` and defined in
  `strategies/factory_{sim,replay,live}.hpp` (`factories.hpp` includes all three). A registered
  factory that no file compiles is a link error naming it. `StrategyRegistry::add` is replaced by
  `try_add`, which returns `AddResult::Added`, `AlreadyPresent`, `Conflict` or `Invalid`;
  `register_strategy` throws `StrategyConflict` on a conflict. `make_engine_runner` moves to
  `strategies/engine_factory.hpp`, `EngineConfig` to `core/engine_config.hpp` and `LiveBackend` /
  `make_live_runner` to `live/live_backend.hpp`; `sim::make_sim_or_replay_runner` is removed.
- The built-in strategies are the new `fastmm::strategies` library, registered for all three
  runtimes by `fastmm::register_builtin_strategies(StrategyRegistry&)` (one generated file per
  strategy and runtime); `fastmm::backtest` links it and `bt::register_builtin_strategies()` wraps
  it. `apps/fastmm-live/runner_*.cpp`, `live_runners.*` and `src/backtest/registrations.cpp` are
  gone.
- **The live session and the command lines are libraries.** `fastmm::live` (net builds) holds
  `run_live` (`live/session.hpp`) and `fastmm::cli::live`; `fastmm::backtest` holds
  `fastmm::cli::backtest` and `fastmm::cli::replay`. Each takes the app's strategy modules; the
  built-in strategies are always registered first, and a strategy name registered by different
  code exits with code 3. `fastmm-live`, `fastmm-backtest` and `fastmm-replay` are 3-line mains.
  Usage and error messages start with the program name; log lines keep the `fastmm-live:` tag.
- The install exports `fastmm::strategies` and, in net builds, `fastmm::live`; the package config
  reports components, and `find_package(fastmm COMPONENTS live)` against an install built with
  `FASTMM_BUILD_NET=OFF` fails with a message saying so. `examples/external-project/` replaces
  `tests/install-consumer`, and the CI install step builds and runs it
  (`scripts/ci-external-project.sh`).
- The opt-in testnet tests (`venues.live.*` in `fastmm_venues_tests`) carry the ctest label `live`
  instead of `unit` and `fixture`: `ctest -L live` selects them, the test presets exclude them, and
  they still skip unless `FASTMM_LIVE_TESTS=1`.
- **Status segment version 2** (`venue_rejects` and per-reason reject counts). `fastmm-top` and
  `fastmm-live` must come from the same build; `fastmm-top` reports a mismatch as `<file> was
  written by a different FastMM build (status segment version <n>, this fastmm-top reads version
  <m>)` (exit code 3 with `--once`) instead of misreading it or saying there is no status segment.
  `fastmm-top` shows the reject counters on a `rejects` line instead of the `engine` line.
- **Journal format v2 (ADR-0010).** Every consumed event and fired timer carries the engine clock
  (an int32 ns delta in `EventHeader::reserved0`, flag `kEngineTime`; `EngineTimeMsg` records hold
  absolute values at start, finish and on overflow). The header holds the session epoch, quoting
  enabled (dry run), per-venue cancel-replace and the effective configuration as TOML without API
  keys; `config_hash` is the hash of that text, after command-line overrides. Version 1 journals
  are still read and replay as before. `tools/journal_dump.py` prints the new fields.
- The engine reads its clock once per consumed event, per fired timer, at start and at finish, and
  uses that time for every decision and Out* stamp (`Engine::now()`). Golden outbound hashes of
  SimClock runs are unchanged.
- `fastmm-replay` uses the configuration embedded in a session journal by default, checks
  `--config` against the journal's config hash (a different one is reported as a what-if run),
  needs no `${VAR}` secrets, no longer falls back to `configs/backtest-example.toml` for session
  journals (a v1 session journal needs `--config`), and prints the first mismatching message as
  recorded and as replayed.
- `bt::replay_journal(path)` replays from the journal alone; `replay_journal(path, cfg)` restores
  the session settings from the header too (`ReplayOptions::session_from_journal`).
- **Breaking, strategy hooks (ADR-0012).** `on_trade`, `on_book_ticker` and `on_option_ticker` take
  `(ctx, InstrumentId id, const Msg& m)`; `on_fill` takes `(ctx, const Fill& fill)` instead of
  `(ctx, OmsUpdate, OrderFillMsg)`. Instrument-scoped hooks fire only for instruments in the table.
  `on_fill` now also reaches strategies for late fills and fills for unknown ids.
- **Breaking, strategy context.** `set_quotes` returns `bool` (false while quoting is disabled);
  `add_timer(period, repeat, tag)` is replaced by `every(period, tag)` and `once(delay, tag)`;
  `instrument_count()` is removed. `FASTMM_REGISTER_STRATEGY` is removed (register explicitly).
- The engine checks every hook at compile time: a member with a hook's name whose call does not
  compile fails the build with `fastmm: <hook> has the wrong signature or is not public; expected
  ...`, and likely misspellings (`on_fills`, `on_tick`, ...) warn. `Engine::start()` logs the
  implemented hook set.
- BasicMM, AvellanedaStoikov and OptionsMM requote when quoting resumes and remember a quoted
  mid or theo only when `set_quotes` took the quotes. Their golden outbound hashes are unchanged.
- **Breaking, strategy parameters (ADR-0012).** `FASTMM_PARAM` accepts `Price`, `Qty` and
  `Notional` fields (schema type `decimal`, parsed exactly); `FASTMM_PARAM_BPS` declares a `Ratio`
  configured in bps (type `bps`, up to 4 decimals) and `FASTMM_PARAM_MS` a `Duration` configured in
  whole milliseconds (type `ms`). Exact values accept exponent notation (`2e-05`). `ParamDesc::parse`
  and `format` replace `set` and `get`; range checks use the typed values. An optional
  `validate() const` on the params struct runs after all keys; `configure()` leaves the previous
  values in place on any error. `StrategyBase::params()` is read-only and `params_` is private.
  `describe_params()` prints exact, shortest values (`gamma=0.5`). Config keys and values are
  unchanged; Python reports `decimal` and `bps` as float and `ms` as int.
- BasicMM, AvellanedaStoikov and OptionsMM use the typed parameters and the quoting helpers. BasicMM
  bps parameters keep four decimals instead of two (0.003 bps was rounded to 0), which changes the
  `basic_mm` golden hashes, whose test market uses 0.003 bps; with 0 bps the old hashes are
  reproduced. A non-positive price or quantity level is dropped instead of sent. The
  AvellanedaStoikov, OptionsMM and `sample_1000` hashes are unchanged.

### Added
- **`fastmm-live` wheel (ADR-0013, section 5).** A second distribution from `python/live`, pinned to
  the same `fastmm` version and installed by `pip install "fastmm[live]"` (CPython 3.10 or later).
  Its module `fastmm_live._live` links the network stack, the venue connectors and OpenSSL 3.5.8
  (`scripts/wheels/build-openssl.sh`, static, checksum-verified) and exports only `PyInit__live`.
  This release has `build_info()`, `ca_locations()` and `self_test()`, an in-memory TLS handshake.
  The wheels workflow uses cibuildwheel 4.2.1, adds CPython 3.14 and builds, checks and tests the
  `fastmm-live` wheels; the per-push CI does not build them.
- TLS clients find CA certificates from `SSL_CERT_FILE` and `SSL_CERT_DIR`, then
  `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt` and `/etc/ssl/cert.pem`,
  then a fallback file (`certifi` in `fastmm-live`), then OpenSSL's built-in paths
  (`net/ca_locations.hpp`). A `SSL_CERT_FILE` that does not load is an error naming the variable.
  `TlsContext::server_pem` and `add_ca_pem` take PEM text.
- **Python strategies in backtests (ADR-0012, section 7).** Subclass `fastmm.Strategy` with the C++
  hook names (`on_book(ctx, inst, book)`, `on_fill(ctx, fill)`, `on_quoting(ctx, enabled)`, ...) and
  `fastmm.Param` parameters (typed, C++ error messages), then
  `fastmm.run_backtest(cfg, data, strategy=MyStrategy, params={...})`. The `PyStrategy` adapter,
  compiled only into `fastmm._core`, is an ordinary strategy of the C++ engine: undefined hooks are
  never called, views are reused and raise `fastmm.StaleViewError` outside their hook, the context
  mirrors `StrategyContext` (`set_quotes` with passive tick rounding, `set_quotes_raw`, `send` raising
  `fastmm.OrderRejected`, `every`/`once`, `random()`), and a raising hook stops the run with
  `fastmm.StrategyError` (traceback as `__cause__`, partial result as `.result`). Results and journals
  name the strategy `py:<QualName>`. `examples/python/strategies/basic_mm_exact.py`, an integer port
  of BasicMM, reproduces the C++ outbound hash in CI; `skew_mm.py` is a float example;
  `docs/reference/python-api.md` documents the API, number model, determinism rules and measured
  cost; `bench/python/bench_py_strategy.py` measures it.
- **Per-venue kill switch.** A connector error that makes one venue unusable (error map `Fatal`:
  bad key, signature or permission, failed authentication; `HardStop`: Binance IP ban) sends
  `ControlCommand::TripVenueKill` through the venue's order ring (`venues::emit_venue_kill`), so it
  is journaled and replays. The engine sets that venue's kill bit, pulls its quotes, cancels its
  orders and refuses new orders to it (`RejectReason::VenueKilled`) while the other venues keep
  trading; every venue with instruments killed trips the global switch. `StrategyContext::
  venue_killed(venue)`.
- `KillReason` records why each kill flag was first set (`Engine::kill_reason()`,
  `venue_kill_reason(venue)`); `EngineLiveStats` carries the reasons and `venue_kills` and is
  published as soon as a flag changes.
- **Quick start and tutorial (ADR-0012 step 7).** `examples/quickstart/`: one strategy header and
  a `main` that calls `bt::run_backtest<MyMM>(cfg, source)`, 29 lines, built inside FastMM or with
  `FetchContent` (ctest `examples.quickstart`). `examples/cpp/tutorial/`: the strategy `first_mm`,
  its harness unit test, a C++ backtest, a registration file and `tutorial-backtest`,
  `tutorial-replay` and `tutorial-live` (label `tutorial`), with `configs/tutorial-sim.toml` (the
  simulated exchange with a market-data disconnect fault) and `configs/tutorial-binance-demo.toml`
  (tight risk limits). `scripts/docs/tutorial.sh` runs the tutorial's shell steps; ctest
  `tutorial.script` runs them up to the simulated exchange, including a verified replay of the live
  journal. The C++ examples now run as tests (label `examples`).
- **Docs checks.** `tools/doc_snippets.py` keeps code blocks marked `<!-- snippet: path#region -->`
  identical to their source regions; `tools/docs_links.py` checks relative links and anchors
  offline; `tools/docs_cli_help.py` and `tools/docs_config_ref.py` generate the command-line and
  configuration references from `--help` and `schema.hpp`. All four have `--check`, run in CI
  (lint, and gcc-release for the command lines).
- `tests/docs/strategy_api_doc_test.cpp` (label `docs`): a strategy with every documented hook runs
  in the harness and every hook must fire; `static_assert`s pin each documented context method,
  `Fill` and message field, parameter kind and helper. `docs/api/public-headers.txt` lists the
  tier 1 and tier 2 headers, and ctest `docs.public_headers` compiles each one alone.
- `fastmm-live --strategy <name>` and `--param key=value`. A strategy other than the config's
  ignores `[strategy.params]` (as in `fastmm-backtest`), parameter names are checked before any
  venue is contacted, and the journal embeds and hashes the configuration after the overrides, so
  the session replays with `fastmm-replay --verify` alone.
- `--list-strategies --format json` on `fastmm-live` and `fastmm-backtest`: name, transports and the
  parameter schema of every strategy the app can run.
- `examples/external-project/`: a strategy project to copy, with one strategy header, the
  registration file, live, backtest and replay apps and a `StrategyHarness` test;
  `docs/how-to/strategies/register-a-strategy.md` covers registration in and out of tree.
- **Rejects per reason.** The engine counts risk and venue rejects per `RejectReason`
  (`EngineStats` / `RunnerStats::risk_rejects_by_reason`, `venue_rejects_by_reason`, `venue_rejects`;
  an increment on the reject path). Risk rejects are logged at WARN with reason, instrument, side,
  quantity and price: the first of each reason, then at most one line per reason per
  `EngineConfig::reject_log_interval` (10 s) with the number suppressed. `fastmm-top` shows the most
  frequent reasons (`risk_rejects=17 (MaxPosition 12, RateLimit 5) venue_rejects=3
  (PostOnlyWouldCross 3)`), the `fastmm-live` shutdown summary adds `venue_rejects=` and
  `risk_rejects by reason:` / `venue_rejects by reason:` lines, and `tools/pnl_report.py` reads them.
- `Config::effective_toml()`, `Config::effective_hash()`; `bt::journal_config()`,
  `bt::describe_outbound()`; `JournalInfo` session fields.
- Integration tests that record `fastmm-live` sessions against the simulator (one with TSC
  recalibration steps) and replay them from the journal.
- `include/fastmm/strategies/hooks.hpp`: the hook table, `verify_strategy<S>()`, `hook_status`,
  `implemented_hooks` and the `Fill` view (position delta, booked fee, known, late, order done).
- `on_quoting(ctx, bool enabled)` hook: reports changes of `ctx.quoting_enabled()` (operator
  pull and resume, kill switch trip and reset, reconciliation begin and end) after the triggering
  event, never from inside a context call.
- Strategy context: `contains`, `portfolio`, `working_quote`, `order`, `open_qty`, `every`, `once`;
  `NewOrderRequest::limit(id, side, px, qty)` with `.post_only()`, `.reduce_only()`, `.ioc()` and
  `.tag(n)`; `RejectReason::InvalidTag` (38) for direct orders using a quote-manager tag.
- `fastmm::sim::StrategyHarness<S>` (`include/fastmm/testing/strategy_harness.hpp`): the real engine
  and a simulated venue on virtual time with `book`, `trade`, `fill`, `disconnect`/`reconnect`,
  `pull_quotes`/`resume_quotes`, `advance` and `working_orders`.
- Compile-fail tests (`tests/compile_fail/`, ctest label `compile_fail`) for the hook checker, each
  with a control build, and golden outbound hashes for the three built-in strategies.
- `Ratio` (1 bp = raw 10,000) with `value * ratio` through Int128 and one truncation toward zero,
  `ratio(num, den)`, `Ratio::from_bps`, `to_bps`; exact literals `100.25_px`, `0.01_qty`, `5_bps`
  (too many decimals is a compile error); `Fixed::parse` for decimals in exponent notation.
- `include/fastmm/strategies/quoting.hpp`: `mid`, `microprice`, `spread_ratio`, `away_from`,
  `inventory_allows`, `keep_passive`; `Instrument::ticks(n)`; `DesiredQuotes::bid`, `ask` and
  `uncross`. `include/fastmm/strategy.hpp` includes everything a strategy header needs.
- `docs/reference/fixed-point.md`; `bench_strategies` (`BM_BasicMM_ComputeQuotes`).
- Deribit connector (`kind = "deribit"`), JSON-RPC 2.0 over WebSocket for options and futures:
  reference data from public/get_instruments (tick_size_steps, contract_size, inverse), book sync on
  change_id/prev_change_id with resubscribe on gaps, ticker and trades, client_credentials auth with
  token refresh, heartbeat replies, private/buy, sell, edit and cancel (label = client id,
  reject_post_only), user.orders/user.trades, open-order reconciliation, cancel-on-disconnect with a
  REST cancel-all, credit-based order rate limiting; `configs/deribit-testnet.toml` and the opt-in
  `live.deribit` test. The public part passed against the testnet; private payloads follow the
  published schemas and a fake exchange, not recorded traffic (no keys were available).
- `EventType::OptionTicker` (`OptionTickerMsg`): mark price, implied vols, greeks and underlying,
  with an `on_option_ticker` strategy hook, journal support and `tools/journal_dump.py` decoding.
- `core/options/black76.hpp`: Black-76 price and greeks and a bounded implied-volatility solver.
- `options_mm` strategy (Sim, Replay, Live): Black-76 quotes on the venue's or a smoothed implied vol
  with portfolio-delta and inventory skew, vega widening and delta, vega and position limits
  (docs/options.md).
- `fastmm::codecs` (M3), a core-only library for exchange wire protocols. FIX 4.4: zero-copy
  `FixView`, `FixBuilder` with BodyLength/CheckSum backfill, framer, initiator/acceptor session
  (logon, heartbeats, test requests, gap detection with resend, PossDup and GapFill, sequence
  resets) over a bounded message store, ExecutionReport/OrderCancelReject/market-data decoding and
  NewOrderSingle/cancel/cancel-replace encoding. A simulation test drives a session against the
  matching engine with 2% message loss and checks fills, positions, open orders and the book.
- Nasdaq protocol family in `fastmm::codecs`: TotalView-ITCH 5.0 (every message type, exact
  Price(4)/Price(8) conversion, decoding to L3 add/execute/cancel/replace events and trades, and an
  encoder for simulation), MoldUDP64 (framing, gap detection, re-requests, in-order delivery),
  SoupBinTCP 3.00/4.00/4.10 client and server sessions, and OUCH 4.2 and 5.0 order entry. Checked
  against the matching engine: the ITCH-fed L3 book equals the engine's book after every step
  (16 seeds), OUCH sessions over SoupBinTCP reproduce the engine's fills and open orders, and
  MoldUDP64 delivers every message once and in order under loss, duplication and reordering.
  Limitations are listed in docs/codecs-nasdaq.md (for example, OUCH Replace sends the command's
  quantity, which matches OUCH's "total liable" only for unfilled orders).
- CME MDP 3.0 in `fastmm::codecs`: `tools/sbe_gen.py`, a standard-library SBE generator, emits
  memcpy-based flyweights from a committed subset of CME's official schema (templates_FixBinary.xml
  version 13); the decoder turns MBP book entries into book deltas with per-instrument RptSeq
  tracking, trade summaries into trades and futures definitions into instruments; the feed handler
  arbitrates lines A and B, detects gaps and recovers from the snapshot loop with buffered
  incrementals. Simulation tests keep decoded books equal to the matching engine's top levels with
  no loss, 30% loss on A, burst loss on both lines and duplicates with reordering. Not handled yet:
  MBO, the implied book, statistics, options and spreads, TCP replay.
- `net::Reactor` io_uring backend (`ReactorBackend::IoUring`) on raw io_uring syscalls: multishot
  polls, poll updates, nanosecond timeouts, syscall-free busy polling, and a kernel probe.
  `[engine] net_backend = "epoll" | "io_uring"` selects it for fastmm-live and the simulated
  exchange, falling back to epoll when unavailable. Reactor, WebSocket, HTTP, TLS, DNS and
  connection tests run on both backends; `bench_reactor` shows the same loopback echo p50 for both
  on WSL2 (the syscalls dominate).
- `fastmm-top`: a terminal dashboard for live sessions. `fastmm-live` publishes its state to
  `/dev/shm/fastmm-<engine>.status` every 250 ms (`--status`, `--no-status`) from a seqlocked copy
  of the engine's counters, PnL, kill-switch state and latency, plus every venue's status.
- `[engine] reject_backoff_ms` / `reject_backoff_max_ms`: after a venue rejects a new quote (other
  than a post-only cross) the quote manager pauses that side, doubling up to the cap.
- A `wheels` workflow builds manylinux wheels for CPython 3.9-3.13 and the sdist; publishing to
  PyPI is a manual, opt-in step (docs/python.md).
- Binance Spot Demo Mode: `configs/binance-demo.toml`, and `FASTMM_BINANCE_ENV=demo` for the live
  test. The live test passed against Demo Mode (book sync, far post-only order, cancel, cancel-all).
- CI builds the Docker image and runs the compose stack for 20 seconds.
- **Shipped config test** (ctest label `config`, `fastmm_config_tests`): every `configs/*.toml` is
  loaded with placeholder values for the `${VAR}` it references, checked against the schema, and
  its strategy is configured through the registry, so a bad `[strategy.params]` value or key, a
  schema error or an unknown strategy in a shipped config fails the build's tests. A second case
  checks that mutated copies (`quote_qty = "abc"`, an unknown parameter, a string where the schema
  wants an integer, an unknown strategy) are reported.
- **Venue hot-path coverage.** `tests/hotpath/venues_noalloc_test.cpp` (label `noalloc`): after a
  warm-up pass over the recorded fixtures, the market-data parser, the private parser and the order
  encoder (new, cancel, replace) of Binance, Bybit and Deribit decode and encode 200 rounds without
  an allocation; `fastmm_hotpath_tests` now links `fastmm::venues`. Benchmarks for the Bybit trade,
  Bybit execution, Deribit book change, option ticker, trades, user order and user trade frames
  (`bench_json`) and the three order encoders (`bench_order_encoders`), with p50 budgets in
  `bench/ci_budget.toml` for these, the Binance execution report and the Bybit order book. Measured
  p50 (release, pinned, median of three runs): Bybit trade 144 ns, Bybit orderbook 20 levels 703 ns,
  Deribit book change 620 ns, option ticker 398 ns, three trades 398 ns; private Binance execution
  report 309 ns, Bybit execution 380 ns, Deribit user order 294 ns, user trade 431 ns; encoders
  Binance order.place with HMAC signature 1474 ns, Bybit order.create 312 ns, Deribit private/buy
  319 ns.

### Fixed
- **`BM_TickToOrder_Sim` measured no order events.** Since the quote manager applies a requote
  target on the ack of a pending order, every timed tick found both quotes pending and sent
  nothing (`order_events_pct` 0.000; its p50 came from a handful of start-up events). The rig now
  runs the venue and acks, untimed, until nothing is in flight and both quotes are working, and
  quotes 3 ticks wide so the New of a cancel-then-new no longer meets the other side's old quote
  and fails self-trade prevention. Every timed tick sends a cancel per side. Median of 5 repetitions,
  release-native, pinned: p50 1023 ns, p99 1535 ns, 1720 ns per iteration (the published 639 ns p50
  from 2026-09-13 predates the regression and no longer applies). The benchmark fails
  (`SkipWithError`) when fewer than 50 % of the ticks send orders or the rig does not settle; the
  bench-smoke tests fail on a benchmark error; `tools/check_budgets.py` fails on errored benchmarks
  and on median counters below the new `[min_counters]` floors in `bench/ci_budget.toml`.
  `BM_EngineStep_Sim` now starts each batch from the same settled state (`out_msgs_per_step` 2).
- `scripts/bench.sh` removes the host name from the result JSON and records UTC dates, and
  `tools/bench_table.py` no longer fails with `UnboundLocalError` (a local variable shadowed
  `machine()`), which left `bench/README.md` empty. `--only <bench>` reruns selected executables
  and keeps the other results.
- The OMS no longer completes a replace on a duplicate ack for the original id. Binance acks every
  new order twice (WS API response and user-stream NEW); a requote between the two made the second
  ack look like the replace's ack, so the new price was booked unconfirmed, the old leg's cancel
  then ended the order and left the new id mapped to a freed slot, and the new order stayed live at
  the venue untracked (a debug build asserted in `Oms::on_ack`). Only the replacement id's ack
  completes a replace now.
- `calibrate_tsc()` (and `TscClock::calibrate()`) measures the TSC rate against
  `CLOCK_MONOTONIC_RAW`, like `TscCalibrator::start()`, instead of `CLOCK_REALTIME`. A host
  wall-clock step inside its 50 ms window (WSL2 steps by 0.5-1.5 s every 10-40 s) made the clock run
  tens of times fast, so every order failed the stale-market-data check; this was the occasional
  20 s timeout of the `sim_exchange e2e` integration tests, whose live engine calibrates this way.
  A new overload takes injected `ClockReadings`.
- A `[risk] max_loss` trip or an internal failure no longer leaves `fastmm-live` running silently
  with quoting off (see `[engine] on_kill`), and a fatal error on one venue now stops trading on
  that venue through its kill switch instead of leaving the engine quoting into a venue that
  refuses every order.
- **Live session journals replay exactly.** `fastmm-replay --journal <live journal> --verify`
  reported a mismatch at the first outbound message: replay ran with session epoch 1 (every
  recorded ack named an unknown order and replay cancelled it), drove its clock from receive times
  instead of the live `TscClock`, and fell back to `configs/backtest-example.toml`. Journal format
  v2 records what was missing, and replay restores it.
- Outbound messages the transport refused (ring full) were journaled as sent. They are journaled
  after the hand-off and marked dropped; replay refuses them again. Transports accept a prefix of a
  batch (`LiveTransport` stops at the first refused message).
- `SyntheticSource::next` failed gcc's -Wnull-dereference without LTO; the peek is checked.
- `be*_t`/`le*_t` wire fields held an integer, so a field at an odd offset of a packed layout was
  misaligned and UBSan reported member calls on it; they are byte arrays now.
- gcc 13 with `-Werror` and without LTO rejected `MatchingEngine::remove_resting` for a potential
  null dereference (the assert is compiled out); the missing-level case is handled explicitly.
- Serialize latency was measured from the previous event's strategy decision (about 100 ms with
  BasicMM's stale timer). T3 is now stamped when the strategy calls the order API.
- Binance and Bybit order/user channels reported "Live" again after a silent Stale, which read
  like reconnects and made strategies requote.
- A Binance Demo Mode session (no base asset) resent the rejected ask on almost every requote; with
  the reject backoff the same 60 s session had 6 rejects instead of 20.
- The market-data drop end-to-end test no longer requires cancels: quotes can fill before the drop.
- Bybit positions deduct `spotBorrow` from `walletBalance` (the net holding; `locked` coins were
  already included, as the wallet docs define).
- Bybit `rejectReason` values map to specific reasons (duplicate id, unknown order, self-trade
  prevention, price scale, zero quantity, price limits) instead of a guessed "Balance" match.
- Binance `GET /api/v3/time` is counted with weight 1, as documented.
- Comments that said VERIFY now cite what the Bybit and Binance docs state (Bybit sequencing and
  amend quantity, Binance listenKey removal from 2026-02-20).
- Quotes stayed empty after a reconciliation in a quiet market: End did not requote, a requote that
  met pending orders was dropped, and End's cancellations bypassed the quote manager. The engine now
  re-applies the quotes it paused at Begin, a slot applies a target recorded while its order was
  pending once the order's ack or terminal update arrives (unless quotes were pulled), and End's
  updates go through the normal order-update path.
- Reconciliation cleared a cancel or replace still in flight, and marked cancelled the orders a
  strategy sent after the open-orders request (for example its requote on Live), whose acks were
  then ignored: live orders the OMS no longer tracked. Binance, Bybit and Deribit stamp the Begin
  with the last order id sent before the request (`ReconcileMsg::sent_watermark`), End spares later
  orders and only touches the reconciling venue, pending cancels/replaces stay pending, and an ack
  for an order reconciliation dropped is cancelled at the venue.
- Fills that arrived after the cancel ack (late fills) never reached positions, fees or PnL: the
  terminal record now keeps instrument and side, and the engine falls back to the fill's own
  instrument and side.
- The gcc ASan+UBSan build of the Binance, Bybit and Deribit decoders and order encoders was slow
  enough to nearly time out the gcc-asan CI job: gcc 13's UBSan instrumentation of one large
  function per decoder full of inlined simdjson lookups took minutes per file. The decoders and
  request builders are split into non-inlined functions per frame, item or request kind, with the
  same output. Compile times under the ASan flags (before / after): bybit_private_parser 801 s /
  32 s, binance_order_encoder 549 s / 40 s, binance_md_parser 310 s / 24 s, bybit_md_parser 241 s /
  23 s, deribit_private_parser 140 s / 38 s, bybit_order_encoder 96 s / 47 s.
- `schema.hpp` listed a `binance_futures` venue kind that has no connector; the `kind` description
  now names the kinds the factory accepts.
- `tools/pnl_report.py` replaces the session scripts whose mark-to-market PnL subtracted commission
  charged in the base asset a second time (-48.74 instead of -32.94 USDT on a Binance Demo session).

### Added (tools)
- `tools/pnl_report.py`: per-hour fills, maker share, volume, fees in the quote asset and inventory
  from a journal; the journal's trading PnL; the engine's final PnL line from the log; and, with
  start and end account snapshots, the equity change split into starting-inventory revaluation and
  trading, with the balance changes checked against the fills. `--self-test` checks it on a
  synthetic journal (ctest `tools.pnl_report.self_test`). `tools/journal_dump.py` exposes its
  reader (`parse_header`, `parse_instruments`, `iter_events`, `fill_fields`) with unchanged output.
- `.env.example` lists the Deribit key variables and `FASTMM_BINANCE_ENV`.

### Documentation
- Subtraction pass over README.md and docs/ (59,673 → 52,442 words; README 195 → 85 lines): removed
  meta-commentary, reassurance, restated tables and output, promotional wording and repeated
  warnings; each shutdown, secrets, `stale_ms` and exit-code fact now has one page that the others
  link to. `docs/contributing/writing-docs.md` has the style rules. Corrected claims: tick-to-order
  is a p50 over ticks that sent orders, only heap allocation is enforced on the hot path, the
  FIX/ITCH/MDP3 codecs are not connected to a venue, and the exit codes in `docs/reference/cli.md`.
- **Docs reorganised (ADR-0012 step 7).** `docs/README.md` is the index; pages live in
  `getting-started/`, `tutorials/`, `how-to/`, `reference/`, `explanation/` and `contributing/`.
  Moved: `architecture.md` and `benchmarks.md` to `explanation/`; `configuration.md`, `venues.md`,
  `sim-exchange.md`, `options.md` and the codec pages to `reference/`; `monitoring.md` to
  `how-to/operations/monitor-with-fastmm-top.md`; `dependencies.md` to `contributing/`;
  `adding-a-strategy.md` became `reference/strategy-api.md`. Moved pages leave no pointer.
- New pages: install, quick start, the nine-page tutorial "Your first market maker" (through the
  simulated exchange and Binance Demo), reference pages for the strategy API, public API and header
  tiers, command lines, journal format, status file and a glossary, explanation pages for event
  flow, determinism and the risk model, and a guide to writing docs. The README's strategy example
  is now the quick start's header.
- The configuration reference's key tables are generated from `schema.hpp`, whose doc strings now
  give units and defaults. The shipped-config test fails on load warnings.
  `venues/raw_recorder.hpp` includes `<cstdint>`, so it compiles on its own.
- `docs/how-to/venues/add-a-venue.md` replaces `docs/adding-a-venue.md`, which pointed to a
  registration directory and X-macro that do not exist and gave outdated concept signatures. The
  new guide follows the Binance, Bybit and Deribit connectors: threading contract, file layout,
  `MarketDataFeed` and `ParseStatus`, book sync traits, order entry (the `OrderGateway` concept is
  not used by the shipped encoders), error actions, reconciliation, registration in
  `venue_factory.cpp` and `schema.hpp`, fixtures, tests, and a conformance checklist linked to the
  tests that prove each item.
- Operator how-tos in `docs/how-to/operations/`: running on Binance Demo and the Binance, Bybit and
  Deribit testnets; a go-live checklist; the kill switch (what trips it, the flag bits, the
  shutdown sequence and `cancel_all ok|FAILED`); journals, replay and PnL reconciliation with a
  Binance Demo example (0 fills at 15 bps from the mid, 1640 maker fills at the touch with engine
  and account PnL agreeing to 0.003 USDT); and troubleshooting keyed by the exact log messages.
- README: Deribit ships today; links to the venue guide and the operator how-tos.
- `docs/configuration.md`: the accepted `kind` values, and the kill-switch row describes the real
  shutdown sequence instead of "exit after acks or 5 s".

## [0.1.0] - 2026-09-14

### Added
- Repository skeleton: CMake + CPM with pinned dependencies, presets (debug, release, release-native,
  asan, tsan, coverage, clang, python), GitHub Actions CI, clang-format / clang-tidy, pre-commit.
- `fastmm::core`: fixed-point `Price`/`Qty`/`Notional`, TSC clock, lock-free SPSC rings, `.fmj`
  journal, async logger, latency histograms, L2 and L3 order books with snapshot/delta sync, OMS,
  risk engine, quote manager, `Engine<Strategy, Clock, Transport, Feed>`, BasicMM and
  Avellaneda-Stoikov strategies, TOML configuration with env-var secrets.
- `fastmm::net`: epoll reactor, non-blocking TCP, OpenSSL TLS over BIO pairs, WebSocket client and
  server, HTTP/1.1 client and server, reconnecting connection state machine.
- `fastmm::sim`: price-time matching engine, seeded latency model, queue-position fill model,
  synthetic market generator, simulated transport and driver, journal replay with an outbound hash.
- `fastmm::backtest`: journal, CSV, numpy-array and synthetic data sources, fees, PnL and metrics,
  parameter sweeps; `fastmm-backtest` and `fastmm-replay` apps; golden replay fixture.
- Python research bindings (`pip install -e .`): `run_backtest` and `sweep` with the GIL released,
  zero-copy numpy inputs and result columns, pandas conversion, an `OrderBook` research class,
  strategy parameter schemas, `enable_logging` for the C++ logger, and quickstart / sweep examples.
- `fastmm::venues`: Binance Spot and Bybit v5 connectors (snapshot and delta book sync, WebSocket
  order entry with REST fallback, HMAC and Ed25519 auth, reconciliation, rate limiting) with
  fixtures recorded from both testnets and scripted in-process fake exchanges; `fastmm-live` with
  dry-run, raw recording, journaling and a kill switch that cancels everything on shutdown.
- `fastmm-sim-exchange`: a Binance-compatible simulated exchange (REST, combined market-data
  streams, WS API order entry, user data stream, legacy listenKey) over TCP and TLS, driven by the
  matching engine and a seeded market generator, with signature, `recvWindow` and rate-limit checks
  and one-shot or counted fault injection (dropped connections, skipped depth updates, `-1021`,
  unresponsive REST, delayed acks, rejects, 429s). Conformance and end-to-end tests run
  `fastmm-live` against it; `scripts/run-sim.sh` and `docker compose up` start both.
- Connector-specific venue keys (`stale_ms`, `order_api`, `depth`, ...) are validated and documented
  instead of being reported as unknown.
- The strategy registry keeps one factory per transport kind, so backtest and live registrations of
  the same strategy coexist.
- Periodic TSC recalibration in `fastmm-live` (`[engine] tsc_recalibrate_s`, default 10 s): the main
  thread measures and publishes calibrations through a seqlock, and the engine's clock re-anchors
  without going backwards (it steps, counts and logs when it was more than 1 ms off).
  `ControlCommand::RecalibrateTsc` applies a published calibration immediately. Measured offsets are
  slewed away over one period (at most 500 ppm) rather than accumulated until the clock has to step.
- Network-thread order latency for Binance and Bybit: encode, send and receive-to-wire
  tick-to-trade per venue in `VenueStatus`, printed by `fastmm-live` in the stats line and the
  final summary.
- `fastmm-live` registers its strategies as `TransportKind::Live` factories in the strategy
  registry; `--list-strategies` lists what supports live trading.
- Simulated venue rejects are broken down by cause (post-only cross, level table full, invalid,
  duplicate, other).
- Benchmarks for books, rings, OMS, risk, quote manager, logger, journal, WebSocket, HTTP, matching
  and tick-to-order, with p50 budgets checked by `tools/check_budgets.py`.
- Documentation: architecture overview, ADR 0001-0011, configuration reference, strategy and venue
  guides.

### Fixed
- `Logger::instance()` wired its implementation with an unsynchronised null check, so threads
  that logged for the first time at the same moment (parallel sweep workers warming up their
  engines) raced on it and on the ring table. It now relies on thread-safe static
  initialisation, which also destroys the logger before the state its destructor uses.
- A quote slot could stop quoting for good after a connection loss. After a replace the quote
  manager still remembered the order's original id, so the terminal update of a later cancel was
  dropped and the slot waited for it forever. Slots now recognise their order by handle, instrument
  and tag, and a slot without a live order never waits. Found on a 2-core CI runner, where the
  market-data end-to-end test stopped quoting after the reconnect.
- Orders still pending when quotes were pulled (a New or a replace awaiting its ack) stayed on the
  book once acknowledged. They are now cancelled as soon as they become working.
- The quote manager could take a stale handle for the order that later reused its slot and
  replace or cancel that order.
- Avellaneda-Stoikov never requoted on the first book update after a reconnect: its variance
  update wrote the mid back into the field that gates requotes. The two are separate now.
- Several sources used `<algorithm>` and `<cstdio>` functions without including those headers.
  They compiled against libstdc++ 13 through transitive includes but not against libstdc++ 14, which
  clang picks up on the GitHub ubuntu-24.04 image.
- CI sized for the private-repository runner (2 vCPUs): builds and tests use the core count, the
  build job limit is 60 minutes, and integration tests run serially.
- The clang-tsan build did not link: the hot-path allocation harness replaces global `operator new`,
  which the TSan runtime also defines. The harness is no longer built under TSan.
- Under TSan every concurrent `Seqlocked` store and load was reported as a data race. The optimistic
  copy is racy by design, so TSan builds now serialise it with a spinlock; other builds are unchanged.
- `scripts/tidy.sh` also analysed dependency sources in the CPM cache, whose paths contain `src/`.
- The seqlock torn-write test failed on one core or under a loaded `ctest -j`, because the writer
  could finish before the reader was first scheduled.
- TSC recalibration re-measured the rate over a 50 ms window every time. On a virtualised host the
  resulting rate noise (hundreds to thousands of ppm) drifted the engine clock by milliseconds
  between recalibrations and stepped it every 10 s. The rate is now measured over the whole
  interval since the previous anchor against `CLOCK_MONOTONIC_RAW`, and host wall-clock steps are
  reported separately.
- A requested shutdown logged a spurious "cancel-all failed" error: closing the order channel
  started an asynchronous cancel-all that the disconnect then aborted. It is skipped during an
  intentional disconnect, and a kill switch requested by the control thread logs a warning
  instead of an error.
- Binance and Bybit connectors did not reconcile open orders after the order channel reconnected,
  and the user/private streams re-queried open orders every time a quiet stream returned from
  Stale. Reconciliation now runs exactly on real reconnects.
- After a reconnect BasicMM and Avellaneda-Stoikov stayed unquoted until the mid moved past the
  requote threshold; they now requote as soon as the venue is Live again.
- `fastmm-live` lost the PnL and fee figures of its final summary to the logger's string-argument
  cap; the summary is logged as numbers, and truncated string arguments now end in `...`.
- Order book sync resynced needlessly on Binance when no buffered delta was newer than the REST
  snapshot: the first live delta was checked with the "follows exactly" rule and an overlapping
  delta (U <= L+1 <= u) looked like a gap. It is now checked with the first-delta rule.
- Stack buffer overflow when a `BookDeltaMsg` was built by value: its `hdr.len` is longer than the
  struct. Messages are now always built in correctly sized buffers.
- Division by zero in the L3 book benchmark on longer runs.
- Logger ring slots were never released by exiting threads, so repeated sweeps eventually dropped
  all log records.
- BasicMM could skew a post-only quote through the market and have it rejected; quotes are now
  kept one tick inside the touch.
- Replay hashes included latency stamps and differed between the original run and the replay.
- Recorded-data fills applied level increases before decreases within one update.
- The example backtest capped inventory at one quote size, so one side stopped quoting whenever
  the strategy held inventory (0.3% quote uptime); it now allows five and keeps both sides quoted.
- Examples and sweep tables no longer headline an annualised Sharpe computed from minutes of
  synthetic data; they show the per-bar Sharpe and split PnL into spread and maker rebates.
- sim, backtest and bindings build cleanly under clang `-Werror` and clang-tidy.
- CI: invalid workflow YAML, TSan on high-entropy ASLR kernels, clang `-Werror` failures, clang-tidy
  errors, redundant `pip install cmake`, oversized Docker build context, bootstrap treating CMake 4
  as too old.
