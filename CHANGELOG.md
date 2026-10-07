# Changelog

All notable changes are recorded here (Keep a Changelog format).

## [Unreleased]

### Added
- `fastmm init` also writes `live.toml`: the starter class on BTCUSDT on Binance Spot Demo Mode,
  journal on, keys from `FASTMM_BINANCE_API_KEY` / `FASTMM_BINANCE_API_SECRET` (a `--dry-run`
  needs none). The starter's `backtest.py` takes `--config` and `--data`, so a recorded journal
  replays with `python backtest.py --config live.toml --data runs/<journal>.fmj`.
- A backtest the kill switch stopped says so: a `stopped` line at the top of the summary table
  (`by the kill switch ([risk] max_loss) at 101.2s of 120.0s`), `kill_reason` and `kill_at_s` in
  `summary.json` and in `BacktestResult.stats()` (None when it did not trip). In C++,
  `RunnerStats::kill_reason` / `kill_ts_ns` and `Engine::kill_ts()`.
- The `@fastmm.hot` IR check names what the hook does and where, when Numba's typed IR shows it:
  `it creates a dict at strategy.py:21` (lists, sets, strings, arrays and `print` too, in the hook
  or an `njit` helper it calls), then the runtime symbols.

### Changed
- `fastmm-engine` no longer installs on CPython 3.9 (end of life): `requires-python` is `>=3.10`,
  as `fastmm-engine-live` and the `hot` extra already needed, and no cp39 wheel is built. On 3.9,
  `pip install "fastmm-engine[hot,live]"` failed with an unexplained `ResolutionImpossible`.
- `examples/quickstart/CMakeLists.txt` fetches FastMM `v0.4.0` instead of `main`. A version bump
  updates the tag; configuring FastMM with examples fails when it differs from the project version.
- The release tarball needs glibc 2.28 or newer and nothing else, was the build host's glibc
  (2.39) and libssl 3: it is built in `manylinux_2_28` with OpenSSL (`scripts/wheels/build-openssl.sh`),
  libstdc++ and libgcc linked statically, and runs on Ubuntu 22.04, Debian 12, Rocky Linux 9 and
  Amazon Linux 2023 as well as Ubuntu 24.04. `scripts/package-release.sh --container` builds it
  that way with docker; without `--container` it builds on the host, into
  `build/package-<preset>` (was `build/<preset>`), still with static OpenSSL and libstdc++.
  `scripts/check-release.sh` runs a tarball on those five distributions; `release.yml` runs it
  before publishing.
- The tarball holds `fastmm-sim-exchange`, `configs/sim.toml`, `configs/sim-local.toml`,
  `configs/sim-local-tls.toml` and the simulator's TLS certificate (`tests/fixtures/tls/`). Its
  README lists all eight programs and starts with the simulated exchange, which needs no network
  namespace; the ITCH example runs in `sudo unshare -n` (Ubuntu 24.04 restricts unprivileged user
  namespaces) as the calling user, and both stop the simulator when the engine exits.
- The starter (`fastmm init`) quotes 0.0001 BTC a side, not 0.002: its default 120 s backtest
  paid 100 in fees and was stopped by `[risk] max_loss` at 101 s. It still loses, on fees.
- `fastmm-live --dry-run` (and `python -m fastmm run --dry-run`) quotes on paper: each venue that
  takes orders sits behind a `PaperVenue` (`live/paper_venue.hpp`) that acknowledges every order,
  cancel and replace at once and never fills, so the strategy, the quote manager and the risk
  checks run as in a keyed session and nothing is sent. Every 5 s and at the stop the log has one
  line per instrument, `[<venue>] dry run <symbol>: bid <px> x <qty> ask <px> x <qty> would send
  new=.. cancel=.. replace=..`, and the summary `dry run: would have sent N orders, M cancels, R
  replaces`. The engine counters and `fastmm-top` (`paper orders=`) count the paper orders.
  `ctx.quoting_enabled()` is true in a dry run, and the journal header's `quoting_enabled` is 1; a
  strategy attached to a gateway still does not quote in a dry run. The engine is unchanged.
- A dry run leaves the keyed sessions' state alone: it runs as engine `<name>-dryrun` (its status
  file, control socket and journal), starts flat, and opens no store, kill ledger
  (`[engine] kill_file`), strategy state file or instance lock. Before, its store record became
  the "previous session" a keyed start restored its position and execution replay from, and it
  counted in the kill ledger. `--dry-run` with `--standby` or `--takeover` exits 2.
- The per-venue status line comes every 10 s when the log goes to stderr, and every second into a
  `[logging] file` as before. `[logging] status_interval_s` sets it (0 = none). fastmm-gateway too.
- A planned stop (`--duration`, SIGINT/SIGTERM, `fastmm-ctl stop` or `handoff`) logs `stopping:
  pulling quotes and cancelling all` at INFO instead of the WARN `kill switch requested`, which
  stays for `fastmm-ctl kill`. `kTripKillPlannedStop` is the `TripKill` argument that says so.
- The end-of-session quoting line reads `quoting two_sided=93.5% of 60.43 instrument-seconds`: the
  time is summed over the instruments that quoted.

### Fixed
- `scripts/bootstrap.sh` uses `$CXX` when it is set, and otherwise the first of `g++`, `g++-15` to
  `g++-13`, `clang++` and `clang++-21` to `clang++-16` that is gcc 13 or clang 16 or newer, and
  exports it with the matching C compiler as `CXX` and `CC` for the configure. It found only `g++`
  and `clang++`, so on Ubuntu 24.04 with just the `g++-13` package it stopped with "no C++
  compiler found", even with `CXX=g++-13`.
- `fastmm.BacktestConfig.from_toml` and `from_toml_string` no longer resolve `${VAR}` in
  `[venues.*]`, as `fastmm-backtest` already did: a live config with unset API key variables
  loads for a backtest instead of raising `ConfigError`.

### Documentation
- [Register a strategy](docs/how-to/strategies/register-a-strategy.md) installs FastMM from a
  build without its tests, benchmarks and examples, in `build/sdk`: a third of the targets.
- The quick start in the README and [Install](docs/getting-started/install.md) creates a virtual
  environment first and names `python3-venv`: on a fresh Debian or Ubuntu there is no `pip`, and
  the system Python refuses to install packages (PEP 668).
- Install: the Ubuntu 24.04 package line installs `g++` (gcc 13) instead of `g++-13`, which
  provides no `c++` or `g++` for CMake to find.

## [0.4.0] - 2026-10-06

### Added
- `[venues.<name>] order_rate_threshold` (Binance Spot and USDⓈ-M, default 0.9): the share of the
  10 s and 1 d order-count limits the connector fills before it refuses an order, apart from the
  request weight's share.
- Handing a running `fastmm-live` over to a new process. `[engine] instance_lock = true` (default
  false) takes an exclusive `flock` on `[engine] lock_file` (default `<journal_dir>/<name>.lock`)
  before the session reads its state or contacts a venue, and releases it after its shutdown;
  a second start on the same engine name exits 8. `fastmm-live --takeover` loads its reference
  data, sends `handoff` to the running session's control socket (cancel-all, flush, release) and
  starts once it holds the lock, within `[engine] handoff_timeout_ms` (default 75000); `--standby`
  waits for the lock without asking. `fastmm-ctl handoff` is the new control command. Python:
  `python -m fastmm run --standby/--takeover`, `run_live(standby=, takeover=)`. The systemd units
  do not restart on exit 8. [Hand over a running session](docs/how-to/operations/hand-over-a-session.md)
  also designs a handoff that keeps the quotes.
- Warm standby: a `fastmm-live --standby`/`--takeover` whose venues can hold their private
  channels back (`Venue::defer_private`, Binance Spot) connects to market data and runs its
  strategy while the running session trades, with every order refused until each venue's first
  reconciliation, and writes no kill, state, status or store file. Once it holds the lock it reads
  them, sends `ControlCommand::TakeOver` (the carried PnL; the strategy restores the state file
  then) and opens its private channels: the pause is the reconciliation, not a restart. It
  cancels nothing if it stops before taking over. Behind a gateway, with `threading = "single"`
  or another connector the standby waits cold as before.
- `fastmm-sim-exchange`: `SimExchangeServer::accepted_client_order_ids()`, every accepted client
  order id in acceptance order.
- `[venues.<name>] source_ip` / `source_interface`: every connection of the venue (WebSocket,
  REST, the blocking start-up and kill-switch requests) is bound to that address of the host
  before it connects, so the accounts of a pool can leave from different addresses. Binance Spot
  and USDⓈ-M pool accounts count request weight, cooldowns and bans per source address. fastmm-live
  and fastmm-gateway refuse to start when the address is not the host's or the interface does not
  exist. Unset: the kernel chooses, as before. Every connector but `nasdaq_itch`.
- Fill audit: `[venues.<name>] fill_audit_interval_s` (default 0, off) has fastmm-live compare,
  every so often, the account's executions in the venue's trade history with the fills the store
  holds for that venue, by trade id: missing (never booked), phantom (booked, not at the venue),
  differing quantity, price, fee, side or order, and stored twice. Each is logged once and counted
  in the status file (version 17) and the metrics (`fastmm_venue_fill_audit_*`).
  `fill_audit_lag_s` (180) keeps the window behind the execution replay; `fill_audit_mode = "book"`
  also books the missing ones through the replay. The venue's side is read on its network thread,
  apart from the replay, within the bulk share of the request weight. Binance Spot and USDⓈ-M
  (`Venue::audit_executions`).
- `fastmm-pnl audit --exchange <file>`: the store's fills against the venue's executions exported
  to a file (Binance `myTrades` / `userTrades` JSON as returned, or generic JSON or CSV); prints what
  differs and exits 5 when anything does.
- Pool treasury: `[venues.<primary>.treasury]` (fastmm-live, off by default) keeps one asset
  spread over the accounts of an account pool by internal transfers: targets from `weights`
  (equal by default) and `min_free`, a `threshold` before anything moves, `min_amount`,
  `max_amount` and `step`, one transfer at a time with `min_interval_s`, `max_per_hour`,
  `cooldown_s` after a failure or a `timeout_s`, and `dry_run`. Each transfer has a client id and
  is written to `state_file` before it is sent; a restart asks the venue for an unanswered one
  before planning anew, and a failed one is never sent again. The steps are logged, journaled
  (`ControlCommand::Transfer`, `ControlTransferMsg`; `tools/journal_dump.py`) and counted in the
  status file and the `fastmm_treasury_*` metrics (status segment version 18). The plan, the
  limits and the ledger are `core/treasury.hpp`; it runs on its own thread, `fm-treasury`.
- `Venue::transfer`, `Venue::transfer_status`, `Venue::transfer_account` and
  `Venue::request_balances` (`core/transfer.hpp`; `VenueCapabilities::internal_transfer`,
  `VenueCaps::internal_transfer`): a connector can move an asset between two accounts of its
  exchange, blocking and off the reactor thread. Binance Spot and USDⓈ-M do it with the master
  account's `POST /sapi/v1/sub-account/universalTransfer` and its history by `clientTranId`, with
  a master key of its own (`transfer_api_key_env`, `transfer_api_secret_env`, internal transfer
  permission only), `transfer_rest_url` and each account's `sub_account_email`; a USDⓈ-M transfer
  goes through the receiver's spot wallet in two steps.
- `BalanceMsg::kWithdrawable` and `BalanceMsg::withdrawable` (in the former padding): what can
  leave the account by an internal transfer. Binance USDⓈ-M sets it to `maxWithdrawAmount` on its
  balance snapshots; `BalanceBook::Row::transferable()` is the free estimate capped by it (free
  where a venue never says) and reaches `LiveBalance::transferable_raw`; the pool treasury gives
  no more than that.
- backtest: an enabled `[venues.<primary>.treasury]` runs on simulated time against the pool's
  simulated accounts, no strategy hook needed; `[backtest] transfer_latency_ms` (default 0) delays
  each transfer. `BacktestResult::treasury` counts the transfers and lists them (`moved(asset)`),
  `summary.json` and the summary table report them and `transfers.csv` is written; a run without a
  treasury reports as before.
- sim: `SimAccounts::transfer`, `SimTransport::transfer` and `sim::SimTreasuryPort`: the same
  treasury runs against a pool's simulated accounts, and the engine hears the new balances on each
  account's link.
- The journal records what a session starts from: the bytes the strategy was handed to
  `restore()` (`StrategyState` records), and with `[strategy] state_snapshot_interval_s` (default
  0: off) the strategy's `state()` and the engine's positions that often. `[backtest]
  initial_state = "journal"` (default `"state_file"`, as before; `"none"`) starts a backtest from
  them, or, in an older journal, from the positions reconciled before the first order;
  `[backtest] params_from_journal = true` replays the session's parameter updates at their
  times. `fastmm-data calibrate --backtest --from-live-state` compares runs started that way.
  `fastmm-replay` restores the recorded bytes, so a session that restored state replays exactly.
- `fastmm-data calibrate` fits a cancel latency of its own and the venue's per-connection service
  time (`latency_cancel_us`, `latency_cancel_jitter_us`, `order_service_us`) from the journals'
  cancel acks and message bursts, fits the order latency on isolated messages, and replays each
  session's cancels through the simulated intake to show what the fitted keys change in the fill
  check.
- `fastmm-data calibrate` fits fixed latency and jitter from millisecond venue times (Binance) by
  maximum likelihood over the stamps' millisecond intervals; it used to take the median and no
  jitter.
- backtest: `[backtest] order_service_us` (and per venue): the simulated venue takes the order
  messages of one connection one after another, each no sooner than this after the one before, so
  the last cancels of a burst reach the book late, as on a live venue. `latency_cancel_us` /
  `latency_cancel_jitter_us` give cancels a latency of their own. All off by default.
- `fastmm-data fill-check` diagnoses the live fills the `l2_queue` model misses: markouts and the
  pre-fill mid move of every first fill, live and model, by outcome (both, live only, model only),
  how each live fill printed in the recorded trades (at its price, a sweep through it, missing),
  whether the opposite touch had reached it or a cancel had gone out, and fill rates live / model
  by queue ahead, ticks behind the touch, prints and resting time. `--horizons-ms`, `--pre-ms`,
  and `--market` to replay another recording of the same period; `--csv` carries the new columns.
- `[engine] cpu_dma_latency_us` (default -1, off): `fastmm-live` holds a PM QoS CPU latency request
  on `/dev/cpu_dma_latency` for the whole session, so no CPU enters an idle state that takes
  longer than that to wake (0: every CPU polls in C0). Without write access to the device it logs
  a warning and runs on. `scripts/host-setup.sh cstates show|limit <us>|restore [--cpus LIST]
  [--dry-run]` disables the slower idle states of chosen CPUs in sysfs, and `tune
  --cstate-max-latency <us>` runs it.
- `[engine] log_irq_affinity` (default false): `fastmm-live` logs at start each network
  interface's queue interrupts with the CPUs they may run on and which of them are `cpu` or
  `net_cpus`, and warns about one that may run on the engine's CPU. `scripts/host-setup.sh
  irq-affinity <iface> <cpu-list> [--dry-run]` spreads an interface's queue interrupts over the
  listed CPUs.
- `[engine] rt_priority` and `net_rt_priority` (default 0, off): `fastmm-live` runs the engine
  thread and the network threads under `SCHED_FIFO` at that priority (1..99). Without
  `CAP_SYS_NICE` or a high enough `RLIMIT_RTPRIO` it logs a warning and runs on. The systemd units
  carry `LimitRTPRIO` and `AmbientCapabilities=CAP_SYS_NICE` commented out.
- Strategy hook `on_batch_end(ctx)`: `ctx.request_batch_end()` from any hook asks for one call
  once the engine has handled the events waiting in its input rings (or the step's
  `max_events_per_step` is used up). Work a burst repeats per event (a requote after every balance
  report) runs once after it. The call is journaled, so a replay makes it after the same event.
- `[engine] feed_budget_per_ring` (default 64, as before): events the engine takes from one input
  ring before the next ring's turn. 1 interleaves the rings, so a burst of one account's cancel
  acks and balances does not hold up the market data queued behind it.
- `[engine] net_spin_dedicated` (default false): with `spin_mode = "adaptive"`, a network thread
  pinned to a core of its own polls without ever blocking, as with `"busy"`; threads on shared or
  unpinned cores still block when idle. An adaptive thread blocked and woke up to a thousand times
  a second, a few microseconds each.
- `binance_usdm`: `md_ticker_conns = <n>` (0 to 8) opens n more `/public/stream` connections
  carrying only the bookTicker streams. Binance hands each connection to a push server that lags
  now and then; the feed keeps the first copy of every update id per symbol and drops the rest
  (`stale_tickers`). The extra connections' state is logged only: the md connection still decides
  whether the books are live.
- `binance_usdm`: `one_way_mode = true` switches an account in hedge mode to one-way mode at start
  when it holds no position and no open order; `leverage = <n>` sets every enabled symbol's
  leverage at start. Both are off by default; nothing on the account changes unless asked.
- backtest: `journal:…,remap=1` maps a recording's instruments to the configuration's by symbol
  (and sets the events' venue to the configured instrument's), so recordings of different
  instrument sets merge; a symbol the configuration lacks is dropped and counted in the report.
- Account pools: `[venues.<member>] pool_of = "<primary>"` puts several accounts of one exchange
  behind one venue, with one instrument table and one feed (Binance Spot counts orders per
  account: 100 per 10 s, 200,000 a day). A member is of the primary's kind, lists no instruments
  and is not `public_only`; a pool holds at most 8 accounts. The engine sends a new order to the
  account `NewOrderRequest::account` (`.account(venue)`) names, else to the usable member whose
  balance covers it with the most of the venue's 10 s order window left (then the daily one), and
  the order keeps it: `Order::venue`, the `OmsUpdate` and `Fill` events and the store's rows carry
  the member's id, cancels and replaces follow it, and `ctx.balance(member, asset)` and
  `ctx.order_budget(member)` are per account. `ctx.pool(primary)`, `ctx.pool_primary(venue)`,
  `ctx.account_usable(venue)`, `ctx.balance_room(id, side, px, account)`; on a pooled instrument
  `balance_room(id, side, px)` is what one account can hold (its room plus its own open orders on
  the side) net of the pool's open quantity, so `fit_to_balance` cuts a ladder to what one order
  can be. A member whose order link is down or whose kill switch is
  engaged is routed around; the primary's kill stops the pool. `RejectReason::InvalidAccount` (40)
  for an account outside the pool. Live, Binance Spot and USDⓈ-M run as members
  (`VenueCapabilities::account_pools`): order, user and REST sessions, reconciliation, balance
  snapshot, execution replay and kill-switch cancel-all over the primary's instruments, no market
  data; a member's position reports are summed into the instrument's position. In a backtest a
  member is a simulated venue of its own over the primary's books, with its own
  `[backtest.venues.<member>.balances]` account; `[backtest] orders_10s` / `orders_1d` (and per
  venue) give the simulated venues Binance's order-count intervals, refused with `VenueRateLimit`
  past them and reported through `ctx.order_budget`, so the routing by remaining window works in
  backtests too (`rejects_rate_limit` in the sim stats).
- sim: a trade through a resting order fills it with the print's quantity, not the whole order
  (the aggressor had a fixed size; what it printed at the deeper level is what it would have taken
  from us). `l2_queue`, `ctx.queue_ahead` and `fastmm-data fill-check` share the rule. The
  `basic_mm/l2_queue` and `sample_1000` golden hashes are re-baselined (1143 -> 1418 fill
  events: more of them partial).
- `[venues.<name>] public_only = true`: a venue can be market data only. Its keys are not required,
  its private sessions are not opened, nothing is sent to it and nothing is cancelled on it at stop;
  for a venue a strategy only reads prices from.
- The simulated fee is charged on the contract's notional, multiplier included (futures contracts
  worth a fraction of the base coin were charged a hundred times the real fee).
- `gate_usdt`: a base coin label longer than the instrument field is shortened with a warning
  instead of refusing the contract.
- Gate USDT perpetual futures connector, `kind = "gate_usdt"` (`include/fastmm/venues/gate/`):
  market data from `futures.obu` (the in-stream snapshot and `U`/`u` increments, 50 levels every
  20 ms or 400 every 100 ms), `futures.book_ticker`, `futures.trades` and `futures.tickers` (mark,
  index, funding to `PerpStateMsg`); orders over the WebSocket API after `futures.login`
  (`futures.order_place` / `order_cancel` / `order_amend`, 1.6 ms round trip from AWS Tokyo) with
  the REST fallback; the `orders`, `usertrades`, `positions` and `balances` channels; the fill
  replay from `my_trades_timerange`, the open-order and position reconciliation, the balance leg
  from `/accounts`, `countdown_cancel_all` as the dead man's switch and `DELETE /orders?contract=`
  as the kill switch. Quantities are contracts (`contract_multiplier` = `quanto_multiplier`).
  `net::hmac_sha512_hex` and `net::sha512_hex` for the APIv4 signature. `configs/gate-usdt.toml`; recorded
  fixtures in `tests/fixtures/gate/`.
- `journal:x.fmj` reads the parts of a session recorded with `[engine] journal_max_bytes`
  (`x.1.fmj`, `x.2.fmj`, ... until one is missing) after `x.fmj`, as one stream; the repeated
  header, instrument table and configuration are not events, so nothing plays twice. A part whose
  first sequence number does not follow the previous part's last (a part missing in between, or
  the previous one cut short) is read with a warning in the log and in the run's report; a file
  numbered like the next part but of another session is an error. `parts=0` reads the one file.
  `strip_own` collects the session's orders from every part (`collect_own_orders` and
  `OwnOrderStripper` take a span of readers). `fastmm-replay` over a market-data journal reads
  the parts the same way; over a session journal it still verifies one part at a time.
- Quotes and the `[risk] orders_per_sec` bucket: a quote short of an order token is held back
  instead of refused (`QuoteStats::kept_rate_limit`, no `RateLimit` risk reject and no log line),
  and the engine places the latest quotes of the instruments that ran out, in the order they ran
  out, as the bucket refills. `[engine] quote_token_reserve` keeps that many tokens for orders the
  strategy sends itself and for quotes that reduce the position (cancels never take one).
  `OrderBudget::local_wait_ns`: the time until the bucket admits the next order.

### Changed
- A strategy declares up to 128 parameters (`kMaxParams` and the journal's parameter table, was
  64; the file layout is unchanged). A reader built with 64 keeps a newer file's first 64 entries
  and drops updates of the rest. Python hot strategies follow the same limit.
- The once-a-minute execution replay reads only the symbols with order activity (an order sent,
  an order event received) since their watermark less the settle margin, on Binance Spot and
  USDⓈ-M; a quiet symbol's watermark moves as if it had answered empty. 85 symbols' `myTrades`
  cost 1700 weight a minute, and a pool of four accounts on one IP spent more than its bulk share
  on them, so the queries waited 180 s and failed and depth resyncs starved. The replays before a
  reconciliation, after a reconnect or a restart, and retries still read every symbol.
  `ReplaySchedulerBase::Hooks::active`, `skipped()`; `OrderActivity`.

### Fixed
- Binance Spot and USDⓈ-M: a signed REST request was stamped and signed when queued. The REST
  connection carries one request at a time, so one queued behind an execution sweep's `myTrades`
  queries (a database read each at the venue) could reach it older than `recv_window_ms` and be
  refused with -1021, and its retry queued the same way. Requests are now stamped and signed when
  they are written, and a replay keeps at most 4 requests on the connection, sending the next as
  each reply comes in, so an order or a snapshot sent meanwhile does not wait behind the sweep.
- `l2_queue` fill model: one trade print went in full to each of our resting orders on its side,
  so a print through two levels of a ladder filled both with up to its whole quantity. It is now
  shared in price priority (best for the aggressor first, at one price the shorter queue first),
  each order taking its fill and its queue from what the better ones left; the fill check does
  the same. `ctx.queue_ahead` follows: a print first carries our maker fills at its venue time,
  whether the fill comes before or after it, so it matches the fill model again. One order per
  side fills as before; a three-level `basic_mm` on the synthetic market fills 13 % fewer orders.
- Account pools on Binance Spot and USDⓈ-M: the accounts count request weight in one window per
  IP, as Binance does, and a 429 or 418 answered to one pauses or stops them all. Each account
  counted only its own, so a pool starting together sent several times the 6000/minute and was
  banned (418). A lone venue keeps its own limiter. `RateLimiter::share_ip`, `shared_rate`;
  `weight_bucket` and `order_bucket` return a copy (`std::optional<RateBucket>`).
- A fill that arrives after its order ended (Binance answers the cancel on the WS API before the
  user stream reports the execution it raced, or rejects the cancel of an order it already filled)
  was booked into the position but left the order `Canceled` with `cum_qty` 0. The OMS's terminal
  record now takes the fill; an order it completes becomes `Filled`, and the store's `orders` row
  is updated (`cum_qty`, `state`) to match its fills.
- A journal whose parameter table is longer than the reader's limit opens: a strategy with 33
  parameters, recorded by a build with the 64-entry limit, was refused as `HeaderCorrupt` by a
  `fastmm-data` built before it. The entries past the limit are left out and updates of those
  fields dropped. Every refusal now says which check failed (`HeaderCorrupt (the header is N
  bytes, M with this build's 128-byte instruments ...)`, `BadVersion (format version 4, this build
  reads 1 to 3 ...)`), in `fastmm-data`, `fastmm-replay` and the journal sources.
- `binance_usdm`: an account in hedge mode exits 3 (a setting to fix), not 4, like Bybit and Gate,
  and the message names the setting and `one_way_mode`. Instruments in several settlement
  currencies with `max_loss` and no `[accounting]` print the `[accounting]` and `[accounting.fx]`
  lines to add.
- Account pools: an account whose 10 s or daily order window is full gets no automatic orders, and
  an order whose account's window is full is refused by `send` with `RateLimit` instead of by the
  venue (a strategy's retries made a storm of `VenueRateLimit` refusals). A quote resting on one
  account was sized to another account's room on top of it, so it was cancelled on its ack and
  placed again without end; the pooled `balance_room` now counts one account only.
- `--data "a;b"` (`MergedSource`) copied every event it yielded into an EventBuf, 8 KiB, the size
  of a 256-level book: a journal's depth snapshot with more levels (Binance USDⓈ-M records up to
  1000 a side) was written past the end of the source, which glibc reported as `corrupted
  double-linked list` part-way through the run. The merge now yields the input's own event and
  advances that input on the next call, so nothing is copied.

### Documentation
- README rewritten around the engine's features; a new page, How fast it is, with the latency and
  load of a production session.
- README rewritten around the engine's features; a new page, How fast it is, gives the latency and
  load of a production session.

## [0.3.0] - 2026-10-03

### Added
- `[strategy] state_file` / `state_interval_s`: a strategy with `std::string_view state()` and
  `bool restore(std::string_view)` (Python: `state(self)` and `restore(self, data)`) keeps its
  bytes across sessions. The engine calls `restore` once after `on_start` with the file's
  contents (no file: a first start; bytes it does not take: it starts fresh) and takes `state()`
  every `state_interval_s` and at the end; the live session and the backtest write the file
  atomically off the engine thread. A backtest's end state feeds the next run; a journal replay
  leaves the file alone. `BacktestConfig.state_file` / `state_interval_s` in Python.
- `on_risk_reject(ctx, const RiskReject&)`: an optional hook that reports each new order or
  replace the engine's own `[risk]` check refused (instrument, side, type, reason, price,
  quantity, `user_tag`, the order a replace would have changed), after the hook that asked has
  returned, as `on_quoting` is. A strategy without it pays nothing. `ctx.order_budget(venue)`
  (`core/order_budget.hpp`): the orders the `[risk] orders_per_sec` bucket admits now and, live,
  the venue's own counts by window (Binance orders per 10 s, minute and day, request weight per
  minute) as its connector last published them from its rate limiter, plus whether the venue has
  it paused (429, 418). Python: `on_risk_reject(self, ctx, reject)`, `ctx.order_budget()`,
  `RiskReject` and `OrderBudget`.
- Binance USDⓈ-M: `post_only_rpi = true` sends post-only orders with `timeInForce` `RPI` (retail
  price improvement: matched only against app and web orders, hidden from the API depth; the
  venue does not modify one, so pair it with `supports_replace = false`). `TRADIFI_PERPETUAL`
  contracts (the tokenised equity and commodity perpetuals) are accepted beside `PERPETUAL`.
- `HedgeExecutor` (`strategies/hedge_executor.hpp`, in `fastmm/strategy.hpp`): a component a
  strategy owns to hedge the base position of source instruments on one or more hedge instruments
  in preference order. Sizing from positions, one order in flight across all of them, IOC limits
  through the touch capped at `max_qty` and held under the lot, `min_qty` or `min_notional`, the
  balance check, the retry and the hold after an unreported outcome; failover while a hedge venue
  is down, killed, gated, its book invalid or stale, its balance short, or after `max_failures`
  (benched), and back; a halt when the last one fails; optional stepwise de-risking of the sources
  with reduce-only IOC orders; `status()` and `stats()`. `xmm` is rebuilt on it and gains
  `fallback_instrument`, `fallback_tolerance_bps`, `failover_bench_ms`, `derisk_after_ms`,
  `derisk_step_qty`, `derisk_interval_ms` and `derisk_tolerance_bps`. `examples/cpp/hedged_mm.cpp`
  uses it for a quoter hedged on two venues.
- `fastmm-data calibrate <journal>...`: the `l2_queue` fill model and the simulated latencies
  fitted to live sessions. Per session, fill-check over a grid of `queue_conservatism` and without
  the book ticker, the trade tape or both; hit, miss and false-fill rates, time to fill, the
  model's queue ahead at the live fills, and each venue's order round trips. The conservatism is
  cross-validated (fitted on each session, scored on the others) and printed with the latency keys
  as a `[backtest]` snippet; `--backtest` re-runs each session as a `strip_own` backtest as
  configured and with the fitted keys, beside the live session (fills, time to fill, PnL
  decomposition, markouts, all marked against the journal's ticker mids).
- Mark, index and funding of perpetuals: `PerpStateMsg` (`EventType::PerpState`, market data,
  journaled) carries a venue's mark price, index price, funding rate per interval, the interval,
  the next funding time and open interest, from Binance USD-M `markPrice@1s` (the interval from
  `fundingInfo`), OKX `mark-price` / `index-tickers` / `funding-rate` / `open-interest`, Bybit
  `tickers`, Deribit `ticker` and Gemini `@markPrice` / `@fundingAmount`. The engine keeps a per-instrument table with the time
  each field arrived (`core/perp_book.hpp`): `ctx.mark`, `ctx.index`, `ctx.funding`,
  `ctx.perp_state`, `on_perp_state` (C++ and Python), stale after `[accounting] stale_mark_ms` /
  `stale_funding_ms`. `[accounting] mark = "venue"` (default) values a derivative's position at
  its venue's fresh mark for the unrealized PnL, `max_loss` and the exposure caps, in the engine
  and in `fastmm-gateway`'s account; `"mid"` keeps the mid. `xmm`: `funding_horizon_s` (the
  perpetual legs' funding over the expected holding time shifts fair value) and `mark_basis`.
  Status segment version 14 (the perp table), `fastmm_perp_*` metrics, `journal_dump.py`.
- Balances, margin and collateral: `BalanceMsg` (`EventType::Balance`, journaled) reports one asset
  of a venue's account; every connector sends a snapshot after each reconciliation
  (`ReconcileDriver`'s balance leg) and its private stream's updates (Binance Spot
  `outboundAccountPosition`, USD-M `ACCOUNT_UPDATE` via a refresh, OKX `account`, Bybit `wallet`,
  Deribit `user.portfolio`, Gemini `balances@account`, Coinbase Exchange `balance`; Coinbase
  Advanced refreshes after fills). The engine keeps a per-venue table moved by its own orders and
  fills until the next report (`core/balance_book.hpp`): `ctx.balance`, `ctx.margin`,
  `ctx.balance_room`, `on_balance`, `RiskHeadroom::balance_buy_qty` / `balance_sell_qty`.
  `[risk] check_balance` (default on) refuses what the balance cannot cover (`BalanceShort`);
  `[[instruments]] initial_margin` sizes a derivative's margin. `fastmm-gateway` checks the
  account's balance over every strategy (`[gateway] check_balance`, `GatewayBalanceShort`) and
  routes balances to every attachment. `basic_mm` cuts its ladder to the balance; `xmm` drops a side
  the quote venue cannot fill and holds a hedge the hedge venue cannot cover. Status segment
  version 13 (balances; eleven gateway refusal counters), `fastmm_balance_*` metrics.
- Coinbase Advanced Trade connector (`kind = "coinbase_advanced"`), spot, production with a CDP
  API key: ES256 JWTs (`net::EcdsaP256Key`, `net::base64url_encode`), `level2` with the
  connection's `sequence_num` as the loss check, `market_trades`; REST orders and
  `batch_cancel`; order states from the `user` channel, and each order's executions read with
  their trade ids when its cumulative quantity grows; fill replay with order lookups, the
  start-up sweep, the kill-path cancel-all. No venue-side dead man's switch exists.
  `configs/coinbase-advanced.toml`; the public feed and the read-only private calls were run
  against production, orders only against a scripted fake exchange.
- Coinbase Exchange connector (`kind = "coinbase_exchange"`), spot, sandbox: HMAC signing with a
  passphrase, `level2_batch` (or signed `level2`) with trade-id loss checks, `matches`; REST
  orders with UUID client ids, the signed `user` channel, fill replay with order lookups,
  `DELETE /orders` for the kill switch. `configs/coinbase-sandbox.toml`; the public feed was run
  against production and the sandbox, the private side only against a scripted fake exchange.
- `kMaxHttpHeaders` 64 (was 32): `api.coinbase.com` answers with more than 32 headers.
- `[gateway.shared."<venue>:<symbol>"]`: several strategies attached to `fastmm-gateway` trade one
  instrument. Fills of earlier sessions go to their strategy by `[engine] name` (the epochs the
  gateway gave it and, protocol 6, those its store lists); events naming no order and funding to
  an optional `primary`, else the account alone; the venue's position records to no strategy. The
  account splits its position into each strategy's share and an unattributed part and shows what
  the venue's position leaves unexplained (status segment version 12, `fastmm_account_unattributed`,
  `fastmm_account_unexplained`, a log warning). An order that would trade with another strategy's
  resting order is refused (`GatewaySelfTrade`). One attachment per `[engine] name`.
- `[risk.underlying.<BASE>] max_net` and `[gateway.underlying.<BASE>] max_net`: a limit on the net
  position in one base asset, in base units, over every instrument and venue (the gateway's over
  every strategy too). Contracts count `qty * contract_multiplier`, inverse contracts
  `qty * multiplier / mark`; options do not count. Worst case with the open orders on the order's
  side, as `max_position`; an order that brings the net towards zero passes. An inverse contract
  with a position and no current mark refuses orders on its underlying (`UnderlyingMarkUnknown`,
  `GatewayUnderlyingMarkUnknown`); over the limit: `MaxUnderlyingNet`, `GatewayUnderlyingNet`.
  `fastmm-ctl limits underlying.BTC.max_net=...` (`ControlCommand::SetUnderlyingLimit`, journaled).
  Status segment version 11 (the net per underlying; nine gateway refusal counters); metrics
  `fastmm_underlying_net`, `fastmm_account_underlying_net` and their `_max_net`.
- `xmm`, a built-in strategy that quotes one instrument and hedges on another venue with IOC
  orders. Fair value is the hedge book plus an EWMA basis; the hedge is derived from the two
  positions in base units (contract multipliers applied), one hedge at a time, so replayed,
  duplicated and late fills never double-hedge. Stale books or a hedge venue that is down pull the
  quotes; `max_unhedged` drops the side that would grow the gap; repeated failed hedges halt it.
  `configs/xmm-demo.toml` (Binance USDⓈ-M demo quotes, Bybit testnet hedges),
  `docs/how-to/strategies/xmm.md`.
- OKX v5 connector (`kind = "okx"`) for USDT-margined perpetual swaps (instType SWAP), net position
  mode: `books` (or the tbt depth channels) with the seqId chain, `bbo-tbt` and `trades`; order
  entry over the private WebSocket by `instIdCode`, REST fallback; `orders`, `positions` and
  `balance_and_position`; reconciliation with the fill replay (`trade/fills`, `fills-history`),
  `orders-pending` and `account/positions`; funding from `account/bills` type 8;
  `cancel-all-after` refreshed every window/3 (default 60 s); long/short or spot account mode
  refused with exit 3. Quantities are contracts (`contract_multiplier` = `ctVal` x `ctMult`).
  `configs/okx-demo.toml` for demo trading (`testnet = true` sends `x-simulated-trading: 1`). The
  public stream was run against OKX; the private side only against a scripted fake exchange.
- `api_passphrase`, a generic `[venues.<name>]` credential (OKX's third one): `${VAR}` only,
  cleared in a dry run, masked in logs and left out of journals.
- `SyncReason::ChecksumMismatch`: a venue book checksum that disagrees resyncs the book.
- Venue state for strategies. `ctx.fees(id)` returns the maker/taker rates of an instrument
  (`FeeRates`, the table a backtest charges; `sim::FeeModel` is now `FeeTable`, core/fees.hpp).
  `ctx.risk_headroom(id)` returns what each `[risk]` limit still admits (`RiskHeadroom`: rate
  limiter tokens, open orders, position room per side, gross and net exposure room, loss budget),
  from the inputs the next check uses. `ctx.venue_health(v)` returns the venue's feed lag
  (`recv_ts - exch_ts` against its minimum over 8 s) and the ack round trip of our orders. Python:
  `ctx.fees`, `ctx.risk_headroom`, `ctx.venue_health`.
- `[risk] max_feed_lag_ms`: while a venue's market data arrives later than its baseline by more
  than the limit, its quotes are pulled and orders that could rest there without reducing the
  position are refused (`RejectReason::FeedLag` = 20); quoting resumes 100 ms after the last late
  message. `fastmm-ctl limits max_feed_lag_ms=N`. `[backtest] md_arrival = "recorded"` replays
  market data at its recorded receive time, so a backtest over a live journal sees its feed lag.
- Binance Spot `fetch_fees = true`: `fastmm-live` reads the account's rates per symbol
  (`GET /api/v3/account/commission`) at start-up, uses them instead of the configured fees and
  writes them into the journal's embedded configuration. `fastmm-sim-exchange` answers the
  endpoint with its fees.
- `[accounting]`: instruments in several settlement currencies in one session or gateway. The PnL
  totals, `max_loss` and the exposure caps (engine and gateway) are in `reporting_currency`, each
  other currency converted at the mid of its `[accounting.fx]` source instrument; positions, PnL
  and fees stay in their own currencies. A rate is unknown until its source's book is valid and
  stale with `stale_md_ms`; without a current one, an order adding exposure in that currency is
  refused (`FxRateUnknown`, `GatewayFxRateUnknown`), a reducing one passes, and booked PnL stays at
  the last rate. Backtest and replay convert the same way. Status segment version 9 (a seventh
  gateway refusal counter).
- Walk-forward sweeps: `fastmm.walk_forward(cfg, grid, folds=K, data=...)` and
  `bt::walk_forward` cut the data into K consecutive time slices, run the grid on each and score
  the best point of fold i-1 on fold i, next to the hindsight best there. `folds=1` is the plain
  sweep. `bt::TimeSliceSource` yields one slice of any source, starting on the book as of its
  first instant.
- Execution-history recovery: a reconciliation asks the venue what the account executed before it
  asks what is open. `Venue::request_executions(since_venue_ms)` and
  `VenueCapabilities::executions` are the interface; Binance Spot implements it with
  `GET /api/v3/myTrades`. Each execution reaches the OMS as an ordinary fill carrying the venue's
  execution id (`OrderFillMsg::kReplayed`), so only the ones the engine never saw are booked, with
  their real price and their real fee. This is what makes a fill that *finished* an order during a
  private-stream outage recoverable: the open-order snapshot no longer mentions such an order at
  all. The snapshot's `Begin` carries `ReconcileMsg::kExecutionsExact` when the replay was complete;
  without it the engine counts an estimated reconciliation and says so. A failed query is retried
  every 5 s until the venue answers rather than waiting for the next reconnect.
- Bybit replays executions from `GET /v5/execution/list` (account-wide, 7-day windows, cursor
  paging, emitted oldest first) and declares `VenueCapabilities::executions`, so its
  reconciliations can be exact and a restarted session carries its position over.
- A fill booked from a `cum_qty` jump before any execution named it is corrected, not counted twice,
  when the replay names it: `OmsUpdate::corrected_qty`, `PositionTracker::correct_fill`,
  `EngineStats::corrected_fills`. The quantity is already in the position, so what the execution
  replaces is the estimate's price and its missing fee.
- Binance USDⓈ-M replays executions with `GET /fapi/v1/userTrades` (`fromId`, or a start time
  walked forward a week at a time) and declares `executions = true`, so its reconciliations are
  exact and a restart restores its position.
- The simulated exchange serves `GET /api/v3/myTrades` from a trade log that outlives the orders, so
  the executions of an order the venue has forgotten are still reportable.
- Venue-side dead man's switch, the only thing that clears resting quotes after a SIGKILL, an OOM
  kill or a dead host. Binance USDⓈ-M arms `POST /fapi/v1/countdownCancelAll` per symbol and
  refreshes it every `dead_mans_switch_ms`/3 (default 60000 ms, 0 disables); a clean shutdown sends
  `countdownTime=0`. Bybit sets `POST /v5/order/disconnected-cancel-all` and subscribes the
  `dcp.spot` topic that DCP needs to fire, off by default (`dead_mans_switch_s`, 3 to 300 s)
  because Bybit grants DCP to institutional accounts only. Binance Spot has no such endpoint at
  all, and the production guide now says so instead of claiming only Deribit has one.
- `KillReason::DeadMansSwitchLost`: if Binance USDⓈ-M refuses the countdown refresh for a whole
  window while the process is alive, the venue has cancelled that symbol's orders, so the connector
  goes fatal and kills the venue rather than requoting into a kill switch it cannot see.
- Binance Spot `order.amend.keepPriority` (`PUT /api/v3/order/amend/keepPriority`): a replace that
  only reduces the quantity at the same price keeps the venue order, its id and its place in the
  queue, instead of going to the back of it through `order.cancelReplace`. Anything else, and
  `amend_keep_priority = false`, still uses `cancelReplace`, as does an order that has used up
  `max_order_amends` (the venue's `MAX_NUM_ORDER_AMENDS` filter, 10). `OrderAckMsg::kAmendedInPlace`
  tells the OMS to rekey the order without resetting its filled quantity.
- RFC 6455 conformance gate: Autobahn|Testsuite 25.10.1 (`crossbario/autobahn-testsuite`, pinned by
  digest) runs cases 1-7, 9 and 10 against `net::WsClient` (`tests/net/autobahn_client.cpp`
  against `wstest -m fuzzingserver`) and `net::WsServerConnection` (the echo server
  `tests/net/autobahn_server.cpp` under `wstest -m fuzzingclient`). `scripts/autobahn.sh` runs a
  side, `tools/autobahn_check.py` fails on any FAILED case; ctest label `autobahn`, left out of the
  test presets, skipped without docker; CI job `autobahn` uploads the reports. Both sides: 294 OK,
  4 NON-STRICT (6.4.1-6.4.4: invalid UTF-8 is caught when the message completes, not at the
  fragment), 3 INFORMATIONAL, 0 FAILED.
- Execution view for strategies ([Strategy API](docs/reference/strategy-api.md#execution-view)):
  `ctx.own_qty(id, side, px[, at])` and `ctx.best_ex_self(id, side)` give our resting quantity a
  live venue's feed shows at a price (venue-time aligned, as `strip_own` strips it; 0 in the
  simulator) and the touch without it; `ctx.queue_ahead(order_id)` runs the `l2_queue` queue model
  on the market data the strategy sees, with the new `[engine] queue_conservatism`;
  `ctx.order_times(order_id)` and `OmsUpdate::times` hold the send time and the ack's venue and
  receive times. Python: `Context.own_qty`, `best_ex_self`, `queue_ahead` (and `_raw`), and
  `sent_ns`, `venue_ack_ns`, `local_ack_ns` on `Order` and `OrderUpdateView`. Transports answer
  `own_in_feed(venue)`.

### Changed
- An unknown configuration key or section is an error with its line, no longer a warning: a
  misspelled or renamed key left the real one at its default. The same for a `[venues.<name>]`
  key the connector its `kind` names does not have (`validate_venues`, now without a warnings
  argument), a `[backtest]` key the backtest does not read, and a `[strategy.params]` key the
  strategy does not declare (a `LiveStrategy` with a schema is checked like a registered one).
  Python: `BacktestConfig.from_toml` raises `ConfigError` for them.
- A strategy declares up to 64 parameters (`kMaxParams`, was 32; the journal's parameter table
  grows with it, with no change to the file layout). The 33rd used to vanish from the schema
  without a word; one past the limit now refuses the strategy. One `ParamUpdate` still carries
  at most 32 values.
- A venue's `stale_ms` above its `dead_ms` (or a `stale_ms` of 0) is refused when the connector
  is built, as a configuration error with the venue's name. The Connection refused it on the
  venue's thread, where nothing caught the exception and the process aborted.
- `HedgeExecutor` also cuts a hedge to what `max_gross_notional` and `max_net_notional` leave:
  `RiskHeadroom` gains `exposure_buy_notional` / `exposure_sell_notional` (C++ and Python), the
  two caps as the notional of one order on the instrument in its settlement currency, converted
  by the risk engine at the rate its check uses; unlimited for the side that reduces the
  position, zero while the currency's rate is unknown.
- The check for executions stored by more than one session (run at every `fastmm-live` start and
  by every `fastmm-pnl` command) no longer groups the whole `fills` table: schema version 7 adds
  indexes on the venue ids (`fills_exec_any`, `funding_id_any`) and the check walks them in id
  order, reading table rows only for an id that repeats (0.14 s instead of 1.7 s for a million
  fills). `fastmm.open_store(...).duplicates()` returns the rows of `fastmm-pnl duplicates`.
- `xmm` sends no hedge to a hedge venue whose kill switch is on, whose feed-lag gate holds it or
  whose book is older than `stale_ms` (before, the hedge went out priced from that book, or was
  refused by risk and counted towards a halt); its quotes come off while the hedge venue's kill
  switch is on. Otherwise the same: two seeded sessions hash to the same values
  before and after (`tests/strategies/xmm_equivalence_test.cpp`).
- The queue model (`l2_queue`, `ctx.queue_ahead`, fill-check): an order at the touch of a book
  ticker newer than the depth joins behind the touch's quantity instead of the lesser of it and
  the depth's (a throttled depth update can predate the level); trades printed after the depth
  update or the ticker take their quantity from its levels (`TradeTape`), at placement, in a delta's
  old quantity (no longer counted as cancels ahead as well) and under the ticker's cap; a level
  that shows less than we have resting there predates our order and counts as others' quantity.
  The `basic_mm/l2_queue` golden hash is re-baselined (1014 -> 1169 fills).
- The command lines of all nine programs (`fastmm::cli::live`, `backtest`, `replay`, `data`, and
  `fastmm-pnl`, `fastmm-sim-exchange`, `fastmm-sim-itch`, `fastmm-ctl`, `fastmm-top`) are parsed
  by CLI11 2.7.2, header-only, fetched by CPM and private to the programs. The hand-written argv
  loops, the `usage()` texts and the three copies of the duration parser are gone. Flag names and
  exit codes are unchanged; a bad command line still exits with 2 and now prints
  `<program>: <message>` and a pointer to `--help`. `--help` has CLI11's layout (the regenerated
  blocks in [Command lines](docs/reference/cli.md)). Numbers are checked where they were not:
  `fastmm-pnl --session x` and `--limit x` are usage errors instead of 0. `fastmm-data` and
  `fastmm-pnl` commands are subcommands; their options may still come on either side.
  `fastmm-ctl` and `fastmm-top` gained `--version`.
- The io_uring reactor backend runs on liburing 2.15, fetched by CPM and linked statically into
  `fastmm_net`. The hand-written ring setup, mmap, SQ/CQ barriers and SQE encoding are gone, and
  `src/net/io_uring_uapi.hpp` with them. `ReactorBackend::IoUring`, its setup flags, the support
  probe and the epoll default are unchanged. Configuring with `FASTMM_BUILD_NET=ON` now needs
  `make`, which liburing's `configure` calls.
- `[backtest] fill_model = "l2_queue"` and `fastmm-data fill-check` cap an order's queue with a
  book ticker newer than the depth book: nothing ahead of an order priced better than its touch,
  at most the touch's quantity at the touch. `fill-check` reports the tickers it read.
  `[backtest] queue_conservatism` defaults to `[engine] queue_conservatism`.

### Removed
- `lead_mm`'s `own_in_feed` parameter: the imbalance takes our quantity from `ctx.own_qty`.
- The experimental user-space TCP client (`net::UserTcp`, RFC 9293 subset), the `AF_PACKET` frame
  ring (`net::PacketRing`) and `nasdaq_itch`'s `order_transport = "user_tcp"` with its keys
  `user_tcp_ip`, `user_tcp_port`, `user_tcp_interface` and `user_tcp_gateway`. A config that still
  sets `order_transport = "user_tcp"` is refused at start (`venues.<name>.order_transport: 'user_tcp'
  is not supported; valid: kernel`); the other four keys are reported as unknown and ignored. OUCH
  runs on kernel TCP with `TCP_NODELAY`; kernel bypass for it is OpenOnload or NVIDIA XLIO through
  `LD_PRELOAD` ([Low-latency TCP](docs/how-to/operations/low-latency-tcp.md)). Over a veth the
  kernel path measured the same (`bench_order_tcp`: 7.9 µs p50 to the server's read for both). With
  it went the XDP program's TCP and ARP redirect (`xdp::TcpMatch`, `XdpConfig::tcp_*` and
  `tx_frames`, the XDP TX ring), the DPDK source's frame sink and `frame_tx()`, `net::FrameSink`,
  `net::FrameTx`, `nasdaq::ByteLink` and the counters `XdpStats::to_sink`, `tx_frames`, `tx_drops`,
  `tx_kicks` and `DpdkStats::to_sink`, `tx_frames` (`DpdkStats::tx_drops` now counts ARP replies not
  sent). `scripts/bench-e2e.sh` and `scripts/bench-2host.sh` lost `--order-transport`,
  `--user-tcp-ip` and `--user-tcp-port`.
- The zlib dependency. Nothing called it: `fastmm_net` never negotiates permessage-deflate. The
  build, the installed package config and the Docker images no longer need `zlib1g-dev`.

### Fixed
- Binance Spot and USDⓈ-M with many symbols: a start sent every depth snapshot (weight 20 to 50
  each), `myTrades` / `userTrades` query and `symbolConfig` query within a second, past the
  minute's weight; the 429 came, the execution replay's fixed 5 s retry fell inside the pause the
  venue asked for, and the 418 ban followed. The snapshots, history queries and order lookups now
  take half the weight at most (`RateLimiter::kBulkShare`) and wait for the housekeeping timer
  otherwise; a replay query that cannot go out waits for a later tick instead of failing its
  stream (`ReplayScheduler::Hooks::can_query`, 180 s at most); a second 429 within a minute of the
  last pause waits 2, 4, 8 ... s on top of `Retry-After`, up to 120 s; the start's blocking
  `symbolConfig` and `account/commission` queries wait for their weight. Spot's `myTrades` queries
  and both connectors' order lookups now read a 429 and a 418 by their status, and Spot's did not
  check the weight before sending. Without `depth_limit` the snapshot depth is 1000 with up to 10
  subscribed symbols and 100 above (it was 1000 always).
- A combined-stream URL of more than about fourteen symbols never connected: the request target
  was copied through a 1 kB buffer, came out empty, and the upgrade request was refused. The
  target is now sized by the URL.
- Binance USDⓈ-M: the `userTrades` and `income` replay queries wait while the minute's weight is
  spent or the venue asked for a pause instead of being sent regardless; a 429 answer to one
  waits `Retry-After` and a 418 stops REST (the body's -1003 used to be read as a plain rate
  limit). `symbolConfig` is asked for the enabled instruments only.
- A session killed inside its start-up execution replay could lose a fill for good. A fill that
  reached it live (an order left resting by the dead session) was stored before the replay had
  read the trades before it; the next start resumed after the newest stored trade - on Binance
  in-process, after the highest trade id - and never asked for the earlier ones. The store now
  records when a venue's replay ended whole (`ReplayedRecord`, `session_venues.replayed_seq`,
  schema version 6), and a restart resumes from the last fill stored up to the newest session
  that got that far: by time on every venue, and on Binance from the trade id after it, skipping
  the ids the later sessions stored above it (`Venue::resume_known_trade_ids`). Sessions recorded
  before version 6 are judged as before (an order placed on the venue, or a clean shutdown).
- The store dropped a fill whose venue execution id another fill of the session already carried:
  the unique index was `(session_id, exec_id)`, so the same number on a second venue (OKX and
  Binance both count their trades), on a second symbol of a venue with per-symbol ids (Binance),
  or on the other side of a self-trade was ignored as a repeat. Schema version 5 keys the index
  `(session_id, instrument_id, side, exec_id)`, the OMS's own key; existing stores are migrated at
  open, and `fastmm-pnl` and `fastmm.open_store` still read a store that has not been.
  `fastmm-pnl duplicates` groups by side as well.
- `HedgeExecutor` (and `xmm`) sent a hedge larger than a `[risk]` per-order limit whole, so the
  engine refused it every time (`risk reject MaxOrderNotional`) and the position stayed unhedged
  until the executor halted. Each hedge and de-risk order is now cut to what `ctx.risk_headroom`
  admits - `max_order_qty`, `max_order_notional` at the order's price, the `max_position` room and
  the `[risk.underlying]` `max_net` room on its side - and the rest follows as the next orders, as
  a hedge over the instrument's `max_qty` already did. When the limits leave no room the
  instrument's minimums allow, the order still goes out whole and its refusal counts as a failure
  (failover, then halt).
- A restart booked executions twice after consecutive crashes. The start-up execution replay
  skipped only the trade ids stored by the newest session that held a fill of the venue, while its
  window (one second before that session's last fill) still reached executions stored by the
  sessions before it: after a `kill -9` seconds after a restart, the next start booked the first
  session's fills again, and the one after that the second's (wrong stored positions and PnL, and
  a hedge of a position that was not there until the venue reported its own). The ids are now
  those of every session of the engine (venue by name), for the venue-time resume, the
  engine-clock fallback and behind `fastmm-gateway`; Binance's per-symbol trade-id start is the
  highest id of every session. A session that may have died inside its own replay (no order
  placed on the venue, no shutdown recorded) no longer moves the replay's start past executions
  that replay had not reached. A store that already holds an execution in two sessions is
  reported, not rewritten: an error per execution in the start-up summary, a warning from every
  `fastmm-pnl` command, and `fastmm-pnl duplicates` (exit code 4) lists them.
- OKX: a bill whose `balChg` has more than 8 decimals (a funding payment, `-0.0008785476995335`) is
  rounded and booked; it failed the whole bills request before, so the funding was never booked and
  the request was repeated.
- OKX logged "trade channel -> Live" and "private channel -> Live" every 20 s through a session
  without a reconnect: a quiet private or trade channel goes Stale after `stale_ms` and back on
  the pong of the next ping, and the return was logged as a transition (it was already not
  reported to the engine). Only real transitions are logged now; Bybit, Deribit, Gemini and both
  Coinbase connectors had the same line.
- Binance spot with `[risk] check_balance`: a requote of a side the balance covers once (0.00059
  BTC, a resting ask of 0.0005) was refused `BalanceShort` after the cancel's ack, about 19 times
  an hour live, the ask missing until the next requote. The acks taken from WS API responses
  carried no venue time, so when the account's report reached the engine before the ack, the order
  stayed held twice until the next report and the cancel released only one of the two. The
  responses' `transactTime` now stamps those acks and cancel acks (cancelReplace's two legs, the
  amend too). A New the quote manager sends after its old order ended is checked against the
  balance first: if the old order filled instead of cancelling and the balance no longer covers
  it, it is withheld (`QuoteStats::kept_balance`) rather than refused, and the strategy's next
  quotes decide.
- The same unstamped response acks elsewhere: OKX (the order's `ts`), Bybit (`Timenow`, REST
  `time`), Deribit (`usOut`, rounded up to the ms), Coinbase Exchange (`created_at`) and Binance
  USDⓈ-M (`updateTime`) now stamp the acks and cancel acks they take from order responses. Coinbase
  Advanced Trade's responses carry no time. Quote orders held back because the balance no longer
  covered them are counted in the status (`balance_withheld` on the `rejects` line and in the JSON,
  status version 15) and as `fastmm_quotes_withheld_balance_total`.
- Binance USDⓈ-M (and Spot) books resynced on many starts ("book resync (sequence gap)" within
  half a second of connecting, the book back 2 s later). The depth snapshot was requested as the
  stream opened and could come back before the stream's first event and older than it; with
  nothing buffered it was applied, and the first event, which did not bracket it, broke the
  chain. A new stream's snapshot is requested once its first event is buffered, as the venue's
  procedure says, or 0.5 s after the stream opens if none has come (a book that does not change).
- An `[accounting.fx]` rate whose source is a quiet book (Binance sends a depth update only when
  the book changes) went unknown after `[risk] stale_md_ms` and refused orders that add exposure
  (`FxRateUnknown`, `GatewayFxRateUnknown`) on a healthy feed. FX rates have their own age limit,
  `[accounting] stale_fx_ms` (default 60000; 0: a valid book is enough); an invalid book still
  makes the rate unknown.
- Fill-check dropped a live fill reported after its order's end (Binance's API cancel response
  can arrive before the user stream's execution report; a reconciliation can miss an order that
  just filled) and counted a trade in a cancelled order's last millisecond that the venue did not
  fill it with.
- After a `fastmm-gateway` restart, a fill of an earlier session's order made while the gateway
  was down never reached its strategy: Binance's trade history names the venue's order id alone,
  which the new gateway had never seen, so a shared instrument's account booked it for nobody (or
  not at all) and an owned one's strategy booked it naming no order. The attach request (protocol
  7) carries the store's orders that were open at their last record with their venue ids
  (`Recovery::past_orders`, at most 256); executions no attached strategy received are kept (4096
  per venue) and given to the strategy that attaches owning them, whichever replay found them
  first; an owned instrument's account books nothing before its first owner's store seeds it (a
  replay met first used to be booked and then overwritten by the seed). The account's "older than
  the gateway" start is compared in the venue's clock, and connectors publish the clock offset
  their reference data measured. `ReplayScheduler`: a restart while a replay runs (a second attach)
  goes on from its start afterwards, where it used to be dropped or to replace the first's start.
- Behind `fastmm-gateway` with several strategies, a connector swept only the order shadows of
  the snapshot watermark's session epoch, so another strategy's orders whose end was lost stayed
  in its 8192-slot table for good; once it was full, orders went out untracked (no instrument on
  their replies, no amend, and a USD-ⓈM modify left the venue's later fills booked to the order it
  replaced). Shadows now carry the connector's send sequence and the sweep compares that; an order
  or replace the table has no room for is refused with the new `RejectReason::OrderTableFull` (39),
  counted in `VenueStatus::shadows_refused` and logged once per episode.
- An order a venue reported ended with its cumulative quantity before the execution arrived
  (Bybit's `order` and `execution` topics are not ordered; OKX pushes out of order) was booked
  twice: once from the cum_qty as a synthetic fill, again from the live execution. A live execution
  whose quantity lies under the booked cum_qty, or that has no cum_qty and arrives after the order
  ended, now corrects the estimate's price and fee instead.
- The WebSocket client and server accepted text messages that are not UTF-8 and close frames with a
  1-byte payload, a code that may not be sent (0-999, 1004-1006, 1015-2999, 5000 and up) or a
  reason that is not UTF-8: 86 of the 301 Autobahn cases failed. `WsMessageAssembler` now answers
  them with close code 1007 or 1002 (§5.5.1, §7.4, §8.1). The close-frame check applies on every
  connection, venue connections included: such a venue close now fails the connection with
  1002/1007 before the usual reconnect. Text is validated with
  `simdjson::validate_utf8` (`fastmm_net` links simdjson privately), per complete message, under
  `WsClientConfig::validate_utf8` and `WsServerConfig::validate_utf8` (default on).
  `ConnectionConfig::ws` turns it off: every venue handler parses text with simdjson, whose first
  stage rejects invalid UTF-8, so the venue receive path is unchanged (`bench_ws`
  `BM_WsAssembleUtf8` measures the check).
- `BinanceVenue::cancel_all()` gave up on a rate-limited refusal (418/429). The kill switch has no
  other remedy than that call, so it now retries a bounded number of times before reporting the
  failure.
- Binance USDⓈ-M charged one unit of IP weight for every WebSocket API order request. The endpoints
  charge 0 for a place and a modify and 1 for a cancel, with the order limits taking the other
  side, so at the venue's 1200 orders/minute the overcharge alone consumed half the 2400/minute IP
  budget and the connector refused to quote long before the venue would.

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
- Backtests on public market data. `--data <spec>`, `[backtest] source` and `run_backtest(data=)`
  resolve through a registry of named sources (`include/fastmm/backtest/data_registry.hpp`), each
  parsing its own options and declaring what it carries, so a new source is one file plus a
  registration and nothing about it reaches the configuration schema
  ([Market-data sources](docs/reference/data-sources.md)). Two archives ship:
  `binance:BTCUSDT,2024-03-27` reads data.binance.vision `bookTicker` + `aggTrades` (top of book
  and trades, USDⓈ-M futures 2023-05-16 to 2024-03-30), `tardis:binance-futures,BTCUSDT,2026-09-01`
  reads datasets.tardis.dev `incremental_book_L2` + `trades` (full L2; free on the first day of a
  month). `python3 -m fastmm.data fetch` downloads them into `$FASTMM_DATA_HOME` with the
  archive's own checksums and resumable transfers, `fastmm-data list` prints the sources and
  `fastmm-data convert` packs one into an `.fmj` that replays about twelve times faster.
  `configs/backtest-binance.toml` runs `basic_mm` on BTCUSDT perpetual at Binance USDⓈ-M VIP 0
  fees ([Backtest on real BTCUSDT data](docs/how-to/backtesting/binance-public-data.md)).
- `fastmm.data_sources()` and `fastmm.convert_data()`; `fastmm.data` is now a package
  (`fastmm.data.binance`, `fastmm.data.tardis`, `fastmm.data.Cache`), with `fastmm.load_csv`
  unchanged.
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
- A queryable record of every session, next to the journal ([ADR-0016](docs/adr/0016-storage-backends.md),
  `docs/reference/storage.md`). The engine hands fills, orders, position snapshots and kill events
  to a second `MsgRing` exactly as it does the journal (`include/fastmm/core/record_stream.hpp`),
  and a `fm-store` thread batches them into a storage backend. Backends are chosen by name from a
  free-form `[storage]` section (`backend = "sqlite"` by default; `"none"` allocates no ring and
  starts no thread) and sit behind `fastmm::store::StoreRegistry`, so one can be written out of
  tree without touching engine code and its own keys never reach the central schema. The SQLite
  backend (bundled amalgamation, WAL, `synchronous=NORMAL`) holds sessions, their journal parts and
  instruments, fills with venue ids, exec ids, fees, fee asset and liquidity, orders in their
  terminal state, position snapshots, per-day PnL by instrument and settlement currency, and kill
  events; the schema is versioned with one migration step per version and a store from a newer
  FastMM is refused. A full ring or a failing backend drops records and counts them rather than
  blocking the engine or stopping the session; a backend that cannot be opened stops the session
  before it trades.
- `fastmm-pnl`: `sessions`, `fills`, `orders`, `pnl`, `positions` and `recover` over a store, with
  `--since yesterday`, `--instrument`, `--session`, `--limit` and `--csv`
  (`docs/how-to/operations/query-trading-records.md`).
- `fastmm.open_store(path)` returns the same records as pandas DataFrames, with raw fixed point
  decoded to floats and nanosecond columns to UTC datetimes.
- `fastmm-live` logs what the previous session of the same `[engine] name` left behind before it
  starts: PnL, whether it shut down cleanly, the kill state, the journal parts, the last position
  per instrument and every order still open at its last record. It does not fetch execution history
  from a venue, so a fill from while the process was down appears only when the venue's
  reconciliation snapshot arrives.
- Journal durability and lifecycle: `[engine] journal_sync` (`async`, the previous behaviour, or
  `fdatasync`), `[engine] journal_max_bytes` (roll over into numbered parts, each a complete
  journal with the same session id) and `[engine] journal_retention_days` (delete older `.fmj`
  files in `journal_dir` at start-up). Extents are reserved with `posix_fallocate`, so a full
  filesystem is an error rather than a `SIGBUS` on a sparse page; `JournalFileWriter::failed()`
  latches every write error and `fastmm-live` trips the kill switch and exits with code 5 on one.
- `fastmm::research`, a feature and forward-markout extractor over any `MdSource`, so a signal can
  be judged before anyone writes a strategy around it. `extract_features()` drives the data into
  the engine's own `L2Book` and emits structure-of-arrays rows (mid, microprice, touch and sizes,
  imbalance, spread) plus the mid at configurable horizons, default 100 ms, 1 s, 10 s and 1 minute;
  a forward value past the end of the data stays unset and is counted, never substituted.
  `evaluate_signal()` reports the Spearman information coefficient and its stability over blocks, a
  decile table of the forward move, and the conditional touch markout: what a quote resting at the
  touch would have made or lost per signal bucket, in basis points. In Python,
  `fastmm.features(data=..., horizons=[...])` returns the columns as zero-copy numpy views and
  `fastmm.evaluate_signal(table, values)` takes a built-in feature name or an array.
  [Judging a signal before writing a strategy](docs/explanation/signal-research.md) publishes the
  BTCUSDT 2024-03-27 tables.

- **A control plane for a running session.** `fastmm-live` opens an `AF_UNIX` `SOCK_SEQPACKET`
  socket at `<journal_dir>/<engine name>.ctl` (mode 0600; `--control <path>`, `--no-control`) and
  the new `fastmm-ctl` talks to it: `pull` and `resume` for the session, one venue or one
  instrument, `param name=value` (validated against the strategy's schema before anything is
  published), `limits key=value` (the `[risk]` keys, applied with `RiskEngine::set_limits`),
  `flatten`, `kill`, `unkill`, `stop` and `status`. One datagram is a command and one is the reply,
  so `socat - UNIX-CONNECT:<socket>,socktype=5` works during an incident. Everything but `param`,
  `status` and `stop` goes through the engine's control ring, so the journal records it and a
  replay reproduces the session ([Operating a running
  session](docs/how-to/operations/operate-a-running-session.md)).
- **An engine-owned flatten.** `fastmm-ctl flatten [--instrument SYM] [--max-slippage-bps N]`
  (`ControlCommand::Flatten`) stops quoting in its scope, cancels its orders and, on an engine
  timer, sends reduce-only IOC orders priced that far through the touch until the position is gone
  -- without asking the strategy, which may be what broke. A slice is exempt from `[risk]
  max_position` and from self-trade prevention but not from the price collar or the other limits,
  and only one is in flight per instrument. `[engine] flatten_interval_ms`,
  `flatten_timeout_ms` (0 never gives up) and `flatten_slippage_bps` configure it; the state, the
  instruments left and the orders sent are in the status file, in `fastmm-top` and in Prometheus. A
  flatten that times out leaves the position and keeps quoting off; a restart abandons it.
- `ControlCommand::PullQuotes` and `ResumeQuotes` honour `hdr.instrument` and `hdr.venue`, so
  quoting can stop for one instrument or one venue while the rest of the session keeps trading, and
  `ControlCommand::SetLimits` carries a whole `RiskLimits` (`ControlLimitsMsg`, a `ControlMsg` with
  the limits after it). `tools/journal_dump.py` decodes control commands and names the engine
  timers.

### Changed
- **FIX 4.4 and CME MDP 3.0 are behind build options, off by default.** No connector drives either
  codec, so `-DFASTMM_CODEC_FIX=ON` and `-DFASTMM_CODEC_MDP3=ON` now decide whether they are
  compiled into `fastmm::codecs`; their tests and benchmarks follow the option, and their headers,
  fixtures and generated SBE flyweights stay in the tree
  (`docs/getting-started/install.md#optional-codecs`). The default build carries 8,631 fewer lines
  of source.
- **Connector plumbing lives in one place.** `include/fastmm/venues/connector_common.hpp` holds the
  `net::ConnState` mapping, response-header parsing, URL origins, the venue kill switch, the
  published-status read and the `extra`-key readers the five connectors each had a copy of, and
  `include/fastmm/venues/book_sync.hpp` holds `StreamBookSync<Traits>`, which `BybitBookSync` and
  `DeribitBookSync` are instantiations of. No behaviour change.
- **Venues are pluggable.** A connector is one entry in `fastmm::venues::VenueRegistry`
  (`include/fastmm/venues/registry.hpp`): the `kind` it answers to and its aliases, a one-line
  summary, the capabilities it declares (`credentials`, `order_entry`, `replace`, `positions`,
  `polls`), the `[venues.<name>]` keys it owns and a factory. `make_venue()` resolves through the
  registry, `VenueKind` and `venue_factory.hpp` are gone, and `fastmm-live` asks the registry
  whether a venue needs API keys instead of testing `kind == nasdaq_itch`. Adding a venue is one
  new translation unit plus a call in `register_builtin_venues()`; a venue outside FastMM registers
  the same way (`examples/external-venue/`, built against the installed headers).
- **A venue owns its configuration keys.** The 68 connector entries left
  `include/fastmm/config/schema.hpp` (811 lines and 154 entries down to 377 and 86) for the five
  connectors, where they became 92 keys with per-venue defaults. `fastmm::venues::validate_venues()`
  checks each `[venues.<name>]` section against its connector's declaration: a key the venue does
  not own warns with its line, a key of the wrong type stops the session. The key tables of
  [Configuration](docs/reference/configuration.md) are generated per connector from those
  declarations. `VenueSection::extra_lines` carries the line of every key the generic parser did
  not read.
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
- `PositionTracker::set()` takes the instrument and remeasures the unrealized PnL of the new
  position at the last mark; it used to leave the previous position's unrealized PnL in the totals
  the max-loss check reads.
- `fastmm-live` refuses to start when the instruments settle in more than one currency and
  `[risk] max_loss` is set (the PnL totals are one currency-less number); it warns otherwise.
- `Fixed::from_double()` saturates at `max()`/`min()` and maps NaN to zero; it used to return
  `INT64_MIN` for NaN and for anything out of range.
- `RiskEngine::on_book()` / `on_trade()` leave the collar and fat-finger bands unset when the band
  would overflow, instead of wrapping into a pass-through.
- `OmsUpdate::replaced_cl_ord_id` reports the client order id a completed cancel-replace superseded.
  The OMS renames its record in place, so that id never produced an update of its own and anything
  tracking orders by id saw it as open forever.
- `fastmm-replay` refuses a journal whose writer never closed it, or whose last block is damaged,
  instead of replaying a stream that stops short of what the session sent; `--allow-incomplete`
  replays what is there with a warning. `JournalReader::complete()` and `JournalInfo::complete`
  give the verdict that `truncated_tail()` and `has_trailer()` only hinted at.

### Added
- `order_transport = "user_tcp"` on every receive backend: the OUCH frames go through the
  backend's device. `af_xdp`: the XDP program also redirects TCP to `user_tcp_ip` (and
  `user_tcp_port`) and ARP for it, poll() hands those frames to the link (`net::FrameSink`), and
  the first socket on `user_tcp_interface` has a TX ring. `dpdk`: the RX burst demuxes ARP and
  the link's TCP, which sends with `rte_eth_tx_burst` on the same port. `user_tcp_port` fixes the
  local port, which lets the link use the host's own address on `af_xdp` and `dpdk` (the next
  hop's MAC then comes from the kernel's neighbour table on `af_xdp`).
- DPDK kernel exception path: `dpdk_exception_port` (a `net_tap` vdev) with
  `dpdk_exception_ip` gives the kernel an interface with the port's MAC behind a `vfio-pci` port;
  frames the venue does not take go to it and its frames leave through the port (ARP, GLIMPSE,
  re-requests, IGMP joins, kernel TCP). Read every `dpdk_exception_interval_us` (20). Without one,
  the source answers ARP for unicast line addresses.
- Unicast market data: a `nasdaq_itch` line may be a local unicast address (no join) on all three
  backends; `fastmm-sim-itch --line-a/--line-b` send to it. For networks without multicast.
- `scripts/bench-2host.sh`: the end-to-end benchmark across two hosts (simulator over ssh, unicast
  lines, kernel / af_xdp / dpdk, kernel or user_tcp OUCH). `scripts/host-setup.sh` prepares an
  Ubuntu 24.04 VM (packages, hugepages, irqbalance, interrupts, `vfio-pci` no-IOMMU bind and
  restore, AF_XDP queue setup, NIC report). `scripts/package-release.sh` and the `release-dpdk`
  preset build a portable tarball with DPDK linked in. `docs/how-to/operations/two-host-benchmark.md`.
- `scripts/bench-e2e.sh --md unicast`, `--dpdk-exception`, `--user-tcp-ip`, `--user-tcp-port`;
  the table is `scripts/bench-table.py`. ctest: `integration.nasdaq_itch_processes_unicast`,
  `dpdk.nasdaq_itch_processes_exception_user_tcp`, DPDK unicast/ARP and UserTcp echo cases.
  `scripts/xdp-test.sh --e2e` (root) adds the AF_XDP UserTcp cases and bench-e2e runs.
- Run-to-completion: `[engine] threading = "single"` (default `"split"`) runs the one venue's
  reactor, the engine and order sending on the engine thread, with no ring hop between the packet
  read and the order write (docs/explanation/architecture.md#run-to-completion). Market data reaches
  the engine as the venue commits it (`EventSink::set_drain_hook`, `Engine::drain()`), orders go to
  the new `Venue::send_now` through `LiveTransport::set_direct`, and `Engine::run_inline` /
  `IEngineRunner::run_inline` run the loop. More than one venue is a configuration error. Journals
  and replay are unchanged. `scripts/bench-e2e.sh --threading single|split --replay`; ctest runs
  both modes with `fastmm-replay --verify` (`integration.nasdaq_itch_processes[_single]`).
- Binance Spot SBE market data (`md_format = "sbe"`, `sbe_ws_url`): `@depth`, `@bestBidAsk` and
  `@trade` from the SBE stream host decoded by `BinanceSbeMdParser` into the same messages as the
  JSON feed; depth sync unchanged. Needs an Ed25519 API key (upgrade header only, works in a dry
  run). Decode per message: depth 10+10 levels 51 ns (JSON 680 ns), trade 19 ns (JSON 120 ns).
- Ed25519 keys for Binance USDⓈ-M (`session.logon` on the WS API order connection, unsigned
  orders after it, Ed25519-signed REST); `private_key_env` for both Binance connectors.
- `tools/sbe_gen.py`: `<data>` fields, implicit block lengths and `valueRef` constants
  (Binance `stream_1_0.xml`, generated into `venues/binance/generated/binance_stream_sbe.hpp`).
- fastmm-sim-exchange: Ed25519 accounts (`account.ed25519_public_key_file`), `session.logon` /
  `session.status` / `session.logout` and the unsigned `userDataStream.subscribe`.
- `net::HmacSha256Key` (precomputed pad midstates) and `net::Ed25519Key` (parsed once, sign and
  verify).
- `rx_backend = "dpdk"` for `nasdaq_itch` (`net::DpdkDatagramSource`, `-DFASTMM_WITH_DPDK=ON`, off
  by default): `rte_eth_rx_burst` on one port, Ethernet/802.1Q/IPv4/UDP parsed with
  `parse_udp_frame`, mbufs freed before `poll()` returns, IGMP joins through kernel sockets. DPDK
  comes from pkg-config or is built into the build tree by `scripts/build-dpdk.sh` (static 25.11, no
  root). Runs unprivileged with `--no-huge --no-pci --in-memory` and the `net_af_packet` vdev in a
  user namespace: ctest label `dpdk` (only in DPDK builds) and `scripts/bench-e2e.sh --backend dpdk`.
  Needs `spin_mode = "busy"`. Status `backend` 2.
- `order_transport = "user_tcp"` for `nasdaq_itch` OUCH (experimental): `net::UserTcp`, a
  single-connection user-space TCP client (ARP, MSS, RFC 6298 RTO, fast retransmit, out-of-order
  reassembly, zero-window probes, FIN/RST, RFC 5961 challenge ACKs) over `net::PacketRing`
  (`AF_PACKET` `PACKET_MMAP` RX/TX rings, `PACKET_QDISC_BYPASS`, BPF filter), with its own IPv4
  address (`user_tcp_ip`). Tested against a scripted peer and against the kernel's TCP over a veth
  with 3% loss each way; `scripts/bench-e2e.sh --order-transport user_tcp` and ctest
  `integration.nasdaq_itch_processes_user_tcp` trade through it. The OUCH session writes through
  `ByteLink` (`TcpLink` or `UserTcpLink`).
- `bench_order_tcp`: the send call of a kernel TCP socket against `UserTcp` over a veth. Results
  and a list of kernel-bypass order-entry options with the hardware each needs: `bench/README.md`.
- `[engine] timer_slack_ns` (PR_SET_TIMERSLACK for fastmm-live's threads; 0 keeps the kernel's
  50 us) and `[engine] lock_memory` (mlockall). `configs/profiles/production-latency.toml`: the
  `[engine]` settings for a dedicated host (busy spinning on isolated cores, timer slack 1 ns,
  locked memory); the shipped-config test loads `configs/profiles/` too.
- `scripts/build-pgo.sh [--compiler gcc|clang] [--bolt]`: PGO build of `release-native` trained on
  the hot-path benchmarks, a synthetic backtest and `scripts/bench-e2e.sh`; `--bolt` rewrites
  `fastmm-live`, `fastmm-sim-itch` and three benchmarks with `llvm-bolt` (instrumentation mode;
  llvm-bolt is unpacked from the distribution package without root when not installed).
- `HotArray` (`core/hot_array.hpp`): zeroed, resident, 2 MiB-page tables; `CounterKeyMap` for keys
  handed out by counters; `net::HmacSha256` with a precomputed key.
- Benchmarks `BM_Ouch50_EncodeNewIds`, `BM_Ouch50_EncodeColdMap`, `BM_Ouch42_EncodeNewIds`,
  `BM_ItchL2Bridge_Message_LargeBook` (added as `_DefaultBook`, renamed when it was given a working
  set that matches the book); `scripts/bench-e2e.sh --timer-slack`.
- Nasdaq TotalView-ITCH venue (`kind = "nasdaq_itch"`, ADR-0015 section 5; docs/reference/venues.md,
  docs/how-to/operations/multicast-feeds.md, `configs/nasdaq-itch-sim.toml`): lines A and B over the
  `kernel` or `af_xdp` datagram source, `moldudp::Receiver` arbitration and re-requests, one
  `L3Book` per instrument through `ItchL2Bridge`, T0 per receive batch and `recv_ts` from the kernel
  receive time. Joins mid-stream from a GLIMPSE snapshot plus a recovery buffer
  (`recovery_buffer_packets`, allocated at start); an unrecoverable gap or any L3 book error rebuilds
  the books from GLIMPSE, and the buffer overflowing during two snapshots in a row trips the venue's
  kill switch with the new `KillReason::FeedLost`. `order_entry = "none"` rejects orders
  (`VenueReject`); `"sim_ouch"` trades OUCH 5.0 over SoupBinTCP with `fastmm-sim-itch`, naming the
  triggering ITCH sequence number in the ClOrdID. No API keys are needed for this kind.
- `Venue::poll()`: called by the network thread after every reactor iteration; `nasdaq_itch` polls
  its sockets there with `spin_mode = "busy"`.
- Status file version 4: p99.9 in every latency, and a multicast feed block per venue (packets per
  line, A/B skew, gaps, recovered and given-up sequences, snapshots, reorder high-water mark,
  kernel-to-T0 histogram, XDP statistics and mode). `fastmm-top` shows p99.9 and a feed line, and
  `--json` prints the snapshot as JSON. `WireLatencyStats` carries p99.9 and max.
- `scripts/bench-e2e.sh`: `fastmm-sim-itch` and `fastmm-live` in two network namespaces joined by a
  veth pair, pinned to separate cores; prints wire-to-wire, kernel-to-T0, the engine hops and the
  network thread's tick-to-trade at p50, p99 and p99.9 (`--backend af_xdp` needs root). ctest runs it
  for 5 s unpinned (`integration.nasdaq_itch_processes`).
- `moldudp::Receiver::reset()`: resume delivery at a given sequence number (the End of Snapshot
  sequence); the hole up to the highest sequence seen is requested as a gap.
- `ItchL2Bridge::set_stamp()`: the receive stamp of the snapshot and state events `mark_*()` emits.
- `fastmm-sim-itch` times Replace Orders whose ClOrdID is a sequence token, as it times Enter Orders
  (`host::ReplaceView::seq_token`).
- `fastmm-sim-itch` (ADR-0015, section 6; `configs/sim-itch.toml`, docs/reference/sim-itch.md): a
  Nasdaq-style simulator. `MatchingEngine` and `MarketGenerator` per symbol, engine effects
  published as ITCH 5.0 (A, E, X, D; opening spin O, R, S, Q, H), packed into MoldUDP64 and sent
  to lines A and B with `sendmmsg`, with seeded per-line drops, token-bucket pacing and bursts,
  heartbeats and End of Session. It answers MoldUDP64 re-requests from a ring history, serves
  GLIMPSE 5.0 snapshots (R, H, A per resting order, End of Snapshot) consistent with the stream,
  and accepts OUCH 5.0 Enter / Replace / Cancel over SoupBinTCP (Accepted with the ITCH order
  reference, Replaced, Canceled, Executed with the ITCH match number, Rejected; cancel on
  disconnect). Orders whose ClOrdID is a sequence token are timed wire to wire, from the
  `sendmmsg` of the datagram carrying that sequence number to the read that returned the order;
  `--summary-json` writes the histogram. `--cpu`, `--busy-poll`, `--duration` and the bind and
  port flags serve `scripts/bench-e2e.sh`.
- `codecs::itch::glimpse` (`itch/glimpse.hpp`): End of Snapshot `G` and `GlimpseClient`, a
  SoupBinTCP client session that hands every snapshot message and the End of Snapshot sequence
  number to a handler.
- OUCH 5.0 host side: `host::parse_enter()`, `parse_replace()`, `parse_cancel()`, and the
  sequence token `put_seq_token()` / `parse_seq_token()` (ClOrdID `T` + 13 digits).
- `sim::itch::ItchPublisher` (`sim/itch/itch_publisher.hpp`): `MatchingEngine` effects as ITCH
  messages, with reference and match numbers shared across symbols. The ITCH property and L2
  bridge tests use it instead of their own publisher.
- `moldudp::TransmitterConfig::overwrite_oldest`: a ring history that evicts the oldest messages;
  `Transmitter::oldest()`, `evicted()`, and a message limit for `next_packet()`.
- `MatchingEngine::for_each_resting()`: resting orders best level first, in queue order.
- AF_XDP multicast receive (ADR-0015, section 3): `net::XdpDatagramSource`
  (`net/xdp_datagram_source.hpp`) opens one XDP socket per (interface, RX queue) with its own UMEM,
  fill and RX rings, attaches a BPF filter per interface through `BPF_LINK_CREATE` (native with a
  zero-copy bind, native with a copy bind, then generic; `xdp_mode` pins one), joins each group with
  a kernel socket for IGMP, and delivers UDP payloads with `RxMeta` (T0 per batch, line index).
  `poll()` allocates nothing and returns RX descriptors to the fill ring before it returns.
  `open()` fails with the missing capabilities and the `setcap` command, before Linux 5.11, and with
  `-EBUSY` when an interface already has an XDP program. Statistics: `XDP_STATISTICS`, the per-CPU
  count of packets passed to the kernel for lack of a socket on their queue, bad frames, the chosen
  mode, and busy-poll options the kernel refused. The filter is BPF bytecode built in C++
  (`net/bpf_asm.hpp`, `net/xdp_program.hpp`) and loaded over raw `bpf(2)`; no libbpf, libxdp or
  BPF compiler. `net/udp_frame.hpp` parses Ethernet/802.1Q/IPv4/UDP frames and optionally verifies
  checksums (`bench_udp_frame`).
- `scripts/xdp-test.sh` (run with `sudo`) runs the privileged AF_XDP tests: verifier load,
  `BPF_PROG_TEST_RUN` against crafted frames compared with the parser, and receive over a veth pair
  in generic and native copy modes. Without privileges `ctest` skips them; the parser, the
  assembler encodings and the program (under a small BPF interpreter) are tested unprivileged.
- UDP multicast receive (ADR-0015, step 1): `net::UdpSocket` (any-source and source-specific
  joins with the interface by name or address, `SO_RCVBUF`, `recvmmsg`/`sendmmsg`, `send_to`,
  `IP_MULTICAST_IF`/`TTL`/`LOOP`, `SO_TIMESTAMPING`, and `SO_BUSY_POLL`, `SO_PREFER_BUSY_POLL` and
  `SO_BUSY_POLL_BUDGET` setters that return the errno), `net::enable_hw_timestamps(ifname)`
  (`SIOCSHWTSTAMP`, not called by default), the `net::DatagramSource` concept with `net::RxMeta`
  (`net/datagram_source.hpp`), and its `kernel` backend `net::KernelDatagramSource`: one socket
  per subscription (interface, group, port, optional source), batches of `recvmmsg` into buffers
  allocated at `open`, kernel and NIC receive timestamps, T0 as `rdtscp` plus a `CLOCK_REALTIME`
  read per batch, oversized datagrams counted and dropped. Busy-poll options that the process may
  not set are reported by `open` and do not fail it. Multicast tests run in an unprivileged user
  and network namespace and pass with a message where none can be created; `bench_udp` measures
  unicast and multicast receive on loopback.
- Binance USDⓈ-M perpetual futures connector (`kind = "binance_usdm"`,
  `binance_usdm::BinanceUsdmVenue`) and `configs/binance-usdm-demo.toml` for Demo Trading: depth
  sync with `pu` chaining on the `/public` stream, `bookTicker` and `aggTrade` (`/market`), orders
  over the WebSocket API (`order.place`, `order.cancel`, `order.modify`) with a REST fallback,
  post-only as GTX, `reduceOnly`, the listenKey user stream (`ORDER_TRADE_UPDATE`,
  `ACCOUNT_UPDATE`), reconciliation of open orders and positions, a check of `ACCOUNT_UPDATE`
  positions against the fills, and a read-only account check that refuses hedge mode. New
  connector key `position_from_account_update`. Market-data fixtures are recorded on Demo Trading;
  the private payloads are hand-written from the documentation because the Demo account had no
  futures margin balance. Funding payments are not booked. `binance::BinanceDepthSync` is now
  `BasicBinanceDepthSync<BinanceSpotSyncTraits>`.
- `codecs::itch::ItchL2Bridge` (ADR-0015, step 3): ITCH messages to one `L3Book` per configured
  instrument to `BookSnapshotMsg` / `BookDeltaMsg` / `TradeMsg` / `ConnectionStateMsg` for the
  engine. At most one delta per instrument per datagram (`end_datagram()`) with absolute level
  quantities over the top `depth` levels; trades for P/Q, E and printable C; `mark_complete()` /
  `mark_incomplete()` for snapshot recovery; T0 from a per-datagram `DatagramStamp`. Messages for
  unconfigured locates are skipped after the header. `ItchDecoder::decode_into()` and
  `ScratchSink` decode one message without a ring.
- Slow methods in live sessions (ADR-0013, sections 1 and 4): `fastmm.run_live` and
  `python -m fastmm run` run `on_start` before any venue connection, the `@fastmm.every` methods
  on a `fastmm-slow` thread and `on_stop` after the session, with snapshots, recent rows and fills
  from the engine as in backtests. The control thread's watchdog
  (`live/slow_watchdog.hpp`) stops the session with exit code 7 when a slow method raises, a call
  runs past its `timeout`, the fills ring is full or the slow thread ends, and logs
  `fastmm-live: slow tier failed (<cause>)`. `run_live` gains `fills_capacity`, `recent_rows` and
  `slow_tier_timeout_ms`; `python -m fastmm run` gains `--slow-tier-timeout-ms` and exits with
  `os._exit` when the slow thread does not end in time. A class with slow methods defaults
  `max_param_age_ms` to 3 periods (at least 1000 ms) live as in backtests, and live journals record
  the starting parameters and `max_param_age_ms`, so `fastmm.replay` replays them.
- Python strategies live (ADR-0013, section 3): `fastmm.run_live(StrategyClass, config, params=None,
  ...)` and `python -m fastmm run module:Class --config file.toml` run a class with hot hooks in the
  `fastmm-live` session from `fastmm_live._live`, with the GIL released, and return its exit code.
  Hooks compile (exit code 3 on failure) and warm up before any venue is contacted; a failing hook
  trips `StrategyError` (exit code 6 with `on_kill = "exit"`). The session moves every other thread
  of the process off the CPUs pinned in `[engine]`, runs one per process (`RuntimeError`) and is
  inert in a forked child. `strategy.publish` sends parameter updates through the session's slow
  channel (`fastmm_live._live.SlowChannel`), the only producer on its ring. New exit code 7 (`kExitSlowTier`) and `LiveOptions::watchdog`, `strategy`
  (`LiveStrategy`) and `confine_other_threads`; `run_live` restores the SIGINT/SIGTERM handlers it
  replaced.
- Journal format 3 gains optional strategy metadata (`meta_bytes`, `meta_crc32c`; `key=value` lines)
  after the parameter table: a Python session records the class, a hash of the hot-hook source and
  the package versions. `inspect_journal` returns it as `strategy_meta`. Journals without it read
  unchanged.
- `HotStrategy` applies `ParamUpdate` messages to its parameter blocks (`HotProgram::params`,
  `strategies/hot_params.hpp`), and `ParamPublisher` takes a parameter schema built at run time.
- Python hot hooks in backtests (ADR-0013, section 1): `@fastmm.hot` methods (`on_book`, `on_fill`,
  `on_quoting`, `on_connection`, and timer hooks with `every=`) compiled by Numba and called by the
  engine thread through the C ABI in `strategies/hot_abi.h`, with `fastmm.State`, `fastmm.fx` and
  the `fastmm-engine[hot]` extra. A failing hook trips the kill switch with the new
  `KillReason::StrategyError`; `StrategyContext::trip_kill(reason)` is new.
- Python slow methods in backtests (ADR-0013, sections 1 and 2): `on_start`, `on_stop` and
  `@fastmm.every(period, timeout=)` methods beside hot hooks, with `ctx.snapshot()`,
  `ctx.recent(inst)`, `ctx.fills()`, `ctx.publish(inst=None, **values)`, `strategy.publish()` from
  any thread and a hot `on_params` hook. They run at simulated times; `run_backtest` gains
  `slow_delay_ms`, `max_param_age_ms`, `fills_capacity` and `recent_rows`, and
  `BacktestResult.slow_methods` reports their wall time. `fastmm.replay(journal, MyMM)` replays a
  hot strategy from a journal's parameter updates. The engine side is `strategies/slow_channel.hpp`
  (snapshot seqlock, recent rows, fills ring, watchdog state, parameter sink); `HotStrategy` applies
  per-instrument updates. `sim::SimDriver::set_slow_hooks`, `sim::ParamSchedule::threaded_sink`,
  `bt::ReplayStrategy` and `BacktestConfig.max_param_age_ms` (Python) are new. Backtests with
  `journal_out` record the strategy metadata with the starting parameters and `max_param_age_ms`.

### Changed
- **Benchmark harness: what the published numbers mean.** Several of them measured the harness
  rather than the code, so they were corrected and everything was re-measured (bench/README.md,
  docs/explanation/benchmarks.md).
  - A benchmark declares how many cores it needs (`FASTMM_BENCH_NEEDS_CORES`, `bench/bench_pin.hpp`)
    and fails if it is given fewer; `scripts/bench.sh` runs those unpinned in a second pass.
    `BM_ReactorEchoThread` was pinned to the same core as its echo thread: its busy-polling rows
    published 8.0 ms per round trip against 11 µs unpinned.
  - `SimTransportConfig::hash_outbound` makes the simulator's outbound SHA-256 optional.
    `BM_TickToOrder_Sim` now runs without it, since it is a determinism check of the simulator and
    not engine work; `BM_TickToOrder_SimHash` keeps it and shows what it costs (about 180 ns of a
    300 ns figure, two messages per tick). The "p50 991 -> 247 ns" improvement recorded below was
    therefore in large part the benchmark's own checksum getting faster, not the engine.
  - `BM_TickToOrder_Sim` and `BM_EngineStep_Sim` use `UseManualTime`: the reported time is the
    `rdtsc` interval around the tick. They used to report Google Benchmark's own per-iteration time
    with `PauseTiming`/`ResumeTiming` around the untimed settle loop, which added about 600 ns
    to a roughly 300 ns operation. That inflated figure was what `bench/ci_budget.toml` gated on.
  - `scripts/bench.sh` keeps every repetition (and `--rounds N` full passes of the suite);
    `tools/bench_table.py` publishes the median over them and the min-to-max spread, plus CPU time
    next to wall time. Google Benchmark's `stddev` aggregate, the standard deviation of five
    repetition means, is gone from the table: it read `0.0 ns` on more than sixty rows.
  - `BM_Risk_CheckNew_Pass`, `BM_Fixed_Mul`, `BM_Fixed_RoundToTick` and `BM_L2_UpdateNearTop` chain
    each result into the next iteration, so they measure the latency of one operation instead of how
    many independent ones the pipeline overlaps: `BM_Fixed_RoundToTick` 0.7 -> 3.6 ns and
    `BM_Fixed_Mul` 0.8 -> 2.2 ns, while `BM_L2_UpdateNearTop` (2.2 ns) and `BM_Risk_CheckNew_Pass`
    (6.5 ns) do not move, because the book's memory barrier and the risk checks' own data flow
    already serialised them. The benchmarks that stay throughput measurements say so in their
    source.
  - `BM_ItchL2Bridge_Message_LargeBook` replaces `_DefaultBook`: it drives the 2^20-order book with
    200,000 to 260,000 resting orders. `_DefaultBook` configured the big book but replayed a stream
    with 2,000 to 6,000 live orders, so it measured the same time as the small book while the docs
    claimed it showed cache and dTLB misses.
  - `bench/ci_budget.toml` budgets are set from the measured time on the reference machine rather
    than at twice it, `tools/check_budgets.py` compares the fastest repetition and allows 10 % by
    default instead of 25 %, and CI runs the check over a subset of the benchmarks.
- HMAC-SHA256 signing no longer fetches an OpenSSL provider and allocates per call: Binance order
  encode 1628 -> 736 ns (`BM_Encode_BinanceOrderPlace`). Ed25519 keys are parsed once, not per
  signature.
- The simulator's outbound SHA-256 (replay proof) uses the x86 SHA extensions when present: it was
  half of `BM_TickToOrder_Sim` (p50 991 -> 543 ns, p99 1279 -> 671 ns). That benchmark runs through
  `SimTransport` and never signs a Binance request. It no longer hashes at all (see the harness
  entry above), so that half of the gain was a benchmark artefact: the engine never did this work.
- Hot paths (bench/README.md, "Hot-path changes"): OUCH 5.0 encode in bench-e2e 4.4 us -> 0.1 us
  p50 and wire to wire 35 -> 25 us p50; `BM_TickToOrder_Sim` p50 991 -> 247 ns; `BM_EngineStep_Sim`
  9.2 -> 2.9 us; Binance order.place encode 1440 -> 523 ns; ITCH bridge with the default book 84 ->
  55 ns per message. Outbound hashes and journals are unchanged. Two of those figures are measured
  differently now: the tick-to-order pair no longer includes the simulator's SHA-256 or Google
  Benchmark's pause overhead, and the default-book row was measured with a working set that fit L2.
  - `OpenHashMap` keeps the occupancy flag in the slot and allocates a `HotArray`: no page fault on
    the first insert into a page (every new OUCH order paid one), one cache line per probe.
  - `ouch50::UserRefMap` is direct-mapped (`CounterKeyMap`); the OUCH 4.2 / 5.0 encoders write
    messages in place; `write_cl_ord_id()` formats the ClOrdID with SWAR hex.
  - `PositionTracker` keeps realized / unrealized / fee totals; `net_pnl()` no longer sums every
    instrument on each market-data event.
  - Fixed-point products and quotients stay in 64 bits unless they overflow (`detail::mul_div`).
  - The simulator hashes outbound orders with SHA-NI when available, its order scheduler heaps keys
    instead of 200-byte payloads, and acks format ids with `std::to_chars`.
  - L3 book index, orders and levels are `HotArray`s; `ItchL2Bridge` prefetches the index slot of
    the order a message names.
  - Binance and Bybit signers key HMAC-SHA256 once; `JsonWriter` and `QueryBuilder` append in bulk
    and `QueryBuilder` no longer zeroes its buffer.
- `release-native` stays gcc `-O3 -march=native` with LTO (bench/README.md compares `-O2`, no LTO,
  `x86-64-v2`, clang 18, PGO and BOLT).
- Order sends are coalesced on the network thread: `on_wake()` of every venue drains the outbound
  ring through `drain_outbound_coalesced()` and writes all orders of the drain with one system call.
  `TcpLink::cork()` / `uncork()` (SoupBinTCP/OUCH: header and message no longer go out in two writes
  per order) and `WsClient` / `net::Connection` / `ConnectionSlot` `cork()` / `uncork()` (WebSocket
  frames of one drain in one TLS record write); a short write or `EAGAIN` stays queued for
  writability. `WireLatencyRecorder::begin_batch()` / `end_batch()` stamp every order of a drain with
  the return of that write. With `spin_mode = "busy"` the engine no longer writes the network
  thread's eventfd; it sets the wake flag (release) that the busy loop checks. `bench-e2e.sh` over
  veth (WSL2): wire to wire p50 47 to 55 µs -> 30 to 35 µs, T0 to T5 p50 4.6 to 5.4 µs -> 2.6 to
  3.1 µs, T0 to OUCH write p50 30 to 39 µs -> 20 to 26 µs (bench/README.md).
- `moldudp::Receiver` (ADR-0015, step 2): A/B arbitration, a reorder buffer and
  `gap_timeout_ns`. `on_packet(line, datagram, now_ns, meta)` replaces `on_packet(datagram)`
  (`on_packet(datagram, now_ns)` is line 0); requests are stamped with the packet time instead of
  the last `on_timer()` tick. `Receiver<H, Meta>` passes `meta` to `on_message(seq, msg, meta)`
  when the handler takes it, also for messages drained later. Packets ahead of a gap are copied
  into a `ReorderBuffer` (`reorder_packets`, default 256, of `max_packet_bytes`) instead of
  dropped; a gap is declared after `gap_timeout_ns` (default 2 ms) or when the buffer is full,
  and requested one hole at a time. New `ReceiverConfig` fields `max_request_attempts`,
  `can_request` and `follow_session`; an optional `on_gap_unrecoverable(from_seq, count)` reports
  gaps that are given up. `ReceiverStats` gains per-line packets, duplicates and A/B skew, and
  held, overflow, unrecoverable and session counters.
- `L3Book` is no longer a template: capacity and price window are constructor arguments
  (`L3BookConfig{price_window_ticks, max_orders, max_overflow_levels}`), allocated once. Orders
  outside the window go to a bounded per-side overflow store instead of failing with
  `OutOfWindow` (now returned only when that store is full), and the window recentres only when a
  touch leaves it. A bitmap of non-empty levels speeds up best-level repair and depth walks.
  `find()`, `at()` and handle forms of `execute()` / `cancel()` are new.
- `OrderExecL3Msg` gains `exec_flags` with `kNonPrintable`: `ItchDecoder` keeps the Printable flag
  of C messages (zero, the old padding, reads as printable).
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
