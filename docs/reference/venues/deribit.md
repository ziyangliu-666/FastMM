# Deribit

`kind = "deribit"` trades Deribit options and futures over one JSON-RPC 2.0 WebSocket (`configs/deribit-testnet.toml`).

| | |
|---|---|
| Markets | options, futures, perpetuals |
| Order entry | WebSocket (`private/buy`, `private/sell`, `private/edit`, `private/cancel`) |
| Environments | testnet (`wss://test.deribit.com/ws/api/v2`), production |
| Keys | client id and client secret |
| Replace | `private/edit`, in place |
| Dead man's switch | cancel-on-disconnect, scope `connection` |
| Example config | `configs/deribit-testnet.toml` |

The same methods are available over REST at `https://test.deribit.com/api/v2/<method>`.

## Connections

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url`, unauthenticated | `public/set_heartbeat`; `public/subscribe` to `book.NAME.100ms`, `ticker.NAME.100ms`, `trades.NAME.100ms` |
| private | `ws_private_url` (default `ws_url`), a second connection | authentication, heartbeat, cancel-on-disconnect, the user subscriptions, order entry and reconciliation (below) |
| rest | `rest_url` | `public/get_time`, `public/get_instruments?currency=C&kind=option\|future`; the kill switch |

| private method | use |
|---|---|
| `public/auth` | `client_credentials`, then `refresh_token` |
| `public/set_heartbeat` | heartbeats, interval at least 10 s |
| `private/enable_cancel_on_disconnect` | the venue-side switch |
| `private/subscribe` | `user.orders.KIND.CURRENCY.raw`, `user.trades.KIND.CURRENCY.raw` |
| `private/buy`, `private/sell`, `private/edit`, `private/cancel`, `private/cancel_by_label` | order entry |
| `private/get_user_trades_by_currency_and_time` | execution replay |
| `private/get_open_orders_by_currency`, `private/get_positions` | reconciliation |
| `private/get_account_summaries` | balances |
| `private/cancel_all_by_instrument` over REST, `Authorization: Basic base64(client_id:client_secret)` | kill switch, private-connection loss |

## Keys

| key | value |
|---|---|
| `api_key` | the client id, `${FASTMM_DERIBIT_CLIENT_ID}` |
| `api_secret` | the client secret, `${FASTMM_DERIBIT_CLIENT_SECRET}` |

`raw` book intervals need an authenticated connection.

## Messages

Requests are `{"jsonrpc":"2.0","id":..,"method":..,"params":{..}}`. Order requests use the string id `n|c|r` + the 14-character client id; control requests use small integers. Notifications are `{"method":"subscription","params":{"channel","data"}}`, and responses carry `result` or `error {code, message, data}`. Prices and amounts often have exponents (`1.0002e6`) and are parsed exactly from the raw token.

## Units

Engine quantities are contracts: `contract_multiplier` is Deribit's `contract_size`, 1 BTC for BTC options and 10 USD for BTC-PERPETUAL. Orders go with `contracts`; book, trade and fill amounts are divided by the contract size. Prices are rounded passively onto `tick_size_steps`: for BTC options, 0.0001, and 0.0005 from 0.005.

| fill `fee_currency` | fee asset |
|---|---|
| the quote coin | `Quote`; BTC options are quoted in BTC, so base equals quote |
| the base coin, linear instrument | `Base` |
| the base coin, inverse future | `Other`: counted in `unconverted_fees`, not booked |

## Market data

The first `book.NAME.interval` notification is a full snapshot. Every later change must have `prev_change_id` equal to the previous `change_id`; the ids are not consecutive. A mismatch emits `Resyncing` and unsubscribes and subscribes the book channel for a new snapshot. A snapshot missing for 10 s does the same, at most once per 2 s per instrument.

A snapshot or change longer than 1024 levels a side keeps the 1024 nearest the touch; BTC-PERPETUAL snapshots run to about 1200 bids. The parser selects levels by price and does not rely on their order.

Each option ticker yields a `BookTicker` and an `OptionTicker` with mark, IVs, greeks and the underlying price ([Options](../options.md)). Futures and perpetual tickers also yield a `PerpStateMsg` ([Mark, index and funding](connector-behaviour.md#mark-index-and-funding)):

| field | source |
|---|---|
| mark, index | `mark_price`, `index_price` |
| funding rate | perpetuals: `current_funding`, per 8 h; `next_funding` 0, because funding accrues continuously |
| open interest | `open_interest` in contracts; USD for inverse contracts, divided by the contract size |

## Orders

The label is the FastMM client id. `post_only` is always sent, because Deribit defaults it to `true`. Post-only orders set `reject_post_only` (configuration `reject_post_only`), so a crossing order is rejected with 11054 instead of repriced.

`private/edit` keeps the order id and label; its `contracts` is the new total including fills. The engine receives an ack for its new client id. While the edit is in flight, a `user.orders` "open" update for the old label is not acked; later updates and fills for the old label map to the new id.

Fills take `cum_qty` and `leaves_qty` from the connector's order shadow, because `user.trades` has no cumulative quantity.

## Session

Both connections enable heartbeats and answer every `test_request` with `public/test`. An interval below 10 s is refused with -32602. The access token goes into `params.access_token` of every private request. It is refreshed with `grant_type=refresh_token` at 80 % of `expires_in`, and an order answered with 13009 re-authenticates.

## Reconciliation and fills

After the first private connect and after every reconnect, the connector replays the executions, then reconciles across all configured currencies. Open orders and positions become one Begin / OpenOrder* / Position* / End; an instrument `get_positions` does not list is flat. Nothing on the private stream reports a position, so a delivery, a liquidation or another client's trade reaches the engine only this way.

The replay reads `private/get_user_trades_by_currency_and_time` per currency with `kind: any` ([Execution replay](connector-behaviour.md#execution-replay)):

| rule | value |
|---|---|
| page | `count` ≤ 1000; a page with `has_more` is followed from its last row's timestamp |
| exact | when every currency ends with `has_more: false` |
| watermark older than 23 h | read with `historical: true` up to 23 h ago, then `historical: false`, which covers 24 h |
| end of the query | 60 s past the replay's start, because the venue's clock is not ours |
| identity | `trade_id`, unique per currency; `order_id`; `label` is the client id |

The schema describes a user trade's `direction` as the taker's, while the row's `liquidity` and `fee` are the account's own. The connector takes the side from the order it still holds, and otherwise reads `direction` as the account's side.

When the private connection drops, the connector cancels every subscribed instrument over REST (`cancel_on_order_channel_loss`), on top of the venue's cancel-on-disconnect.

## Balances

`private/get_account_summaries` gives one row per currency, and `user.portfolio.{currency}` streams changes ([Balances](connector-behaviour.md#balances)).

| FastMM | Deribit field |
|---|---|
| free | `available_funds` |
| locked | `initial_margin` |
| total | `balance` |
| equity | `equity` |
| maintenance | `maintenance_margin` |

With `cross_collateral_enabled`, an account row in USD comes from the `total_*_usd` fields.

## Funding

Perpetual funding is not booked. Deribit has no per-payment event: funding accrues continuously into the session PnL and is realised at the 08:00 UTC settlement. What reports it is aggregate or per interval:

| source | what it carries |
|---|---|
| `user.changes` position, `realized_funding` | the session so far |
| `private/get_transaction_log`, `interest_pl` | per settlement, trade or position change; 1 request/s |
| `private/get_settlement_history_by_instrument`, `funding` | per settlement |

## Dead man's switch

`private/enable_cancel_on_disconnect` with scope `connection` cancels that socket's orders. It fires when the TCP connection is closed properly, after 10 minutes of inactivity, or when a heartbeat detects a disconnection. It does not fire after `private/logout`. A host that dies without sending a FIN triggers neither of the first two quickly, so the connector always enables `public/set_heartbeat`. See [Dead man's switch](connector-behaviour.md#dead-mans-switch).

## Rate limits

Order requests use a leaky bucket sized by `matching_engine_rate` and `matching_engine_burst`: Tier 4 defaults to 5/s with a burst of 20. `private/get_account_summary` `limits` gives the account's tier. New orders and edits are refused locally when the bucket is empty; cancels are always sent. A 10028 `too_many_requests`, after which the venue drops the session, drains the bucket.

`private/mass_quote` has its own buckets and is not used ([Batch order entry](connector-behaviour.md#batch-order-entry)). Its bulk cancels, `private/cancel_all_by_instrument` and `private/cancel_all_by_currency`, serve the kill switch and the channel-loss path.

## Errors

The error map follows Deribit's "Complete RPC Error Codes Reference"; among others:

| code | action |
|---|---|
| 11054 | post-only reject |
| 10009, 10039 | insufficient funds |
| 10004, 11044 | unknown order; 10004 reconciles |
| 10043, 10026 | tick |
| 10005 to 10007 | price band |
| 10028 | rate limit |
| 13004, 13021 | fatal |
| 13009 | re-authenticate |
| 10040, 10041, 10047, 11051, 13028 | back off |

## Configuration

`stale_ms` defaults to 10000 ms; `configs/deribit-testnet.toml` sets 15000 ms. `dead_ms` is at least three heartbeat intervals, 30000 ms by default. Keys: [Configuration](../configuration.md#connectors).

## Unconfirmed

- The private payloads (`user.orders`, `user.trades`, order and auth responses, open orders) follow the OpenAPI and AsyncAPI schemas and are not recorded. The live test covers them when testnet keys are exported.
- Whether a `reject_post_only` rejection arrives as error 11054 or 11006. The error map also matches on the message text.
- Whether the public `trades` `direction` is the taker side, which the documentation does not state.
- Which side a user trade's `direction` gives; neither reading has been checked on testnet.
- The account's matching-engine tier is not queried.

Sources: the [Deribit documentation](https://docs.deribit.com) (articles, OpenAPI and AsyncAPI specs) and its support article "Inverse Perpetual".
