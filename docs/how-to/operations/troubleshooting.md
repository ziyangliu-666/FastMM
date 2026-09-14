# Troubleshooting

Find the message you see, then read its meaning and what to do. Messages are quoted as the code
writes them; `<...>` stands for a value, and `<venue>` is the `[venues.<name>]` name. Log lines start
with a timestamp, the level (`INFO`, `WARN`, `ERROR`), the thread id and the source file. Messages
marked *stderr* are printed before the logger starts.

Search the log for a message with `grep`, for example `grep -n 'channel lost' runs/demo-1/engine.log`.

## Exit codes of fastmm-live

| Code | Meaning |
|---|---|
| 0 | Normal stop (duration elapsed or signal) and `cancel_all ok` |
| 2 | Bad command line, or a venue has no keys (and no `--dry-run`) |
| 3 | Bad config, unknown strategy or instruments |
| 4 | A venue's reference data failed to load |
| 5 | Runtime failure: `cancel_all FAILED`, the journal cannot be opened, a ring overflowed, an uncaught error |

## Startup and configuration

| Message | Meaning | What to do |
|---|---|---|
| `fastmm-live: --config is required` (stderr) | No config file given | Pass `--config configs/<file>.toml` |
| `fastmm-live: venue '<venue>' needs API keys: environment variable <VAR> is not set` (stderr) | A `${VAR}` in `api_key` or `api_secret` is not exported | Export the variable ([Run on a testnet](run-on-testnet.md#2-keys-in-the-environment)) or add `--dry-run` |
| `fastmm-live: venue '<venue>' has no api_key/api_secret` (stderr) | The keys are empty and this is not a dry run | As above |
| `fastmm-live: venues.<venue>.<key>: environment variable <VAR> is not set` (stderr) | A `${VAR}` in a URL or extra key is not exported | Export it or write the value into the config |
| `config: unknown key '<section>.<key>' ignored (line <n>)` | A key the schema does not know; it has no effect unless it is a connector key | Check the spelling against [Configuration](../../configuration.md) |
| `fastmm-live: unknown strategy '<name>' (available: ...)` (stderr) | `[strategy] name` or `--strategy` is not registered in this app | Pick one from `./build/release/bin/fastmm-live --list-strategies`, or register it (see [Register a strategy](../strategies/register-a-strategy.md)) |
| `fastmm-live: <strategy>: unknown parameter '<key>' (see --list-strategies)` (stderr) | `[strategy.params]` or `--param` names a parameter the strategy does not have; after `--strategy` switches strategies the config's parameters are ignored | Use the names from `--list-strategies` |
| `<program>: strategy '<name>' (<kind>) is already registered by different code; ...` (stderr, exit 3) | Two strategy libraries in the app use the same strategy name, or one reuses a built-in name | Rename one of the strategies |
| `fastmm-live: no [[instruments]] configured` (stderr) | The config has no instruments | Add `[[instruments]]` tables |
| `fastmm-live: cannot open journal <path>` (stderr) | The journal file cannot be created | Check the directory permissions and free space, or pass `--journal <path>` |
| `cannot create session epoch directory <dir>: <error>` | The parent directory of `[engine] epoch_file` cannot be created | Fix the path; without the epoch file, client order ids can repeat across restarts |
| `[engine] net_backend = "io_uring" but io_uring is not available (kernel too old, disabled or not permitted); falling back to epoll` | io_uring is blocked (old kernel, `kernel.io_uring_disabled`, seccomp) | Nothing, or set `net_backend = "epoll"` |
| `fastmm-live: fatal: <error>` | An uncaught error in the session; exit code 5 | Read the error text; check the lines just before it |

## Reference data

| Message | Meaning | What to do |
|---|---|---|
| `<venue>: exchangeInfo failed (<error>); keeping configured tick/lot` (Binance), `<venue>: instruments-info failed (<error>); keeping configured tick/lot` (Bybit), `<venue>: get_instruments <currency> <kind> failed (<error>)` (Deribit) | The REST reference-data request failed at startup | Check `rest_url` and the network; unless `allow_offline_reference_data = true` the start fails with exit code 4 |
| `<venue>: <symbol> tick/lot from exchangeInfo override config (<tick> / <lot> -> <tick> / <lot>)` (Binance), `... from instruments-info ...` (Bybit) | The venue's tick or lot differs from the config | Nothing; update the config to silence it |
| `<venue>: <symbol> status is <status> (not TRADING): disabled` (Binance), `(not Trading)` (Bybit), `<venue>: <symbol> is not open for trading (state <state>): disabled` (Deribit) | The symbol is halted, delisted or expired; it is not traded | Choose another symbol |
| `<venue>: <symbol> not in reference data; keeping the configured values` (Deribit) | The instrument name does not exist, typically an expired option | Replace the `[[instruments]]` with live names ([Run on a testnet](run-on-testnet.md#deribit-testnet)) |
| `<venue>: <symbol> does not allow LIMIT_MAKER (post-only)` (Binance) | Post-only orders will be rejected on this symbol | Choose another symbol, or quote without `post_only` |

## Clock

| Message | Meaning | What to do |
|---|---|---|
| `<venue>: clock offset to venue is <n> ms`, `... (recvWindow <m> ms)` (Binance), `... (recv_window <m> ms)` (Bybit) | The local clock differs from the venue's by more than 1000 ms; signed requests are rejected once the offset nears `recv_window_ms` | Synchronise the system clock (chrony or systemd-timesyncd; on WSL2 check the Windows host's time). Raising `recv_window_ms` is only a stopgap |
| `host wall clock stepped by <n> ns relative to CLOCK_MONOTONIC_RAW within <t> s; the engine clock follows it` | The system clock was stepped (NTP, or the hypervisor on WSL2 and VMs) | Nothing on WSL2: our Demo sessions logged it several hundred times an hour without effect. On bare metal, make the time daemon slew instead of step |
| `TSC recalibration stepped the engine clock by <n> ns (threshold <m> ns)` | The engine clock was more than 1 ms off and was stepped instead of slewed | As above; see [Clock calibration](../../architecture.md#clock-calibration) |
| `TSC recalibration skipped (no TSC mapping or baseline too short)` | No invariant TSC, or the first recalibration came too soon | Nothing |

## Connectivity and market data

| Message | Meaning | What to do |
|---|---|---|
| `<venue>: md channel lost`, `<venue>: private channel lost` (Bybit, Deribit), `<venue>: trade channel lost` (Bybit) | A WebSocket connection dropped; the connector reconnects with backoff, quotes are pulled, and orders are cancelled over REST when the order channel is lost | Nothing if it recovers. If it repeats, check the network, DNS and the endpoint URL |
| Status line stays `md=conn` or `md=down`, or `books=0/1` | The market-data connection or the book never came up | Check `ws_url`; try `--dry-run`; look for the messages in this section |
| Status line shows `md=stale` and quotes are pulled during quiet markets | No traffic for `stale_ms` (default 2000 ms) | Raise `stale_ms` on quiet testnets (the shipped testnet configs use 10000 ms) |
| `<venue>: malformed market-data frame (<n> so far)` | A frame did not parse; it is counted in `malformed=` | If it grows, record frames with `--record-raw <dir>` and report it: the venue's format may have changed |
| `<venue>: depth snapshot for <symbol> failed: status=<s> err=<e>` (Binance) | The REST depth snapshot for a resync failed; it is retried | Check REST access and rate limits |
| `<venue>: market-data subscribe rejected: <msg>` (Bybit), `<venue>: market-data subscription incomplete or rejected: <msg>` (Deribit) | The venue refused a subscription, usually an unknown symbol | Check the `symbol` of the instrument |

## Keys and authentication

| Message | Meaning | What to do |
|---|---|---|
| `<venue>: session.logon failed: <code> <msg>`, `<venue>: userDataStream.subscribe failed: <code> <msg>` (Binance) | The WebSocket API refused the key | Check that the key belongs to this environment (Demo keys are not testnet keys) and that `key_type` matches it |
| `<venue>: private auth failed: <msg>`, `<venue>: trade auth failed: <code> <msg>` (Bybit) | Bybit refused the key or signature | Check the key, its permissions, its IP allowlist and the clock |
| `<venue>: could not send the auth request` (Bybit), `<venue>: could not send public/auth` (Deribit) | The connection closed before the request was written | Transient; if it repeats, see "channel lost" |
| `<venue>: public/auth failed: <code> <msg>` (Deribit) | Wrong client id or secret, or a main-net key on the testnet | Use a key created on test.deribit.com |
| `<venue>: token refresh failed (<code> <msg>); re-authenticating` (Deribit) | The refresh token was refused; the connector logs in again | Nothing unless followed by `public/auth failed` |
| `<venue>: fatal venue error (<code> <msg>); order entry disabled` | Bad key, bad signature or missing permission; that venue refuses every further order (cancels still go out) | Stop the session, fix the key, restart |

## Rate limits

| Message | Meaning | What to do |
|---|---|---|
| `<venue>: rate limited (<code> <msg>); cooling down <n> ms` (Binance, Bybit), `<venue>: rate limited (<code> <msg>)` (Deribit) | The venue signalled a rate limit; the connector sends nothing for the cooldown | Lower `[risk] orders_per_sec`, raise `[engine] min_requote_ticks` and `min_requote_interval_ms` |
| `<venue>: HTTP 403 (IP rate limit): REST paused for 10 minutes` (Bybit) | Bybit's per-IP limit | Wait; reduce the request rate |
| `<venue>: HTTP 418 IP ban: REST stopped until restart` (Binance) | Binance banned the IP after ignored 429 responses; no REST request is sent until restart, and the kill-switch cancel-all will fail | Cancel orders on the website, stop, wait for the ban to expire, lower the request rate |
| `<venue>: REST hard stop (<code> <msg>)` (Bybit) | Bybit returned an error that stops REST | As for the IP ban |

## Orders and reconciliation

| Message | Meaning | What to do |
|---|---|---|
| `order <id> rejected: <reason> (<code>)` | The venue or a local check rejected a new order; `[engine] reject_backoff_ms` pauses that side after rejects other than post-only crosses | `PostOnlyWouldCross` is normal when you quote at the touch (260 in a 1640-fill Demo hour). Other reasons: see the venue's error code |
| `risk reject <reason> on new order: <symbol> <side> <qty> @ <price>` (or `on replace order`), optionally `(<n> more suppressed)` | A pre-trade check refused the order; nothing was sent. Logged for the first reject of a reason, then at most once per reason every 10 s; `<n>` rejects of that reason were not logged in between. `fastmm-top` and the shutdown summary (`risk_rejects by reason:`) show the counts per reason | Find the limit: `MaxOrderQty` / `MaxOrderNotional` `[risk] max_order_qty` / `max_order_notional`; `MaxPosition` `max_position` (position plus open orders on the same side); `MaxOpenOrders` `max_open_orders`; `PriceCollar` `price_collar_bps` (distance from the mid); `FatFinger` `fat_finger_bps` (distance from the last trade); `StaleMarketData` `stale_md_ms` or a book that stopped updating; `RateLimit` `orders_per_sec` / `burst`; `SelfTradePrevention` `stp`; `InvalidTick` / `InvalidLot` / `BelowMinNotional` the instrument's tick, lot and minimums; `KillSwitch` / `VenueKilled` the kill switch is engaged |
| `<venue>: venue rejected a filter/precision rule (<code> <msg>); check tick/lot config` (Binance), `... a precision/filter rule ...` (Bybit), `<venue>: venue rejected an instrument rule (<code> <msg>); check the instrument` (Deribit) | Price or quantity is off the venue's grid, or below its minimum notional | Check `quote_qty` against `min_qty` and `min_notional`, and the strategy's price rounding |
| `order <id> exceeded cancel-reject retries; reconciliation needed` | A cancel was rejected repeatedly | Watch for the next reconciliation; check the order on the venue |
| `cancelling unknown live order <id>` | The venue reported a live order the OMS does not know (a previous session, or a lost acknowledgement); it is cancelled | Nothing if rare; if frequent, check for a second engine on the account |
| `fill for unknown order <id> qty <q> @ <price>` | A fill for an order that is not in the OMS; the position is still booked from the fill | Reconcile the account ([Journals, replay and PnL](journals-replay-pnl.md#check-pnl)) |
| `fill commission in an asset other than base or quote is not included in fees or positions (first on order <id>)` | Commission paid in a third asset, for example BNB on Binance | Turn off paying fees with BNB, or account for them outside FastMM |
| `<venue>: open orders reply could not be parsed`, `<venue>: open orders could not be fetched for every currency; reconciliation skipped` (Deribit) | Reconciliation after a reconnect did not complete | Check the open orders on the venue |
| `<venue>: only <n> of <m> user channels subscribed; cancels are acknowledged from request responses` (Deribit) | Some private channels were refused | Check the key's scopes |
| `<venue>: listenKey user streams are gone (HTTP 410); switching to the WebSocket API user stream` (Binance) | `user_stream = "listen_key"` is no longer offered | Set `user_stream = "ws_api"` (the default) |

## Rings, kill switch and shutdown

| Message | Meaning | What to do |
|---|---|---|
| `order ring overflow on <venue>: tripping the kill switch` | The engine did not drain order events fast enough; the session shuts down with exit code 5 | Raise `[engine] order_ring_bytes`; check that the engine thread is not starved (`cpu`, `spin_mode`) |
| `outbound transport full: <n> message(s) dropped; tripping kill switch` | The ring to the venue's network thread was full | Raise `[engine] order_ring_bytes`; check the network thread |
| `control ring full: kill switch message dropped` | The shutdown command did not reach the engine; the venues' REST cancel-all still runs | Confirm on the venue that no orders are left |
| `kill switch requested (flags=<hex>); pulling quotes and cancelling all` | Normal shutdown (WARN) | Nothing |
| `kill switch engaged (flags=<hex>); pulling quotes and cancelling all` | A risk limit (usually `max_loss`) or an internal failure tripped it (ERROR); quoting has stopped but the process runs | See [Kill switch and shutdown](kill-switch-and-shutdown.md#what-trips-it) |
| `fastmm-live: shutdown took <n> ms (cancel_all FAILED)` | A venue's REST cancel-all failed | Follow [When cancel_all failed](kill-switch-and-shutdown.md#when-cancel_all-failed) |

## Monitoring

| Message | Meaning | What to do |
|---|---|---|
| `status file <path> unavailable: <error>` | The status file for `fastmm-top` could not be created | Check `/dev/shm` permissions and space, or pass `--status <path>` or `--no-status` |
| `fastmm-top` marks a running session as stale | The engine has not published for more than 3 s; the process has probably died | Check the process and the end of its log ([Monitoring](../../monitoring.md)) |

## Symptoms without a message

| Symptom | Likely cause | What to do |
|---|---|---|
| No orders at all (`orders=0`) | `--dry-run` (quoting is disabled), the book is not synced, or a risk limit refuses every order (`risk_rejects` grows) | Check the status line and `risk_rejects=` in `fastmm-top` or the summary: the reasons are listed next to it (`risk_rejects=17 (MaxPosition 12, RateLimit 5)`, `risk_rejects by reason: ...`) and in `risk reject <reason>` warnings |
| Orders but no fills | Quotes too far from the touch: a Binance Demo session at 15 bps from the mid had 0 fills in an hour, the same strategy at the touch had 1640 | Compare your spread with the venue's typical spread; test in a backtest first |
| The strategy is missing from `--list-strategies` | The app does not pass the registration function of the library that defines it | Pass it to `fastmm::cli::live` / `backtest` / `replay` (see [Register a strategy](../strategies/register-a-strategy.md)) |
| `fastmm-replay --verify` reports a mismatch | Different binary, config or parameters, or a determinism bug | See [Replay](journals-replay-pnl.md#replay) |
