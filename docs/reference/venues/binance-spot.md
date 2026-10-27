# Binance Spot

`kind = "binance_spot"` (aliases `binance`, `sim`) trades Binance spot pairs over the WebSocket API, with REST as the fallback.

| | |
|---|---|
| Markets | spot |
| Order entry | WebSocket API; REST fallback |
| Environments | production, Demo Mode, Spot testnet, [fastmm-sim-exchange](../sim-exchange.md) |
| Keys | HMAC (default) or Ed25519 |
| Replace | amend in place for a size-down, cancel-replace otherwise |
| Dead man's switch | none at the venue |
| Account pools, internal transfers | yes ([Account pools](connector-behaviour.md#account-pools)) |
| Example config | `configs/binance-demo.toml`, `configs/binance-testnet.toml`, `configs/sim.toml` |

## Connections

| Channel | Endpoint | Purpose |
|---|---|---|
| md | `<ws_url>?streams=<sym>@depth@100ms/<sym>@bookTicker/<sym>@trade` | JSON combined stream |
| md, `md_format = "sbe"` | `<sbe_ws_url>?streams=<sym>@depth/<sym>@bestBidAsk/<sym>@trade` | binary SBE frames |
| user | `<ws_api_url>`, subscribed with `userDataStream.subscribe.signature` (HMAC) or `session.logon` then `userDataStream.subscribe` (Ed25519) | `executionReport`, `outboundAccountPosition`, `balanceUpdate` |
| order | `<ws_api_url>` | `order.place`, `order.cancel`, `order.amend.keepPriority`, `order.cancelReplace`, `openOrders.status`, `openOrders.cancelAll` |
| rest | `<rest_url>` | reference data, depth snapshots, clock, open orders, fee rates, REST order entry, kill-switch cancel-all |

The listenKey user stream remains as `user_stream = "listen_key"` for the simulator only; Binance removed it on 2026-02-20.

## Keys and session logon

| `key_type` | How requests are signed |
|---|---|
| `hmac` (default) | every request carries `apiKey` and a hex HMAC-SHA256 signature; a signature costs about 244 ns ([Benchmarks](../../explanation/benchmarks.md)) |
| `ed25519` | `session.logon` once per WS API connection on the order and user connections; later requests carry only `timestamp` and `recvWindow` |

With Ed25519 the key comes from `private_key_file` or `private_key_env`. The order channel is Live only after the logon reply. Encoding an `order.place` takes 715 ns signed with HMAC and 268 ns on an Ed25519 session.

A failed logon is fatal for a bad key, signature or permission. Timestamp, rate-limit and server errors retry the logon after 2 s. A revoked session (`status` 401 with a null id) goes through the error map, where -2015 is fatal.

REST requests sign each request with Ed25519, about 30 µs each, so use `order_api = "ws"` with Ed25519 keys. Binance takes `session.logon` with Ed25519 keys only, on production, the Spot testnet and Demo Mode. FastMM does not support RSA keys.

## Fee rates

`fetch_fees = true` makes `fastmm-live` read the account's rates for each configured symbol at start-up, with `GET /api/v3/account/commission?symbol=S` (signed, weight 20 a symbol). The bulk `/sapi/v1/asset/tradeFee` is not on the testnet or in Demo Mode.

| Item | Rule |
|---|---|
| rate of a fill | `standardCommission` + `specialCommission` + `taxCommission`, maker or taker, plus the larger of each group's `buyer` and `seller` rate |
| BNB discount | left out: a commission paid in BNB is not booked |
| rounding | 1 cbps (1e-6) |
| effect | replaces `[venues.<x>.fees]` and the instrument's `maker_bps` / `taker_bps`; `ctx.fees` returns them ([Strategy API](../strategy-api.md)) |
| journal | written into the embedded configuration, so a replay and a backtest use them |
| refused request | the start stops, exit code 4 |

Each symbol's rates are logged, as a warning when they differ from the configuration. Fills are still booked with the commission each `executionReport` carries. `fastmm-gateway` does not fetch rates; a strategy attached to it reads its own configuration.

## Market data

### Depth sync

The connector follows Binance's "How to manage a local order book correctly":

1. Buffer the deltas of a new stream.
2. Once the first delta is buffered, or 0.5 s after the stream opened, fetch `GET /api/v3/depth?symbol=S&limit=L`.
3. Drop deltas with `u <= lastUpdateId`. The first applied delta must have `U <= lastUpdateId+1 <= u`; each later one `U == prev_u + 1`.
4. On a gap, emit `ConnectionState{Resyncing}` and fetch a new snapshot, at most once per `min_snapshot_interval` (2 s by default).

| Setting | Value |
|---|---|
| snapshot depth `L` | `depth_limit`; else 1000 (weight 50) with up to 10 symbols, 100 (weight 5) above |
| when a snapshot is sent | when it fits the rate limiter's bulk share (half the weight); else retried from the housekeeping timer |
| a delta over 1024 levels a side | keeps the 1024 nearest the touch instead of breaking the chain |

The update that reveals a gap starts the buffer for the next snapshot. Every resync is logged with its reason and update ids, and the book's return with the time it was away.

### SBE market data

`md_format = "sbe"` reads Binance's [SBE market-data streams](https://developers.binance.com/docs/binance-spot-api-docs/sbe-market-data-streams). It needs an Ed25519 API key in the `X-MBX-APIKEY` upgrade header, with no signature, so it requires `key_type = "ed25519"` and runs in a dry run without the private key.

| Environment | Host |
|---|---|
| production | `wss://stream-sbe.binance.com[:9443]` |
| Demo Mode | `wss://demo-stream-sbe.binance.com` |
| testnet | `wss://stream-sbe.testnet.binance.vision` |

`sbe_ws_url` defaults to `ws_url` with `stream.` or `demo-stream.` replaced by `stream-sbe.` or `demo-stream-sbe.`.

| Stream | Template | Message |
|---|---|---|
| `<sym>@depth` | `DepthDiffStreamEvent` 10003 | `BookDelta`; `firstBookUpdateId` / `lastBookUpdateId` as U / u |
| `<sym>@bestBidAsk` | `BestBidAskStreamEvent` 10001 | `BookTicker`; `bookUpdateId` as sequence |
| `<sym>@trade` | `TradesStreamEvent` 10000 | one `Trade` per group entry |
| `<sym>@depth20` | `DepthSnapshotStreamEvent` 10002 | decoded, not subscribed |

Prices and quantities are int64 mantissas with a per-message exponent, converted exactly to 1e-8 fixed point. A value finer than 1e-8 makes the frame malformed. The depth snapshot still comes from REST as JSON, and depth sync is unchanged. The decoder is generated from schema 1, version 0 (`tools/sbe/binance_spot_stream_1_0.xml`).

| Stream, decode per message | SBE | JSON |
|---|---:|---:|
| best bid and ask | 18.0 ns | 121.7 ns |
| trade | 20.0 ns | 119.4 ns |
| depth, 20 levels | 49.6 ns | 702.4 ns |
| depth, 100 levels | 157.6 ns | 3.10 µs |

Method: [Benchmarks](../../explanation/benchmarks.md). WS API responses stay JSON: SBE order events would save about 300 ns each, on a schema Binance retires every few months.

## Orders

### Replace: amend or cancel-replace

| Replace | Request | Effect |
|---|---|---|
| size-down at the same price | `order.amend.keepPriority` | keeps the order, its `orderId`, its fills and its queue position |
| price change, size-up, no shadow of the original | `order.cancelReplace` | a new venue order at the back of the queue, fills from zero |

An amended order takes the engine's new client id (`newClientOrderId`). Its ack carries `OrderAckMsg::kAmendedInPlace`, from the response or from an `executionReport` with `x` = `REPLACED`. The OMS rekeys the order without resetting its cumulative quantity.

`exchangeInfo`'s `MAX_NUM_ORDER_AMENDS` caps amends per order (10 where published). The connector counts them and falls back to `cancelReplace` at `max_order_amends`. `amend_keep_priority = false` sends every replace as `cancelReplace`.

An amend costs IP weight 4 and nothing against the order-count limits.

### Batch order entry

Binance Spot has no batch endpoint. The order lists (OCO, OTO, OTOCO, OPO, OPOCO) are contingency structures, not a way to send two independent quotes.

### Warm standby

Binance Spot is the one connector that takes `defer_private()`: a [warm standby](../../how-to/operations/hand-over-a-session.md#warm-standby) opens market data only, and nothing of the account until `enable_private()`.

## Reconciliation and fills

The start-up sweep runs when the order channel first goes Live. Generic behaviour: [Reconciliation](connector-behaviour.md#reconciliation), [Execution replay](connector-behaviour.md#execution-replay).

| Item | Binance Spot |
|---|---|
| history query | `GET /api/v3/myTrades` per symbol, weight 20, limit 1000 |
| window | `fromId` (the trade id after the last one read) once known, else `startTime`; ranges walked 24 h at a time |
| pages a replay may read | 10 per symbol |
| restart | `resume_trade_ids` gives the next trade id per symbol ([Recovery at start-up](../storage.md#recovery-at-start-up)) |

### Which symbols a replay reads

Every replay asks only for the symbols showing signs of an execution since the symbol's watermark, less the settle margin:

- a fill reported or replayed;
- a cancel that found the order traded or gone (`executedQty` above zero, or `Unknown order sent`);
- an order whose fate the venue did not say: -1006, -1007, or a REST order that got no reply.

Orders sent are not a sign, since a market maker sends them on every symbol. A skipped symbol keeps its watermark and `fromId`. Every symbol is read at least once in 30 minutes, the first deadlines spread over the symbols. The first replay of a connection, and the one after the user stream or order connection returns, read every symbol.

### The order a trade names

`myTrades` names the order by `orderId` only. The connector maps it through the order ids its acks and snapshots carried, keeping the last 8192 pairings.

| Case | What happens |
|---|---|
| order id not seen | `GET /api/v3/order?orderId=` (weight 4) before the window's fills go out |
| lookup limit | 16 per replay, none while the rate limiter has no room |
| lookup failed or skipped | the fill goes out with `OrderFillMsg::kUnresolved`, naming no order; the next replay (5 s later, 5 times at most) sends it again, naming its order |
| order not FastMM's | not asked for again; the last copy goes without the flag |

An engine keeps one copy of a fill (same trade id). `fastmm-gateway` gives a flagged fill to no strategy until the copy that settles it.

### Balances

| Item | Source |
|---|---|
| snapshot | WS API `account.status`, else `GET /api/v3/account?omitZeroBalances=true` (weight 20) |
| stream | `outboundAccountPosition`; forwarded as a position only with `position_from_balance = true` |
| free / locked / total / equity / maintenance | `free` / `locked` / sum / sum / 0 |
| venue time | stream `u`; snapshot: the connector's venue clock |
| deposits, withdrawals, transfers | `balanceUpdate` (`d` at `T`), an `AccountFlow` event ([Journal format](../journal-format.md#messages)) |

## Internal transfers

A pool treasury moves assets between accounts with the master account's universal transfer ([Internal transfers](connector-behaviour.md#internal-transfers)).

| Item | Value |
|---|---|
| send | `POST /sapi/v1/sub-account/universalTransfer`, IP weight 1, `clientTranId` = the transfer's client id; SPOT to SPOT in one request |
| look up | `GET /sapi/v1/sub-account/universalTransfer?clientTranId=…&fromEmail=…` |
| status | `SUCCESS` done, `FAILURE` failed, otherwise pending, no row not found |
| errors | 4xx is a refusal; 5xx or a transport error is Unknown |
| key | a master-account HMAC key with "internal transfer" enabled, read from `transfer_api_key_env` / `transfer_api_secret_env`; used for nothing else |
| account name | `sub_account_email`; empty means the master account |

The trading keys need no transfer permission. The sub-account endpoints (`subToSub`, `subToMaster`) take no client id, so a lost answer could not be looked up.

## Dead man's switch

Binance Spot has none: the REST, WebSocket and FIX APIs have no countdown, session auto-cancel or cancel-on-disconnect. The FIX `News` countdown counts down to a maintenance logout and touches no orders. Orders a dead process left resting are cancelled by the next session's start-up sweep.

## Rate limits

REST runs on two connections, one request at a time each. Orders, REST-fallback cancels and cancel-all use one; snapshots, reconciliation, replays and the clock use the other. A signed request is stamped and signed when it is written, so queueing does not count against `recvWindow`.

| Rule | Value |
|---|---|
| headers read | `X-MBX-USED-WEIGHT-1M`, `X-MBX-ORDER-COUNT-10S`, `Retry-After` |
| 429 | pauses every send for `Retry-After` (10 s without the header) |
| another 429 within a minute of a pause's end | waits 2, 4, 8 … s more, up to 120 s |
| 418 | stops REST |
| windows | roll on the venue's clock, as Binance's do on the UTC minute and 10 s |
| bulk share | half the weight; the process's bulk requests may fill a fifth of it at the minute's start and all of it at the end |
| orders and cancels | not paced |
| an order the limiter refuses | `VenueRateLimit`, counted in the status line's `rate_refused(weight/10s/1m/1d/paused)` |
| execution replay | at most 4 requests in flight on its connection |

A count the venue reports raises the local estimate to it. It lowers the estimate to that count plus what was sent in the last 2 s, except in a window's first 2 s.

A pool's accounts that leave from one IP share one weight window, keyed by the REST URL and the [source address](../configuration.md#source-address). A 429 pause or a 418 stop answered to one account holds them all. The kill switch's `cancel_all()` retries a 418 or 429 three times before reporting failure.

## Errors

| Code | Action |
|---|---|
| -1003, -1015, HTTP 429 | cool down |
| HTTP 418 | stop REST |
| -1021 | resync the clock |
| -1022, -2014, -2015 | fatal |
| -2010, -2011 | by message: `Order would immediately match and take.`, `Unknown order sent.`, `Account has insufficient balance for requested action.`, `Duplicate order sent.` |
| -2013 | unknown order |
| -2021, -2022 | cancel-replace failed |
| -2038 | amend refused: the order is gone, the quantity did not go down, or the amends are used up; the original keeps working and the replacement id is rejected |

## Connection timeouts

| Key | Value |
|---|---|
| `stale_ms` | 2000 ms; the testnet and Demo configs set 10000 ms, because testnet BTCUSDT is often quiet for over 2 s |
| `dead_ms` | at least 45000 ms; the venue pings every 20 s |

## What a Binance-compatible simulator must implement

A simulator that `BinanceVenue` can trade against implements the following. [fastmm-sim-exchange](../sim-exchange.md) is one.

### REST

Parameters go in the query string. Signed requests carry `timestamp`, `recvWindow`, `signature` and the `X-MBX-APIKEY` header. The signature is lowercase hex HMAC-SHA256 of the query without `&signature=...`. Errors are `{"code":-NNNN,"msg":"..."}`.

| Method and path | Request | Response the connector reads |
|---|---|---|
| `GET /api/v3/exchangeInfo?symbols=["S"]` (percent-encoded) | public | `serverTime`; `rateLimits[]{rateLimitType,interval,intervalNum,limit}`; `symbols[]{symbol,status,baseAsset,quoteAsset,orderTypes[],filters[]}` with `PRICE_FILTER.tickSize`, `LOT_SIZE.{stepSize,minQty,maxQty}`, `NOTIONAL.{minNotional,maxNotional}` or `MIN_NOTIONAL.minNotional` |
| `GET /api/v3/time` | public | `{"serverTime": ms}` |
| `GET /api/v3/depth?symbol=S&limit=<depth_limit>` | public | `{"lastUpdateId":L,"bids":[["px","qty"]...],"asks":[...]}` |
| `GET /api/v3/account/commission?symbol=S` | signed, with `fetch_fees` | `standardCommission`, `specialCommission`, `taxCommission`, each `{maker,taker,buyer,seller}` |
| `GET /api/v3/openOrders[?symbol=S]` | signed | array of `{symbol,orderId,clientOrderId,price,origQty,executedQty,status,timeInForce,type,side}` |
| `DELETE /api/v3/openOrders?symbol=S` | signed | 200 with an array; `400 {"code":-2011}` means nothing open |
| `GET /api/v3/order?symbol=S&orderId=N` | signed | `{orderId,clientOrderId}`, also for an order that has ended and traded |
| `POST /api/v3/order`, `DELETE /api/v3/order`, `POST /api/v3/order/cancelReplace`, `PUT /api/v3/order/amend/keepPriority` | signed; parameters as the WS API methods below | the WS API `result` or `error` objects |
| `POST/PUT /api/v3/userDataStream` | listenKey mode only | `{"listenKey":"..."}` |

### Market-data WebSocket

`/stream?streams=...` sends `{"stream":"<name>","data":{...}}`. A raw `/ws/<stream>` payload without the wrapper is also accepted. The connector answers server pings and takes any frame or ping as liveness.

| Stream | Payload |
|---|---|
| `<sym>@depth@100ms` | `{"e":"depthUpdate","E":ms,"s":"SYM","U":first,"u":last,"b":[["px","qty"]],"a":[...]}`; qty `"0"` deletes |
| `<sym>@bookTicker` | `{"u":id,"s":"SYM","b":"px","B":"qty","a":"px","A":"qty"}` |
| `<sym>@trade` | `{"e":"trade","E":ms,"s":"SYM","t":id,"p":"px","q":"qty","T":ms,"m":bool}`; `m` true means the seller aggressed |

### WebSocket API

Requests are `{"id":"<string>","method":"...","params":{...}}`, with params in alphabetical order plus `apiKey`, `timestamp`, `recvWindow` and `signature`. The signature is HMAC over the sorted `k=v&...` string. Responses are `{"id","status","result"|"error":{"code","msg","data"?},"rateLimits":[...]}`.

| Method | Params used | Result read |
|---|---|---|
| `userDataStream.subscribe.signature` | apiKey, recvWindow, timestamp, signature | `{"subscriptionId":N}`; request id `"uds"` |
| `session.logon` | apiKey, recvWindow, timestamp, signature; Ed25519 keys only | status 200; request ids `"logon-o"`, `"logon-u"` |
| `userDataStream.subscribe` | none, after `session.logon` | `{"subscriptionId":N}` |
| `order.place` | newClientOrderId, newOrderRespType=ACK, price (not MARKET), quantity, side, symbol, timeInForce (LIMIT only), type LIMIT / LIMIT_MAKER / MARKET | `{symbol,orderId,clientOrderId,transactTime}` |
| `order.cancel` | orderId or origClientOrderId, symbol | `{symbol,origClientOrderId,orderId,executedQty,status}` |
| `order.cancelReplace` | cancelOrderId or cancelOrigClientOrderId, cancelReplaceMode=STOP_ON_FAILURE, newClientOrderId, newOrderRespType=ACK, price, quantity, side, symbol, timeInForce, type | `{cancelResult,newOrderResult,cancelResponse{orderId,origClientOrderId,executedQty},newOrderResponse{orderId,clientOrderId}}`; on failure `error.data` carries the same keys and `newOrderResponse.code/msg` |
| `order.amend.keepPriority` | newClientOrderId, newQty, orderId or origClientOrderId, symbol | `{transactTime,executionId,amendedOrder{symbol,orderId,origClientOrderId,clientOrderId,price,qty,executedQty,status}}` |
| `openOrders.status` | recvWindow, timestamp; no symbol means all | as REST `openOrders`; request id `"oo"` |
| `openOrders.cancelAll` | symbol | array; request id `"ca"` |

Order request ids are `n`, `c`, `r` or `a` followed by the 14-character client id (`fm` + 12 hex), so a simulator only echoes `id`. `a` is an amend, whose response differs from a cancel-replace's.

### User data events

Events arrive on the subscribed WS API connection as `{"subscriptionId":0,"event":{...}}`. Legacy raw events on `/ws/<listenKey>` are also accepted.

| Event | Fields read |
|---|---|
| `executionReport` | `E,s,c,S,q,p,C,x,X,r,i,l,z,L,n,N,T,t,m`; `x` is `NEW`, `CANCELED` (the cancelled id in `C`), `REPLACED`, `REJECTED`, `TRADE`, `EXPIRED` or `TRADE_PREVENTION` |
| `outboundAccountPosition` | `E,B[]{a,f,l}` |
| `balanceUpdate` | `a,d,T` |

## Unconfirmed

| Item | Status |
|---|---|
| Ed25519 and SBE | `session.logon`, unsigned orders and SBE streams tested against simulators and frames built from the schema only |
| SBE in Demo Mode | whether Demo Mode takes Ed25519 keys for SBE streams |
| HMAC key on `session.logon` | the error Binance returns; the simulator answers -4056 |
| combined SBE stream | whether it sends one event per binary frame |
| `newQty` at or below the filled quantity | undocumented; the connector never sends it and the simulator refuses it with -1013 |
| `C` on a `REPLACED` report | read as the pre-amend client id, implied but not stated |
| `GET /api/v3/time` | weight 1 or 2 |
| listenKey validity and keepalive | documentation removed; simulator mode only |

Sources: [Spot API docs](https://developers.binance.com/docs/binance-spot-api-docs), [user data stream](https://developers.binance.com/docs/binance-spot-api-docs/user-data-stream), [SBE market-data streams](https://developers.binance.com/docs/binance-spot-api-docs/sbe-market-data-streams).
