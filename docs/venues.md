# Venue connectors

FastMM ships two crypto spot connectors behind the control-path `fastmm::venues::Venue`
interface (`include/fastmm/venues/venue.hpp`): **Binance Spot** (testnet or the local
Binance-compatible simulator) and **Bybit v5 spot** (testnet). `make_venue()`
(`venue_factory.hpp`) picks one from `[venues.<name>] kind`:

| kind | connector |
|---|---|
| `binance_spot`, `binance`, `sim` | `binance::BinanceVenue` |
| `bybit`, `bybit_spot` | `bybit::BybitVenue` |

Every connector runs on its own `net::Reactor` thread and writes normalised messages into two
rings per venue: market data (lossy: a full ring drops the delta and forces a resync) and order
events (never dropped: bounded spin, overflow trips the kill switch in `fastmm-live`).
`cancel_all()` uses an independent blocking REST connection, so it works from any thread even if
the reactor is wedged.

## Binance Spot

| channel | endpoint | purpose |
|---|---|---|
| md | `<ws_url>?streams=<sym>@depth@100ms/<sym>@bookTicker/<sym>@trade` | combined stream, dispatch by stream suffix |
| user | `<ws_api_url>` + `userDataStream.subscribe.signature` (HMAC) or `session.logon` + `userDataStream.subscribe` (Ed25519) | `executionReport`, `outboundAccountPosition` |
| order | `<ws_api_url>`: `order.place` / `order.cancel` / `order.cancelReplace` / `openOrders.status` / `openOrders.cancelAll` | order entry; REST fallback |
| rest | `<rest_url>` | `exchangeInfo`, `depth`, `time`, `openOrders`, REST order entry, kill-switch cancel-all |

The listenKey user stream (`POST/PUT /api/v3/userDataStream` + `/ws/<listenKey>`) is kept as
`user_stream = "listen_key"` for the simulator only: Binance removed it on 2026-02-20
(spot API CHANGELOG, 2025-10-24 announcement).

Depth sync follows "How to manage a local order book correctly": buffer deltas, fetch
`GET /api/v3/depth?symbol=S&limit=1000`, drop `u <= lastUpdateId`, first applied delta must have
`U <= lastUpdateId+1 <= u`, then `U == prev_u + 1`. A gap emits `ConnectionState{Resyncing}` and
re-snapshots (at most once per `min_snapshot_interval`, 2 s by default).

### What a Binance-compatible simulator must implement

This is exactly what `BinanceVenue` uses; anything else is optional.

**REST** (query-string parameters; signed requests carry `timestamp`, `recvWindow`,
`signature` = lowercase hex HMAC-SHA256(secret, query without `&signature=...`) and the
`X-MBX-APIKEY` header):

| method + path | request | response the connector reads |
|---|---|---|
| `GET /api/v3/exchangeInfo?symbols=["S"]` (percent-encoded) | public | `serverTime`, `rateLimits[]{rateLimitType,interval,intervalNum,limit}`, `symbols[]{symbol,status,baseAsset,quoteAsset,orderTypes[],filters[]}`; filters `PRICE_FILTER.tickSize`, `LOT_SIZE.{stepSize,minQty,maxQty}`, `NOTIONAL.{minNotional,maxNotional}` or `MIN_NOTIONAL.minNotional` |
| `GET /api/v3/time` | public | `{"serverTime": ms}` |
| `GET /api/v3/depth?symbol=S&limit=1000` | public | `{"lastUpdateId":L,"bids":[["px","qty"]...],"asks":[...]}` |
| `GET /api/v3/openOrders[?symbol=S]` | signed | array of `{symbol,orderId,clientOrderId,price,origQty,executedQty,status,timeInForce,type,side}` |
| `DELETE /api/v3/openOrders?symbol=S` | signed | 200 with an array; `400 {"code":-2011,...}` is treated as "nothing open" |
| `POST /api/v3/order`, `DELETE /api/v3/order`, `POST /api/v3/order/cancelReplace` | signed; same parameters as the WS API methods below | same bodies as the WS API `result` / `error` objects |
| `POST/PUT /api/v3/userDataStream` | simulator listenKey mode only | `{"listenKey":"..."}` |

Headers read: `X-MBX-USED-WEIGHT-1M`, `X-MBX-ORDER-COUNT-10S`, `Retry-After` (429/418).
Error envelope: `{"code":-NNNN,"msg":"..."}`.

**Market-data WebSocket**: `/stream?streams=...` sending `{"stream":"<name>","data":{...}}`:

* `<sym>@depth@100ms`: `{"e":"depthUpdate","E":ms,"s":"SYM","U":first,"u":last,"b":[["px","qty"]],"a":[...]}` (qty `"0"` deletes)
* `<sym>@bookTicker`: `{"u":id,"s":"SYM","b":"px","B":"qty","a":"px","A":"qty"}`
* `<sym>@trade`: `{"e":"trade","E":ms,"s":"SYM","t":id,"p":"px","q":"qty","T":ms,"m":bool}` (`m` = buyer is maker, so the seller aggressed)

A raw `/ws/<stream>` payload without the wrapper is also accepted. Server pings are answered
automatically; the connector treats any frame or ping as liveness.

**WebSocket API**: `<ws_api_url>` (e.g. `/ws-api/v3`). Requests are
`{"id":"<string>","method":"...","params":{...}}` with params in alphabetical order, `apiKey`,
`timestamp`, `recvWindow` and `signature` (HMAC over the sorted `k=v&...` string). Responses:
`{"id":..,"status":HTTP,"result":..|"error":{"code","msg","data"?},"rateLimits":[{rateLimitType,interval,intervalNum,limit,count}]}`.

| method | params used | result read |
|---|---|---|
| `userDataStream.subscribe.signature` | apiKey, recvWindow, timestamp, signature | `{"subscriptionId":N}` (request id `"uds"`) |
| `session.logon` / `userDataStream.subscribe` | Ed25519 keys only | status 200 |
| `order.place` | newClientOrderId, newOrderRespType=ACK, price (not MARKET), quantity, side BUY/SELL, symbol, timeInForce (LIMIT only), type LIMIT/LIMIT_MAKER/MARKET | `{symbol,orderId,clientOrderId,transactTime}` |
| `order.cancel` | orderId or origClientOrderId, symbol | `{symbol,origClientOrderId,orderId,executedQty,status}` |
| `order.cancelReplace` | cancelOrderId or cancelOrigClientOrderId, cancelReplaceMode=STOP_ON_FAILURE, newClientOrderId, newOrderRespType=ACK, price, quantity, side, symbol, timeInForce, type | `{cancelResult,newOrderResult,cancelResponse{orderId,origClientOrderId,executedQty},newOrderResponse{orderId,clientOrderId}}`; on failure `error.data` carries the same keys plus `newOrderResponse.code/msg` |
| `openOrders.status` | recvWindow, timestamp (no symbol = all) | array as in REST openOrders (request id `"oo"`) |
| `openOrders.cancelAll` | symbol | array (request id `"ca"`) |

Request ids for orders are `n|c|r` + the 14-character client id (`fm` + 12 hex), so the
simulator only has to echo `id`.

**User data events** arrive on the subscribed WS API connection as
`{"subscriptionId":0,"event":{...}}` (legacy `/ws/<listenKey>` raw events are also accepted):

* `executionReport`: `E,s,c,S,q,p,C,x,X,r,i,l,z,L,n,N,T,t,m`, with `x` in `NEW`, `CANCELED` (the cancelled id is in `C`), `REPLACED`, `REJECTED`, `TRADE`, `EXPIRED`, `TRADE_PREVENTION`
* `outboundAccountPosition`: `E,B[]{a,f,l}` (forwarded only with `position_from_balance = true`)

Error codes the connector acts on: -1003/-1015 (cool down), -1021 (resync clock), -1022/-2014/-2015
(fatal), -2010/-2011 message texts (`Order would immediately match and take.`,
`Unknown order sent.`, `Account has insufficient balance for requested action.`,
`Duplicate order sent.`), -2013, -2021/-2022, HTTP 418/429.

## Bybit v5 spot

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`/v5/public/spot`) | `orderbook.<depth>.SYM` snapshot/delta, `orderbook.1.SYM` to BookTicker, `publicTrade.SYM` |
| private | `ws_private_url` (default: ws_url host + `/v5/private`) | `op: auth`, then `order`, `execution`, `wallet` |
| trade | `ws_api_url` (`/v5/trade`) | `op: auth`, then `order.create` / `order.amend` / `order.cancel` with `reqId` and `X-BAPI-TIMESTAMP` / `X-BAPI-RECV-WINDOW` headers |
| rest | `rest_url` | `/v5/market/instruments-info`, `/v5/market/time`, `/v5/order/realtime`, `/v5/order/create|amend|cancel|cancel-all` |

Signing: REST `X-BAPI-SIGN` = hex HMAC-SHA256(secret, timestamp + api_key + recv_window +
payload), where the payload is the exact query string (GET) or JSON body (POST) sent. WS auth:
`{"op":"auth","args":[key, expires_ms, hex HMAC("GET/realtime" + expires)]}`. Every channel sends
`{"op":"ping"}` every 20 s.

Book sync: the stream's `snapshot` resets the book; each `delta` must have a larger `u`; `u == 1`
or a missing snapshot (10 s) re-subscribes the depth topic to get a fresh snapshot. Amend keeps
the venue's `orderLinkId`: the engine receives an ack for its new client id and later
`order`/`execution` events for the old link id are translated to it.

## Configuration keys

Common: `kind`, `ws_url`, `ws_api_url`, `rest_url`, `api_key`, `api_secret` (always
`${ENV_VAR}`), `supports_replace`, `insecure_tls`, `ca_file`, `recv_window_ms`.

Venue-specific keys are read from the section's extra keys. The core schema warns
"unknown key ... ignored", but they are applied:

* Binance: `user_stream` (`ws_api` | `listen_key` | `none`), `key_type` (`ed25519`),
  `private_key_file`, `order_api` (`ws` | `rest`), `depth_limit`, `stale_ms`, `dead_ms`,
  `position_from_balance`, `allow_offline_reference_data`, `cancel_on_order_channel_loss`,
  `emit_ack_from_response`.
* Bybit: `ws_private_url`, `depth`, `order_api`, `stale_ms`, `dead_ms`, `ping_interval_ms`,
  `orders_per_second`, `position_from_wallet`, `allow_offline_reference_data`,
  `cancel_on_order_channel_loss`, `emit_ack_from_response`.

The testnet configs set `stale_ms = 10000`: testnet BTCUSDT is often silent for more than 2 s, and
with the default every quiet spell is reported as Stale, which clears the engine book and
forces a resync.

## Open questions (`VERIFY:` in the code)

* Bybit: whether `u` increments by exactly one per delta (recorded testnet deltas did; the sync
  only requires increase).
* Bybit: whether amend `qty` includes the filled quantity (the testnet config therefore uses
  cancel + new, `supports_replace = false`).
* Bybit: whether `walletBalance` includes `locked`; balance-related `rejectReason` strings.
* Binance: `GET /api/v3/time` weight (1 vs 2) and listenKey validity/keepalive figures (the
  documentation was removed; simulator mode only).
