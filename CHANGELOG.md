# Changelog

All notable changes are recorded here (Keep a Changelog format).

## [Unreleased]

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
- Simulated venue rejects are broken down by cause (post-only cross, level table full, invalid,
  duplicate, other).
- Benchmarks for books, rings, OMS, risk, quote manager, logger, journal, WebSocket, HTTP, matching
  and tick-to-order, with p50 budgets checked by `tools/check_budgets.py`.
- Documentation: architecture overview, ADR 0001-0011, configuration reference, strategy and venue
  guides.

### Fixed
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
