# Coinbase

Two connectors trade Coinbase spot over REST: `coinbase_advanced` for individual accounts on the Advanced Trade API, and `coinbase_exchange` for institutional accounts on the Exchange API.

| | Advanced Trade | Exchange |
|---|---|---|
| `kind` | `coinbase_advanced` | `coinbase_exchange` |
| Markets | spot (`BTC-USD`, ...) | spot (`BTC-USD`, ...) |
| Order entry | REST | REST |
| Environments | production only | sandbox (default), production |
| Keys | CDP key, EC P-256 | key, secret, passphrase |
| Replace | none: cancel and new | none: cancel and new |
| Dead man's switch | none | none |
| Example config | `configs/coinbase-advanced.toml` | `configs/coinbase-sandbox.toml` |

## Advanced Trade

The Advanced Trade sandbox answers with fixed replies and matches nothing, so `testnet = true` with `api.coinbase.com` is refused.

### Connections

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`wss://advanced-trade-ws.coinbase.com`) | `level2`, `market_trades`, `heartbeats`; no key |
| user | `ws_private_url` (default `wss://advanced-trade-ws-user.coinbase.com`) | `user` (a JWT in the subscribe), `heartbeats` |
| rest | `rest_url` (`https://api.coinbase.com`), paths under `/api/v3/brokerage` | the requests below; order entry |

| request | use |
|---|---|
| `GET /market/products/<id>`, `GET /time` | reference data, clock offset (public) |
| `POST /orders` | new order |
| `POST /orders/batch_cancel` `{"order_ids":[..]}` | cancel, kill switch, user-channel loss |
| `GET /orders/historical/batch?order_status=OPEN&product_ids=` | reconciliation, kill switch, paged by `cursor` |
| `GET /orders/historical/fills?order_ids=` | the executions of an order whose cumulative quantity grew |
| `GET /orders/historical/fills?product_ids=&start_sequence_timestamp=` | fill replay, paged by `cursor` |
| `GET /orders/historical/<order_id>` | the client id of an order a replayed fill names |
| `GET /accounts`, paged by `cursor` | balances |

### Keys and signing

| key | value |
|---|---|
| `api_key` | the CDP key name, `organizations/<org>/apiKeys/<id>` (`${COINBASE_API_KEY_NAME}`) |
| `api_secret` | the EC P-256 private key PEM; `\n` escapes are read as newlines (`${COINBASE_API_PRIVATE_KEY}`) |

Every private request carries `Authorization: Bearer <JWT>`, a new JWT each time, signed ES256. The header is `{"alg":"ES256","kid":<key name>,"nonce":<random hex>,"typ":"JWT"}`, and the payload:

```text
{"sub": <key name>,
 "iss": "cdp",
 "nbf": <now>,
 "exp": <now + 120 s>,
 "uri": "<METHOD> api.coinbase.com<path>"}
```

The `uri` has no query string; the user subscribe carries a JWT without `uri`. Ed25519 CDP keys, which Advanced Trade does not accept, are refused at start-up. Every request carries `User-Agent`.

### Reference data

| FastMM | Advanced Trade field |
|---|---|
| tick | `quote_increment` |
| lot | `base_increment` |
| minimum size | `base_min_size` |
| minimum notional | `quote_min_size` |
| base, quote | `base_currency_id`, `quote_currency_id` |

A `product_type` other than `SPOT` is refused. A `status` other than `online`, `trading_disabled`, `is_disabled`, `view_only` or `cancel_only` disables the product.

### Market data

A `level2` snapshot is the whole book, tens of thousands of levels for BTC-USD, each side best first. The connector keeps each side's first 1024 levels and resyncs on a side out of price order. Updates carry each level's new size; an update side longer than 1024 levels keeps the levels nearest the touch.

Every message of the connection carries `sequence_num`, one more than the last. A gap resyncs every book of the connection (`SequenceGap`) and resubscribes `level2` for each product, at most every 2 s per product. A connection that goes quiet while a snapshot is still arriving waits for it instead of resubscribing.

`market_trades` `side` is the maker's, so the aggressor is the other side. Its snapshot of trades before subscribing is dropped.

### Orders

`client_order_id` is FastMM's client order id. All orders are sized in `base_size`.

| FastMM | Advanced Trade order |
|---|---|
| post-only | `limit_limit_gtc` with `post_only: true` |
| IOC | `sor_limit_ioc` |
| FOK | `limit_limit_fok` |
| market | `market_market_ioc` |

There is no replace; `supports_replace = true` is refused. A refusal is `"success": false` with `new_order_failure_reason`, or the deprecated `error`. `INVALID_LIMIT_PRICE_POST_ONLY` becomes `PostOnlyWouldCross`. A cancel names the venue's `order_id`; one sent before the reply or the user channel brings that id waits for it.

### User channel

Every message carries an order's whole state and no execution. Orders with other client ids are ignored.

| order state | engine event |
|---|---|
| first message of an order | ack |
| `CANCELLED` | cancel ack; `OrderExpired` for an IOC or FOK |
| `EXPIRED` | `OrderExpired` |
| `FAILED` | reject |
| larger `cumulative_quantity` | the connector reads `fills?order_ids=` and sends the new fills |

Fills go out once per `trade_id`, with their price and commission in the quote currency. A fill reaches the engine a REST round trip after the user channel reported it. Fills the endpoint does not list yet are read again every 500 ms, ten times, then left to the replay. A `sequence_num` gap on the user channel reconciles.

### Reconciliation and fills

A reconciliation replays the fills of the subscribed products since the watermark, then lists the open orders as one Begin / OpenOrder* / End. Spot has no positions. A fill names its order by `order_id` only; the connector asks `GET /orders/historical/<id>` for ids it has not seen, as on [Binance Spot](binance-spot.md). See [Execution replay](connector-behaviour.md#execution-replay).

On user-channel loss the connector lists and cancels the products' open orders over REST (`cancel_on_order_channel_loss`). It reconciles when the channel is back.

### Balances

`GET /api/v3/brokerage/accounts`, paged by `cursor`, gives free = `available_balance`, locked = `hold`, total and equity = their sum. Vault, futures and international accounts are left out. There is no balance stream for spot, so every fill asks for the leg again ([Balances](connector-behaviour.md#balances)).

### Dead man's switch and kill switch

Nothing in the Advanced Trade API cancels spot orders when a client disconnects. Orders a process left resting stay until the next session's start-up sweep cancels them.

The kill switch lists the open orders of the subscribed products and sends `batch_cancel`, 50 a request, over its own blocking connection. It repeats until the list is empty, at most 3 rounds, and waits out a 429.

### Rate limits

Client-side `orders_per_second` (default 8) applies to new orders; a 429 pauses sending for 1 s. A market-data connection without traffic for `stale_ms` (default 2000 ms) is `Stale`. The documentation gives no REST figures. WebSocket connections and unauthenticated messages are limited to 8 a second per IP.

### Errors

| venue answer | action |
|---|---|
| `INSUFFICIENT_FUND(S)`, `INVALID_LEDGER_BALANCE` | reject, `InsufficientBalance` |
| `INVALID_LIMIT_PRICE_POST_ONLY` | reject, `PostOnlyWouldCross` |
| `INVALID_PRICE_PRECISION`, `INVALID_SIZE_PRECISION` | reject, instrument disabled |
| `DUPLICATE_CLIENT_ORDER_ID` | reject, `DuplicateId` |
| `UNTRADABLE_PRODUCT`, `TRADING_DISABLED`, `ORDER_ENTRY_DISABLED` | reject, instrument disabled |
| `GEOFENCING_RESTRICTION` | fatal |
| `UNKNOWN_CANCEL_ORDER`, `ORDER_IS_FULLY_FILLED` | unknown order |
| HTTP 401, 403 | fatal |
| HTTP 429 | rate limit |
| HTTP 5xx | back off |

### Not handled

Order edits (`/orders/edit`), perpetuals and US futures, stop and bracket orders, `quote_size`, portfolios other than the key's, and the `ticker` channels.

### Unconfirmed

No order has been placed through this connector yet. Unconfirmed:

- the create, cancel and user-channel payloads of real orders;
- whether the fills endpoint lists newest first, and how soon it lists an execution the user channel reported;
- the `batch_cancel` maximum, the REST rate limits and the `cancel_reason` values.

Sources (read 2026-09-30): the [OpenAPI spec](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/rest-api/advanced-trade-spec.yaml), the [AsyncAPI spec](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/advanced-trade-asyncapi.json) and the [Advanced Trade docs](https://docs.cdp.coinbase.com/advanced-trade/).

## Exchange

The Exchange API is for institutional accounts. Its sandbox is an account of its own, with fake funds, and matches orders. `testnet = true` with a production host, or `false` with a sandbox host, is refused.

### Connections

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url`: production `wss://ws-feed.exchange.coinbase.com`, sandbox `wss://ws-feed-public.sandbox.exchange.coinbase.com` | `level2_batch` (or `level2`, signed), `matches`, `heartbeat` |
| user | `ws_private_url` (default `ws_url`, a second connection) | signed subscribe to `user` and `heartbeat` |
| rest | `rest_url`: production `https://api.exchange.coinbase.com`, sandbox `https://api-public.sandbox.exchange.coinbase.com` | the requests below; order entry |

| request | use | documented limit |
|---|---|---|
| `GET /products/<id>`, `GET /time` | reference data, clock offset (30 s tolerance) | 10 / s per IP, bursts of 15 |
| `POST /orders` | new order | 15 / s per profile, bursts of 30, for every private request |
| `DELETE /orders/client:<client_oid>?product_id=` | cancel | as above |
| `DELETE /orders?product_id=` | kill switch, user-channel loss | as above |
| `GET /orders?status=open&status=pending&status=active&limit=1000` | reconciliation, paged by `CB-AFTER` | as above |
| `GET /fills?product_id=&start_date=` | fill replay, newest first, paged by `CB-AFTER` | 10 / s per profile, bursts of 20 |
| `GET /orders/<order_id>` | the client id of an order a replayed fill names | 15 / s per profile |
| `GET /accounts` | balances | as above |

### Keys and signing

| key | environment variable |
|---|---|
| `api_key` | `${FASTMM_COINBASE_API_KEY}` |
| `api_secret` | `${FASTMM_COINBASE_API_SECRET}`, base64 |
| `api_passphrase` | `${FASTMM_COINBASE_API_PASSPHRASE}` |

`CB-ACCESS-SIGN` is base64 HMAC-SHA256 over timestamp + method + path with query + body, keyed with the base64-decoded secret. It goes with `CB-ACCESS-KEY`, `CB-ACCESS-TIMESTAMP` (seconds, three decimals) and `CB-ACCESS-PASSPHRASE`. A WebSocket subscribe carries `signature`, `key`, `passphrase` and `timestamp`, signed over timestamp + `GET/users/self/verify`.

Every REST request carries `User-Agent`; the venue answers 400 without one. A secret that is not base64 stops the session at start-up.

### Reference data

| FastMM | Exchange field |
|---|---|
| tick | `quote_increment` |
| lot, minimum size | `base_increment` |
| minimum notional | `min_market_funds` |
| base, quote | `base_currency`, `quote_currency` |

A `status` other than `online`, `trading_disabled` or `cancel_only` disables the product.

### Market data

Both depth channels send the whole book as a snapshot, best level first, then updates of a level's new size. The connector keeps each snapshot side's first 1024 levels and resyncs on a side out of price order. An update side longer than 1024 levels keeps the levels nearest the touch.

The level2 channels carry no sequence number, so the feed numbers each product's book messages itself. It treats a lost trade on the same connection as a lost update. Either of these emits `Resyncing` (`SequenceGap`) and resubscribes the depth channel, at most every 2 s per product:

- a `match` whose `trade_id` is not the previous one plus one;
- a heartbeat's `last_trade_id` still unseen at the next heartbeat, one a second.

`matches` gives the trades. Its `side` is the maker's, so the aggressor is the other side.

### Orders

Orders go over REST only; the connector does not use FIX. `client_oid` must be a lowercase variant-1 UUIDv4: FastMM's 48-bit client order id is the last 12 hex digits of `666d0000-0000-4000-8000-` + id.

| FastMM | Exchange order |
|---|---|
| post-only | `post_only: true` with `time_in_force` `GTC` |
| IOC, FOK | `time_in_force` |
| self-trade prevention | `stp` key (default `dc`) |

There is no replace: `supports_replace = true` is refused, and the quote manager sends cancel and new. A post-only order that would take comes back 200 with `status: rejected` and a `reject_reason`, which becomes `PostOnlyWouldCross`. New orders are acknowledged from the reply and from `received`.

### User channel

Orders of other software on the same profile are ignored.

| message | engine event |
|---|---|
| `received` | ack; the only message naming `client_oid`, it starts tracking the order by `order_id` |
| `match` | fill for the tracked maker or taker leg; `trade_id` is the execution id |
| `done`, `reason: canceled` | cancel ack; `OrderExpired` when `cancel_reason` is 101 (time in force) |
| `change` | the order's new remaining size |

A fill's fee is price × size × `maker_fee_rate` or `taker_fee_rate`, in the quote currency.

### Reconciliation and fills

A reconciliation replays the fills of each subscribed product since the watermark, then lists the open orders as one Begin / OpenOrder* / End. Spot has no positions. A fill names its order by `order_id` only; the connector asks `GET /orders/<order_id>` for ids it has not seen, at most 16 a replay. See [Execution replay](connector-behaviour.md#execution-replay).

On user-channel loss the connector cancels each product's orders over REST (`cancel_on_order_channel_loss`). It reconciles when the channel is back.

### Balances

`GET /accounts` gives free = `available`, locked = `hold`, total and equity = their sum, truncated to 8 decimals. After each fetch the connector subscribes the authenticated `balance` channel with the account ids of the kept assets. A fill also asks for the leg again, because the channel "does not track every update" ([Balances](connector-behaviour.md#balances)).

### Dead man's switch and kill switch

The Exchange cancels on disconnect only for orders entered on a FIX session that asked for it at logon (tag 8013). Nothing on REST or the WebSocket feed arms one. Orders a process left resting stay until the next session's start-up sweep cancels them.

The kill switch sends `DELETE /orders?product_id=` per product over its own blocking connection. It repeats until a round cancels nothing, at most `cancel_all_rounds` (3) rounds, and waits out a 429.

### Rate limits

Client-side `orders_per_second` (default 8) applies to new orders; a 429 pauses sending for 1 s. A market-data connection without traffic for `stale_ms` (default 2000 ms) is `Stale`.

### Errors

Errors are mapped by HTTP status, then refined by the `message` text.

| answer | action |
|---|---|
| 401, 403 | fatal |
| 429 | rate limit |
| 404 | unknown order |
| 5xx | back off |
| message about the timestamp | clock resync |
| insufficient funds | reject |
| price or size precision | disable the instrument |

### Not handled

`level3` and `full`, `ticker`, FIX order entry and market data, stop orders, and profiles other than the key's. Batch order entry exists only on FIX, 15 orders a message ([Batch order entry](connector-behaviour.md#batch-order-entry)).

### Unconfirmed

The public feed has run against production and the sandbox; no private request has run yet. Unconfirmed:

- every private request and the user channel;
- the `message` texts of refused requests, and the form of `cancel_reason`;
- whether a post-only order that would take comes back `rejected` or as `done` with reason 101;
- whether `change.new_size` is the remaining size.

Sources (read 2026-09-30): the [Exchange docs](https://docs.cdp.coinbase.com/exchange/), covering the REST reference, WebSocket channels, authentication, rate limits and sandbox, and their changelog.
