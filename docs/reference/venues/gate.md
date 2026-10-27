# Gate

`kind = "gate_usdt"` (aliases `gate`, `gate_futures`) trades Gate APIv4 USDT-settled perpetual futures over the futures WebSocket API, in single position mode.

| | |
|---|---|
| Markets | USDT perpetuals (`BTC_USDT`, `ETH_USDT`); `settle = "usd1"` or `"btc"` selects the other settle currencies' endpoints |
| Order entry | WebSocket API, REST fallback |
| Environments | production, Gate futures TestNet |
| Keys | HMAC-SHA512 |
| Replace | amend in place; the shipped configs use cancel and new |
| Dead man's switch | `countdown_cancel_all`, per account |
| Example config | `configs/gate-usdt.toml` |

## Connections

All three WebSocket connections use `ws_url` (`wss://fx-ws.gateio.ws/v4/ws/usdt`).

| channel | purpose |
|---|---|
| md | `futures.obu` (`ob.SYM.50` or `.400`), `futures.book_ticker` as BookTicker, `futures.trades`, `futures.tickers` as PerpState |
| private | `futures.login`, then `futures.orders`, `futures.usertrades`, `futures.positions`, `futures.balances`, each with the auth block and the account's user id |
| trade | `futures.login`, then `futures.order_place`, `futures.order_cancel`, `futures.order_amend` with `req_id` = `<kind><client id>` |
| rest | `rest_url` (`https://fx-api.gateio.ws`), the requests below |

| request under `/api/v4/futures/{settle}/` | use |
|---|---|
| `contracts/{name}` | reference data |
| `order_book?limit=1` | clock offset; the host has no time endpoint |
| `accounts` | user id, balances |
| `positions?holding=true` | reconciliation |
| `orders?status=open` | reconciliation |
| `orders` (POST, DELETE, PUT) | order entry fallback |
| `orders?contract=` (DELETE) | kill switch |
| `my_trades_timerange` | fill replay |
| `fee` | account rates, with `fetch_fees = true` |
| `countdown_cancel_all` | dead man's switch |

Every connection sends `futures.ping` every `ping_interval_ms`. It upgrades with `X-Gate-Size-Decimal: 1`, so sizes arrive as strings and contracts may be fractional.

| key | default |
|---|---|
| `stale_ms` | 2000 ms |
| `dead_ms` | twice `ping_interval_ms` plus 5000 ms; 35000 ms by default |

## Keys and signing

| where | signature |
|---|---|
| REST | headers `KEY`, `Timestamp` (Unix seconds, within 60 s of the venue), `SIGN` = hex HMAC-SHA512(secret, `METHOD\npath\nquery\nhex SHA512(body)\nTimestamp`) |
| subscription `auth` | signs `channel=<c>&event=subscribe&time=<t>` |
| WebSocket API `payload.signature` | signs `api\n<channel>\n<req_param>\n<timestamp>`; `req_param` is empty for the login |

The signature covers the exact bytes sent.

## Instruments and account

| field | value |
|---|---|
| quantity | contracts |
| `contract_multiplier` | `quanto_multiplier`, base units a contract |
| tick | `order_price_round` |
| lot | 1 contract |
| minimum, maximum | `order_size_min`, `order_size_max`; no minimum notional |
| base, quote | the contract's prefix, the settle currency |
| accepted | `type` `direct`; a contract not `trading` or `in_delisting` is disabled |

[Accounting](../configuration.md#accounting) settles in the settle currency. With keys, `load_reference_data()` reads `GET /accounts` for the user id the private channels need. It refuses an account in dual position mode, and `fastmm-live` exits 3. A `dual_long` or `dual_short` position seen later is fatal for the venue. Leverage and margin mode are not read or set.

## Market data

`futures.obu` sends a full snapshot (`full: true`, depth id `u`) after subscribing, then increments with `U` and `u`. An increment applies when `U` is the local id + 1. Anything else re-subscribes the stream for a new snapshot, at most once per 2 s, and again after 10 s without one.

| `book_level` | push interval |
|---|---|
| 50 | 20 ms |
| 400 | 100 ms |

## Orders

| field | value |
|---|---|
| `size` | signed contracts, a bid positive |
| `price` | a string; `"0"` with `tif` `ioc` for a market order |
| `tif` | `gtc`, `ioc`, `poc` (post-only), `fok` |
| `text` | `t-` + FastMM's 14-character id; Gate requires the prefix |
| `reduce_only` | when the engine sets it |

The venue answers an order request twice: `{"ack":true}` when it took it, then the result or `data.errs` with a label and message. The engine's ack comes from the result (`emit_ack_from_response`) or from the `orders` channel's first `open`. Cancel and amend name the order by the venue's id once a reply gave it, else by its text.

Amend keeps the order in place. A size increase moves it to the back of the queue.

## Private channels

| channel | event | engine message |
|---|---|---|
| `orders` | `finished`, `finish_as` `cancelled` | cancel ack, `size - left` filled |
| `orders` | `finish_as` `ioc`, `stp`, `reduce_only`, `position_closed`, `reduce_out`, `liquidated`, `auto_deleveraged` | `OrderExpired` |
| `orders` | `filled`; `open` with a part filled | nothing; fills come on `usertrades` |
| `usertrades` | a trade | fill: `id` the execution id, `fee` in the settle currency (a rebate negative), `role` the liquidity |
| `positions` | a position | compared with forwarded fills; corrects the engine when they differ for 1 s (`position_from_stream`) |
| `balances` | an item | asks for the balances again, at most once a second |
| `balances` | `type` `fund`, `text` starting with a subscribed contract | `Funding` under `<time_ms>:<contract>` |

The connector completes a fill's cumulative and remaining quantity from its order shadow. The shape of a funding item's `text` is not documented: a payment the connector cannot place is not booked.

## Reconciliation and fills

The fill replay runs first, then `orders?status=open&contract=C&limit=100` per subscribed contract and `positions?holding=true`. They become one Begin, OpenOrder*, Position*, End sequence; a contract the positions do not list is flat.

| aspect | fill replay |
|---|---|
| query | `GET /my_trades_timerange?from=&to=&limit=1000&offset=` |
| scope | one stream for the account |
| window | a day a query, 30 days back |
| paging | newest first, by `offset` |

## Balances

The balance leg reads `GET /accounts` after every snapshot, one row for the settle currency ([Balances](connector-behaviour.md#balances)).

| field | source |
|---|---|
| free | `available` |
| locked | `order_margin` + `position_margin` |
| total | `total` |
| equity | `total` + `unrealised_pnl` |
| maintenance | `maintenance_margin` |

## Dead man's switch

`POST /countdown_cancel_all {"timeout":N}` with `dead_mans_switch_s` is refreshed at a third of the window. It covers every contract of the account. A lapse kills the venue with `DeadMansSwitchLost`, and `disconnect()` sends `timeout` 0 on a blocking connection ([Dead man's switch](connector-behaviour.md#dead-mans-switch)).

## Rate limits

| limit | value |
|---|---|
| order placement and amend | 100 r/s per UID, WebSocket API and REST together |
| cancels | 200 r/s |
| other private endpoints | 200 r/10 s each |
| public endpoints | 200 r/10 s per endpoint and IP |
| client-side `orders_per_second` | default 50, on new orders and amends |

The limiter reads `X-Gate-RateLimit-Requests-Remain` and `-Limit` on REST (reset in seconds) and `x_gate_ratelimit_*` on the WebSocket API (reset in ms). A 429 or `TOO_MANY_REQUESTS` cools down until the reset.

## Errors

| label | connector action |
|---|---|
| `INVALID_KEY`, `INVALID_SIGNATURE`, `IP_FORBIDDEN`, `READ_ONLY`, `FORBIDDEN`, `ACCOUNT_LOCKED`, `POSITION_DUAL_MODE` | fatal |
| `REQUEST_EXPIRED` | clock resync |
| `TOO_MANY_REQUESTS`, `TOO_BUSY` | rate limit |
| `SERVER_ERROR`, `INTERNAL` | back off |
| `ORDER_NOT_FOUND`, `ORDER_NOT_OWNED` | reconcile |
| `INSUFFICIENT_AVAILABLE` | insufficient balance |
| `ORDER_POC_IMMEDIATE` | post-only would cross |
| `SIZE_TOO_SMALL`, `SIZE_TOO_LARGE`, `PRICE_TOO_DEVIATED`, `INVALID_PRECISION` | lot, size and price rejects |

## Not handled

- `futures.order_book_update`, the REST-snapshot variant of the book.
- Batch orders (`futures.order_batch_place`) and `futures.order_cancel_ids`.
- The fractional lot step of a contract with `enable_decimal`: the lot stays 1 contract.
- The account book as a funding source.
- USD1- and BTC-settled contracts beyond the endpoint names.
- SBE market data (`/v4/ws/usdt/sbe`).

## Sources

Read 2026-10-03: [Gate futures WebSocket](https://www.gate.com/docs/developers/futures/ws/en/) (channels and the WebSocket API) and [Gate APIv4](https://www.gate.com/docs/developers/apiv4/en/) (signing, rate limits, labels).
