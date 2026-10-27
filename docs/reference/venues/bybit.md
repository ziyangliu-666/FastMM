# Bybit

`kind = "bybit"` (alias `bybit_spot`) trades Bybit v5 spot or linear perpetuals over the WebSocket trade endpoint, one `category` per `[venues.<name>]` section.

| | |
|---|---|
| Markets | spot (`category = "spot"`, the default); USDT- and USDC-margined linear perpetuals (`category = "linear"`) |
| Order entry | WebSocket `/v5/trade`, REST fallback |
| Environments | production, Bybit testnet |
| Keys | HMAC |
| Replace | amend, keeping the venue's `orderLinkId`; the testnet config uses cancel and new |
| Dead man's switch | Disconnect-Cancel-All, off by default (institutional accounts only) |
| Example config | `configs/bybit-linear-testnet.toml` |

Every request that takes a category gets the section's, and the private topics are filtered by it. The sections below describe spot; [Linear perpetuals](#linear-perpetuals) lists what changes.

## Connections

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`/v5/public/spot`) | `orderbook.<depth>.SYM` snapshot and delta, `orderbook.1.SYM` as BookTicker, `publicTrade.SYM` |
| private | `ws_private_url`, default the `ws_url` host + `/v5/private` | `op: auth`, then `order`, `execution`, `wallet`, and `dcp.spot` when `dead_mans_switch_s` is set |
| trade | `ws_api_url` (`/v5/trade`) | `op: auth`, then `order.create`, `order.amend`, `order.cancel` |
| rest | `rest_url` | the requests below |

| request | use |
|---|---|
| `GET /v5/market/instruments-info` | reference data |
| `GET /v5/market/time` | clock offset |
| `GET /v5/order/realtime` | reconciliation |
| `GET /v5/execution/list` | execution replay |
| `POST /v5/order/create`, `amend`, `cancel` | order entry fallback |
| `POST /v5/order/cancel-all` | kill switch |
| `POST /v5/order/disconnected-cancel-all` | dead man's switch |

Trade requests carry a `reqId` and the `X-BAPI-TIMESTAMP` and `X-BAPI-RECV-WINDOW` headers. Every channel sends `{"op":"ping"}` every 20 s.

## Keys and signing

| where | signature |
|---|---|
| REST | `X-BAPI-SIGN` = hex HMAC-SHA256(secret, timestamp + api_key + recv_window + payload) |
| WebSocket | `{"op":"auth","args":[key, expires_ms, hex HMAC("GET/realtime" + expires)]}` |

The REST payload is the exact query string of a GET or the JSON body of a POST.

## Market data

The stream's `snapshot` resets the book, and each `delta` must carry a larger `u`. A `u` of 1, or no snapshot within 10 s, re-subscribes the depth topic for a fresh snapshot.

| key | default |
|---|---|
| `stale_ms` | 2000 ms |
| `dead_ms` | twice `ping_interval_ms` plus 5000 ms; 45000 ms by default |

## Orders

Amend keeps the venue's `orderLinkId`. The engine receives an ack for its new client id, and later `order` and `execution` events for the old link id are translated to it.

Bybit sends single orders. Its batch methods report per-order outcomes inside a reply whose top-level `retCode` stays 0, so a two-sided quote could come back half-accepted ([Batch order entry](connector-behaviour.md#batch-order-entry)).

## Reconciliation and fills

`GET /v5/order/realtime?category=spot&limit=50` is paged with `cursor` = `result.nextPageCursor` until the cursor is empty, 40 pages at most. Bybit answers a rate limit (10006, 10018) and a clock or signature error (10002, 10004) with HTTP 200 and a non-zero `retCode`. The snapshot is therefore used only when `retCode` is 0 as well as the HTTP status.

The execution replay is one query for the account: `GET /v5/execution/list?category=spot&startTime=<watermark>&limit=100`. Rows for unsubscribed symbols are dropped ([Execution replay](connector-behaviour.md#execution-replay)).

| aspect | behaviour |
|---|---|
| window | 7 days a query; an older watermark is walked in 7-day windows, the last without `endTime` |
| history | 2 years; an older start is moved up and the replay is not exact |
| paging | `cursor` to the last page, then the window is emitted oldest first |
| long windows | more than 20 pages: read again up to the oldest row seen |
| limit | more than 100 requests: the replay stops, reports itself incomplete and continues from the housekeeping retry |
| identity | `execId`; `orderLinkId` is the client id |
| first connect | the replay runs before the start-up sweep's snapshot, from the resumed watermark if any |

| `execType` | handling |
|---|---|
| `Trade` | a fill |
| `Funding` | a funding payment, linear only ([below](#linear-perpetuals)) |
| `AdlTrade`, `BustTrade`, `Settle`, `Delivery` | skipped, as on the private stream |

The fee asset is `feeCurrency`. Without it the spot rule applies: a buy pays in base, a sell in quote, reversed for a negative maker rate.

## Balances

The snapshot is `GET /v5/account/wallet-balance?accountType=UNIFIED`, and the `wallet` topic streams changes; the venue time is the reply's `time` or the push's `creationTime` ([Balances](connector-behaviour.md#balances)).

| field | per coin | account row (linear, cross margin) |
|---|---|---|
| free | `walletBalance - locked - totalOrderIM - totalPositionIM - bonus` | `totalAvailableBalance` |
| locked | `locked + totalOrderIM + totalPositionIM` | `totalInitialMargin` |
| total | `walletBalance` | `totalWalletBalance` |
| equity | `equity` | `totalEquity` |
| maintenance | `totalPositionMM` | `totalMaintenanceMargin` |

The account row is in USD and is left out when `/v5/account/info` says `ISOLATED_MARGIN`.

## Dead man's switch

Disconnect-Cancel-All is an account setting, not a countdown the client refreshes. With `dead_mans_switch_s` set, each private connect sends `POST /v5/order/disconnected-cancel-all` with `{"product":"SPOT","timeWindow":N}` and subscribes `dcp.spot`.

| aspect | behaviour |
|---|---|
| window | `dead_mans_switch_s`, seconds, clamped to [3, 300] |
| trigger | Bybit starts the clock once every private connection subscribing `dcp.*` is gone, and resets it when one reconnects |
| availability | institutional accounts, enabled by an account manager; a refusal is logged and the session continues |
| shutdown | nothing is disarmed; the session cancels its own orders |

## Linear perpetuals

`category = "linear"` with `ws_url` on `/v5/public/linear` trades the USDT- and USDC-margined perpetuals.

| topic | linear |
|---|---|
| reference data | `instruments-info?category=linear`; only `contractType` `LinearPerpetual` (`LinearFutures` is refused) |
| units | `qty` in the base coin, multiplier 1; tick `priceFilter.tickSize`, lot `lotSizeFilter.qtyStep`, minimum `minOrderQty`, minimum notional `minNotionalValue` |
| instrument | a perpetual with reduce-only; base `baseCoin`, quote `settleCoin` (USDT for `BTCUSDT`, USDC for `BTCPERP`) |
| orders | `order.create` adds `"positionIdx":0`, and `"reduceOnly":true` when the engine sets it; a market order has no `marketUnit` |
| amend, cancel | take `category` and nothing else |
| `orders_per_second` | default 10, the per-UID limit for linear create, amend and cancel |
| private topics | `order`, `execution`, `position`, and `dcp.future` with the dead man's switch; no `wallet` |
| dead man's switch | product `DERIVATIVES`, covering USDT and USDC perpetuals, futures and inverse contracts |
| fees | `execFee` in the settlement coin, a maker rebate negative; empty or quote `feeCurrency` is `Quote`, another coin `Other` |
| cancel-all | `POST /v5/order/cancel-all` with `{"category":"linear","symbol":S}` per subscribed symbol |

[Accounting](../configuration.md#accounting) settles a linear instrument in its quote, so a configured quote that differs is replaced, with a warning.

### Position mode

Linear trades one-way only. Bybit has no call that reads the mode, so `load_reference_data()` reads `GET /v5/position/list?category=linear&symbol=S` for each symbol. One row (`positionIdx` 0) is one-way; one per side (1 and 2) is hedge mode.

A symbol in hedge mode, or one whose mode cannot be read, is refused and `fastmm-live` and `fastmm-gateway` exit 3. A hedge-mode row that appears later, on the position topic or in a reconciliation, is fatal for the venue. Leverage and margin mode are not read or set.

### Linear reconciliation

The execution replay runs first, with `category=linear` and no symbol. Then, for each settle coin of the subscribed instruments, two paged queries become one reconciliation: `GET /v5/order/realtime?category=linear&settleCoin=C&limit=50` and `GET /v5/position/list?category=linear&settleCoin=C&limit=200`.

The reconciliation holds one `Position` per subscribed instrument. Bybit lists only non-zero positions by settle coin, so an absent symbol is flat. The start-up sweep does the same, so a restart converges to the venue's position. A failed position list emits nothing.

### Positions between reconciliations

Bybit publishes the position on every order create, amend and cancel, changed or not. The connector compares it with the sum of the fills it forwarded once neither has changed for 1 s. It sends a `PositionUpdate` only when they differ, after a liquidation, ADL or another client's trade.

The two topics are not ordered against each other, so forwarding every position would count fills twice. `position_from_stream = false` turns the check off.

### Funding

An execution with `execType` `Funding` becomes a `Funding` event under its `execId`, on the `execution` topic or from `execution/list`. The amount is `-execFee` in `feeCurrency`, the settle coin when empty.

The sign is inferred from the transaction log page, which calls its `funding` field the opposite of `execFee`. Whether the `execution` topic pushes Funding rows is not documented, so the replay's one-minute sweep books what the stream does not.

### Mark, index and funding rate

| field | source |
|---|---|
| stream | `tickers.<sym>`: a snapshot, then deltas of the changed fields, cached per symbol and cleared on reconnect |
| mark, index | `markPrice`, `indexPrice` |
| funding | `fundingRate`, `nextFundingTime`, `fundingIntervalHour` (instruments-info `fundingInterval` if absent) |
| open interest | `singleOpenInterest`, one side; `openInterest` is both |

The stream pushes about 8 times a second per symbol, with `markPrice` in about one delta in ten ([Mark, index and funding](connector-behaviour.md#mark-index-and-funding)).

## Not handled

- `AdlTrade` and `BustTrade` rows: they reach the engine only through the position check or the next reconciliation.
- Leverage and margin mode on linear.
- `LinearFutures` and inverse contracts.

## Unconfirmed

| topic | open question |
|---|---|
| book sync | whether `u` increments by exactly one per delta; the sync requires only an increase |
| amend | whether amend `qty` includes the filled quantity, hence cancel and new in the testnet config |
| balances | whether `walletBalance` includes `locked`; the balance-related `rejectReason` texts |
| linear | the private `order`, `execution` and `position` payloads on testnet; whether `feeCurrency` is filled for linear (empty is read as the settle coin); whether a hedge-mode symbol with no position returns both rows |
| Disconnect-Cancel-All | the arming request and `dcp.*` topics are tested against a scripted fake exchange only; Bybit documents no way to turn the window off except through an account manager |

## Sources

Read 2026-09-26: [Bybit v5 API](https://bybit-exchange.github.io/docs/v5/intro) pages for instruments-info, place, amend and cancel order, open orders, cancel-all, trade history, position info, set DCP, [wallet balance](https://bybit-exchange.github.io/docs/v5/account/wallet-balance), [ticker](https://bybit-exchange.github.io/docs/v5/websocket/public/ticker), and the private `order`, `execution`, `position`, `wallet` and `dcp` topics.
