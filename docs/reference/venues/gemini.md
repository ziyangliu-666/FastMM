# Gemini

`kind = "gemini"` trades Gemini's linear perpetuals and spot through one WebSocket API; the example configuration is `configs/gemini-sandbox.toml`.

| | |
|---|---|
| Markets | linear perpetuals (`btcgusdperp`), spot (`btcusd`) |
| Order entry | WebSocket API; REST for a cancel while it is down |
| Environments | sandbox (the default), production |
| Keys | account-scoped key (`account-...`) with a time-based nonce |
| Replace | none: cancel and new |
| Dead man's switch | `cancelOnDisconnect`, on; optional heartbeat |
| Example config | `configs/gemini-sandbox.toml` |

Keys for the section are in [Configuration: gemini](../configuration.md#gemini).

## Connections

| Channel | Endpoint | Purpose |
|---|---|---|
| md | `ws_url` + `?snapshot=-1` | `{sym}@depth@100ms`, `{sym}@bookTicker`, `{sym}@trade`; `time` for the clock offset |
| order | `ws_api_url` (default `ws_url`) + `?cancelOnDisconnect=true` | authenticated at the upgrade; `orders@account`, `balances@account`, `order.place`, `order.cancel` |
| rest | `rest_url` | the requests below |

| Request | Use |
|---|---|
| `GET /v1/symbols/details/{symbol}` | reference data |
| `POST /v1/orders` | key check at start-up, reconciliation |
| `POST /v1/positions` | reconciliation, with a perpetual subscribed |
| `POST /v1/mytrades` | trade replay, per symbol |
| `POST /v1/balances`, `/v1/margin` | balances |
| `POST /v1/perpetuals/fundingPayment?since=&to=` | funding |
| `POST /v1/order/cancel` | a cancel while the order connection is down |
| `POST /v1/order/cancel/session` | kill switch, order-connection loss |
| `POST /v1/heartbeat` | with `heartbeat = true` |

| Environment | REST | WebSocket |
|---|---|---|
| production | `api.gemini.com` | `ws.gemini.com` |
| sandbox | `api.sandbox.gemini.com` | `ws.sandbox.gemini.com` |

The URLs default to the sandbox, and `testnet` must match the hosts. The sandbox lists more perpetuals than production and returns nothing from the public `fundingamount`.

## Keys and signing

REST requests are `POST`s with an empty body. The JSON payload goes base64-encoded in `X-GEMINI-PAYLOAD`, signed with hex HMAC-SHA384 in `X-GEMINI-SIGNATURE`, next to `X-GEMINI-APIKEY`. The order connection signs a fresh nonce in the same headers on its upgrade request.

The WebSocket takes only account-scoped keys (`account-...`) with a time-based nonce; a `master-` key is refused at start-up. The connector sends venue-time milliseconds, each above the last, because the sandbox refuses a nonce that has not increased. One key serves one process: two clients on a key refuse each other's nonces.

## Accounts

Perpetuals trade in a derivatives account, opened after Gemini's Derivatives Knowledge Assessment and funded from the exchange account, with a key scoped to it. With a perpetual configured, start-up asks `/v1/positions`. An exchange account answers `AccountNotOfTypeRequired` and `fastmm-live` exits 3; its perpetual orders would be refused with `InsufficientFunds`.

## Instruments

| Field | Value |
|---|---|
| Contract | 1 base unit; quantities are base units, `contract_multiplier` 1 |
| Tick | `quote_increment` |
| Lot | `tick_size`, Gemini's name for the quantity step |
| Minimum | `min_order_size` |
| Base, quote | `base_currency`, `quote_currency`; GUSD or USDC for perpetuals, the currency PnL is booked in |

An inverse perpetual is refused. A `status` other than `open`, `post_only` or `limit_only` disables the instrument.

## Market data

With `snapshot=-1` the first `depthUpdate` after subscribing is the full book, and the rest are diffs.

| Diff | Action |
|---|---|
| `U` ≤ last `u` < `u` | applied; consecutive frames overlap at `U` = previous `u` |
| `u` ≤ last `u` | dropped |
| `U` > last `u` | gap: `Resyncing`, then unsubscribe and subscribe the depth stream |

A snapshot looks like any diff, so the first frame after the connection opens, or after the unsubscribe's reply, is taken as the snapshot. Past 512 levels a side the book keeps those nearest the touch. Production spot asks at 1e10 and above do not fit the 8-decimal fixed point and are left out.

The stream has no sequence number and the venue sends no heartbeat. The connector sends `ping` every `ping_interval_ms` and reconciles after every reconnect.

## Orders

| Engine order | Gemini |
|---|---|
| limit | `type` `LIMIT`, `timeInForce` `GTC` |
| IOC, FOK | `timeInForce` `IOC`, `FOK` |
| post-only | `timeInForce` `MOC` |
| market | `type` `MARKET` |
| reduce-only | sent as a plain order; Gemini has none |

There is no amend, so `supports_replace` is off. `order.cancel` takes the venue's id only, as a string. A cancel of an order not yet named, by the `order.place` result or its `NEW` event, waits for it. With the order connection down, a new order is refused (`TransportFull`) and a cancel goes over REST.

## Order events

`orders@account` sends an `orderUpdate` per change.

| Status | Engine event |
|---|---|
| `NEW` | ack |
| `PARTIALLY_FILLED`, `FILLED` | fill: `t`, `L`, `Z` this execution, `z` left, `m` maker; fee `n` on `FILLED` only |
| `CANCELED` | cancel ack with the cumulative `Z` |
| `CANCELED` for `MakerOrCancelWouldTake`, `ImmediateOrCancelWouldPost`, `FillOrKillWouldNotFill`, `SelfCrossPrevented`, `ExceedsPriceLimits` | `OrderExpired` |
| `REJECTED` | reject, reason in `r` |

A refused placement is answered by both a `400` reply and a `REJECTED` event; the engine gets one reject.

## Reconciliation and fills

A reconciliation replays trades, then reads `/v1/orders` and `/v1/positions` as one snapshot ([Reconciliation](connector-behaviour.md#reconciliation)). A perpetual absent from the positions is flat, and spot has no position rows. A broken trade (`break` = `full`) is not booked.

The replay is `POST /v1/mytrades` per symbol: trades at or after `timestamp` (ms), at most 500 a page, listed newest first. Rows carry `tid`, `order_id` and `client_order_id`, so no order lookup is needed; the next page starts from the newest row ([Execution replay](connector-behaviour.md#execution-replay)).

## Balances

| Source | Rows |
|---|---|
| `POST /v1/balances` | per asset: free `available`, locked `amount - available`, total and equity `amount` |
| `POST /v1/margin`, with a perpetual | account row: free `available_margin`, locked `initial_margin`, total and equity `margin_assets_value`, maintenance `margin_maintenance_limit` |
| `balances@account` on the order connection | updates; a perpetual fill asks for the snapshot again |

An exchange account answers `/v1/margin` with `AccountNotOfTypeRequired` and gets no account row. The snapshot's venue time is the reply's latest `_timestamp`; a push's is `u`.

## Mark price and funding

| Stream | Carries |
|---|---|
| `{sym}@markPrice` on `wss://ws.gemini.com` | mark `p` every 5 s; `i` is the index times an undocumented per-symbol factor, so no index is sent |
| `{sym}@fundingAmount` | each minute: `f`, the estimated funding of one long contract over the hour in quote; rate `f / p` per `i` minutes (60); next funding `T` |

Both are subscribed for perpetuals in their own request and are not in the published docs. Neither sends a snapshot on subscribe, so funding arrives up to a minute after connect. There is no open interest ([Mark, index and funding](connector-behaviour.md#mark-index-and-funding)).

Funding payments come from `perpetuals/fundingPayment`: hourly transfers, `Debit` paid and `Credit` received, in `assetCode`. The query runs with every reconciliation's replay and once a minute. A transfer has no id, so its symbol and time are its id.

## Dead man's switch

`cancel_on_disconnect` (on by default) opens the order connection with `cancelOnDisconnect=true`. The venue then cancels the orders placed on it when it closes.

A key with "Requires Heartbeat" has its orders cancelled after 30 s without an authenticated request. `heartbeat = true` sends `POST /v1/heartbeat` every 15 s, and a lapse trips `DeadMansSwitchLost`.

The kill switch and an order-connection loss send `order/cancel/session`: the key's orders on every symbol, since Gemini has no per-symbol cancel-all. A 429 is waited out. Gemini has no batch order entry.

## Rate limits

| Limit | Value |
|---|---|
| REST, private endpoints | 600 a minute, 5 a second recommended; a burst of 5 queued, then 429 |
| WebSocket, from `conninfo` | 3500 orders and 7000 request weight per 10 s |
| client-side `orders_per_second` | default 20 |

A 429 or `-1003` pauses sending for 1 s.

## Connection health

`stale_ms` defaults to 2000 ms. `dead_ms` is at least twice `ping_interval_ms` plus 5000 ms, 30000 ms by default.

## Errors

| Error | Action |
|---|---|
| `RateLimit`, 429, `-1003` | rate limit |
| `InvalidNonce` | clock resync |
| `InvalidSignature`, `InvalidApiKey`, `MissingRole`, `EndpointMismatch`, 401, 403, `-1002` | fatal |
| `RemoteAddressForbidden` | hard stop |
| `InsufficientFunds`, `InvalidPrice`, `LimitPriceOffTick`, `InvalidQuantity`, `DuplicateOrder`, `MakerOrCancelWouldTake`, `ExceedsPriceLimits` | reject |

## Not handled

The archived v1 and v2 streams, stop orders, `positions@account` (documented for prediction markets), OAuth, master keys and their `account` field.

## Unconfirmed

* The sandbox's `/v1/mytrades` returns `[]` even for hours-old trades, so on the sandbox the replay finds nothing. A fill the order stream missed is then booked only as the cumulative quantity a snapshot reports.
* Whether production's `mytrades` pages forward from `timestamp`, given the list is newest first.
* The stream's fee `n` was 0 on every sandbox fill over the WebSocket, while a REST IOC was charged.
* Whether `fundingPayment` takes `since` and `to` in the query; perpetual order entry.
* How soon `cancelOnDisconnect` acts on a connection that dies without a FIN; after a SIGKILL the sandbox cancelled within 2 s.
* Whether `orders@account` covers orders placed over REST.
* Whether `btcusdcperp` and `btcgusdperp` are one book.

## Sources

Read 2026-09-30: [developer.gemini.com](https://developer.gemini.com/), its OpenAPI spec `specs/openapi/rest.yaml` and AsyncAPI spec `specs/asyncapi/websocket.yaml` (0.10.7), and the support article "How is the Funding Amount calculated?".
