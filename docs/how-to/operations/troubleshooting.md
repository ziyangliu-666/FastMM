# Troubleshooting

Messages are quoted as the code writes them; `<...>` stands for a value, and `<venue>` is the `[venues.<name>]` name. Log lines start with a timestamp, the level (`INFO`, `WARN`, `ERROR`), the thread id and the source file. Messages marked *stderr* are printed before the logger starts. Exit codes of `fastmm-live` are listed in [Command lines](../../reference/cli.md#exit-codes).

## Startup and configuration

| Message | Cause | Action |
|---|---|---|
| `fastmm-live: --config is required` (stderr) | No config file given | Pass `--config configs/<file>.toml` |
| `fastmm-live: venue '<venue>' needs API keys: environment variable <VAR> is not set` (stderr) | A `${VAR}` in `api_key` or `api_secret` is not exported | Export the variable ([Run on a testnet](run-on-testnet.md#2-keys-in-the-environment)) or add `--dry-run` |
| `fastmm-live: venue '<venue>' has no api_key/api_secret` (stderr) | The keys are empty and this is not a dry run | As above |
| `fastmm-live: venues.<venue>.<key>: environment variable <VAR> is not set` (stderr) | A `${VAR}` in a URL or extra key is not exported | Export it or write the value into the config |
| `config: unknown key '<section>.<key>' ignored (line <n>)` | The schema does not know the key; connector-specific keys also produce it and are applied ([Venue connectors](../../reference/venues.md#configuration-keys)) | Check the spelling against [Configuration](../../reference/configuration.md) |
| `fastmm-live: unknown strategy '<name>' (available: ...)` (stderr) | `[strategy] name` or `--strategy` is not registered in this app | Pick one from `./build/release/bin/fastmm-live --list-strategies`, or [register it](../strategies/register-a-strategy.md) |
| `fastmm-live: <strategy>: unknown parameter '<key>' (see --list-strategies)` (stderr) | `[strategy.params]` or `--param` names a parameter the strategy does not have; after `--strategy` switches strategies the config's parameters are ignored | Use the names from `--list-strategies` |
| `<program>: strategy '<name>' (<kind>) is already registered by different code; ...` (stderr, exit 3) | Two strategy libraries in the app use the same strategy name, or one reuses a built-in name | Rename one of the strategies |
| `fastmm-live: no [[instruments]] configured` (stderr) | The config has no instruments | Add `[[instruments]]` tables |
| `fastmm-live: cannot open journal <path>` (stderr) | The journal file cannot be created | Check the directory permissions and free space, or pass `--journal <path>` |
| `cannot create session epoch directory <dir>: <error>` | The parent directory of `[engine] epoch_file` cannot be created; without the epoch file, client order ids can repeat across restarts | Fix the path |
| `[engine] net_backend = "io_uring" but io_uring is not available (kernel too old, disabled or not permitted); falling back to epoll` | The kernel refuses io_uring (`kernel.io_uring_disabled`, seccomp, an old kernel); the session runs on epoll | Set `net_backend = "epoll"` |
| `fastmm-live: fatal: <error>` | An uncaught error in the session; exit code 5 | Check the lines before it |

## Python strategies

`python -m fastmm run` and `fastmm.run_live` print these on stderr; the session's log lines are the ones above.

| Message | Cause | Action |
|---|---|---|
| `fastmm.run_live needs the live runtime (fastmm_live); install it with: pip install "fastmm-engine[live]" (from source: <url>)` (ImportError) | The live runtime is not installed | Install it; from a checkout, `pip install ./python/live` ([Install from source](../../getting-started/install.md#install-from-source)) |
| `fastmm: py:<Class> has no @fastmm.hot methods; a strategy runs live only with hot hooks` (exit 3) | The class defines `fastmm.Strategy` hooks only | Write the hooks as [hot hooks](../strategies/python-hot-hooks.md) |
| `fastmm: <Class>.<hook> is rejected by the IR check: ...`, `fastmm: <Class>.<hook> does not compile in Numba nopython mode: ...` (exit 3) | A hot hook does not compile; no venue was contacted | Fix the hook ([What compiles](../../reference/python-api.md#what-compiles)) |
| `fastmm: note: ignoring [strategy.params] of '<name>' for py:<Class>` | `[strategy] name` names another strategy, so its parameters do not apply to the class | Set `name = "py:<Class>"` or pass the parameters with `--param` |
| `fastmm: cannot load strategy '<module>:<Class>': <error>` (exit 3) | The module is not importable from the current directory or `PYTHONPATH`, or has no such class | Check the name and `PYTHONPATH` |
| `fastmm: py:<Class>.<hook> raised an exception (engine time <n> ns); kill switch tripped: StrategyError` (exit 6 with `on_kill = "exit"`) | The hook raised, for example an index outside a book array | Reproduce it in a backtest with the session's journal |
| `fastmm: a live session is already running in this process; run one session per process` (RuntimeError) | `run_live` was called while another session runs | Run the second session in another process |
| `fastmm: this process was forked while a live session was running and cannot run one; ...` (RuntimeError) | A child process started with `fork` during a session | Use the `spawn` or `forkserver` start method |
| `thread affinity unchanged: every CPU this process may use is pinned in [engine] cpu or net_cpus` | No CPU is left for the process's other threads; they keep their affinity | Leave at least one CPU unpinned |

## Reference data

| Message | Cause | Action |
|---|---|---|
| `<venue>: exchangeInfo failed (<error>); keeping configured tick/lot` (Binance), `<venue>: instruments-info failed (<error>); keeping configured tick/lot` (Bybit), `<venue>: get_instruments <currency> <kind> failed (<error>)` (Deribit) | The REST reference-data request failed at startup; unless `allow_offline_reference_data = true` the start fails with exit code 4 | Check `rest_url` and the network |
| `<venue>: <symbol> tick/lot from exchangeInfo override config (<tick> / <lot> -> <tick> / <lot>)` (Binance), `... from instruments-info ...` (Bybit) | The venue's tick or lot differs from the config; the venue's values are used | Update the tick and lot in the config |
| `<venue>: <symbol> status is <status> (not TRADING): disabled` (Binance), `(not Trading)` (Bybit), `<venue>: <symbol> is not open for trading (state <state>): disabled` (Deribit) | The symbol is halted, delisted or expired | Choose another symbol |
| `<venue>: <symbol> not in reference data; keeping the configured values` (Deribit) | The instrument name does not exist, typically an expired option | Replace the `[[instruments]]` with live names ([Run on a testnet](run-on-testnet.md#deribit-testnet)) |
| `<venue>: <symbol> does not allow LIMIT_MAKER (post-only)` (Binance) | Post-only orders on this symbol are rejected | Choose another symbol, or quote without `post_only` |

## Clock

| Message | Cause | Action |
|---|---|---|
| `<venue>: clock offset to venue is <n> ms`, `... (recvWindow <m> ms)` (Binance), `... (recv_window <m> ms)` (Bybit) | The local clock differs from the venue's by more than 1000 ms; signed requests are rejected once the offset nears `recv_window_ms` | Synchronise the system clock (chrony or systemd-timesyncd; on WSL2 check the Windows host's time). Raising `recv_window_ms` is only a stopgap |
| `host wall clock stepped by <n> ns relative to CLOCK_MONOTONIC_RAW within <t> s; the engine clock follows it` | The system clock was stepped: NTP, or the hypervisor on WSL2 and VMs (several hundred times an hour in WSL2 sessions) | On bare metal, make the time daemon slew instead of step |
| `TSC recalibration stepped the engine clock by <n> ns (threshold <m> ns)` | The engine clock's offset exceeded the threshold, so it was stepped instead of slewed ([Clock calibration](../../explanation/architecture.md#clock-calibration)) | As above |
| `TSC recalibration skipped (no TSC mapping or baseline too short)` | The calibrator had no TSC mapping yet or too short a baseline; the previous calibration stays in use | If it repeats at every `[engine] tsc_recalibrate_s`, raise that period |

## Connectivity and market data

| Message | Cause | Action |
|---|---|---|
| `<venue>: md channel lost`, `<venue>: private channel lost` (Bybit, Deribit), `<venue>: trade channel lost` (Bybit) | A WebSocket connection dropped; the connector reconnects with backoff, quotes are pulled, and orders are cancelled over REST when the order channel is lost | If it repeats, check the network, DNS and the endpoint URL |
| Status line stays `md=conn` or `md=down`, or `books=0/1` | The market-data connection or the book never came up | Check `ws_url`; try `--dry-run` |
| Status line shows `md=stale` and quotes are pulled during quiet markets | No traffic for `stale_ms` ([Venue connectors](../../reference/venues.md#configuration-keys)) | Raise `stale_ms` |
| `<venue>: malformed market-data frame (<n> so far)` | A frame did not parse; it is counted in `malformed=` | If it grows, record frames with `--record-raw <dir>` and report it: the venue's format may have changed |
| `<venue>: depth snapshot for <symbol> failed: status=<s> err=<e>` (Binance) | The REST depth snapshot for a resync failed; it is retried | Check REST access and rate limits |
| `<venue>: market-data subscribe rejected: <msg>` (Bybit), `<venue>: market-data subscription incomplete or rejected: <msg>` (Deribit) | The venue refused a subscription, usually an unknown symbol | Check the `symbol` of the instrument |

## Keys and authentication

| Message | Cause | Action |
|---|---|---|
| `<venue>: session.logon failed: <code> <msg>`, `<venue>: userDataStream.subscribe failed: <code> <msg>` (Binance) | The WebSocket API refused the key: a key from another environment ([Run on a testnet](run-on-testnet.md)), or a `key_type` that does not match it | Use a key of this environment with the matching `key_type` |
| `<venue>: private auth failed: <msg>`, `<venue>: trade auth failed: <code> <msg>` (Bybit) | Bybit refused the key or signature | Check the key, its permissions, its IP allowlist and the clock |
| `<venue>: could not send the auth request` (Bybit), `<venue>: could not send public/auth` (Deribit) | The connection closed before the request was written | If it repeats, see `channel lost` |
| `<venue>: public/auth failed: <code> <msg>` (Deribit) | Wrong client id or secret, or a main-net key on the testnet | Use a key created on test.deribit.com |
| `<venue>: token refresh failed (<code> <msg>); re-authenticating` (Deribit) | The refresh token was refused; the connector logs in again | If `public/auth failed` follows, see that row |
| `<venue>: fatal venue error (<code> <msg>); order entry disabled`, then `<venue>: asking the engine to kill this venue (VenueFatal)` | Bad key, bad signature, missing permission or failed authentication; the venue's kill switch trips ([Venue kill switch](kill-switch-and-shutdown.md#venue-kill-switch)) | Cancel that venue's orders on its website (the key cannot), fix the key, restart |

## Rate limits

| Message | Cause | Action |
|---|---|---|
| `<venue>: rate limited (<code> <msg>); cooling down <n> ms` (Binance, Bybit), `<venue>: rate limited (<code> <msg>)` (Deribit) | The venue signalled a rate limit; the connector sends nothing for the cooldown | Lower `[risk] orders_per_sec`, raise `[engine] min_requote_ticks` and `min_requote_interval_ms` |
| `<venue>: HTTP 403 (IP rate limit): REST paused for 10 minutes` (Bybit) | Bybit's per-IP limit | Wait; reduce the request rate |
| `<venue>: HTTP 418 IP ban: REST stopped until restart` (Binance) | Binance banned the IP after ignored 429 responses; no REST request is sent until restart, the venue's kill switch trips (`VenueHardStop`) and its kill-switch cancel-all fails | Cancel orders on the website, stop, wait for the ban to expire, lower the request rate |
| `<venue>: REST hard stop (<code> <msg>)` (Bybit) | Bybit returned an error that stops REST | As for the IP ban |

## Orders and reconciliation

| Message | Cause | Action |
|---|---|---|
| `order <id> rejected: <reason> (<code>)` | The venue or a local check rejected a new order; `[engine] reject_backoff_ms` pauses that side after rejects other than post-only crosses. `PostOnlyWouldCross`: the post-only quote would have crossed the book, frequent when quoting at the touch | For other reasons, look up the venue's error code |
| `risk reject <reason> on new order: <symbol> <side> <qty> @ <price>` (or `on replace order`), optionally `(<n> more suppressed)` | A pre-trade check refused the order; nothing was sent. `<n>`: rejects of that reason not logged since its previous line ([Reject logging](monitor-with-fastmm-top.md#reject-logging)) | Find the limit: `MaxOrderQty` / `MaxOrderNotional` `[risk] max_order_qty` / `max_order_notional`; `MaxPosition` `max_position` (position plus open orders on the same side); `MaxOpenOrders` `max_open_orders`; `PriceCollar` `price_collar_bps` (distance from the mid); `FatFinger` `fat_finger_bps` (distance from the last trade); `StaleMarketData` `stale_md_ms` or a book that stopped updating; `RateLimit` `orders_per_sec` / `burst`; `SelfTradePrevention` `stp`; `InvalidTick` / `InvalidLot` / `BelowMinNotional` the instrument's tick, lot and minimums; `KillSwitch` / `VenueKilled` the kill switch is engaged |
| `<venue>: venue rejected a filter/precision rule (<code> <msg>); check tick/lot config` (Binance), `... a precision/filter rule ...` (Bybit), `<venue>: venue rejected an instrument rule (<code> <msg>); check the instrument` (Deribit) | Price or quantity is off the venue's grid, or below its minimum notional | Check `quote_qty` against `min_qty` and `min_notional`, and the strategy's price rounding |
| `order <id> exceeded cancel-reject retries; reconciliation needed` | A cancel was rejected repeatedly | Check the order on the venue |
| `cancelling unknown live order <id>` | The venue reported a live order the OMS does not know (a previous session, or a lost acknowledgement); it is cancelled | If frequent, check for a second engine on the account |
| `fill for unknown order <id> qty <q> @ <price>` | A fill for an order that is not in the OMS; the position is still booked from the fill | Reconcile the account ([Journals, replay and PnL](journals-replay-pnl.md#check-pnl)) |
| `order <id> was <symbol> filled while we were not listening: booking <qty> at its own price <px>` | A cancel ack, an expiry or a reconciliation snapshot reported more filled quantity than the fills we received (a private-stream outage). The difference is booked as a synthetic fill at the order's own price; the strategy's `on_fill` does not run for it | Compare the position and the average price with the venue's; a venue that reports positions corrects them at the next snapshot |
| `order <id> has no ack after <n> ms: cancelling it` | `[engine] ack_timeout_ms` elapsed with the order still `PendingNew`: the request or its ack was lost | If frequent, check the order channel's latency and the venue's rate limits |
| `fill commission in an asset other than base or quote is not included in fees or positions (first on order <id>)` | Commission paid in a third asset, for example BNB on Binance | Turn off paying fees with BNB, or account for them outside FastMM |
| `<venue>: open orders reply could not be parsed`, `<venue>: open orders could not be fetched for every currency; reconciliation skipped` (Deribit) | Reconciliation after a reconnect did not complete | Check the open orders on the venue |
| `<venue>: only <n> of <m> user channels subscribed; cancels are acknowledged from request responses` (Deribit) | Some private channels were refused | Check the key's scopes |
| `<venue>: listenKey user streams are gone (HTTP 410); switching to the WebSocket API user stream` (Binance) | `user_stream = "listen_key"` is no longer offered | Set `user_stream = "ws_api"` (the default) |

## Rings, kill switch and shutdown

| Message | Cause | Action |
|---|---|---|
| `order ring overflow on <venue>: tripping the kill switch` | The engine did not drain order events fast enough; the session shuts down with exit code 5 | Raise `[engine] order_ring_bytes`; check that the engine thread is not starved (`cpu`, `spin_mode`) |
| `outbound transport full: <n> message(s) dropped; tripping kill switch` | The ring to the venue's network thread was full; the global kill switch trips (`TransportFull`) | Raise `[engine] order_ring_bytes`; check the network thread |
| `control ring full: kill switch message dropped` | The shutdown command did not reach the engine; the venues' REST cancel-all still runs | Check the venue for open orders ([Go-live checklist](go-live-checklist.md#stopping)) |
| `kill switch engaged (<reason>, flags=<hex>); pulling quotes and cancelling all` | The engine tripped the global kill switch itself: `MaxLoss` (`[risk] max_loss`), `TransportFull`, `JournalOverflow` or `AllVenuesKilled`; `[engine] on_kill` decides what follows ([Kill switch and shutdown](kill-switch-and-shutdown.md#after-a-kill-the-engine-trips-itself)) | Find the cause before restarting: PnL, ring sizes, the venue errors before it |
| `fastmm-live: shutting down (kill switch: <reason>; [engine] on_kill = "exit")` | The shutdown after that kill | [Read the last lines](kill-switch-and-shutdown.md#reading-the-last-lines) |
| `fastmm-live: kill switch engaged (<reason>, flags=<hex>) and [engine] on_kill = "stay": quoting is off ...` | Repeated every 10 s while a `stay` session is killed | Inspect it with `fastmm-top`, then stop it with Ctrl-C |
| `venue <id> kill switch engaged (<reason>, flags=<hex>); pulling its quotes and cancelling its orders, other venues keep trading`, `[<venue>] venue kill switch engaged (<reason>): ...; <n> of <m> venue(s) still trading` | A connector reported a fatal error (`VenueFatal`, `VenueHardStop`); only that venue stopped trading | Read the venue's error line before it; fix and restart |
| `fastmm-live: shutdown took <n> ms (cancel_all FAILED)` | A venue's REST cancel-all failed | Follow [When cancel_all failed](kill-switch-and-shutdown.md#when-cancel_all-failed) |

## Monitoring

| Message | Cause | Action |
|---|---|---|
| `status file <path> unavailable: <error>` | The status file for `fastmm-top` could not be created | Check `/dev/shm` permissions and space, or pass `--status <path>` or `--no-status` |
| `fastmm-top` marks a running session as stale | The engine has not published for more than 3 s; the process has probably died | Check the process and the end of its log ([Monitoring](monitor-with-fastmm-top.md)) |

## Symptoms without a message

| Symptom | Cause | Action |
|---|---|---|
| No orders at all (`orders=0`) | `--dry-run` (quoting is disabled), the book is not synced, or a risk limit refuses every order (`risk_rejects` grows) | Check the status line, and the `risk_rejects` reasons in `fastmm-top` or the log |
| Orders but no fills | Quotes too far from the touch ([Binance Demo example](journals-replay-pnl.md#example-binance-demo)) | Narrow the spread |
| The strategy is missing from `--list-strategies` | The app does not pass the registration function of the library that defines it | Pass it to `fastmm::cli::live` / `backtest` / `replay` ([Register a strategy](../strategies/register-a-strategy.md)) |
| `fastmm-replay --verify` reports a mismatch | Different binary, config or parameters, or a determinism bug | See [Replay](journals-replay-pnl.md#replay) |
