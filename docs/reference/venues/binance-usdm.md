# Binance USDⓈ-M futures

`kind = "binance_usdm"` trades Binance USDⓈ-M perpetuals in one-way position mode over the WebSocket API, with REST as the fallback.

| | |
|---|---|
| Markets | perpetuals: `PERPETUAL` and `TRADIFI_PERPETUAL` contracts; delivery contracts are refused |
| Order entry | WebSocket API; REST fallback |
| Environments | production, Demo Trading |
| Keys | HMAC (default) or Ed25519 |
| Replace | `order.modify`, which loses queue position; the example config uses cancel and new |
| Dead man's switch | `countdownCancelAll`, on by default (60 s) |
| Account pools, internal transfers | yes ([Account pools](connector-behaviour.md#account-pools)) |
| Example config | `configs/binance-usdm-demo.toml` |

## Connections

| Channel | Endpoint | Purpose |
|---|---|---|
| md | `<ws_url>/public/stream?streams=<sym>@depth@100ms/<sym>@bookTicker` | depth diffs, best bid and offer |
| ticker, `md_ticker_conns` > 0 | `<ws_url>/public/stream?streams=<sym>@bookTicker`, up to 8 more connections | best bid and offer; the first copy of each update is used ([redundant feeds](../codecs/nasdaq.md#moldudp64)) |
| trades | `<ws_url>/market/stream?streams=<sym>@aggTrade` | aggregate trades |
| mark | `<sym>@markPrice@1s` on `/market` | mark, index, funding ([below](#mark-index-and-funding)) |
| user | `<ws_private_url>/ws/<listenKey>`, default `<ws_url>/private` | `ORDER_TRADE_UPDATE`, `ACCOUNT_UPDATE`, `listenKeyExpired` |
| order | `<ws_api_url>` | `order.place`, `order.cancel`, `order.modify`; REST fallback `POST`, `DELETE`, `PUT /fapi/v1/order` |
| rest | `<rest_url>` | reference data, depth, clock, listenKey, `userTrades`, `openOrders`, `positionRisk`, account checks, kill switch, dead man's switch |

The routed `/public`, `/market` and `/private` paths replaced the plain `/ws` and `/stream` URLs, which Binance switched off on 2026-04-23.

| Demo Trading | Host |
|---|---|
| REST | `https://demo-fapi.binance.com` |
| streams | `wss://demo-fstream.binance.com` |
| WebSocket API | `wss://testnet.binancefuture.com/ws-fapi/v1` |

The listenKey comes from `POST /fapi/v1/listenKey` and is kept alive with `PUT` every 30 minutes; it is valid for 60. On `listenKeyExpired` or a failed keepalive the connector gets a new key, reopens the user connection and reconciles.

## Keys

HMAC keys sign every request. Ed25519 keys (`key_type = "ed25519"`) log on to the order connection with `session.logon`, request id `"logon"`. After that, orders go without `apiKey` and `signature`, as on [Binance Spot](binance-spot.md#keys-and-session-logon). REST requests are signed with Ed25519 per request. USDⓈ-M has no SBE streams.

## Market data

### Book sync

The book follows the Spot procedure ([Depth sync](binance-spot.md#depth-sync)) with futures rules:

| Rule | Value |
|---|---|
| snapshot | `GET /fapi/v1/depth?symbol=S&limit=L`, once the stream's first delta is buffered and the snapshot fits the bulk share of the minute's 2400 weight |
| snapshot depth `L` | `depth_limit`; else 1000 (weight 20) with up to 10 symbols, 100 (weight 5) above |
| drop | deltas with `u` below `lastUpdateId` |
| first applied delta | `U <= lastUpdateId <= u` |
| later deltas | `pu` equals the previous `u` |
| mismatch | `ConnectionState{Resyncing}` and a new snapshot, at most once per 2 s |
| a delta over 1024 levels a side | keeps the levels nearest the touch |

The trades connection does not report its state to the engine, so a quiet `aggTrade` stream never clears the book.

### Mark, index and funding

The connector sends `PerpStateMsg` from `<sym>@markPrice@1s` ([Mark, index and funding](connector-behaviour.md#mark-index-and-funding)).

| Field | Source |
|---|---|
| mark / index | `p` / `i` |
| funding rate, next funding | `r`, `T` |
| funding interval | `fundingIntervalHours` from `GET /fapi/v1/fundingInfo` at start, 8 h for a symbol it does not list; corrected when `T` steps by whole hours |
| open interest | none |

Updates come once a second per symbol.

## Orders

| Engine order | Sent as |
|---|---|
| limit GTC, IOC, FOK | `LIMIT` with that `timeInForce` |
| post-only | `LIMIT` with `GTX`; a crossing order is rejected with -5022 or expires (`OrderExpired`) |
| post-only with `post_only_rpi = true` | `timeInForce` `RPI` |
| market | `MARKET`, no price, no `timeInForce` |
| reduce-only | `reduceOnly=true` |
| position side | omitted (BOTH) |

An RPI order rests post-only, hidden from the API depth, and matches only orders from Binance's app or web page. It queues behind every other order at its price. The venue does not modify an RPI order, so pair it with `supports_replace = false`.

`order.modify` keeps the venue's `clientOrderId` and takes the total quantity. The connector sends the new remaining quantity plus the filled quantity. It reports later events under the engine's new client id and counts their fills from the modify. A modify answered `CANCELED` or `EXPIRED` becomes a cancel of the original and a reject of the replacement.

Batch endpoints exist only on REST (`POST /fapi/v1/batchOrders`, 5 orders; `DELETE`, 10). The connector stays on the WS API and writes an engine batch in one TCP write, so a batch request would save no round trip.

## Reconciliation and fills

On every user-stream connect and order-channel reconnect, the connector reconciles in this order:

1. `GET /fapi/v1/userTrades` per symbol replays the executions since the last one forwarded ([Execution replay](connector-behaviour.md#execution-replay)).
2. `GET /fapi/v1/openOrders` (weight 40) and `GET /fapi/v3/positionRisk` (weight 5) become one `ReconcileMsg` sequence.
3. The sequence is Begin, one `OpenOrder` per order, one `Position` per configured instrument (flat when absent), End.

| Replay item | Value |
|---|---|
| query | `GET /fapi/v1/userTrades`, weight 5, limit 1000 |
| window | `fromId` = last trade id + 1 once known, else `startTime` = connect time or the restored watermark |
| older than 7 days | walked a week per query; older than 3 months is clamped; either way the replay is not exact |
| order named | `orderId` only, mapped through the last 8192 pairings; an unknown one is looked up with `GET /fapi/v1/order?orderId=` (weight 1), as on [Binance Spot](binance-spot.md#the-order-a-trade-names) |

The minute's replay reads only the symbols with order activity since the symbol's watermark, less the settle margin. Activity is an order sent or an order event received. A quiet symbol's watermark moves as if it had answered empty. Replays before a reconciliation, after a reconnect, a restart or a failure read every symbol, as do all replays in the first two minutes.

### Positions between reconciliations

The engine books every fill, including liquidations and ADL (`autoclose-*` client ids). Once no fill has arrived for 1 s, the connector compares each `ACCOUNT_UPDATE` position with the fills it forwarded. It sends a `PositionUpdate` only when they differ (`position_from_account_update`). The two event types are not ordered against each other, so forwarding every position would count fills twice.

### Account settings

`load_reference_data()` logs the position mode, each symbol's leverage and margin type, and the margin balance. It refuses an account in hedge mode; `fastmm-live` and `fastmm-gateway` exit 3.

| Key | Change |
|---|---|
| `one_way_mode = true` | switches a hedge-mode account to one-way (`POST /fapi/v1/positionSide/dual`), only with no position and no open order |
| `leverage = <n>` | sets every enabled symbol whose leverage differs (`POST /fapi/v1/leverage`) |

A refused change stops the start. Without these keys no account setting changes.

### Balances

| Item | Source |
|---|---|
| snapshot | `GET /fapi/v3/account` (weight 5); the mode from `/fapi/v1/multiAssetsMargin` and `ACCOUNT_CONFIG_UPDATE` `ai.j` |
| stream | `ACCOUNT_UPDATE` asks for a new snapshot: its `B[]` has the wallet balance only |
| free / locked / total / equity / maintenance | `availableBalance` / `initialMargin` / `walletBalance` / `marginBalance` / `maintMargin` |
| multi-assets mode | free is each asset's `maxWithdrawAmount`, plus an account row in USD from the `total*` fields |
| deposits, withdrawals, transfers | each `ACCOUNT_UPDATE` `B[]` row with a reason other than `ORDER` or `FUNDING_FEE` and a non-zero `bc`: an `AccountFlow` event |

A pool member's position report is its own account's; the engine sums the accounts' reports into the instrument's position.

## Funding

`GET /fapi/v1/income?incomeType=FUNDING_FEE` (weight 30, every symbol in one query) is the source. Each row on a subscribed symbol becomes a `Funding` event:

| Field | Source |
|---|---|
| amount | `income`, negative when paid, in `asset` |
| time | `time` |
| id | `tranId` |

The query runs with every reconciliation's replay and once a minute, from its own watermark. It reads windows of at most 7 days, no further back than 3 months, and up to 20 pages. A failed query runs again 5 s later. An `ACCOUNT_UPDATE` with reason `FUNDING_FEE` carries no id, so it only triggers the query a second later.

## Internal transfers

Transfers use the master account's universal transfer, as on [Binance Spot](binance-spot.md#internal-transfers). The endpoint has no futures-to-futures path, so one USDⓈ-M transfer is two steps:

1. the sender's `USDT_FUTURE` to the receiver's `SPOT`, id `<id>a`;
2. the receiver's `SPOT` to its `USDT_FUTURE`, id `<id>b`.

A refused second step is sent again from `transfer_status`. Refused again, the amount stays in the receiver's spot wallet and the transfer is Failed, said in the log.

## Dead man's switch

`POST /fapi/v1/countdownCancelAll?symbol=S&countdownTime=<ms>` (signed, IP weight 10) cancels a symbol's orders when the countdown lapses. Sending it again replaces the running timer; `countdownTime=0` stops it.

| Item | Value |
|---|---|
| window | `dead_mans_switch_ms`, default 60000 ms |
| armed | on every subscribed symbol, from the housekeeping timer |
| refresh | every window/3, at most every 500 ms |
| cost | 10 IP weight per symbol per refresh: 120 a minute for four symbols at the default |
| shutdown | `disconnect()` sends `countdownTime=0` |
| lapse | no refresh succeeded for a whole window: the venue kill trips with `DeadMansSwitchLost` |

The refresh clock moves only when the venue answers. Binance has no WebSocket method for the countdown and no way to read the armed state back. See [Dead man's switch](connector-behaviour.md#dead-mans-switch) for the other venues.

## Rate limits

Limits come from `exchangeInfo.rateLimits`, the WS API `rateLimits`, and the headers `X-MBX-USED-WEIGHT-1M` and `X-MBX-ORDER-COUNT-10S`.

| Request | IP weight | Order limits |
|---|---|---|
| place, modify | 0 | 1 against the 10 s and 1-minute windows |
| cancel | 1 | none |

The `userTrades` and `income` queries, order lookups and the start's `symbolConfig` queries wait while the bulk share is spent or the venue asked for a pause. A 429 waits `Retry-After`, longer for a second 429 within a minute; a 418 stops REST. Windows, pacing and refusal counts work as on [Binance Spot](binance-spot.md#rate-limits).

## Errors

| Code | Action |
|---|---|
| -5022 | post-only reject |
| -2018, -2019, -2027 | insufficient balance or margin |
| -2022, -4118 | reduce-only reject |
| -4164 | minimum notional |
| -4014, -4023 | tick, lot |
| -1003, -1015, HTTP 429 | cool down |
| HTTP 418 | stop REST |
| -1021, -5028 | resync the clock |
| -1022, -2015, -4109, -4061 (hedge mode) | fatal |
| -2011, -2013, -4116 | reconcile |
| HTTP 503 | execution status unknown: reconcile |

## Connection timeouts

| Key | Value |
|---|---|
| `stale_ms` | 2000 ms; the Demo config sets 10000 ms |
| `dead_ms` | at least 45000 ms for market data and 240000 ms for the other connections; the venue pings every 3 minutes |

## Unconfirmed

| Item | Status |
|---|---|
| private payloads | `ORDER_TRADE_UPDATE`, `ACCOUNT_UPDATE` and WS API order responses follow the documentation and the official connector's models; not recorded |
| crossing GTX order on Demo | -5022 or accepted and expired; both are handled |
| `order.modify` | covered by the scripted fake exchange only |
| `countdownTime` bounds | none documented for `/fapi`; `dead_mans_switch_ms` is only clamped to be non-negative |
| countdown cancel on the user stream | expected to look like any other `x: "CANCELED"` |

Sources (read 2026-09-26): [USDⓈ-M general info](https://developers.binance.com/docs/derivatives/usds-margined-futures/general-info), [Account Information V3](https://developers.binance.com/docs/derivatives/usds-margined-futures/account/rest-api/Account-Information-V3), [Mark Price Stream](https://developers.binance.com/legacy-docs/derivatives/usds-margined-futures/websocket-market-streams/Mark-Price-Stream), [Get Funding Rate Info](https://developers.binance.com/legacy-docs/derivatives/usds-margined-futures/market-data/rest-api/Get-Funding-Rate-Info), [WebSocket change notice](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/websocket-market-streams/Important-WebSocket-Change-Notice.md).
