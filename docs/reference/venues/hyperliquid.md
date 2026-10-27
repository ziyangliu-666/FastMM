# Hyperliquid

`kind = "hyperliquid"` (alias `hl`) trades perpetuals on the main dex and on builder-deployed (HIP-3) dexes, with orders over the WebSocket; the example configuration is `configs/hyperliquid.toml`.

| | |
|---|---|
| Markets | perpetuals on the main dex (`BTC`) and HIP-3 dexes (`xyz:NVDA`), one-way positions |
| Order entry | WebSocket `post` actions, REST fallback; orders batched per action |
| Environments | mainnet (`testnet = false`), testnet (`testnet = true`) |
| Keys | `api_key` the account address, `api_secret` the signing key |
| Replace | `batchModify`, carried out by the venue as a new order |
| Dead man's switch | `scheduleCancel`, off by default (`dead_mans_switch_s`) |
| Example config | `configs/hyperliquid.toml` |

Keys for the section are in [Configuration: hyperliquid](../configuration.md#hyperliquid).

## Connections

| Channel | Endpoint | Purpose |
|---|---|---|
| md | `ws_url` (`wss://api.hyperliquid.xyz/ws`) | `l2Book` (20 levels a side, each push a whole book), `bbo`, `trades` per coin |
| private | the same URL, a second connection | `orderUpdates`, `userFills`, `userFundings` of the account address; public, no auth |
| trade | the same URL, a third connection | `{"method":"post"}` actions `order`, `cancel`, `cancelByCloid`, `batchModify`; REST fallback |
| rest | `rest_url` (`https://api.hyperliquid.xyz`) | `POST /info` reads, `POST /exchange` for the fallback, `scheduleCancel` and cancel-all |

The `POST /info` requests are `perpDexs`, `metaAndAssetCtxs`, `extraAgents`, `userRateLimit`, `frontendOpenOrders`, `clearinghouseState`, `spotClearinghouseState` and `userFillsByTime`.

## Keys and signing

| Key | Value |
|---|---|
| `api_key` | the account's address |
| `api_secret` | the signing key: an agent wallet the account approved, or the account's own key |
| `vault_address` | trade for a vault or sub-account of the account |

At start-up `load_reference_data()` reads `extraAgents` and refuses a key the account has not approved; `fastmm-live` exits 3.

An action is signed as Hyperliquid's SDK does: msgpack of the action, the nonce and the vault flag, hashed with keccak256 into an EIP-712 `Agent` message. The signature is secp256k1 ECDSA with deterministic nonces. Nonces are wall-clock milliseconds, strictly increasing per signer.

## Instruments

| Field | Value |
|---|---|
| Symbol | the coin with its dex prefix (`xyz:NVDA`), or without it when `dex` names the dex |
| Asset id | the index in the main dex's meta; on a HIP-3 dex 100000 + dex index × 10000 + index in its meta |
| Lot | 10^-szDecimals |
| Tick | the step at the current mark: 5 significant figures, at most 6 - szDecimals decimals; an integer price is always valid |
| Minimum notional | 10 USDC |
| Quote | USDC |

The encoder puts every price on the tick grid, a bid rounded down and an ask up, so a price crossing a power of ten stays valid.

## Market data

Public trades carry `side` as the taker's (`B` buys) and `tid` as the trade id. `users` is [buyer, seller]: both wallet addresses go into the trade as its parties (`buyer` and `seller` in Python). A trade whose `users` is not two addresses names nobody.

## Orders

| Engine order | Hyperliquid |
|---|---|
| post-only | `Alo` |
| IOC, market | `Ioc`; a market order needs a price |
| any other | `Gtc` |
| FOK | refused |

The cloid is `0x666d000000000000` followed by the client order id. One engine drain goes out as one action per kind, at most 40 orders each ([Batch order entry](connector-behaviour.md#batch-order-entry)). The reply answers the orders by position.

A cancel names the order by its oid once a reply or the stream gave it, else by cloid. With `supports_replace` a replace is a `batchModify`: its ack has no `kAmendedInPlace`, and the old order's `canceled` is not forwarded.

## Order events

The action reply and `orderUpdates` both report an order; the connector forwards whichever comes first, once.

| Venue status | Engine event |
|---|---|
| `resting`, `filled` in the reply | ack |
| error text in the reply | reject |
| `success` in a cancel reply | cancel accepted |
| `open` | ack |
| `canceled` | cancel ack, `origSz - sz` filled |
| other `*Canceled`, `scheduledCancel` | `OrderExpired` |
| `*Rejected` | reject; `badAloPxRejected` is post-only would cross |
| `filled` | nothing; fills come on `userFills` |

A `userFills` row is a fill: exec id `tid`, `crossed` the taker, `fee` in USDC with a rebate negative. Cumulative and remaining quantity come from the order shadow. The snapshots `userFills` and `userFundings` send on subscribing are skipped; the fill replay books what the stream missed.

## Reconciliation and fills

A reconciliation runs the fill replay, then reads `frontendOpenOrders` and `clearinghouseState` of each dex the instruments use ([Reconciliation](connector-behaviour.md#reconciliation)). The replay is `userFillsByTime` from the watermark: oldest first, 2000 rows a reply, 7 days back ([Execution replay](connector-behaviour.md#execution-replay)).

Balances come from `spotClearinghouseState`, one row per token: free is total - hold, locked is hold. A unified account keeps every dex's collateral in its spot USDC, so the dexes' `clearinghouseState` show `accountValue` 0.

## Funding

Each `userFundings` payment becomes a `Funding` event under `<time>:<coin>`.

## Rate limits

| Limit | Scope | Connector |
|---|---|---|
| 1 request per USDC traded since the address's inception, plus 10000 | each new order and modify, per address; growth-mode volume counts 10 %; cancels have a larger limit | reads `userRateLimit` at start and every `rate_limit_refresh_s`, counts what it sends, refuses at `address_reserve` |
| 2000 WebSocket messages a minute | per IP | `posts_per_minute` (default 1800) |
| 100 posts in flight | per IP | |
| | new orders and modifies | `orders_per_second` (default 20) |

The address budget's cap includes `nRequestsSurplus`, the requests reserved with `reserveRequestWeight`. A refusal is `VenueRateLimit` "local rate limit: address budget", published as the day window of the order budget. A rate-limit error cools down 10 s: an address over its budget may send one request every 10 s.

## Dead man's switch

`scheduleCancel` at now + `dead_mans_switch_s`, over REST, refreshed at a third of the window. Each refresh spends one request of the address budget, and the venue allows 10 triggers a day. `disconnect()` removes it.

The venue refuses the switch until the address has traded enough; the connector logs the reason once and runs without it. The kill switch reads `frontendOpenOrders` of each dex and cancels by oid, in signed batches of 40.

## Connection health

`stale_ms` defaults to 5000 ms. `dead_ms` is at least twice `ping_interval_ms` plus 5000 ms (default 60000 ms); the venue closes a connection that sent nothing for 60 s.

Every `latency_log_s` the log gives the post reply's round trip, each order's open lag and the address budget. The open lag is the block time of the order's `open` minus its send time, so it mixes the venue's clock with the host's.

## Not handled

Spot, TWAP and trigger orders, `expiresAfter`, the priority fee, leverage and isolated margin changes, and user-signed actions such as transfers and withdrawals.

## Sources

Read 2026-10-12: [Hyperliquid API docs](https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api) and hyperliquid-python-sdk 0.24.0 for the signing.
