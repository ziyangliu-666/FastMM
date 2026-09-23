# Errors and exit codes

One lookup table for every exit code, reject reason, kill reason and venue error action. Log messages with their causes and fixes are in [Troubleshooting](../how-to/operations/troubleshooting.md).

## Exit codes

There is no shared exit-code enum; each program defines its own, and they disagree. `1` from `fastmm-live` means the process crashed.

### fastmm-live

The last two columns are what an operator has to decide. "Orders cancelled" is the cancel-all the control thread runs over REST at shutdown; `cancel_all FAILED` turns any code into 5 ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md#reading-the-last-lines)).

| Code | Meaning | Orders cancelled | Restart without a human |
|---:|---|---|---|
| 0 | stopped by `--duration`, SIGINT or SIGTERM with `cancel_all ok`; also after a kill with `on_kill = "stay"`; `--help`, `--version`, `--list-strategies` | yes | yes |
| 2 | bad command line; an unset `${VAR}` in `[venues.*]` | nothing started | no: fix the invocation |
| 3 | the configuration, strategy or a parameter does not load, or a `[storage]` backend is unknown or cannot be opened | nothing started | no: fix the config |
| 4 | a venue's reference data failed to load | nothing started | retry once; a repeat means the venue or the network |
| 5 | `cancel_all FAILED`, the journal cannot be opened or written (a full filesystem trips the kill switch), an order-event ring overflowed, or an uncaught error | not certain | no: check the venue for open orders first |
| 6 | the engine tripped the kill switch itself, `on_kill = "exit"`, `cancel_all ok` | yes | no: find the kill reason in the log |
| 7 | a Python strategy's slow tier failed, `cancel_all ok` (`python -m fastmm run` and `fastmm.run_live`; `fastmm-live` never returns it) | yes | no: fix the slow method |

The engine keeps no state across a restart, so a restart after any of these is a fresh trading decision ([Running this in production](../how-to/operations/running-in-production.md#1-nothing-survives-a-restart)).

### The other programs

| Code | `fastmm-backtest` | `fastmm-replay` | `fastmm-sim-exchange` | `fastmm-sim-itch` | `fastmm-top` |
|---:|---|---|---|---|---|
| 0 | ran | matched, or no `--verify` | stopped | stopped | quit, or `--once` printed a frame |
| 1 | — | verification mismatch | — | — | — |
| 2 | bad command line | bad command line | bad command line | bad command line | bad command line |
| 3 | bad config, strategy or parameter | unreadable config or journal, unknown strategy | bad config | bad config, CPU pinning, or the summary file | `--once` with no status file, or one from another build |
| 4 | the market data cannot be read | — | cannot listen on a port | cannot open a socket | — |
| 5 | the run failed | — | — | — | — |

## Reject reasons

`RejectReason` (`include/fastmm/core/enums.hpp`). A reject means the order was not sent, or the venue refused it; nothing else changed. The strategy sees the reason in the `Result` of `ctx.send`; the engine counts them per reason and shows them in `fastmm-top` and the shutdown summary.

### Pre-trade checks (engine)

Checked in this numeric order; the first failure decides. A limit of `0` or omitted turns its check off ([Risk model](../explanation/risk-model.md)). These rejects never reach the venue.

| # | Name | Setting | Clears when |
|---:|---|---|---|
| 1 | `KillSwitch` | — | the process restarts; nothing resets it |
| 2 | `VenueKilled` | — | the process restarts |
| 3 | `InstrumentDisabled` | `[[instruments]] enabled` | the config changes, or the venue re-enables the symbol |
| 4 | `InvalidTick` | `tick` | the strategy rounds prices with `inst.round_price` |
| 5 | `InvalidLot` | `lot`, `min_qty`, `max_qty` | the strategy rounds quantities with `inst.round_qty` |
| 6 | `BelowMinNotional` | `min_notional` | the order is larger |
| 7 | `StaleMarketData` | `[risk] stale_md_ms` | the book updates; also fires when the host clock lags ([Clocks](../how-to/operations/running-in-production.md#8-clocks)) |
| 8 | `PriceCollar` | `[risk] price_collar_bps` | the mid moves to the price, or the price moves to the mid |
| 9 | `FatFinger` | `[risk] fat_finger_bps` | a trade prints nearer the price |
| 10 | `MaxOrderQty` | `[risk] max_order_qty` | the order is smaller |
| 11 | `MaxOrderNotional` | `[risk] max_order_notional` | the order is smaller |
| 12 | `MaxPosition` | `[risk] max_position` | the position or the same-side open orders shrink |
| 13 | `MaxOpenOrders` | `[risk] max_open_orders` | an order of that instrument terminates |
| 14 | `SelfTradePrevention` | `[risk] stp` | our resting order on the other side moves or is cancelled |
| 15 | `RateLimit` | `[risk] orders_per_sec`, `burst` | the token bucket refills |
| 16 | `MaxLoss` | — | never produced: a max-loss breach arrives as `KillSwitch` with `KillReason::MaxLoss` |

Cancels skip every check, including the kill switch, so the engine can always reduce what is in the market.

### OMS and transport

| Value | Name | Cause |
|---:|---|---|
| 32 | `PoolExhausted` | the order pool or the id map is full: more than 4096 working orders (`kMaxOpenOrders`, `include/fastmm/core/oms.hpp`) |
| 33 | `UnknownOrder` | a cancel or replace for an id the OMS does not hold |
| 34 | `InvalidState` | a cancel or replace for an order that is no longer working |
| 35 | `DuplicateId` | the client order id is already in use, locally or at the venue |
| 36 | `TransportFull` | the connector's outbound or REST queue was full; the message was dropped |
| 37 | `NotReconciled` | never produced by the current code |
| 38 | `InvalidTag` | `ctx.send` used a `user_tag` inside the quote manager's reserved range |

### Venue-originated

The connectors map each venue's error codes onto these (`*_error_map.hpp` per venue; the code tables are on [Venue connectors](venues.md)).

| Value | Name | Transient | What to do |
|---:|---|---|---|
| 64 | `VenueReject` | unknown | the code was not in the venue's table; read the message in the log |
| 65 | `PostOnlyWouldCross` | yes | the quote would have taken; expected when quoting at the touch, and excluded from `[engine] reject_backoff_ms` |
| 66 | `InsufficientBalance` | no | fund the account or shrink the quote |
| 67 | `VenueRateLimit` | yes | the connector cools down; lower the request rate |
| 68 | `VenueUnknownOrder` | no | the order is gone at the venue; most codes also trigger a reconciliation |

## Kill reasons

`KillReason` (`include/fastmm/core/enums.hpp`), the first reason each kill-switch bit was set for. `fastmm-top` shows it next to the state and per venue; the status file carries it in `kill_reason`.

| Value | Name | Scope | `on_kill` applies | First thing to check |
|---:|---|---|---|---|
| 0 | `None` | — | — | not killed |
| 1 | `Requested` | global | no, it always shuts down | why the shutdown started: signal, `--duration`, slow tier |
| 2 | `MaxLoss` | global | yes | the PnL and the market before restarting |
| 3 | `TransportFull` | global | yes | `[engine] order_ring_bytes`, and whether the network thread is starved |
| 4 | `JournalOverflow` | global | yes | `[engine] journal_ring_bytes`, and disk write throughput |
| 5 | `AllVenuesKilled` | global | yes | each venue's own kill reason |
| 6 | `VenueFatal` | one venue | when it is the last venue | the key, its permissions and its environment |
| 7 | `VenueHardStop` | one venue | when it is the last venue | a Binance 418 IP ban; terminal until the process restarts |
| 8 | `OrderRingOverflow` | global | no, it always shuts down (exit 5) | `[engine] order_ring_bytes`, engine thread pinning |
| 9 | `StrategyError` | global | yes | a Python hot hook raised, called `ctx.fail` or produced a non-finite value |
| 10 | `FeedLost` | one venue | when it is the last venue | a `nasdaq_itch` feed that cannot rebuild its books |

Nothing resets a kill switch. `fastmm-live` has no command to clear one; restart the process. What each reason does to quoting and orders: [Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md).

## Venue actions

`VenueAction` (`include/fastmm/venues/error_action.hpp`) is what the connector does beyond reporting the reject. Every venue's error map returns one.

| Value | Name | Connector behaviour |
|---:|---|---|
| 0 | `None` | reports the reject and nothing else |
| 1 | `Backoff` | retries later |
| 2 | `RateLimit` | enters the rate limiter's cooldown, honouring `Retry-After` when the venue sends one, else 10 s |
| 3 | `ResyncClock` | fetches the venue's server time and recomputes the offset (a no-op on Deribit) |
| 4 | `Reconcile` | requests the venue's open orders |
| 5 | `DisableInstrument` | the configured tick, lot or minimum is wrong for that symbol |
| 6 | `HardStop` | stops REST until the process restarts, and trips the venue's kill switch with `VenueHardStop` |
| 7 | `Fatal` | disables order entry and trips the venue's kill switch with `VenueFatal` |

## Journal errors

`tools/journal_dump.py` and `fastmm-replay` report these:

| Symptom | Meaning |
|---|---|
| `trailer MISSING` | the session did not shut down cleanly; events before the damaged block are still readable. `fastmm-replay` refuses such a file without `--allow-incomplete`, because the outbound stream it compares against stops short of what the session sent |
| a CRC failure on a block | the block is damaged; `--no-crc` reads past it |
| `replay MISMATCH` | the replayed order stream differs from the recorded one; only the first difference means anything ([Determinism](../explanation/determinism.md)) |
| a what-if warning | `--config` or `--strategy` differs from the recording, so a match is not expected |

## Related

- [Troubleshooting](../how-to/operations/troubleshooting.md): every log message with its cause and fix.
- [Status file](status-file.md): where the counters and reasons are published.
- [Storage](storage.md): `sessions.clean_shutdown`, `journal_complete` and `records_dropped` say what a session's record is missing.
- [Running this in production](../how-to/operations/running-in-production.md): the failures that have no error at all.
