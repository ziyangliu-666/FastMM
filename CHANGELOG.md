# Changelog

All notable changes are recorded here (Keep a Changelog format).

## [Unreleased]

### Added
- replay: `fastmm-replay --check [--json]` lists what a replay needs, without replaying.
- store: `fastmm-pnl ledger` gives one row per execution across sessions, with its flags.
- store: `fastmm-pnl order --order <id>` shows one order's sends, refusals, fills and state.
- live: `fastmm-top` shows per side what is asked, what works and the first obstacle (v21).
- api: `ctx.note_quote` and `ctx.metric` publish the strategy's reasons and numbers live.
- store: refused orders with the limit that refused them and the budget then (schema 9).
- live: the status shows each account's order windows, refusals by limit and the risk bucket.
- store: parameter history (schema 8): `fastmm-pnl params --at`, `param-changes`, `param-diff`.
- store: `fastmm-pnl config` prints the configuration a session ran with, for a backtest of it.
- live: `fastmm-ctl param` answers `queued seq=N`; `--wait` waits for it; `params` reads them back.
- live: `param --source` names who changed a parameter, in the journal and the status file (v19).
- live: `fastmm-live` logs a topology check at start: hot threads sharing a core, and adaptive spin.
- python: `fastmm init --profile production` adds `production.toml`, tuned for a dedicated host.
- python: `fastmm sim` runs the simulated exchange; `fastmm init` writes `sim.toml` to trade it live.

### Changed
- live: the network thread sends queued orders between one socket's event and the next.
- venues: a pool's accounts take their IP's shared weight without a kernel wait on each order.

### Fixed
- core: a pool order refused because its account is full or paused takes no `[risk]` token.

### Documentation
- docs: the economics page, the tutorials and the `fastmm init` README give the numbers alone.
- docs: the README and Install name Linux x86-64 and run the quick start in Docker elsewhere.
- docs: the README and the home page open on a two-minute film; the home page lists all three.

## [0.5.1] - 2026-10-07

### Fixed
- init: the generated README runs `pip` and `python` from the active environment.
- venues: a warm standby started early in a rate-limit minute has its books before the handoff.

## [0.5.0] - 2026-10-07

`fastmm-live --dry-run` quotes on paper, the release tarball runs on any Linux with glibc 2.28 or
newer, and `fastmm init` writes a live config next to the backtest. Python 3.9 is no longer supported.

### Breaking changes
- python: `fastmm-engine` requires Python 3.10 or newer; no cp39 wheels are built.
- live: a dry run runs as engine `<name>-dryrun`, with its own status file, socket and journal.
- live: `--dry-run` with `--standby` or `--takeover` exits with status 2.

### Added
- live: `--dry-run` quotes on paper: orders are acknowledged locally and never sent.
- live: a dry run logs what it would have sent per instrument every 5 s.
- init: `fastmm init` also writes `live.toml` (BTCUSDT on Binance Spot Demo Mode).
- init: the starter `backtest.py` takes `--config` and `--data` to replay a recorded journal.
- backtest: a kill-switch stop reports `kill_reason` and `kill_at_s` in the summary and `stats()`.
- hot: the `@fastmm.hot` check names each allocation and its line.
- logging: `[logging] status_interval_s` sets how often the per-venue status line is printed.

### Changed
- packaging: the tarball is built in `manylinux_2_28` with OpenSSL and libstdc++ linked statically.
- packaging: the tarball is tested on Ubuntu 22.04/24.04, Debian 12, Rocky 9 and Amazon Linux 2023.
- packaging: the tarball ships `fastmm-sim-exchange` and its configs; its README starts with it.
- live: a dry run leaves the keyed session's store, kill ledger, state file and instance lock alone.
- live: a planned stop logs `stopping: pulling quotes and cancelling all` at INFO.
- logging: the status line is printed every 10 s on stderr; a log file still gets it every second.
- init: the starter quotes 0.0001 BTC per side; its default backtest stays under `[risk] max_loss`.
- examples: `examples/quickstart` fetches the release tag, not `main`.

### Fixed
- logging: `[logging] file` opens the log file.
- logging: log files are flushed after each burst.
- build: `scripts/bootstrap.sh` honours `$CXX` and finds `g++-13`, `clang++-16` and later.
- python: `BacktestConfig.from_toml` accepts unset `${VAR}` in `[venues.*]`.

### Documentation
- docs: the quick start creates a virtual environment first (PEP 668).
- docs: Register a strategy installs FastMM as an SDK build without tests, benchmarks and examples.

## [0.4.0] - 2026-10-06

Account pools put several accounts of one exchange behind one venue, with a shared rate limiter, a
treasury and a hot handoff between processes. The release also adds the Gate USDT perpetual
connector, a fill audit against the venue's trade history and more realistic simulator fills.

Highlights:

- Account pools: up to 8 accounts of one exchange behind one venue (Added, venues)
- `fastmm-live --takeover` and `--standby` hand a running session to a new process (Added, live)
- A pool treasury keeps an asset spread over the accounts by internal transfer (Added, live)
- Gate USDT perpetual futures connector (Added, gate)
- Fill audit against the venue's trade history, live and in `fastmm-pnl` (Added, live)
- Simulated fills take the print's quantity, not the whole order (Breaking changes, sim)

### Breaking changes
- sim: a trade through a resting order fills the print's quantity, not the whole order.
- sim: one print is shared across our orders in price priority; backtest fills and hashes change.
- venues: `RateLimiter::weight_bucket` and `order_bucket` return `std::optional<RateBucket>`.
- live: the status file is at version 18 (fill audit and treasury counters).
- binance_usdm: an account in hedge mode exits with status 3.

### Added
- venues: `pool_of = "<primary>"` pools up to 8 accounts of one exchange behind one venue.
- venues: a pooled order goes to the account with room and the most order window left.
- venues: `Venue::transfer` moves an asset between the accounts of one exchange.
- venues: `BalanceMsg::withdrawable` caps a treasury move; USDⓈ-M reports `maxWithdrawAmount`.
- venues: `source_ip` / `source_interface` bind every connection of a venue to a local address.
- venues: `public_only = true` makes a venue market-data only, with no keys and nothing sent.
- engine: `.account(venue)` on a quote level names the pool account its order goes from.
- engine: `ctx.pool()` and `ctx.account_usable()` describe a venue's pool to the strategy.
- engine: `ctx.balance()`, `ctx.order_budget()` and `balance_room()` are per account.
- engine: `on_batch_end(ctx)`, requested by `ctx.request_batch_end()`, runs once after a burst.
- engine: a quote short of a `[risk] orders_per_sec` token is held and placed as the bucket refills.
- engine: `[engine] quote_token_reserve` keeps order tokens for other orders.
- engine: `[engine] feed_budget_per_ring` sets how many events a ring yields before the next one.
- engine: `[engine] net_spin_dedicated` lets network threads on dedicated cores busy-poll.
- engine: `[engine] rt_priority` and `net_rt_priority` run the threads under `SCHED_FIFO`.
- engine: `[engine] cpu_dma_latency_us` holds a CPU latency request for the session.
- engine: `[engine] log_irq_affinity` logs NIC queue interrupt placement at start.
- live: `--takeover`, `--standby`, `fastmm-ctl handoff` and `[engine] instance_lock`: hot handoff.
- live: a warm standby on Binance Spot runs market data and its strategy while the old one trades.
- live: `[venues.<primary>.treasury]` keeps an asset spread over a pool's accounts by transfers.
- live: a treasury has transfer limits, a crash-safe ledger and metrics.
- live: `fill_audit_interval_s` audits the stored fills against the venue's trade history.
- binance: Spot and USDⓈ-M transfer between sub-accounts with a separate transfer-only master key.
- binance: the fill audit runs on Spot and USDⓈ-M and can book the fills it finds missing.
- binance: `order_rate_threshold` (default 0.9) caps the share of the 10 s and daily order limits.
- binance_usdm: `md_ticker_conns` opens extra bookTicker connections; first copy of an update wins.
- binance_usdm: `one_way_mode = true` and `leverage = <n>` set the account up at start.
- gate: Gate USDT perpetual futures connector (`kind = "gate_usdt"`) with a dead man's switch.
- pnl: `fastmm-pnl audit --exchange <file>` exits 5 when stored fills differ from exported trades.
- journal: sessions record restored strategy state and snapshots (`state_snapshot_interval_s`).
- journal: `fastmm-replay` replays restored sessions exactly.
- backtest: the treasury runs on simulated time (`[backtest] transfer_latency_ms`, `transfers.csv`).
- backtest: `initial_state = "journal"` and `params_from_journal = true` start from a live session.
- backtest: `journal:x.fmj` reads all parts of a session split by `journal_max_bytes` as one stream.
- backtest: `journal:…,remap=1` maps a recording's instruments to the configuration's by symbol.
- backtest: `order_service_us` models per-connection order processing; `latency_cancel_us` cancels.
- backtest: `[backtest] orders_10s` / `orders_1d` simulate Binance's order-count limits.
- data: `fastmm-data calibrate` fits cancel latency, order service time and jitter from journals.
- data: `fastmm-data calibrate --from-live-state` compares runs started from live state.
- data: `fastmm-data fill-check` breaks down the fills `l2_queue` misses by markout, queue, print.
- sim: `SimExchangeServer::accepted_client_order_ids()`.
- tools: `scripts/host-setup.sh cstates` limits idle states per CPU.
- tools: `scripts/host-setup.sh irq-affinity` spreads NIC queue interrupts.

### Changed
- engine: a strategy can declare up to 128 parameters (was 64); the journal layout is unchanged.
- binance: the periodic execution replay queries only symbols with recent order activity.
- binance_usdm: a hedge-mode error names the setting and `one_way_mode`.
- binance_usdm: mixed settlement currencies without `[accounting]` print the lines to add.
- gate: a base coin label longer than the instrument field is shortened with a warning.

### Fixed
- binance: a signed REST request queued behind an execution sweep no longer fails with -1021.
- binance: pooled accounts share one request-weight window per IP.
- venues: a pooled account whose order window is full gets no automatic orders.
- venues: quotes on one pooled account are no longer sized to another's room.
- engine: a fill arriving after its order was cancelled marks it `Filled` and updates the store.
- sim: fees are charged on the contract's notional including its multiplier.
- journal: a journal with more parameters than the reader supports opens.
- journal: every journal refusal says which check failed.
- backtest: `--data "a;b"` no longer corrupts memory on depth snapshots deeper than 256 levels.

### Documentation
- docs: the README is rewritten around the engine's features.
- docs: How fast it is, a new page, shows the latency and load of a production session.

## [0.3.0] - 2026-10-03

This release adds OKX and Coinbase connectors, cross-venue hedging with `HedgeExecutor` and the
`xmm` strategy, balances, margin and perpetual mark and funding for strategies, and exact fill
recovery after a restart. Unknown configuration keys are now errors.

Highlights:

- OKX perpetual swaps and both Coinbase APIs (Added, okx and coinbase)
- `HedgeExecutor` and the `xmm` strategy: quote on one venue, hedge on another (Added, strategies)
- Balances, margin and perpetual mark, index and funding reach every strategy (Added, engine)
- Strategy state persists across sessions (Added, engine)
- Fills made during an outage are booked after a restart on Binance and Bybit (Added, venues)
- An unknown configuration key is an error (Breaking changes, config)

### Breaking changes
- config: an unknown key or section is an error with its line number, `[strategy.params]` included.
- config: `BacktestConfig.from_toml` raises `ConfigError` on an unknown key.
- config: a venue with `stale_ms` above `dead_ms`, or `stale_ms = 0`, is refused at start-up.
- net: user-space TCP is removed: `order_transport = "user_tcp"` is refused; OUCH uses kernel TCP.
- strategies: `lead_mm` no longer has the `own_in_feed` parameter.
- core: `sim::FeeModel` is renamed `FeeTable` (`core/fees.hpp`).
- core: `validate_venues` no longer takes a warnings argument.
- build: `FASTMM_BUILD_NET=ON` needs `make` to build liburing from source.
- gateway: attach protocol version 7; strategies and `fastmm-gateway` must be upgraded together.

### Added
- okx: OKX v5 connector for USDT-margined perpetual swaps, with fill replay and `cancel-all-after`.
- coinbase: Coinbase Advanced Trade connector (`kind = "coinbase_advanced"`, spot, CDP keys).
- coinbase: Coinbase Exchange connector (`kind = "coinbase_exchange"`, spot, sandbox).
- strategies: `xmm` quotes one venue and hedges with IOC orders on another (`xmm-demo.toml`).
- strategies: `HedgeExecutor` hedges across several instruments with failover, halt and de-risking.
- examples: `examples/cpp/hedged_mm.cpp`, a quoter on one venue hedged on two others.
- engine: `ctx.mark`, `ctx.funding` and `on_perp_state` carry a perpetual's mark, index and funding.
- engine: `[accounting] mark` values positions at the venue's mark.
- engine: `ctx.balance`, `ctx.margin` and `on_balance`: balances and margin from every connector.
- engine: `state()` / `restore()` and `[strategy] state_file` keep strategy state across sessions.
- engine: `on_risk_reject` reports orders refused by `[risk]`.
- engine: `ctx.order_budget(venue)` shows the remaining order and request-weight budget.
- engine: `ctx.fees`, `ctx.risk_headroom` and `ctx.venue_health`: fees, risk room and feed lag.
- engine: execution view: `ctx.own_qty`, `ctx.best_ex_self`, `ctx.queue_ahead`, `ctx.order_times`.
- engine: `[accounting]` converts several settlement currencies to `reporting_currency` through FX.
- risk: `[risk] check_balance` (default on) refuses orders the balance cannot cover.
- risk: `[risk.underlying.<BASE>] max_net` limits the net position in one base asset across venues.
- risk: `[risk] max_feed_lag_ms` pulls quotes while a venue's market data lags.
- gateway: `[gateway.underlying.<BASE>] max_net` does the same for the account behind a gateway.
- gateway: `[gateway.shared."<venue>:<symbol>"]` shares one instrument between strategies, with STP.
- ctl: `fastmm-ctl limits underlying.<BASE>.max_net=...` sets the limit on a running session.
- venues: perpetual state comes from Binance USDⓈ-M, OKX, Bybit, Deribit and Gemini.
- venues: a reconciliation on Binance Spot, USDⓈ-M and Bybit books the fills made during an outage.
- venues: a restart restores the position.
- venues: `api_passphrase` credential for venues that need one (OKX, Coinbase Exchange).
- venues: a book checksum mismatch resyncs the book.
- binance: `amend_keep_priority` reduces an order's quantity in place and keeps its queue position.
- binance: `fetch_fees = true` on Spot reads the account's commission rates at start-up.
- binance_usdm: venue-side dead man's switch, `dead_mans_switch_ms`, on by default.
- binance_usdm: a lost dead man's switch countdown kills the venue.
- binance_usdm: `post_only_rpi = true` sends RPI post-only orders.
- binance_usdm: `TRADIFI_PERPETUAL` contracts are accepted.
- bybit: venue-side dead man's switch, `dead_mans_switch_s`, off by default.
- backtest: `fastmm.walk_forward` / `bt::walk_forward` run walk-forward sweeps over K time folds.
- backtest: `[backtest] md_arrival = "recorded"` replays market data at its recorded receive time.
- data: `fastmm-data calibrate` fits `l2_queue` and latencies to live journals; prints `[backtest]`.
- net: the WebSocket client and server pass the Autobahn RFC 6455 suite (`scripts/autobahn.sh`).

### Changed
- engine: a strategy can declare up to 64 parameters (was 32); one more refuses the strategy.
- cli: command lines are parsed with CLI11; numbers are validated, flags and exit codes unchanged.
- cli: `fastmm-ctl` and `fastmm-top` take `--version`.
- backtest: the `l2_queue` queue model caps queue position with the book ticker and the trade tape.
- backtest: `queue_conservatism` defaults to `[engine] queue_conservatism`.
- strategies: `xmm` does not hedge on a venue that is killed, feed-lagged or stale.
- strategies: `HedgeExecutor` sizes hedges to the remaining `max_gross_notional`/`max_net_notional`.
- store: `fastmm-pnl duplicates` and the start-up duplicate check are about 10x faster (schema 7).
- net: the io_uring backend uses liburing 2.15.
- build: zlib is no longer needed.

### Fixed
- binance: starting with many symbols stays under the request-weight limit; the IP is not banned.
- binance: combined-stream URLs with more than about 14 symbols connect.
- binance: a book no longer resyncs right after connecting.
- binance: `cancel_all()` retries after a 418 or 429.
- binance_usdm: WebSocket order placement no longer charges request weight.
- binance_usdm: history queries respect the minute's weight and `Retry-After`; a 418 stops REST.
- venues: a requote is no longer refused `BalanceShort` after a cancel ack.
- venues: order and cancel acks carry the venue's time on all connectors.
- venues: a quiet channel returning to Live no longer logs a state change every 20 s.
- store: fills with the same execution id on different venues, symbols or sides are kept (schema 5).
- store: a restart after consecutive crashes no longer books executions twice.
- store: a session killed during its start-up replay keeps its fills on the next start (schema 6).
- engine: an order's fill reported both by cumulative quantity and by execution is booked once.
- engine: `[accounting.fx]` rates from a quiet book no longer go stale (`[accounting] stale_fx_ms`).
- strategies: `HedgeExecutor` and `xmm` split a hedge larger than a `[risk]` per-order limit.
- gateway: after a restart, fills made while it was down reach the strategy that owns the order.
- gateway: orders of several strategies no longer fill the connector's order table.
- okx: funding bills with more than 8 decimals are booked.
- data: `fastmm-data fill-check` keeps fills reported after their order ended.
- net: invalid UTF-8 text and malformed close frames are rejected with close code 1007 or 1002.

## [0.2.0] - 2026-09-23

FastMM 0.2.0 adds US equities through Nasdaq ITCH and OUCH with kernel-bypass receive, Python
strategies in backtests and live sessions, and a control plane for a running session. Strategy
registration, hook signatures and the journal format change; see Breaking changes.

Highlights:

- Nasdaq ITCH 5.0 / OUCH 5.0 venue with A/B arbitration and GLIMPSE recovery (Added, venues)
- Python strategies: Numba-compiled `@fastmm.hot` hooks and slow methods, live too (Added, python)
- `fastmm-ctl` pulls, resumes, retunes, flattens or kills a running session (Added, ctl)
- Run-to-completion threading and coalesced order sends (Added and Changed, engine)
- A SQLite trading record per session, queried with `fastmm-pnl` or from Python (Added, store)
- `fastmm report` writes a run as one self-contained HTML page (Added, backtest)

### Breaking changes
- engine: a strategy library exports one registration function that calls `register_strategy<S>(r)`.
- engine: `StrategyRegistry::add` is replaced by `try_add`.
- engine: `on_trade`, `on_book_ticker`, `on_option_ticker` take `(ctx, InstrumentId, const Msg&)`.
- engine: `on_fill` takes `(ctx, const Fill&)` and `set_quotes` returns `bool`.
- engine: `ParamDesc::parse` and `format` replace `set` and `get`.
- engine: built-in strategies move to the `fastmm::strategies` library.
- engine: `make_engine_runner`, `EngineConfig`, `LiveBackend` moved; the install exports components.
- strategies: `basic_mm` bps parameters keep four decimals; its golden hashes change.
- venues: venues resolve through `VenueRegistry`; `VenueKind` and `venue_factory.hpp` are gone.
- venues: connector keys are validated per venue; a key of the wrong type stops the session.
- live: `[engine] on_kill` defaults to `"exit"`; set `"stay"` for the old behaviour.
- live: an unrequested kill switch ends the session with exit code 6, or 5 if cancel-all failed.
- journal: format version 3 (readers open 1 to 3).
- journal: `fastmm-replay` refuses unclosed or damaged journals unless given `--allow-incomplete`.
- top: status segment version 5; `fastmm-top` and `fastmm-live` must come from the same build.
- backtest: shipped configs charge Binance spot VIP 0 fees, 10 bps a side.
- backtest: each instrument pays its own venue's fees.
- codecs: `moldudp::Receiver::on_packet` takes `(line, datagram, now_ns, meta)`.
- codecs: `L3Book` is no longer a template and takes an `L3BookConfig`.
- python: the PyPI distributions are `fastmm-engine` and `fastmm-engine-live`; imports unchanged.

### Added
- venues: Nasdaq TotalView-ITCH 5.0 venue (`kind = "nasdaq_itch"`) with GLIMPSE and OUCH 5.0 orders.
- net: multicast receive backends `rx_backend = "kernel" | "af_xdp" | "dpdk"`.
- net: AF_XDP loads its own BPF program without libbpf.
- net: DPDK needs `-DFASTMM_WITH_DPDK=ON` and `spin_mode = "busy"`.
- net: unicast market-data lines on all three backends.
- net: `order_transport = "user_tcp"` (experimental): OUCH over user-space TCP, `AF_PACKET` or XDP.
- net: `dpdk_exception_port`, a DPDK kernel exception path.
- sim: `fastmm-sim-itch`, an ITCH/MoldUDP64 publisher with seeded drops, GLIMPSE and OUCH 5.0.
- codecs: `fastmm::codecs`: FIX 4.4, ITCH 5.0, MoldUDP64, SoupBinTCP, OUCH 4.2/5.0 and CME MDP 3.0.
- engine: `[engine] threading = "single"` runs reactor, engine and sends on one thread (one venue).
- engine: `[engine] timer_slack_ns`, `lock_memory`, `net_backend = "io_uring"`, `reject_backoff_ms`.
- engine: `configs/profiles/production-latency.toml` for a dedicated host.
- engine: `[engine] ack_timeout_ms` cancels orders with no ack and frees their slots and exposure.
- engine: runtime parameter updates: `ParamUpdate`, `on_params` and `[strategy] max_param_age_ms`.
- engine: `quoting.hpp` helpers, `verify_strategy<S>()`, `on_quoting` and `every`/`once` timers.
- engine: `StrategyHarness<S>` for testing strategy hooks.
- core: `Ratio` and exact literals (`100.25_px`, `5_bps`).
- core: `Fixed::from_double_checked()`, `Ratio::from_bps_checked()` and `checked_add`/`sub`/`mul`.
- core: Black-76 pricing, greeks and an implied-vol solver.
- core: inverse (coin-margined) contracts booked in their settlement coin.
- risk: `[engine] kill_file` latches a `max_loss` trip and carries cumulative PnL across restarts.
- risk: per-venue kill switch: an unusable venue pulls its quotes and refuses orders; others trade.
- risk: `KillReason` records why a kill switch tripped.
- live: a latched start exits 6 until `fastmm-live --clear-kill`; SIGHUP clears a running session.
- live: `fastmm-live --strategy` and `--param key=value`.
- live: `fastmm-live` logs what the previous session of the same name left behind before it starts.
- ctl: `fastmm-ctl` over a per-session Unix socket, scoped to the session, a venue or an instrument.
- ctl: `pull`, `resume`, `param`, `limits`, `flatten`, `kill`, `unkill`, `stop` and `status`.
- ctl: `fastmm-ctl flatten` closes positions in reduce-only IOC slices within `--max-slippage-bps`.
- top: `fastmm-top`, a terminal dashboard over `/dev/shm/fastmm-<engine>.status`, with `--json`.
- top: `fastmm-top --metrics` serves a Prometheus endpoint.
- top: rejects are counted per `RejectReason`.
- cli: `--list-strategies --format json` on `fastmm-live` and `fastmm-backtest`.
- cli: `fastmm-pnl` reads the record: `sessions`, `fills`, `orders`, `pnl`, `positions`, `recover`.
- python: Python strategies in backtests: subclass `fastmm.Strategy`.
- python: `@fastmm.hot` hooks are compiled by Numba (extra `fastmm-engine[hot]`).
- python: `on_start`, `on_stop` and `@fastmm.every` slow methods.
- python: `fastmm.run_live()` and `python -m fastmm run module:Class` run a Python strategy live.
- python: a failing hook or slow method stops a live session with exit code 7.
- python: `fastmm.replay(journal, MyMM)` replays a hot strategy from a journal's parameter updates.
- python: `fastmm-engine-live` wheel (`fastmm-engine[live]`) with the connectors and static OpenSSL.
- python: `fastmm.data_sources()` and `fastmm.convert_data()`; `fastmm.data` is now a package.
- python: `fastmm.open_store(path)` returns the record as pandas DataFrames.
- python: `fastmm.features()` and `fastmm.evaluate_signal()`.
- deribit: Deribit connector (`kind = "deribit"`) for options and futures, plus `options_mm`.
- binance: USDⓈ-M perpetual futures connector (`kind = "binance_usdm"`); hedge mode is refused.
- binance: Spot SBE market data (`md_format = "sbe"`) decodes depth in 51 ns, JSON in 680 ns.
- binance: Ed25519 keys via `private_key_env` with `session.logon` on both Binance connectors.
- backtest: fill markouts at `[backtest] markout_horizons_s` (1, 10, 60 s) and fill-quality stats.
- backtest: markouts in the summary, `summary.json`, `fills.csv` and `BacktestResult.markouts()`.
- backtest: per-instrument `maker_bps` and `taker_bps` overrides.
- backtest: `--data binance:<SYMBOL>,<DATE>` and `tardis:<exchange>,<SYMBOL>,<DATE>`: public data.
- backtest: `fastmm report` and `fastmm.write_report()` write a run as one offline HTML page.
- data: `python3 -m fastmm.data fetch` fetches public data; `fastmm-data convert` packs `.fmj`.
- store: a SQLite record per session (`[storage]`): fills, orders, positions, daily PnL, kills.
- journal: `[engine] journal_sync`, `journal_max_bytes` (rollover) and `journal_retention_days`.
- journal: a journal write error trips the kill switch and exits 5.
- research: `fastmm::research` feature and forward-markout extractor with `evaluate_signal()`.
- tools: `tools/pnl_report.py`.
- tools: `scripts/bench-e2e.sh`, `bench-2host.sh`, `host-setup.sh` and `xdp-test.sh`.
- tools: `scripts/build-pgo.sh` builds with PGO and optionally BOLT.
- packaging: `scripts/package-release.sh` and the `release-dpdk` preset build a portable tarball.
- examples: `examples/quickstart/`, `cpp/tutorial/`, `external-project/` and `external-venue/`.

### Changed
- codecs: FIX 4.4 and CME MDP 3.0 are off by default (`FASTMM_CODEC_FIX`, `FASTMM_CODEC_MDP3`).
- codecs: MoldUDP64 packets ahead of a gap are buffered (`reorder_packets`, `gap_timeout_ns`).
- codecs: `L3Book` orders outside the price window go to an overflow store.
- codecs: OUCH 5.0 encode 4.4 µs → 0.1 µs; ITCH bridge 84 ns → 55 ns per message.
- venues: a connector registers itself in `VenueRegistry` with its keys and capabilities.
- engine: order sends are coalesced into one system call per drain (wire-to-wire p50 30-35 µs).
- engine: `BM_EngineStep_Sim` 9.2 µs → 2.9 µs; Binance `order.place` encode 1440 ns → 523 ns.
- engine: a venue's unreported `cum_qty` is booked as a synthetic fill (`OmsUpdate::missed_qty`).
- engine: a cancel ack for a fully filled order ends it as `Filled`.
- engine: `OmsUpdate::replaced_cl_ord_id` reports the id a cancel-replace superseded.
- live: `fastmm-live` refuses to start when the session epoch file cannot be written.
- live: `fastmm-live` trips `OrderIdsExhausted` instead of reusing client order ids.
- risk: `fastmm-live` refuses `[risk] max_loss` when instruments settle in more than one currency.
- core: `Fixed::from_double()` saturates out-of-range values and maps NaN to zero.
- backtest: the summary shows `net = spread capture + mid drift - fees + rebates + unexplained`.
- backtest: unknown config keys are reported with their line.
- backtest: `sharpe_annualized` is NaN for runs under a day.
- python: `run_backtest` takes `params=` and a `fastmm.Strategy` subclass; `request_stop()` ends it.
- python: wheels cover CPython 3.9 to 3.14.
- bench: CI benchmark budgets allow 10 % (was 25 %).
- build: `scripts/run-sim.sh` takes `--port` and `--tls-port`.

### Removed
- engine: `FASTMM_REGISTER_STRATEGY` and `StrategyContext::instrument_count()`.
- engine: `ctx.add_timer()`; use `every` and `once`.
- sim: `sim::make_sim_or_replay_runner`.

### Fixed
- deribit: fees are booked in `fee_currency`.
- net: a failed WebSocket send is no longer counted as sent; the REST fallback runs.
- net: `af_xdp` on virtio_net receives every line.
- bybit: a rate-limit or signature error during reconciliation no longer cancels the resting orders.
- bybit: open orders are paged past the first 50.
- bybit: `rejectReason` values map to specific reject reasons.
- bybit: positions deduct `spotBorrow`.
- venues: cancels and cancel-all go through after a venue-fatal error.
- venues: fills without a fee or trade id are no longer deduplicated away or given the previous fee.
- engine: a duplicate Binance ack no longer leaves a replaced order live and untracked.
- engine: reconciliation keeps in-flight cancels and orders sent after the request; quotes resume.
- engine: fills that arrive after the cancel ack reach positions, fees and PnL.
- engine: serialize latency is measured from the order call.
- engine: channels no longer flap to Live after a silent Stale.
- core: TSC calibration survives wall-clock steps (WSL2) and KVM hosts without `nonstop_tsc`.
- journal: live session journals replay exactly with `fastmm-replay --verify`.
- binance: Spot with `key_type = "ed25519"` sends `session.logon` and its order channel goes live.
- python: `hot_abi.h` is installed; hot strategies compile against an installed tree.
- tools: `tools/pnl_report.py` no longer double-counts commission charged in the base asset.

### Documentation
- docs: reorganised into getting started, tutorials, how-to, reference and explanation.
- docs: the site is served at <https://ziy.bio> with generated C++ and Python API references.
- docs: a nine-page tutorial, Your first market maker.
- docs: production operation pages and references for errors, exit codes, the journal and status.
- docs: Backtesting explains markouts and what the simulator cannot tell you.

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
