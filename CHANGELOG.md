# Changelog

All notable changes are recorded here (Keep a Changelog format).

## [0.2.0] - 2026-09-23

### Added
- US equities venue `kind = "nasdaq_itch"`: TotalView-ITCH 5.0 on lines A and B with MoldUDP64
  arbitration and re-requests, one `L3Book` per instrument through `ItchL2Bridge`, a GLIMPSE
  snapshot plus a recovery buffer for a mid-stream join, and OUCH 5.0 over SoupBinTCP order entry
  (`order_entry = "none" | "sim_ouch"`). Keys `lines`, `depth`, `recovery_buffer_packets`,
  `reorder_packets`, `gap_timeout_ns`, `spin_mode`; no API keys. A buffer overflow during two
  snapshots in a row trips the venue kill switch with `KillReason::FeedLost`.
- Multicast receive backends: `rx_backend = "kernel" | "af_xdp" | "dpdk"` on `nasdaq_itch`.
  `af_xdp` takes `xdp_mode` and `queues` and loads BPF bytecode built in C++ over raw `bpf(2)` (no
  libbpf or libxdp); `dpdk` needs `-DFASTMM_WITH_DPDK=ON` and `spin_mode = "busy"`, and runs
  unprivileged with `--no-huge --no-pci --in-memory`. `Venue::poll()` is called after every reactor
  iteration. A line may be a local unicast address (no join) on all three backends.
- `order_transport = "user_tcp"` (experimental): OUCH over `net::UserTcp`, a single-connection
  user-space TCP client with its own address (`user_tcp_ip`, `user_tcp_port`,
  `user_tcp_interface`), over `AF_PACKET` rings, AF_XDP or DPDK. DPDK also takes a kernel exception
  path (`dpdk_exception_port`, `dpdk_exception_ip`, `dpdk_exception_interval_us`, default 20 us).
- `fastmm-sim-itch`: a Nasdaq-style simulator that publishes ITCH 5.0 over MoldUDP64 on two lines
  with seeded drops, pacing and bursts, answers re-requests, serves GLIMPSE snapshots and accepts
  OUCH 5.0 over SoupBinTCP. Orders whose ClOrdID is a sequence token are timed wire to wire
  (`--summary-json`). `configs/sim-itch.toml`, `configs/nasdaq-itch-sim.toml`.
- `fastmm::codecs`, a core-only library for exchange wire protocols: FIX 4.4 (view, builder,
  framer, initiator and acceptor sessions), the Nasdaq family (ITCH 5.0, MoldUDP64, SoupBinTCP
  3.00/4.00/4.10, OUCH 4.2 and 5.0) and CME MDP 3.0 with A/B arbitration and snapshot recovery,
  generated from CME's schema version 13 by `tools/sbe_gen.py`. None is wired to a live venue.
- `[engine] threading = "single"` (default `"split"`): run-to-completion, with the one venue's
  reactor, the engine and order sending on one thread and no ring hop between the packet read and
  the order write. More than one venue is a configuration error; journals and replay are unchanged.
- `[engine]` latency and safety keys: `timer_slack_ns` (PR_SET_TIMERSLACK), `lock_memory`
  (mlockall), `net_backend = "epoll" | "io_uring"`, `reject_backoff_ms` / `reject_backoff_max_ms`,
  and `ack_timeout_ms`, which force-cancels an order still waiting for its ack that long and frees
  the pool slot, the `max_open_orders` slot and the `max_position` exposure it held. Off by default.
  `configs/profiles/production-latency.toml` collects the settings for a dedicated host.
- Durable risk state: `[engine] kill_file` (default `<journal_dir>/<name>.kill`) latches a
  `[risk] max_loss` trip and carries cumulative realised PnL and fees, so `max_loss` is a budget for
  the deployment rather than one per process. A start with a latched trip exits with code 6 until
  `fastmm-live --clear-kill`; SIGHUP clears the switch of a running session.
- Per-venue kill switch: a connector error that makes one venue unusable trips that venue's bit,
  pulls its quotes, cancels its orders and refuses new ones (`RejectReason::VenueKilled`) while the
  other venues trade. `KillReason` records why each flag was first set;
  `StrategyContext::venue_killed(venue)` and `trip_kill(reason)` are new.
- `fastmm-top`, a terminal dashboard. `fastmm-live` publishes to `/dev/shm/fastmm-<engine>.status`
  every 250 ms (`--status`, `--no-status`); `fastmm-top --json` prints the snapshot as JSON. The
  engine counts risk and venue rejects per `RejectReason` and logs the first of each, then at most
  one line per reason per `EngineConfig::reject_log_interval` (10 s).
- Python strategies in backtests: subclass `fastmm.Strategy` with the C++ hook names and
  `fastmm.Param` parameters, then `fastmm.run_backtest(cfg, data, strategy=MyStrategy, params=...)`.
  `@fastmm.hot` methods are compiled by Numba and called by the engine thread through
  `strategies/hot_abi.h` (extra `fastmm-engine[hot]`); `on_start`, `on_stop` and `@fastmm.every`
  methods run beside them with `ctx.snapshot()`, `ctx.recent()`, `ctx.fills()` and `ctx.publish()`.
  `fastmm.replay(journal, MyMM)` replays a hot strategy from a journal's parameter updates.
- Python strategies live: `fastmm.run_live(StrategyClass, config, params=None, ...)` and
  `python -m fastmm run module:Class --config file.toml` run a class with hot hooks and slow methods
  in the `fastmm-live` session. A failing hook or a slow method that raises, overruns its `timeout`
  or ends stops the session; new exit code 7. `run_live` takes `fills_capacity`, `recent_rows` and
  `slow_tier_timeout_ms`; `python -m fastmm run` takes `--slow-tier-timeout-ms`.
- `fastmm-engine-live`, a second wheel installed by `pip install "fastmm-engine[live]"` (CPython
  3.10 or later). Its module `fastmm_live._live` links the network stack, the connectors and static
  OpenSSL 3.5.8, and exports `build_info()`, `ca_locations()` and `self_test()`. TLS clients look
  for CA certificates in `SSL_CERT_FILE`, `SSL_CERT_DIR`, the three usual system bundles, a fallback
  file (`certifi` in `fastmm-live`), then OpenSSL's built-in paths; a `SSL_CERT_FILE` that does not
  load is an error naming the variable.
- Parameter updates: `EventType::ParamUpdate` (26) carries up to 32 (field index, raw value) pairs.
  `ParamPublisher` validates an update off the engine thread; the engine applies it at one event,
  journals it and calls the new `on_params(ctx)` hook. `[strategy] max_param_age_ms` (default off)
  disables quoting before the first update and while none was applied for that long.
  `fastmm-live --strategy <name>` and `--param key=value` are new, and `--list-strategies --format
  json` on `fastmm-live` and `fastmm-backtest` prints name, transports and parameter schema.
- Strategy API: `Ratio` (1 bp = raw 10,000), exact literals `100.25_px`, `0.01_qty`, `5_bps`,
  `include/fastmm/strategies/quoting.hpp` (`mid`, `microprice`, `away_from`, `keep_passive`, ...),
  `hooks.hpp` with the `Fill` view and `verify_strategy<S>()`, the `on_quoting(ctx, enabled)` hook,
  context methods `contains`, `portfolio`, `working_quote`, `order`, `open_qty`, `every`, `once`,
  `fastmm::sim::StrategyHarness<S>` for testing hooks on virtual time, and the checked conversions
  `Fixed::from_double_checked()`, `Ratio::from_bps_checked()`, `checked_add` / `sub` / `mul`.
- Deribit connector (`kind = "deribit"`), JSON-RPC 2.0 over WebSocket for options and futures, with
  `EventType::OptionTicker`, Black-76 price, greeks and an implied-vol solver
  (`core/options/black76.hpp`) and the `options_mm` strategy. `configs/deribit-testnet.toml`.
- Binance USDⓈ-M perpetual futures connector (`kind = "binance_usdm"`): depth sync with `pu`
  chaining, orders over the WebSocket API with a REST fallback, the listenKey user stream,
  reconciliation of open orders and positions, and a check that refuses hedge mode. New key
  `position_from_account_update`; `configs/binance-usdm-demo.toml`. Binance Spot gains SBE market
  data (`md_format = "sbe"`, `sbe_ws_url`; depth 10+10 levels decodes in 51 ns against 680 ns for
  JSON, a trade in 19 ns against 120 ns), and both Binance connectors take Ed25519 keys through
  `private_key_env` with `session.logon` on the order connection.
- Inverse (coin-margined) contracts booked in their settlement coin: `PositionTracker` uses
  `qty * multiplier * (1/avg - 1/px)` and a size-weighted harmonic average entry price.
  `Instrument::inverse_pnl()` and `settlement_ccy()` are new; `pnl_per_tick()` is zero for one.
- Backtest markouts: every fill is marked against the venue mid at fill time plus each horizon of
  `[backtest] markout_horizons_s` (1 s, 10 s and 60 s by default; empty turns them off), reported
  per instrument, side and liquidity flag in the CLI summary, `summary.json`, `fills.csv`,
  `BacktestResult.markouts()` and `tools/pnl_report.py`. Fill quality alongside: realised spread,
  queue position at the fill, time-to-fill percentiles and the share of new orders that filled.
  `[[instruments]] maker_bps` and `taker_bps` override the venue's `[venues.<name>.fees]` for one
  instrument, since `sim::FeeModel` is now a default schedule plus per-instrument overrides.
- Tooling: `tools/pnl_report.py` reconciles journal and account PnL; `scripts/bench-e2e.sh` runs
  `fastmm-sim-itch` against `fastmm-live` over a veth pair, `scripts/bench-2host.sh` across two
  hosts, `scripts/host-setup.sh` prepares an Ubuntu 24.04 VM, `scripts/xdp-test.sh` runs the
  privileged AF_XDP tests, `scripts/build-pgo.sh` builds with PGO and optional BOLT, and
  `scripts/package-release.sh` with the `release-dpdk` preset builds a tarball with DPDK linked in.
  `examples/quickstart/` (a 29-line `main`), `examples/cpp/tutorial/` and
  `examples/external-project/` are strategy projects to copy.
- `fastmm report <run-dir | session.fmj>` writes the run as one self-contained HTML page: equity
  and inventory on one time axis, the PnL decomposition, markouts, fill quality, the quote and
  reject counts and the configuration. Inline CSS and SVG, no JavaScript and no network, so it
  works offline, in dark mode and on paper. Also `fastmm.write_report(result, path)` in Python and
  `python3 tools/report.py <run-dir>` from a checkout with nothing installed
  ([Run report](docs/reference/run-report.md)). `scripts/run-sim.sh` and the quick start end with
  the report's path, and the quick-start example writes `runs/quickstart/`.
- `fastmm-top --metrics <[host:]port>` serves the live status snapshot at `/metrics` in the
  Prometheus text format: engine and venue counters, PnL, kill state, rejects by reason and the
  latency quantiles, in seconds and quote currency. Off unless the flag is given, bound to
  127.0.0.1 by default, and served from `fastmm-top`'s own process, so a scrape never reaches the
  engine ([Monitoring a live session](docs/how-to/operations/monitor-with-fastmm-top.md#scrape-it-with-prometheus)).
- Durable risk state (`include/fastmm/core/session_state.hpp`): `[engine] kill_file` (default
  `<journal_dir>/<name>.kill`) latches a `[risk] max_loss` trip and carries the cumulative realized
  PnL and fees, so `max_loss` is a budget for the deployment rather than one per process. A start
  with a latched trip exits with code 6 until `fastmm-live --clear-kill` or the file is removed.
  `fastmm-top` shows `LATCHED` and the carried PnL (status version 5: `kill_latched`,
  `pnl_carry_raw`). SIGHUP clears the kill switch of a running session
  (`ControlCommand::ResetKill`, which with a venue in the header clears that venue's bit only).
- `[engine] ack_timeout_ms`: orders still waiting for their ack that long are force-cancelled,
  freeing the pool slot, the `max_open_orders` slot and the `max_position` exposure a lost request
  used to hold for the rest of the session. Off by default.
- Inverse (coin-margined) contracts are booked in their settlement coin: `PositionTracker` uses
  `qty * multiplier * (1/avg - 1/px)` and a size-weighted harmonic average entry price, and
  `Instrument::notional()` returns `qty * multiplier / price`. `Instrument::inverse_pnl()` and
  `settlement_ccy()` are new; `pnl_per_tick()` is zero for an inverse contract, whose tick value
  depends on the price.
- `Fixed::from_double_checked()` and `Ratio::from_bps_checked()`, and `checked_add` / `checked_sub`
  / `checked_mul`. `avellaneda_stoikov` and `options_mm` skip a side whose price does not convert
  instead of quoting the result of an unchecked cast.

### Changed
- **`fastmm-live` exits after a kill switch it did not ask for.** `[engine] on_kill = "exit" |
  "stay"` (default `"exit"`): the session runs the normal shutdown (quotes pulled, REST cancel-all,
  summary, status file, journal trailer) and exits with the new exit code 6, or 5 if a cancel-all
  failed. `"stay"` keeps the previous behaviour. `fastmm-live --help` lists every exit code.
- **Journal format version 3** (from 1). Every consumed event and fired timer carries the engine
  clock; the header holds the session epoch, quoting enabled, per-venue cancel-replace, the
  effective configuration as TOML without API keys and its hash, the strategy's parameter table and
  optional strategy metadata (`meta_bytes`, `meta_crc32c`). Readers open versions 1 to 3.
  `fastmm-replay` uses the embedded configuration by default, checks `--config` against the config
  hash and reports a difference as a what-if run; a v1 session journal still needs `--config`.
- **Status segment version 5** (new in this release). It carries kill reasons, per-venue kill flags
  and rejects, p99.9 in every latency, a multicast feed block per venue and the latched kill state
  with carried PnL. `fastmm-top` and `fastmm-live` must come from the same build; a mismatch is
  reported as such (exit code 3 with `--once`) instead of being misread.
- **Breaking, strategy registration.** A strategy library exports one registration function calling
  `fastmm::register_strategy<S>(r)`, which adds the Sim, Replay and Live factories.
  `StrategyRegistry::add` is replaced by `try_add`. The built-in strategies are the new
  `fastmm::strategies` library; `make_engine_runner`, `EngineConfig` and `LiveBackend` moved.
- **Breaking, strategy hooks.** `on_trade`, `on_book_ticker` and `on_option_ticker` take
  `(ctx, InstrumentId id, const Msg& m)`; `on_fill` takes `(ctx, const Fill& fill)`, and now also
  fires for late fills and fills for unknown ids. `set_quotes` returns `bool`. The engine checks
  every hook at compile time and fails the build naming the expected signature.
- **Breaking, strategy parameters.** `FASTMM_PARAM` accepts `Price`, `Qty` and `Notional` fields,
  `FASTMM_PARAM_BPS` declares a `Ratio` in bps and `FASTMM_PARAM_MS` a `Duration` in whole ms.
  `ParamDesc::parse` and `format` replace `set` and `get`; an optional `validate() const` runs after
  all keys. BasicMM bps parameters keep four decimals instead of two, which changes the `basic_mm`
  golden hashes; `avellaneda_stoikov`, `options_mm` and `sample_1000` are unchanged.
- **The live session and the command lines are libraries.** `fastmm::live` holds `run_live` and
  `fastmm::cli::live`; `fastmm::backtest` holds `fastmm::cli::backtest` and `fastmm::cli::replay`.
  `fastmm-live`, `fastmm-backtest` and `fastmm-replay` are 3-line mains, and a strategy name
  registered by different code exits with code 3. The install exports components, so
  `find_package(fastmm COMPONENTS live)` against an `FASTMM_BUILD_NET=OFF` install fails.
- **Backtest results change.** The shipped configs and examples charge Binance spot VIP 0, 10 bps
  each side, instead of a 0.5 bps maker rebate, and each instrument pays its own venue's fees. The
  CLI summary prints `net = spread capture + mid drift - fees + rebates + unexplained` with the
  residual always shown. Outbound hashes are unchanged.
- Order sends are coalesced on the network thread: every venue drains the outbound ring and writes
  all orders of the drain with one system call (`TcpLink::cork()` / `uncork()`, and the same on
  `WsClient` / `net::Connection`). With `spin_mode = "busy"` the engine sets a wake flag instead of
  writing the eventfd. Over veth on WSL2: wire to wire p50 47-55 us to 30-35 us, T0 to T5 p50
  4.6-5.4 us to 2.6-3.1 us.
- Hot paths: OUCH 5.0 encode in `bench-e2e` 4.4 us to 0.1 us p50, `BM_EngineStep_Sim` 9.2 us to
  2.9 us, Binance `order.place` encode 1440 ns to 523 ns, ITCH bridge 84 ns to 55 ns per message.
  `OpenHashMap` and the L3 book tables are resident `HotArray`s, `ouch50::UserRefMap` is
  direct-mapped, `PositionTracker` keeps running totals, HMAC-SHA256 keys are precomputed (Binance
  order encode 1628 ns to 736 ns) and fixed-point products stay in 64 bits unless they overflow.
- **Benchmark harness: what the published numbers mean.** Several benchmarks measured the harness
  rather than the code and everything was re-measured. `BM_ReactorEchoThread` was pinned to the same
  core as its echo thread (8.0 ms per round trip against 11 us unpinned); `BM_TickToOrder_Sim` and
  `BM_EngineStep_Sim` use `UseManualTime` instead of Google Benchmark's pause overhead (about 600 ns
  on a 300 ns operation) and no longer hash outbound orders (`SimTransportConfig::hash_outbound`),
  so the "p50 991 to 247 ns" gain was in large part the benchmark's own checksum. Latency
  benchmarks chain each result into the next (`BM_Fixed_RoundToTick` 0.7 to 3.6 ns). A benchmark
  declares the cores it needs (`FASTMM_BENCH_NEEDS_CORES`) and fails if given fewer.
  `bench/ci_budget.toml` budgets are set from the measured time and allow 10 % instead of 25 %.
- `moldudp::Receiver`: `on_packet(line, datagram, now_ns, meta)` replaces `on_packet(datagram)`.
  Packets ahead of a gap go to a `ReorderBuffer` (`reorder_packets`, default 256) instead of being
  dropped; a gap is declared after `gap_timeout_ns` (default 2 ms) or when the buffer is full.
  `L3Book` is no longer a template either: capacity and price window are `L3BookConfig` constructor
  arguments allocated once, and orders outside the window go to a bounded overflow store instead of
  failing with `OutOfWindow`.
- A `cum_qty` a venue reports that no fill message covered is reported as `OmsUpdate::missed_qty`
  and booked as a synthetic fill at the order's own price; it used to move the OMS counters only. A
  cancel ack or expiry for an order that filled completely ends it as `Filled`, not `Canceled`.
- `SessionEpochStore::next_epoch()` returns a `Result` and writes through a temporary file, an
  `fsync` and a rename; it used to ignore every write error, so an unwritable path handed out epoch
  1 to every session and client order ids repeated across restarts. `fastmm-live` now refuses to
  start. `Oms::next_cl_ord_id()` returns an invalid id once the 32-bit sequence is used up instead
  of reissuing ids, which trips `KillReason::OrderIdsExhausted`.
- `PositionTracker::set()` takes the instrument and remeasures unrealised PnL at the last mark;
  `fastmm-live` refuses to start when the instruments settle in more than one currency and
  `[risk] max_loss` is set, and warns otherwise. `Fixed::from_double()` saturates at `max()` /
  `min()` and maps NaN to zero, and `RiskEngine` leaves a collar or fat-finger band unset when it
  would overflow instead of wrapping into a pass-through.
- `fastmm-backtest`, `fastmm-replay` and `BacktestConfig.from_toml` report unknown configuration
  keys and sections with their line. `sharpe_annualized` is NaN for runs shorter than 1 day and
  `max_drawdown_pct` is `max_drawdown / initial_capital`, NaN without initial capital.
  `fastmm.run_backtest` is a Python function with a `params=` argument applied on top of
  `config.params`, `strategy=` also takes a `fastmm.Strategy` subclass or instance, and
  `ctx.request_stop()` ends a backtest. The existing call forms and golden hashes are unchanged.
- The PyPI distributions are named `fastmm-engine` and `fastmm-engine-live`, because `fastmm` is
  taken; the import names `fastmm` and `fastmm_live` are unchanged. The wheels workflow uses
  cibuildwheel 4.2.1 and covers CPython 3.9 to 3.14; publishing stays a manual step.
- `scripts/run-sim.sh --port` / `--tls-port` (or `FASTMM_SIM_PORT` / `FASTMM_SIM_TLS_PORT`); the
  test presets set no job count, `scripts/bootstrap.sh` never pip-installs into a conda base
  environment, and the opt-in testnet tests carry the ctest label `live` instead of `unit`.

### Removed
- `FASTMM_REGISTER_STRATEGY`, `StrategyRegistry::add`, `StrategyContext::instrument_count()`,
  `ctx.add_timer(period, repeat, tag)` (use `every` and `once`) and
  `sim::make_sim_or_replay_runner`. `tests/install-consumer` is replaced by
  `examples/external-project/`, and `docs/adding-a-venue.md` by
  `docs/how-to/venues/add-a-venue.md`.
- Google Benchmark's `stddev` aggregate from the published benchmark table: it read `0.0 ns` on
  more than sixty rows. `tools/bench_table.py` publishes the median over repetitions and the
  min-to-max spread instead.

### Fixed
- Deribit fills never set `fee_asset`, so a leftover byte of the connector's scratch buffer decided
  how the engine booked the commission. It now follows `user.trades` `fee_currency`; the Deribit and
  Bybit private parsers zero each message before filling it, and the engine books an out-of-range
  `fee_asset` as `Quote` and counts it (`EngineStats::invalid_fee_assets`).
- `net::WsClient::send_text()` returned true when the write failed and closed the stream, so the
  venues counted the order as sent, charged the rate limiter and skipped the REST fallback. A failed
  `uncork()` now rejects every order of that batch.
- Bybit answers a rate limit or a clock/signature error with HTTP 200 and a non-zero `retCode`; the
  connector emitted the reconciliation Begin/End around the failed parse and `Oms::reconcile_end()`
  then cancelled every order resting at the venue. The three connectors now decode the whole
  open-order snapshot before anything reaches the engine, and Bybit pages `GET /v5/order/realtime`
  with `nextPageCursor` instead of stopping at the first 50 orders.
- A venue-fatal error or a REST hard stop refused cancels as well as new orders, so the kill path
  could no longer clear the book. All four connectors admit Cancel and CancelAll on whatever
  transport is still usable.
- Bybit executions without `execFee` kept the previous fill's fee, and Binance Spot and USDⓈ-M fills
  without a trade id shared one exec id, so the OMS dedupe window dropped the second fill of an
  order. The exec id falls back to the venue order id and the cumulative filled quantity.
- The OMS completed a replace on a duplicate ack for the original id. Binance acks every new order
  twice, so a requote between the two left the new order live at the venue untracked. Only the
  replacement id's ack completes a replace now.
- Reconciliation cleared a cancel or replace still in flight and marked cancelled the orders sent
  after the open-orders request, leaving live orders the OMS no longer tracked. Begin is stamped
  with the last order id sent before the request (`ReconcileMsg::sent_watermark`), End spares later
  orders and only touches the reconciling venue. Quotes also stayed empty after a reconciliation in
  a quiet market; the engine now re-applies the quotes it paused at Begin.
- Fills that arrived after the cancel ack never reached positions, fees or PnL: the terminal record
  keeps instrument and side, and the engine falls back to the fill's own instrument and side.
- `calibrate_tsc()` measures the TSC rate against `CLOCK_MONOTONIC_RAW` rather than
  `CLOCK_REALTIME`: a host wall-clock step inside its 50 ms window (WSL2 steps by 0.5-1.5 s every
  10-40 s) made the clock run tens of times fast, so every order failed the stale-market-data check.
  On hosts with `constant_tsc` but without `nonstop_tsc` (KVM cloud VMs) the rate was dropped
  entirely, printing venue latencies of 0 and engine hops of about 1.8e15 us; the rate is now kept
  for intervals while wall time stays on `clock_gettime`.
- **Live session journals replay exactly.** `fastmm-replay --journal <live journal> --verify`
  reported a mismatch at the first outbound message: replay ran with session epoch 1, drove its
  clock from receive times instead of the live `TscClock`, and fell back to
  `configs/backtest-example.toml`. Journal format v3 records what was missing. Outbound messages the
  transport refused (ring full) were journaled as sent; they are journaled after the hand-off and
  marked dropped, and replay refuses them again.
- `af_xdp` on virtio_net lost market data: attaching an XDP program raises the device's queue pairs,
  so the single socket on queue 0 saw one line or none. The program is attached before the sockets
  are bound, and without `queues` there is a socket on every RX queue the interface has. The final
  status kept zero XDP counters because the source was closed before the last publish.
- Binance Spot with `key_type = "ed25519"` never sent `session.logon` on the order connection, so
  the order channel never went Live. A revoked session clears the logon state and applies the error
  action; a transient failure retries after 2 s. `resolve_venue_env` no longer requires `api_secret`
  for Ed25519 keys and keeps `api_key` in a dry run with `md_format = "sbe"`.
- Serialize latency was measured from the previous event's strategy decision (about 100 ms with
  BasicMM's stale timer); T3 is stamped when the strategy calls the order API. Binance and Bybit
  order and user channels also reported "Live" again after a silent Stale, which made strategies
  requote.
- **`BM_TickToOrder_Sim` measured no order events**: every timed tick found both quotes pending and
  sent nothing (`order_events_pct` 0.000). The rig settles untimed before timing and quotes 3 ticks
  wide; the benchmark fails when fewer than 50 % of ticks send orders, and `tools/check_budgets.py`
  fails on errored benchmarks and on median counters below the `[min_counters]` floors.
- `hot_abi.h` was skipped by the header install pattern, so hot strategies did not compile from an
  installed tree. `scripts/host-setup.sh firewall <iface>` allows the interface's subnet when ufw
  is active, and `be*_t` / `le*_t` wire fields are byte arrays, so a field at an odd offset of a
  packed layout is no longer misaligned.
- Bybit `rejectReason` values map to specific reasons instead of a guessed "Balance" match, Bybit
  positions deduct `spotBorrow` from `walletBalance`, and `GET /api/v3/time` is counted with
  weight 1 as documented. `schema.hpp` no longer lists a `binance_futures` kind that has no
  connector. `tools/pnl_report.py` replaces the session scripts whose mark-to-market PnL subtracted
  commission charged in the base asset twice (-48.74 instead of -32.94 USDT on a Demo session).

### Documentation
- The docs tree is reorganised into `getting-started/`, `tutorials/`, `how-to/`, `reference/`,
  `explanation/` and `contributing/`, with `docs/README.md` as the index. Moved pages leave no
  pointer. `docs/contributing/writing-docs.md` has the style rules, and a subtraction pass took
  README.md and docs/ from 59,673 to 52,442 words (README 195 to 85 lines).
- The site is built with MkDocs Material and served from <https://ziy.bio>. Two API references are
  generated on every build and never committed: `/api/cpp/` by Doxygen over the 83 public headers of
  `docs/api/public-headers.txt`, and `/api/python/` by mkdocstrings over `python/fastmm`.
- New pages for running with real money: `how-to/operations/running-in-production.md` (what breaks
  and what is not covered, each with the file that decides it), `explanation/economics.md`,
  `how-to/operations/runbook.md`, `explanation/how-it-works.md`, `reference/errors.md` (exit codes
  per program, every `RejectReason`, `KillReason` and `VenueAction`) and `adr/README.md`.
- `explanation/backtesting.md` explains how to read markouts and lists what the simulator cannot
  tell you: no market impact, a symmetric random-walk mid, coin-flip counterparty side, no informed
  flow and a synthetic spread of about 0.003 bps. At a 10 bps maker fee `basic_mm` captures
  0.008 bps gross and `first_mm` 0.002 bps, so neither has a measurable edge.
- New install and quick-start pages and the nine-page tutorial "Your first market maker", through
  the simulated exchange and Binance Demo; reference pages for the strategy API, public API and
  header tiers, command lines, journal format, status file, fixed-point, the venues, the Nasdaq and
  MDP3 codecs, options, `fastmm-sim-itch`, multicast feeds, the two-host benchmark, the Python API
  with the measured cost of a Python strategy, and a glossary.
- `docs/reference/cli.md` is generated from each program's `--help` and
  `docs/reference/configuration.md` from the key tables in `include/fastmm/config/schema.hpp`;
  `tools/doc_snippets.py` keeps code blocks identical to their sources and `tools/docs_links.py`
  checks relative links offline. All four have `--check` and run in CI.
- `docs/how-to/venues/add-a-venue.md` replaces a guide that pointed at a registration directory and
  X-macro that do not exist; it follows the five shipped connectors, from the threading contract to
  a conformance checklist linked to the tests that prove each item. Operator how-tos cover Binance
  Demo and the testnets, a go-live checklist, the kill switch, journals and replay, and
  troubleshooting keyed by the exact log messages.

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
