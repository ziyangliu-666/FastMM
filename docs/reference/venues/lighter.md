# Lighter

`kind = "lighter"` (alias `zklighter`) trades Lighter's perpetual markets, crypto and equity perps such as `NVDA`, with signed transactions over the WebSocket; the example configuration is `configs/lighter.toml`.

| | |
|---|---|
| Markets | perpetuals, one-way cross-margin positions |
| Order entry | WebSocket `jsonapi/sendtx`, one frame per transaction; REST fallback |
| Environments | mainnet, testnet (`testnet = true`: `testnet.zklighter.elliot.ai`, chain id 300; mainnet 304) |
| Keys | `api_key` the account index, `api_secret` an API key's private key, `api_key_index` its slot |
| Replace | `ModifyOrder`, in place |
| Dead man's switch | scheduled `CancelAll`, off by default (`dead_mans_switch_s`) |
| Example config | `configs/lighter.toml` |

Keys for the section are in [Configuration: lighter](../configuration.md#lighter).

## Connections

| Channel | Endpoint | Purpose |
|---|---|---|
| md | `ws_url` (`wss://mainnet.zklighter.elliot.ai/stream`) | `order_book`, `ticker`, `trade` per market |
| private | the same URL, a second connection | `account_all_orders` (with the auth token), `account_all_trades` |
| trade | the same URL, a third connection | `jsonapi/sendtx`, answered under its id; REST fallback |
| rest | `rest_url` (`https://mainnet.zklighter.elliot.ai`) | `orderBookDetails`, `apikeys`, `nextNonce`, `account`, `accountActiveOrders` (auth), `sendTx` |

## Keys and signing

| Key | Value |
|---|---|
| `api_key` | the account index; a sub-account's works the same |
| `api_secret` | the private key of an API key the account registered, 80 hex digits |
| `api_key_index` | the key's slot, 4 to 254; slots 0 to 3 belong to the web and mobile apps |

At start-up `load_reference_data()` reads `apikeys` and refuses a key whose public key is not in that slot; `fastmm-live` exits 3.

A transaction is signed as Lighter's Go signer does: Poseidon2 over Goldilocks field elements, then a Schnorr signature over the ECgFp5 curve. The signer is native, with no Go runtime in the process. The transaction goes out as JSON with the signature in base64.

The auth token is `<deadline>:<account>:<slot>:<signature>`, valid for at most 8 hours. The account channel subscribes with it and reconnects with a new one at 90 % of `auth_ttl_s`. Authenticated REST calls send it as the `Authorization` header.

## Instruments

The symbol is the market's, from `orderBookDetails` (perps only).

| Field | Value |
|---|---|
| Tick | 10^-price_decimals |
| Lot | 10^-size_decimals |
| Minimum quantity | `min_base_amount` |
| Minimum notional | `min_quote_amount` |
| Quote | USDC |

A price goes out as the integer price × 10^price_decimals (uint32), a bid rounded down and an ask up. Sizes are rounded down.

## Market data

In public trades (`trades` and `liquidation_trades`) the aggressor buys when `is_maker_ask`. `bid_account_id` and `ask_account_id` go into the trade as buyer and seller (`buyer` and `seller` in Python). The frame's other per-trade fields are not carried: order ids, fees and positions before the trade.

## Orders

| Engine order | Lighter |
|---|---|
| post-only | TimeInForce 2 |
| IOC, market | TimeInForce 0, no expiry; a market order needs a price |
| any other | good-till-time, expiring after `order_expiry_s` |
| FOK | refused |

Each new order is one CreateOrder. The client order index is the engine's client order id itself (48 bits, the venue's range), so the account should trade through FastMM only. A cancel names the client order index.

With `supports_replace` a replace is a ModifyOrder of the same order, applied in place. The new id's ack carries `kAmendedInPlace`, and the order's later events, which still name the first index, go out under the new id. The connector sends the new total as the base amount.

## Nonces

Each transaction takes the slot's next nonce, read with `nextNonce` at start-up and after a reconnect. By default it also carries the SkipNonce attribute (`skip_nonce`): the venue then accepts any larger nonce, so a refused transaction does not fail the ones behind it. A refusal that names the nonce makes the connector read it again. Orders, cancels and the scheduled cancel share the trade connection, so their nonces arrive in order.

## Order events

A sendtx reply with code 200 means accepted, not executed. The ack is the order's `open` on `account_all_orders`, or the reply with `emit_ack_from_response`.

| Venue event | Engine event |
|---|---|
| `open` | ack |
| ended before resting: `canceled-post-only`, `canceled-margin-not-allowed`, an IOC with nothing to take | reject |
| `canceled` | cancel ack |
| any other `canceled-*` after the order rested | `OrderExpired` |
| refused transaction | reject with the venue's code and message |

Fills come from `account_all_trades`: the account's side of each trade, both legs of a self-trade. The fee is the trade's maker or taker rate in millionths of the notional, and the trade id is the exec id. Cumulative and remaining quantity come from the order shadow.

A cancel or expiry waits for the fills it counts, at most `terminal_wait_ms`. The history sent on subscribing is skipped.

## Reconciliation and fills

A reconciliation reads `accountActiveOrders` (authenticated) and the positions of `account` ([Reconciliation](connector-behaviour.md#reconciliation)). There is no execution replay: fills the stream missed reach the engine through the position check. Balances are one USDC row from `account`: free is the available balance, total the collateral, equity the total asset value.

## Rate limits

| Limit | Venue | Connector |
|---|---|---|
| sendTx a minute | 4000 on a Premium account with no LIT staked, up to 57600 with LIT; 60 on Standard | `tx_per_minute` (default 3600) |
| volume quota, Premium | each CreateOrder, ModifyOrder and CancelAll spends one unit; 1000 to start, one per 2 USDC traded, one free transaction every 15 s | refuses new orders and modifies at `quota_reserve` |
| new orders and modifies | | `orders_per_second` (default 20) |

The replies carry the quota left. A quota refusal is `VenueRateLimit` "local rate limit: volume quota", published as the day window of the order budget.

Lighter's `jsonapi/sendtxbatch` takes 15 transactions, but each still counts against the sendTx limit and the quota. The connector sends one `jsonapi/sendtx` frame per transaction and a drain's frames in one write ([Batch order entry](connector-behaviour.md#batch-order-entry)).

## Dead man's switch

A scheduled CancelAll at now + `dead_mans_switch_s`, refreshed at a third of the window on the trade connection. The window is 310 s to 15 days, because the venue wants the time at least 5 minutes ahead. `disconnect()` aborts it. The kill switch is one CancelAll over REST.

## Connection health

`stale_ms` defaults to 5000 ms. `dead_ms` is at least twice `ping_interval_ms` plus 5000 ms (default 60000 ms); the venue closes a connection that sent nothing for 2 minutes.

Every `latency_log_s` the log gives the sendtx reply's round trip, each order's open lag and the volume quota left. The open lag is the venue time of the order's `open` minus its send time.

## Not handled

Spot markets, trigger and TWAP orders, grouped orders, `sendTxBatch`, leverage and margin mode, isolated margin, funding events, transfers and withdrawals.

## Unconfirmed

Taken from the documentation only, not yet seen from the venue:

* an accepted sendtx reply, read at the top level or under `data`;
* `account_all_orders` and the order statuses;
* what ModifyOrder's base amount means after a partial fill;
* the quota numbers.

## Sources

Read 2026-10-13: [Lighter API docs](https://apidocs.lighter.xyz), lighter-go and poseidon_crypto v0.0.15 for the signing, lighter-python 1.1.6 for the REST shapes.
