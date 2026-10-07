# Changelog

All notable changes are recorded here (Keep a Changelog format).

## [0.5.1] - 2026-10-07

### Fixed
- init: the generated README runs `pip` and `python` from the active environment instead of `.venv/bin/`.
- venues: a warm standby (`fastmm-live --standby`) started early in a rate-limit minute gets its
  depth snapshots before the handoff instead of waiting about 15 s.

## [0.5.0] - 2026-10-07

`fastmm-live --dry-run` now quotes on paper, the release tarball runs on any Linux with glibc 2.28
or newer, and `fastmm init` writes a live config next to the backtest. Python 3.9 is no longer
supported.

### Breaking changes
- python: `fastmm-engine` requires Python 3.10 or newer; no cp39 wheels are built.
- live: a dry run runs as engine `<name>-dryrun`, with its own status file, control socket and
  journal. `--dry-run` with `--standby` or `--takeover` exits with status 2.

### Added
- live: `--dry-run` quotes on paper: orders are acknowledged locally and never sent, and the log
  shows what would have been sent per instrument every 5 s.
- init: `fastmm init` also writes `live.toml` (BTCUSDT on Binance Spot Demo Mode), and the starter
  `backtest.py` takes `--config` and `--data` to replay a recorded journal.
- backtest: a run stopped by the kill switch reports why and when (`kill_reason`, `kill_at_s` in
  `summary.json` and `BacktestResult.stats()`).
- hot: the `@fastmm.hot` check names each allocation and its line, e.g. `it creates a dict at strategy.py:21`.
- logging: `[logging] status_interval_s` sets how often the per-venue status line is printed.

### Changed
- packaging: the release tarball is built in `manylinux_2_28` with OpenSSL and libstdc++ linked
  statically; it is tested on Ubuntu 22.04 and 24.04, Debian 12, Rocky Linux 9 and Amazon Linux 2023.
- packaging: the tarball includes `fastmm-sim-exchange` and its configs, and its README starts with
  the simulated exchange.
- live: a dry run no longer reads or writes the keyed session's store, kill ledger, state file or
  instance lock.
- live: a planned stop logs `stopping: pulling quotes and cancelling all` at INFO instead of a
  `kill switch requested` warning.
- logging: the status line is printed every 10 s on stderr; a log file still gets it every second.
- init: the starter quotes 0.0001 BTC per side instead of 0.002, so its default backtest no longer
  trips `[risk] max_loss`.
- examples: `examples/quickstart` fetches the release tag instead of `main`.

### Fixed
- logging: `[logging] file` is now opened; it was parsed and ignored.
- logging: log files are flushed after each burst, so `tail -f` follows the session.
- build: `scripts/bootstrap.sh` honours `$CXX` and finds versioned compilers (`g++-13`, `clang++-16` and later).
- python: `BacktestConfig.from_toml` no longer fails on unset `${VAR}` in `[venues.*]`.

### Documentation
- The quick start creates a virtual environment first, as Debian and Ubuntu require (PEP 668).
- Register a strategy installs FastMM as an SDK build without tests, benchmarks and examples.

## [0.4.0] - 2026-10-06

This release adds account pools: several accounts of one exchange behind one venue, with order
routing, a shared rate limiter, a treasury that balances them by internal transfer and a hot
handoff between processes. It also adds the Gate USDT perpetual futures connector, a fill audit
against the venue's trade history and more realistic fills and latency in the simulator.

### Breaking changes
- sim: a trade through a resting order fills it with the print's quantity, not the whole order, and
  one print is shared across our orders in price priority; backtest fills and golden hashes change.
- venues: `RateLimiter::weight_bucket` and `order_bucket` return `std::optional<RateBucket>`.
- live: the status file is at version 18 (fill audit and treasury counters).
- binance_usdm: an account in hedge mode exits with status 3 instead of 4.

### Added
- venues: account pools: `[venues.<member>] pool_of = "<primary>"` puts up to 8 accounts of one
  exchange behind one venue; orders go to the account with room and the most order window left.
- engine: pool-aware strategy API: `.account(venue)`, `ctx.pool()`, `ctx.account_usable()`,
  per-account `ctx.balance()`, `ctx.order_budget()` and `balance_room()`.
- live: `fastmm-live --takeover` and `--standby`, `fastmm-ctl handoff` and `[engine] instance_lock`
  hand a running session over to a new process.
- live: a warm standby on Binance Spot runs market data and its strategy while the old session
  trades, so the handoff pause is one reconciliation, not a restart.
- live: pool treasury (`[venues.<primary>.treasury]`) keeps an asset spread over a pool's accounts
  by internal transfers, with limits, a crash-safe ledger and metrics.
- venues: `Venue::transfer` and related calls; Binance Spot and USDⓈ-M transfer between
  sub-accounts with a separate transfer-only master key.
- venues: `BalanceMsg::withdrawable`; Binance USDⓈ-M reports `maxWithdrawAmount`, and the
  treasury never moves more than that.
- backtest: the treasury runs on simulated time against the pool's simulated accounts
  (`[backtest] transfer_latency_ms`, `transfers.csv`).
- gate: Gate USDT perpetual futures connector (`kind = "gate_usdt"`): order book, trades, funding,
  WebSocket order entry with REST fallback, reconciliation and a dead man's switch.
- venues: `[venues.<name>] source_ip` / `source_interface` bind every connection of a venue to a
  local address, so pooled accounts can leave from different IPs.
- venues: `[venues.<name>] public_only = true` makes a venue market-data only, with no keys and
  nothing sent or cancelled.
- live: fill audit (`[venues.<name>] fill_audit_interval_s`) compares stored fills with the
  venue's trade history and can book missing ones (Binance Spot and USDⓈ-M).
- pnl: `fastmm-pnl audit --exchange <file>` compares the store's fills with an exported trade
  history and exits 5 on any difference.
- binance: `[venues.<name>] order_rate_threshold` (default 0.9) caps the share of the 10 s and
  daily order-count limits the connector uses.
- binance_usdm: `md_ticker_conns` opens extra bookTicker connections and keeps the first copy of
  each update.
- binance_usdm: `one_way_mode = true` and `leverage = <n>` set the account up at start.
- engine: `on_batch_end(ctx)` hook, requested with `ctx.request_batch_end()`, runs once after a
  burst of events.
- engine: quotes short of a `[risk] orders_per_sec` token are held and placed as the bucket refills
  instead of being rejected; `[engine] quote_token_reserve` keeps tokens for other orders.
- engine: `[engine] feed_budget_per_ring` sets how many events are taken from one input ring
  before the next.
- engine: `[engine] net_spin_dedicated` lets network threads on dedicated cores busy-poll in
  adaptive spin mode.
- engine: `[engine] rt_priority` and `net_rt_priority` run the engine and network threads under
  `SCHED_FIFO`.
- engine: `[engine] cpu_dma_latency_us` holds a CPU latency request for the session;
  `scripts/host-setup.sh cstates` limits idle states per CPU.
- engine: `[engine] log_irq_affinity` logs NIC queue interrupt placement at start;
  `scripts/host-setup.sh irq-affinity` spreads them.
- journal: sessions record restored strategy state and optional periodic snapshots
  (`[strategy] state_snapshot_interval_s`); `fastmm-replay` replays restored sessions exactly.
- backtest: `[backtest] initial_state = "journal"` and `params_from_journal = true` start a backtest
  from a live session's state and parameter updates.
- backtest: `journal:x.fmj` reads all parts of a session split by `journal_max_bytes` as one stream.
- backtest: `journal:…,remap=1` maps a recording's instruments to the configuration's by symbol.
- backtest: `[backtest] order_service_us` and `latency_cancel_us` model per-connection order
  processing and a separate cancel latency.
- backtest: `[backtest] orders_10s` / `orders_1d` simulate Binance's order-count limits.
- data: `fastmm-data calibrate` fits cancel latency, order service time and latency jitter from
  journals, and `--from-live-state` compares runs started from live state.
- data: `fastmm-data fill-check` breaks down the live fills the `l2_queue` model misses, by
  markout, queue position and how the fill printed.
- sim: `SimExchangeServer::accepted_client_order_ids()`.

### Changed
- engine: a strategy can declare up to 128 parameters (was 64); the journal layout is unchanged.
- binance: the periodic execution replay queries only symbols with recent order activity.
- binance_usdm: a hedge-mode error names the setting and `one_way_mode`; mixed settlement currencies
  without `[accounting]` print the lines to add.
- gate: a base coin label longer than the instrument field is shortened with a warning.

### Fixed
- binance: signed REST requests are signed when written, not when queued, so they no longer fail
  with -1021 behind an execution sweep.
- binance: pooled accounts share one request-weight window per IP, so a pool starting together is
  no longer banned.
- venues: a pooled account whose order window is full gets no automatic orders, and quotes on one
  account are no longer sized to another's room.
- engine: a fill that arrives after its order was cancelled now marks the order `Filled` and
  updates the store.
- sim: fees are charged on the contract's notional including its multiplier.
- journal: a journal with more parameters than the reader supports opens, and every refusal says
  which check failed.
- backtest: `--data "a;b"` no longer corrupts memory on depth snapshots deeper than 256 levels.

### Documentation
- The README is rewritten around the engine's features, and a new page, How fast it is, shows the
  latency and load of a production session.

## [0.3.0] - 2026-10-03

FastMM 0.3.0 adds connectors for OKX perpetuals and both Coinbase APIs, cross-venue hedging
(`HedgeExecutor` and the `xmm` strategy), balance, margin and perpetual mark/funding tracking, and
exact fill recovery after a restart on Binance and Bybit. Unknown configuration keys are now errors.

### Breaking changes
- config: an unknown key or section is an error with its line number instead of a warning; this
  covers `[venues.*]`, `[backtest]` and `[strategy.params]` too, and `BacktestConfig.from_toml` raises `ConfigError`.
- config: a venue with `stale_ms` above `dead_ms`, or `stale_ms = 0`, is refused at start-up.
- net: the user-space TCP client is removed; `order_transport = "user_tcp"` is refused at start
  and its `user_tcp_*` keys are unknown. OUCH runs on kernel TCP (use OpenOnload or XLIO for bypass).
- strategies: `lead_mm` no longer has the `own_in_feed` parameter.
- core: `sim::FeeModel` is renamed `FeeTable` (`core/fees.hpp`); `validate_venues` no longer takes a warnings argument.
- build: `FASTMM_BUILD_NET=ON` needs `make` (liburing is built from source); zlib is no longer needed.
- gateway: the attach protocol is version 7; strategies and `fastmm-gateway` must be upgraded together.

### Added
- okx: OKX v5 connector for USDT-margined perpetual swaps, with WebSocket order entry, fill
  replay, funding and `cancel-all-after` (`configs/okx-demo.toml`).
- coinbase: Coinbase Advanced Trade connector (`kind = "coinbase_advanced"`, spot, CDP keys).
- coinbase: Coinbase Exchange connector (`kind = "coinbase_exchange"`, spot, sandbox).
- strategies: `xmm`, a built-in strategy that quotes one venue and hedges with IOC orders on
  another (`configs/xmm-demo.toml`).
- strategies: `HedgeExecutor`, a reusable component that hedges positions across several hedge
  instruments with failover, halt and optional de-risking (`examples/cpp/hedged_mm.cpp`).
- engine: perpetual mark, index, funding and open interest from Binance USDⓈ-M, OKX, Bybit,
  Deribit and Gemini (`ctx.mark`, `ctx.funding`, `on_perp_state`); `[accounting] mark` values
  positions at the venue's mark.
- engine: balances, margin and collateral from every connector (`ctx.balance`, `ctx.margin`,
  `on_balance`); `[risk] check_balance` (default on) refuses orders the balance cannot cover.
- engine: strategy state persists across sessions with `state()` / `restore()` and
  `[strategy] state_file` / `state_interval_s`, in live trading and backtests.
- engine: `on_risk_reject` reports orders refused by `[risk]`, and `ctx.order_budget(venue)`
  shows the remaining order and request-weight budget.
- engine: `ctx.fees`, `ctx.risk_headroom` and `ctx.venue_health` expose fee rates, remaining risk
  room and feed lag / ack round trip.
- engine: execution view for strategies: `ctx.own_qty`, `ctx.best_ex_self`, `ctx.queue_ahead` and `ctx.order_times`.
- risk: `[risk.underlying.<BASE>] max_net` and `[gateway.underlying.<BASE>] max_net` limit the net
  position in one base asset across instruments and venues (`fastmm-ctl limits underlying.BTC.max_net=...`).
- risk: `[risk] max_feed_lag_ms` pulls quotes while a venue's market data lags.
- gateway: `[gateway.shared."<venue>:<symbol>"]` lets several strategies trade one instrument,
  with per-strategy attribution and self-trade prevention.
- engine: `[accounting]` supports several settlement currencies, converted to `reporting_currency`
  through `[accounting.fx]` instruments.
- venues: venue-side dead man's switch on Binance USDⓈ-M (`dead_mans_switch_ms`, on by default)
  and Bybit (`dead_mans_switch_s`, off by default); a lost Binance countdown kills the venue.
- venues: reconciliation replays executions from the venue's trade history on Binance Spot,
  Binance USDⓈ-M and Bybit, so fills made during an outage are booked and a restart restores the position.
- binance: `amend_keep_priority` reduces an order's quantity in place and keeps its queue position.
- binance: `post_only_rpi = true` on USDⓈ-M sends RPI post-only orders; `TRADIFI_PERPETUAL` contracts are accepted.
- binance: `fetch_fees = true` on Spot reads the account's commission rates at start-up.
- venues: `api_passphrase` credential for venues that need one (OKX, Coinbase Exchange).
- venues: a book checksum mismatch resyncs the book.
- backtest: `fastmm.walk_forward` / `bt::walk_forward` run walk-forward sweeps over K time folds.
- backtest: `[backtest] md_arrival = "recorded"` replays market data at its recorded receive time.
- data: `fastmm-data calibrate` fits the `l2_queue` fill model and simulated latencies to live
  journals and prints a `[backtest]` snippet.
- net: the WebSocket client and server pass the Autobahn RFC 6455 test suite (`scripts/autobahn.sh`).

### Changed
- engine: a strategy can declare up to 64 parameters (was 32); one past the limit refuses the strategy.
- cli: all programs parse their command lines with CLI11; flags and exit codes are unchanged, numbers
  are validated, and `fastmm-ctl` and `fastmm-top` gain `--version`.
- backtest: the `l2_queue` queue model caps queue position with the book ticker and the trade tape;
  `queue_conservatism` defaults to `[engine] queue_conservatism`.
- strategies: `xmm` does not hedge on a venue that is killed, feed-lagged or stale.
- strategies: `HedgeExecutor` sizes hedges to the remaining `max_gross_notional` and `max_net_notional`.
- store: `fastmm-pnl duplicates` and the start-up duplicate check are about 10x faster (schema version 7).
- net: the io_uring backend uses liburing 2.15.

### Fixed
- binance: starting with many symbols no longer exceeds the request-weight limit and gets the IP banned.
- binance: combined-stream URLs with more than about 14 symbols now connect.
- binance: USDⓈ-M no longer charges request weight for WebSocket order placement, which halved the quoting budget.
- binance: books no longer resync right after connecting because of an early depth snapshot.
- binance: `cancel_all()` retries after a 418/429 instead of giving up.
- binance: USDⓈ-M history queries respect the minute's weight and `Retry-After`; a 418 stops REST.
- venues: a requote is no longer refused `BalanceShort` after a cancel ack; order and cancel acks
  carry the venue's time on all connectors.
- store: fills on different venues, symbols or sides with the same execution id are no longer dropped (schema version 5).
- store: a restart after consecutive crashes no longer books executions twice.
- store: a session killed during its start-up replay no longer loses fills on the next start (schema version 6).
- engine: an order's fill reported both by cumulative quantity and by execution is no longer booked twice.
- strategies: `HedgeExecutor` and `xmm` split hedges larger than a `[risk]` per-order limit instead of having them refused.
- gateway: after a restart, fills made while it was down reach the strategy that owns the order.
- gateway: orders of several strategies no longer fill the connector's order table.
- okx: funding bills with more than 8 decimals are booked.
- venues: a quiet channel returning to Live no longer logs a state change every 20 s.
- engine: `[accounting.fx]` rates from a quiet book no longer go stale; `[accounting] stale_fx_ms` sets their age limit.
- data: `fastmm-data fill-check` keeps fills reported after their order ended.
- net: invalid UTF-8 text and malformed close frames are rejected with close code 1007 or 1002.

## [0.2.0] - 2026-09-23

FastMM 0.2.0 adds US equities through Nasdaq TotalView-ITCH and OUCH with kernel-bypass receive
(AF_XDP and DPDK), Python strategies that run in backtests and live sessions, and a control plane
for operating a running session. It also adds Binance USDⓈ-M and Deribit connectors, a queryable
trading record, backtests on public Binance and Tardis data, and an HTML run report. Strategy
registration, hook signatures and the journal format change; see Breaking changes.

Highlights:

- Nasdaq ITCH 5.0 / OUCH 5.0 venue with A/B arbitration, GLIMPSE recovery and a matching simulator.
- Python strategies: Numba-compiled `@fastmm.hot` hooks plus slow methods, in backtests and live.
- `fastmm-ctl` to pull, resume, retune, flatten or kill a running session.
- Run-to-completion threading (`[engine] threading = "single"`) and coalesced order sends.
- A SQLite trading record per session, queried with `fastmm-pnl` or `fastmm.open_store()`.
- `fastmm report` writes a run as one self-contained HTML page.

### Breaking changes
- engine: a strategy library exports one registration function calling
  `fastmm::register_strategy<S>(r)`; `StrategyRegistry::add` is replaced by `try_add`.
- engine: `on_trade`, `on_book_ticker` and `on_option_ticker` take `(ctx, InstrumentId, const Msg&)`;
  `on_fill` takes `(ctx, const Fill&)`; `set_quotes` returns `bool`.
- engine: `ParamDesc::parse` and `format` replace `set` and `get`; `basic_mm` bps parameters keep
  four decimals, which changes its golden hashes.
- engine: built-in strategies move to the `fastmm::strategies` library; `make_engine_runner`,
  `EngineConfig` and `LiveBackend` moved, and the install exports components.
- venues: venues resolve through `VenueRegistry`; `VenueKind` and `venue_factory.hpp` are gone.
- venues: connector keys are validated per venue; a key of the wrong type stops the session.
- live: `[engine] on_kill` defaults to `"exit"`: a kill switch the session did not ask for ends it
  with exit code 6 (5 if cancel-all failed). Set `"stay"` for the old behaviour.
- journal: format version 3 (readers open 1 to 3); `fastmm-replay` refuses unclosed or damaged
  journals unless given `--allow-incomplete`.
- top: status segment version 5; `fastmm-top` and `fastmm-live` must come from the same build.
- backtest: shipped configs charge Binance spot VIP 0 fees (10 bps a side) instead of a maker
  rebate, and each instrument pays its own venue's fees.
- codecs: `moldudp::Receiver::on_packet` takes `(line, datagram, now_ns, meta)`; `L3Book` is no
  longer a template and takes an `L3BookConfig`.
- python: the PyPI distributions are `fastmm-engine` and `fastmm-engine-live`; import names are unchanged.

### Added
- venues: Nasdaq TotalView-ITCH 5.0 venue (`kind = "nasdaq_itch"`) on lines A and B with MoldUDP64
  arbitration, re-requests, GLIMPSE snapshot recovery and OUCH 5.0 order entry over SoupBinTCP.
- net: multicast receive backends `rx_backend = "kernel" | "af_xdp" | "dpdk"`; AF_XDP loads its own
  BPF program without libbpf, DPDK needs `-DFASTMM_WITH_DPDK=ON` and `spin_mode = "busy"`.
- net: unicast market-data lines on all three backends, for networks without multicast.
- net: `order_transport = "user_tcp"` (experimental), a user-space TCP client for OUCH over
  `AF_PACKET`, AF_XDP or DPDK, with a DPDK kernel exception path (`dpdk_exception_port`).
- sim: `fastmm-sim-itch`, a Nasdaq-style ITCH/MoldUDP64 publisher with seeded drops, re-requests,
  GLIMPSE and OUCH 5.0 order entry, timing orders wire to wire.
- codecs: `fastmm::codecs` with FIX 4.4, ITCH 5.0, MoldUDP64, SoupBinTCP, OUCH 4.2/5.0 and CME MDP 3.0.
- engine: `[engine] threading = "single"` runs the venue reactor, engine and order sends on one
  thread with no ring hop; one venue only.
- engine: `[engine] timer_slack_ns`, `lock_memory`, `net_backend = "io_uring"`,
  `reject_backoff_ms`, and `configs/profiles/production-latency.toml` for a dedicated host.
- engine: `[engine] ack_timeout_ms` force-cancels orders whose ack never arrives and frees their
  slots and exposure.
- risk: `[engine] kill_file` latches a `max_loss` trip and carries cumulative PnL across restarts;
  a latched start exits 6 until `fastmm-live --clear-kill`, and SIGHUP clears a running session.
- risk: per-venue kill switch: an unusable venue pulls its quotes and refuses orders while the
  others trade; `KillReason` records why.
- ctl: `fastmm-ctl` over a per-session Unix socket: `pull`, `resume`, `param`, `limits`, `flatten`,
  `kill`, `unkill`, `stop` and `status`, scoped to the session, a venue or an instrument.
- ctl: `fastmm-ctl flatten` closes positions with reduce-only IOC slices within
  `--max-slippage-bps`, without involving the strategy.
- top: `fastmm-top` terminal dashboard over `/dev/shm/fastmm-<engine>.status`, with `--json` and
  `--metrics` for a Prometheus endpoint; rejects are counted per `RejectReason`.
- python: Python strategies in backtests: subclass `fastmm.Strategy`, mark hooks `@fastmm.hot`
  (compiled by Numba, extra `fastmm-engine[hot]`) and add `on_start`, `on_stop` and
  `@fastmm.every` slow methods.
- python: `fastmm.run_live()` and `python -m fastmm run module:Class` run a Python strategy live;
  a failing hook or slow method stops the session with exit code 7.
- python: `fastmm.replay(journal, MyMM)` replays a hot strategy from a journal's parameter updates.
- python: `fastmm-engine-live` wheel (`pip install "fastmm-engine[live]"`) with the network stack,
  connectors and static OpenSSL.
- engine: runtime parameter updates (`ParamUpdate` events, `on_params` hook,
  `[strategy] max_param_age_ms`), `fastmm-live --strategy` and `--param key=value`.
- cli: `--list-strategies --format json` on `fastmm-live` and `fastmm-backtest`.
- engine: strategy API additions: `Ratio`, exact literals (`100.25_px`, `5_bps`), `quoting.hpp`
  helpers, `verify_strategy<S>()`, `on_quoting`, `every`/`once` timers and `StrategyHarness<S>`.
- core: checked conversions `Fixed::from_double_checked()`, `Ratio::from_bps_checked()` and
  `checked_add`/`sub`/`mul`.
- deribit: Deribit connector (`kind = "deribit"`) for options and futures, with Black-76 pricing,
  greeks, an implied-vol solver and the `options_mm` strategy.
- binance: Binance USDⓈ-M perpetual futures connector (`kind = "binance_usdm"`) with WebSocket API
  orders, a REST fallback and position reconciliation; hedge mode is refused.
- binance: Spot SBE market data (`md_format = "sbe"`), decoding depth in 51 ns against 680 ns for JSON.
- binance: Ed25519 keys via `private_key_env` with `session.logon` on both Binance connectors.
- core: inverse (coin-margined) contracts booked in their settlement coin.
- backtest: fill markouts at `[backtest] markout_horizons_s` (1, 10 and 60 s) and fill-quality
  stats in the summary, `summary.json`, `fills.csv` and `BacktestResult.markouts()`.
- backtest: per-instrument `maker_bps` and `taker_bps` overrides.
- backtest: public market data: `--data binance:BTCUSDT,2024-03-27` and
  `tardis:binance-futures,BTCUSDT,2026-09-01`, fetched with `python3 -m fastmm.data fetch` and
  packed to `.fmj` with `fastmm-data convert`.
- python: `fastmm.data_sources()` and `fastmm.convert_data()`; `fastmm.data` is now a package.
- backtest: `fastmm report` writes a run as one offline HTML page (equity, PnL decomposition,
  markouts, fills, config); also `fastmm.write_report()`.
- journal: a SQLite trading record per session (`[storage]`, pluggable backends) holding fills,
  orders, positions, daily PnL and kill events.
- cli: `fastmm-pnl` queries the record (`sessions`, `fills`, `orders`, `pnl`, `positions`, `recover`).
- python: `fastmm.open_store(path)` returns the record as pandas DataFrames.
- live: `fastmm-live` logs what the previous session of the same name left behind (PnL, kill state,
  positions, open orders) before it starts.
- journal: `[engine] journal_sync`, `journal_max_bytes` (rollover) and `journal_retention_days`;
  a journal write error trips the kill switch and exits 5.
- research: `fastmm::research` feature and forward-markout extractor with `evaluate_signal()`
  (rank IC, decile table, touch markout); `fastmm.features()` and `fastmm.evaluate_signal()` in Python.
- tools: `tools/pnl_report.py`, `scripts/bench-e2e.sh`, `scripts/bench-2host.sh`,
  `scripts/host-setup.sh`, `scripts/xdp-test.sh`, `scripts/build-pgo.sh` (PGO and BOLT).
- packaging: `scripts/package-release.sh` and the `release-dpdk` preset build a portable tarball.
- examples: `examples/quickstart/`, `examples/cpp/tutorial/`, `examples/external-project/` and
  `examples/external-venue/`.

### Changed
- codecs: FIX 4.4 and CME MDP 3.0 are off by default (`-DFASTMM_CODEC_FIX=ON`, `-DFASTMM_CODEC_MDP3=ON`).
- venues: a connector registers itself in `VenueRegistry` with its keys and capabilities, so an
  out-of-tree venue needs no engine changes.
- engine: order sends are coalesced into one system call per drain: wire-to-wire p50 down from
  47-55 µs to 30-35 µs over veth.
- engine: hot-path speedups: OUCH 5.0 encode 4.4 µs to 0.1 µs, `BM_EngineStep_Sim` 9.2 µs to 2.9 µs,
  Binance `order.place` encode 1440 ns to 523 ns, ITCH bridge 84 ns to 55 ns per message.
- codecs: MoldUDP64 packets ahead of a gap are buffered (`reorder_packets`) instead of dropped;
  gaps are declared after `gap_timeout_ns`.
- codecs: `L3Book` orders outside the price window go to an overflow store instead of failing.
- engine: a venue's unreported `cum_qty` is booked as a synthetic fill (`OmsUpdate::missed_qty`).
- engine: a cancel ack for a fully filled order ends it as `Filled`.
- engine: `OmsUpdate::replaced_cl_ord_id` reports the id a cancel-replace superseded.
- live: `fastmm-live` refuses to start when the session epoch file cannot be written, and trips
  `OrderIdsExhausted` instead of reusing client order ids.
- risk: `fastmm-live` refuses `[risk] max_loss` when instruments settle in more than one currency.
- core: `Fixed::from_double()` saturates out-of-range values and maps NaN to zero.
- backtest: the summary shows `net = spread capture + mid drift - fees + rebates + unexplained`.
- backtest: unknown config keys are reported with their line; `sharpe_annualized` is NaN for runs
  under a day.
- python: `run_backtest` takes `params=` and a `fastmm.Strategy` subclass; `ctx.request_stop()` ends a run.
- python: wheels cover CPython 3.9 to 3.14.
- bench: benchmarks were re-measured after fixing harness artefacts; CI budgets allow 10 % instead of 25 %.
- build: `scripts/run-sim.sh` takes `--port` and `--tls-port`.

### Removed
- engine: `FASTMM_REGISTER_STRATEGY`, `StrategyContext::instrument_count()`,
  `ctx.add_timer()` (use `every` and `once`) and `sim::make_sim_or_replay_runner`.

### Fixed
- deribit: fees are booked in `fee_currency` instead of a leftover buffer byte.
- net: a failed WebSocket send is no longer counted as sent; the REST fallback now runs.
- bybit: a rate-limit or signature error during reconciliation no longer cancels every resting
  order, and open orders are paged past the first 50.
- venues: cancels and cancel-all go through after a venue-fatal error, so the kill path can clear the book.
- venues: fills without a fee or trade id are no longer deduplicated away or given the previous fee.
- engine: a duplicate Binance ack no longer leaves a replaced order live and untracked.
- engine: reconciliation no longer drops in-flight cancels or orders sent after the request, and
  quotes resume after it.
- engine: fills that arrive after the cancel ack reach positions, fees and PnL.
- core: TSC calibration no longer breaks on wall-clock steps (WSL2) or KVM hosts without `nonstop_tsc`.
- journal: live session journals replay exactly with `fastmm-replay --verify`.
- net: `af_xdp` on virtio_net receives every line.
- binance: Spot with `key_type = "ed25519"` sends `session.logon`, so the order channel goes live.
- engine: serialize latency is measured from the order call, and channels no longer flap to Live
  after a silent Stale.
- python: `hot_abi.h` is installed, so hot strategies compile against an installed tree.
- bybit: `rejectReason` values map to specific reject reasons, and positions deduct `spotBorrow`.
- tools: `tools/pnl_report.py` no longer double-counts commission charged in the base asset.

### Documentation
- The docs are reorganised into getting-started, tutorials, how-to, reference and explanation,
  served at <https://ziy.bio> with generated C++ and Python API references.
- New pages for production operation, a nine-page tutorial "Your first market maker", and
  reference pages for errors, exit codes, the journal and status formats.
- Backtesting explains markouts and what the simulator cannot tell you.

## [0.1.0] - 2026-09-14

The first release. FastMM is a C++ market-making engine with a backtester, a simulated exchange,
Binance Spot and Bybit v5 connectors, and Python bindings for research.

### Added
- core: fixed-point `Price`/`Qty`/`Notional`, TSC clock, lock-free SPSC rings, async logger,
  latency histograms and the `.fmj` journal.
- engine: L2 and L3 order books, OMS, risk engine, quote manager and `Engine<Strategy, Clock, Transport, Feed>`.
- engine: BasicMM and Avellaneda-Stoikov strategies, registered per transport kind for backtest and live.
- engine: TOML configuration with `${VAR}` secrets; connector-specific venue keys are validated.
- net: epoll reactor, non-blocking TCP, OpenSSL TLS, WebSocket and HTTP/1.1 clients and servers,
  reconnecting connections.
- sim: price-time matching engine, seeded latency and queue-position fill models, synthetic market
  generator and journal replay with an outbound hash.
- backtest: journal, CSV, numpy and synthetic data sources, fees, PnL and metrics, parameter sweeps;
  `fastmm-backtest` and `fastmm-replay`.
- python: `run_backtest` and `sweep` with the GIL released, zero-copy numpy inputs and results,
  pandas conversion and an `OrderBook` research class.
- venues: Binance Spot and Bybit v5 connectors with book sync, WebSocket order entry with REST
  fallback, HMAC and Ed25519 auth, reconciliation and rate limiting.
- live: `fastmm-live` with dry run, raw recording, journaling, a kill switch that cancels everything
  on shutdown, periodic TSC recalibration and per-venue tick-to-trade latency.
- sim: `fastmm-sim-exchange`, a Binance-compatible simulated exchange over TCP and TLS with fault
  injection; `scripts/run-sim.sh` and `docker compose up` start it with `fastmm-live`.
- build: CMake + CPM with pinned dependencies, presets (debug, release, asan, tsan, coverage, clang,
  python), CI, clang-format, clang-tidy and pre-commit.
- bench: benchmarks for books, rings, OMS, risk, logger, journal, network and tick-to-order, with
  p50 budgets checked in CI.
- docs: architecture overview, ADR 0001-0011, configuration reference, strategy and venue guides.

### Fixed
- engine: a quote slot no longer stops quoting for good after a connection loss.
- engine: orders pending when quotes are pulled are cancelled as soon as they are acknowledged.
- engine: BasicMM and Avellaneda-Stoikov requote as soon as a venue is live again after a reconnect.
- engine: BasicMM keeps post-only quotes one tick inside the touch instead of crossing.
- venues: Binance and Bybit reconcile open orders exactly once per real reconnect.
- venues: Binance book sync no longer resyncs needlessly after a REST snapshot.
- live: TSC recalibration no longer drifts or steps the clock on virtualised hosts.
- live: a requested shutdown no longer logs a spurious "cancel-all failed" error.
- live: the final summary keeps its PnL and fee figures.
- core: the logger is safe to initialise from several threads at once, and exiting threads release
  their ring slots.
- sim: replay hashes match between a run and its replay.
- examples: the example backtest keeps both sides quoted and reports per-bar Sharpe instead of an
  annualised one.
