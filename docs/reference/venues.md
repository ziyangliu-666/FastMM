# Venue connectors

FastMM ships five connectors behind the control-path `fastmm::venues::Venue` interface (`include/fastmm/venues/venue.hpp`): Binance Spot (testnet, Demo Mode or the local Binance-compatible simulator), Binance USDⓈ-M perpetual futures (Demo Trading), Bybit v5 spot (testnet), Deribit options and futures (testnet) and Nasdaq TotalView-ITCH market data (with order entry to fastmm-sim-itch). `make_venue()` (`venue_factory.hpp`) picks one from `[venues.<name>] kind`:

| kind | connector |
|---|---|
| `binance_spot`, `binance`, `sim` | `binance::BinanceVenue` |
| `binance_usdm` | `binance_usdm::BinanceUsdmVenue` |
| `bybit`, `bybit_spot` | `bybit::BybitVenue` |
| `deribit` | `deribit::DeribitVenue` |
| `nasdaq_itch` | `nasdaq::NasdaqItchVenue` |

Every connector runs on its own `net::Reactor` thread and writes normalised messages into two rings per venue: market data (lossy: a full ring drops the delta and forces a resync) and order events (never dropped: bounded spin, overflow trips the kill switch in `fastmm-live`). `cancel_all()` uses an independent blocking REST connection, so it works from any thread even if the reactor is wedged (`nasdaq_itch`: it shuts the OUCH connection down, see below). `Venue::poll()` runs after every reactor iteration; `nasdaq_itch` polls its sockets there in `spin_mode = "busy"`.

## Binance Spot

| channel | endpoint | purpose |
|---|---|---|
| md | `<ws_url>?streams=<sym>@depth@100ms/<sym>@bookTicker/<sym>@trade` | combined stream, dispatch by stream suffix |
| md (`md_format = "sbe"`) | `<sbe_ws_url>?streams=<sym>@depth/<sym>@bestBidAsk/<sym>@trade` | binary SBE frames, dispatch by template id |
| user | `<ws_api_url>` + `userDataStream.subscribe.signature` (HMAC) or `session.logon` + `userDataStream.subscribe` (Ed25519) | `executionReport`, `outboundAccountPosition` |
| order | `<ws_api_url>`: `order.place` / `order.cancel` / `order.cancelReplace` / `openOrders.status` / `openOrders.cancelAll` | order entry; REST fallback |
| rest | `<rest_url>` | `exchangeInfo`, `depth`, `time`, `openOrders`, REST order entry, kill-switch cancel-all |

The listenKey user stream (`POST/PUT /api/v3/userDataStream` + `/ws/<listenKey>`) is kept as `user_stream = "listen_key"` for the simulator only: Binance removed it on 2026-02-20 (spot API CHANGELOG, 2025-10-24 announcement).

### Keys and session logon

* HMAC keys (`key_type = "hmac"`, the default) sign every request: apiKey + hex HMAC-SHA256. The key pads are hashed once (`net::HmacSha256Key`), so a signature costs about 170 ns.
* Ed25519 keys (`key_type = "ed25519"`, `private_key_file` or `private_key_env`) log on once per WS API connection with `session.logon` (the only signed request) on the order and user connections. Later requests carry neither `apiKey` nor `signature`, only `timestamp` and `recvWindow`. The order channel is Live only after the logon reply. A failed logon is fatal for bad key, signature or permission errors; timestamp, rate-limit and server errors retry the logon after 2 s. A revoked session (`{"id":null,"status":401,...}`, key deleted or IP not whitelisted) is logged and acted on through the error map (-2015 is fatal). REST requests (fallback, reconciliation, kill switch) are signed with Ed25519 per request, about 30 µs each: keep `order_api = "ws"` with Ed25519 keys.
* Binance supports `session.logon` with Ed25519 keys only, on production, the Spot testnet (`wss://ws-api.testnet.binance.vision/ws-api/v3`) and Demo Mode (`wss://demo-ws-api.binance.com/ws-api/v3`); RSA keys are not supported by FastMM.

Measured on the order encode (`bench_order_encoders`, release-native, one pinned core): HMAC 736 ns (1628 ns before the key pads were precomputed), Ed25519 after `session.logon` 284 ns, Ed25519 without a session 31 µs.

### SBE market data

`md_format = "sbe"` reads the SBE market-data streams (<https://developers.binance.com/docs/binance-spot-api-docs/sbe-market-data-streams>): `wss://stream-sbe.binance.com[:9443]`, Demo Mode `wss://demo-stream-sbe.binance.com`, testnet `wss://stream-sbe.testnet.binance.vision`. `sbe_ws_url` defaults to `ws_url` with `stream.` / `demo-stream.` replaced by `stream-sbe.` / `demo-stream-sbe.`. The connection needs an Ed25519 API key in the `X-MBX-APIKEY` upgrade header (no signature; the server answers `400 No X-MBX-APIKEY header` without it), so the setting requires `key_type = "ed25519"` and works in a dry run without the private key.

| stream | template | message |
|---|---|---|
| `<sym>@depth` | `DepthDiffStreamEvent` 10003 | `BookDelta` (`firstBookUpdateId` / `lastBookUpdateId` as U / u) |
| `<sym>@bestBidAsk` | `BestBidAskStreamEvent` 10001 | `BookTicker` (`bookUpdateId` as sequence) |
| `<sym>@trade` | `TradesStreamEvent` 10000 | one `Trade` per group entry |
| `<sym>@depth20` | `DepthSnapshotStreamEvent` 10002 | decoded, not subscribed |

The schema is `tools/sbe/binance_spot_stream_1_0.xml` (schema id 1, version 0, from the binance-spot-api-docs repository); `tools/sbe_gen.py` generates `include/fastmm/venues/binance/generated/binance_stream_sbe.hpp` (CI checks it is current). Timestamps are microseconds; prices and quantities are int64 mantissas with a per-message exponent, converted exactly to 1e-8 fixed point (a value finer than 1e-8 makes the frame malformed). The depth snapshot still comes from REST (JSON), and depth sync is unchanged. Decode per message (`bench_json`, release-native): depth 10+10 levels 51 ns (JSON 680 ns), 50+50 levels 147 ns (JSON 2.9 µs), best bid/ask 17 ns (bookTicker 118 ns), trade 19 ns (JSON 120 ns).

WS API responses stay JSON: `responseFormat=sbe` would move order acks and execution reports to SBE schema 3 (retired every few months, currently version 5), for a saving of about 300 ns per order event.

Depth sync follows "How to manage a local order book correctly": buffer deltas, fetch `GET /api/v3/depth?symbol=S&limit=1000`, drop `u <= lastUpdateId`, first applied delta must have `U <= lastUpdateId+1 <= u`, then `U == prev_u + 1`. A gap emits `ConnectionState{Resyncing}` and re-snapshots (at most once per `min_snapshot_interval`, 2 s by default).

### What a Binance-compatible simulator must implement

`BinanceVenue` uses only the following.

#### REST

Parameters go in the query string. Signed requests carry `timestamp`, `recvWindow`, `signature` = lowercase hex HMAC-SHA256(secret, query without `&signature=...`) and the `X-MBX-APIKEY` header.

| method + path | request | response the connector reads |
|---|---|---|
| `GET /api/v3/exchangeInfo?symbols=["S"]` (percent-encoded) | public | `serverTime`, `rateLimits[]{rateLimitType,interval,intervalNum,limit}`, `symbols[]{symbol,status,baseAsset,quoteAsset,orderTypes[],filters[]}`; filters `PRICE_FILTER.tickSize`, `LOT_SIZE.{stepSize,minQty,maxQty}`, `NOTIONAL.{minNotional,maxNotional}` or `MIN_NOTIONAL.minNotional` |
| `GET /api/v3/time` | public | `{"serverTime": ms}` |
| `GET /api/v3/depth?symbol=S&limit=1000` | public | `{"lastUpdateId":L,"bids":[["px","qty"]...],"asks":[...]}` |
| `GET /api/v3/openOrders[?symbol=S]` | signed | array of `{symbol,orderId,clientOrderId,price,origQty,executedQty,status,timeInForce,type,side}` |
| `DELETE /api/v3/openOrders?symbol=S` | signed | 200 with an array; `400 {"code":-2011,...}` is treated as "nothing open" |
| `POST /api/v3/order`, `DELETE /api/v3/order`, `POST /api/v3/order/cancelReplace` | signed; same parameters as the WS API methods below | same bodies as the WS API `result` / `error` objects |
| `POST/PUT /api/v3/userDataStream` | simulator listenKey mode only | `{"listenKey":"..."}` |

Headers read: `X-MBX-USED-WEIGHT-1M`, `X-MBX-ORDER-COUNT-10S`, `Retry-After` (429/418). Error envelope: `{"code":-NNNN,"msg":"..."}`.

#### Market-data WebSocket

`/stream?streams=...` sends `{"stream":"<name>","data":{...}}`:

* `<sym>@depth@100ms`: `{"e":"depthUpdate","E":ms,"s":"SYM","U":first,"u":last,"b":[["px","qty"]],"a":[...]}` (qty `"0"` deletes)
* `<sym>@bookTicker`: `{"u":id,"s":"SYM","b":"px","B":"qty","a":"px","A":"qty"}`
* `<sym>@trade`: `{"e":"trade","E":ms,"s":"SYM","t":id,"p":"px","q":"qty","T":ms,"m":bool}` (`m` = buyer is maker, so the seller aggressed)

A raw `/ws/<stream>` payload without the wrapper is also accepted. Server pings are answered; the connector treats any frame or ping as liveness.

#### WebSocket API

`<ws_api_url>` (e.g. `/ws-api/v3`). Requests are `{"id":"<string>","method":"...","params":{...}}` with params in alphabetical order, `apiKey`, `timestamp`, `recvWindow` and `signature` (HMAC over the sorted `k=v&...` string). Responses: `{"id":..,"status":HTTP,"result":..|"error":{"code","msg","data"?},"rateLimits":[{rateLimitType,interval,intervalNum,limit,count}]}`.

| method | params used | result read |
|---|---|---|
| `userDataStream.subscribe.signature` | apiKey, recvWindow, timestamp, signature | `{"subscriptionId":N}` (request id `"uds"`) |
| `session.logon` | apiKey, recvWindow, timestamp, signature (Ed25519 keys only; request ids `"logon-o"` / `"logon-u"`) | status 200 |
| `userDataStream.subscribe` | none, after `session.logon` | `{"subscriptionId":N}` |
| `order.place` | newClientOrderId, newOrderRespType=ACK, price (not MARKET), quantity, side BUY/SELL, symbol, timeInForce (LIMIT only), type LIMIT/LIMIT_MAKER/MARKET | `{symbol,orderId,clientOrderId,transactTime}` |
| `order.cancel` | orderId or origClientOrderId, symbol | `{symbol,origClientOrderId,orderId,executedQty,status}` |
| `order.cancelReplace` | cancelOrderId or cancelOrigClientOrderId, cancelReplaceMode=STOP_ON_FAILURE, newClientOrderId, newOrderRespType=ACK, price, quantity, side, symbol, timeInForce, type | `{cancelResult,newOrderResult,cancelResponse{orderId,origClientOrderId,executedQty},newOrderResponse{orderId,clientOrderId}}`; on failure `error.data` carries the same keys plus `newOrderResponse.code/msg` |
| `openOrders.status` | recvWindow, timestamp (no symbol = all) | array as in REST openOrders (request id `"oo"`) |
| `openOrders.cancelAll` | symbol | array (request id `"ca"`) |

Request ids for orders are `n|c|r` + the 14-character client id (`fm` + 12 hex), so the simulator only has to echo `id`.

#### User data events

User data events arrive on the subscribed WS API connection as `{"subscriptionId":0,"event":{...}}` (legacy `/ws/<listenKey>` raw events are also accepted):

* `executionReport`: `E,s,c,S,q,p,C,x,X,r,i,l,z,L,n,N,T,t,m`, with `x` in `NEW`, `CANCELED` (the cancelled id is in `C`), `REPLACED`, `REJECTED`, `TRADE`, `EXPIRED`, `TRADE_PREVENTION`
* `outboundAccountPosition`: `E,B[]{a,f,l}` (forwarded only with `position_from_balance = true`)

Error codes the connector acts on: -1003/-1015 (cool down), -1021 (resync clock), -1022/-2014/-2015 (fatal), -2010/-2011 message texts (`Order would immediately match and take.`, `Unknown order sent.`, `Account has insufficient balance for requested action.`, `Duplicate order sent.`), -2013, -2021/-2022, HTTP 418/429.

## Binance USDⓈ-M futures

Perpetual contracts in one-way position mode, with HMAC or Ed25519 keys. Sources: the USDⓈ-M documentation at <https://developers.binance.com/docs/derivatives/usds-margined-futures/general-info> (read 2026-09-15), the field names of the official connector's generated models (`binance-connector-python`, `derivatives_trading_usds_futures`) and recorded Demo Trading market data (`tests/fixtures/binance_usdm/fixtures.meta.json`).

| channel | endpoint | purpose |
|---|---|---|
| md | `<ws_url>/public/stream?streams=<sym>@depth@100ms/<sym>@bookTicker` | depth diffs, best bid and offer |
| trades | `<ws_url>/market/stream?streams=<sym>@aggTrade` | aggregate trades |
| user | `<ws_private_url>/ws/<listenKey>`, default `<ws_url>/private` | `ORDER_TRADE_UPDATE`, `ACCOUNT_UPDATE`, `listenKeyExpired` |
| order | `<ws_api_url>`: `order.place` / `order.cancel` / `order.modify` | order entry; REST fallback `POST` / `DELETE` / `PUT /fapi/v1/order` |
| rest | `<rest_url>` | `exchangeInfo`, `depth`, `time`, `listenKey`, `openOrders`, `positionRisk`, account checks, kill-switch `DELETE /fapi/v1/allOpenOrders` |

The `/public`, `/market` and `/private` paths come from the 2026-03-05 URL split; the unrouted `/ws` and `/stream` URLs were decommissioned on 2026-04-23 ("Important WebSocket Change Notice"). Demo Trading hosts: REST `https://demo-fapi.binance.com`, streams `wss://demo-fstream.binance.com`, WebSocket API `wss://testnet.binancefuture.com/ws-fapi/v1`.

Ed25519 keys (`key_type = "ed25519"`) log on to the WS API order connection with `session.logon` (request id `"logon"`; the USDⓈ-M WS API documents it for Ed25519 keys only) and send `order.place` / `order.cancel` / `order.modify` without `apiKey` and `signature` after that, as on Spot. REST requests are signed with Ed25519. There are no SBE streams for USDⓈ-M.

The listenKey comes from `POST /fapi/v1/listenKey` and is kept alive with `PUT` every 30 minutes (valid for 60). On `listenKeyExpired` or a failed keepalive the connector requests a key, reopens the user connection and reconciles.

### Book sync

`GET /fapi/v1/depth?symbol=S&limit=1000` (weight 20). Deltas with `u` below `lastUpdateId` are dropped, the first applied delta has `U <= lastUpdateId <= u`, and each later delta's `pu` must equal the previous `u` (`BinanceFuturesSyncTraits`). A mismatch emits `ConnectionState{Resyncing}` and fetches a new snapshot, at most once per 2 s. The trades connection does not report its state to the engine, so a quiet `aggTrade` stream never clears the book. Mark price is not subscribed: the engine has no message for it.

### Orders

* LIMIT maps `timeInForce` GTC, IOC and FOK directly; post-only is LIMIT with `GTX`; MARKET has no price and no `timeInForce`. `reduceOnly=true` is sent when the engine sets it; `positionSide` is omitted (BOTH). A crossing GTX order is rejected with -5022 or expires (`OrderExpired`).
* `order.modify` keeps the venue's `clientOrderId` and takes the total quantity. The connector sends the new remaining quantity plus the filled quantity, reports later events for that order under the engine's new client id, and counts their fills from the modify. A modify answered with status `CANCELED` or `EXPIRED` becomes a cancel of the original and a reject of the replacement. A modified order loses its queue position, so `configs/binance-usdm-demo.toml` uses cancel and new (`supports_replace = false`).
* Rate limits come from `exchangeInfo.rateLimits`, the WS API `rateLimits` and the REST headers `X-MBX-USED-WEIGHT-1M` and `X-MBX-ORDER-COUNT-10S`.

### Positions and reconciliation

* On every user-stream connect and order-channel reconnect, `GET /fapi/v1/openOrders` (weight 40) and `GET /fapi/v3/positionRisk` (weight 5) become one `ReconcileMsg` sequence: Begin, one `OpenOrder` per order, one `Position` per configured instrument (flat when absent), End.
* The engine books every fill, including liquidations and ADL (`autoclose-*` client ids). The connector compares each `ACCOUNT_UPDATE` position with the fills it forwarded once no fill has arrived for 1 s, and sends a `PositionUpdate` only when they differ (`position_from_account_update`). The two event types are not ordered against each other, so forwarding every position would count fills twice.
* `load_reference_data()` refuses an account in hedge mode and logs the position mode, the leverage and margin type of each symbol and the margin balance. It changes no account setting.
* Funding payments are not booked: they arrive as balance-only `ACCOUNT_UPDATE` events, which the connector ignores.

### Errors

`binance_usdm_error_map.hpp` follows the USDⓈ-M error code page: -5022 post-only reject, -2018/-2019/-2027 insufficient balance or margin, -2022/-4118 reduce-only rejects, -4164 minimum notional, -4014/-4023 tick and lot, -1003/-1015 rate limit, -1021/-5028 clock resync, -1022/-2015/-4109 and -4061 (hedge mode) fatal, -2011/-2013/-4116 reconcile, HTTP 418 (stop REST), 429 (cool down) and 503 (execution status unknown, reconcile).

## Bybit v5 spot

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`/v5/public/spot`) | `orderbook.<depth>.SYM` snapshot/delta, `orderbook.1.SYM` to BookTicker, `publicTrade.SYM` |
| private | `ws_private_url` (default: ws_url host + `/v5/private`) | `op: auth`, then `order`, `execution`, `wallet` |
| trade | `ws_api_url` (`/v5/trade`) | `op: auth`, then `order.create` / `order.amend` / `order.cancel` with `reqId` and `X-BAPI-TIMESTAMP` / `X-BAPI-RECV-WINDOW` headers |
| rest | `rest_url` | `/v5/market/instruments-info`, `/v5/market/time`, `/v5/order/realtime`, `/v5/order/create|amend|cancel|cancel-all` |

Signing: REST `X-BAPI-SIGN` = hex HMAC-SHA256(secret, timestamp + api_key + recv_window + payload), where the payload is the exact query string (GET) or JSON body (POST) sent. WS auth: `{"op":"auth","args":[key, expires_ms, hex HMAC("GET/realtime" + expires)]}`. Every channel sends `{"op":"ping"}` every 20 s.

Book sync: the stream's `snapshot` resets the book; each `delta` must have a larger `u`; `u == 1` or a missing snapshot (10 s) re-subscribes the depth topic to get a fresh snapshot. Amend keeps the venue's `orderLinkId`: the engine receives an ack for its new client id and later `order`/`execution` events for the old link id are translated to it.

## Deribit (options and futures)

Deribit speaks JSON-RPC 2.0 over one WebSocket endpoint (`wss://test.deribit.com/ws/api/v2`), and the same methods are available over REST (`https://test.deribit.com/api/v2/<method>`). The sources are the official documentation at <https://docs.deribit.com> (articles, OpenAPI and AsyncAPI specs) and recorded testnet responses (`tests/fixtures/deribit/fixtures.meta.json`).

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url`, unauthenticated | `public/set_heartbeat`, `public/subscribe` `book.NAME.100ms`, `ticker.NAME.100ms`, `trades.NAME.100ms` |
| private | `ws_private_url` (default `ws_url`), a second connection | `public/auth` (client_credentials, refresh_token), `public/set_heartbeat`, `private/enable_cancel_on_disconnect`, `private/subscribe` `user.orders.KIND.CURRENCY.raw` + `user.trades.KIND.CURRENCY.raw`; order entry `private/buy`, `private/sell`, `private/edit`, `private/cancel`, `private/cancel_by_label`; reconciliation `private/get_open_orders_by_currency` |
| rest | `rest_url` | `public/get_time`, `public/get_instruments?currency=C&kind=option|future`; kill switch `private/cancel_all_by_instrument` with `Authorization: Basic base64(client_id:client_secret)` |

### Messages

Requests are `{"jsonrpc":"2.0","id":..,"method":..,"params":{..}}`. Order requests use the string id `n|c|r` + the 14-character client id (the testnet echoes string ids); control requests use small integers. Notifications are `{"method":"subscription","params":{"channel","data"}}`, and responses carry `result` or `error {code, message, data}`. Prices and amounts are JSON numbers, often with exponents (`1.0002e6`), and are parsed exactly from the raw token.

### Units

Engine quantities are contracts: `contract_multiplier` = Deribit `contract_size` (1 BTC for BTC options, 10 USD for BTC-PERPETUAL). Orders are sent with `contracts`, and book, trade and fill amounts are divided by the contract size. The label is the FastMM client id, and `post_only` is always sent (Deribit defaults it to `true`). Post-only orders set `reject_post_only` (config `reject_post_only`), so a crossing order is rejected (11054) rather than repriced. Prices are rounded passively onto `tick_size_steps` (BTC options: 0.0001, and 0.0005 from 0.005).

### Book sync

The first `book.NAME.interval` notification is a full snapshot. Every later change must have `prev_change_id` equal to the previous `change_id` (the ids are not consecutive integers). A mismatch emits `ConnectionState{Resyncing}` and sends `public/unsubscribe` then `public/subscribe` for that book channel, which yields a new snapshot. A missing snapshot (10 s) does the same, at most once per 2 s per instrument. Snapshots deeper than 1024 levels per side are truncated to the best levels.

### Options data

Each option ticker yields a `BookTicker` and an `OptionTicker` (mark, IVs, greeks, underlying price; see [options.md](options.md)).

### Session

Both connections enable heartbeats (interval >= 10 s; smaller values are refused with -32602) and answer every `test_request` with `public/test`. The access token goes into `params.access_token` of every private request. It is refreshed with `grant_type=refresh_token` at 80 % of `expires_in`, and an order answered with 13009 re-authenticates. After a private reconnect the venue reconciles open orders across all configured currencies (one `ReconcileMsg` Begin/End pair). When the private connection drops it cancels every subscribed instrument over REST (`cancel_on_order_channel_loss`), in addition to the venue-side cancel-on-disconnect.

### Edits

`private/edit` keeps the order id and label, and its `contracts` is the new total including fills. The engine receives an ack for its new client id. While the edit is in flight, a `user.orders` "open" update for the old label is not acked; later updates and fills for the old label map to the new id. Fills get `cum_qty`/`leaves_qty` from the venue's order shadow, because `user.trades` has no cumulative quantity.

### Rate limits

The credit model is from the rate-limits article: order requests use a leaky bucket sized by `matching_engine_rate` / `matching_engine_burst` (Tier 4 default 5/s, burst 20); check `private/get_account_summary` `limits` for the account's tier. New orders and edits are refused locally when the bucket is empty; cancels are always sent. A 10028 `too_many_requests` (after which the venue drops the session) drains the bucket.

### Errors

`deribit_error_map.hpp` follows the "Complete RPC Error Codes Reference" table. Among others: 11054 post_only_reject, 10009/10039 insufficient funds, 10004/11044 unknown order (10004 reconciles), 10043/10026 tick, 10005-10007 price bands, 10028 rate limit, 13004/13021 fatal, 13009 re-authenticate, and 10040/10041/10047/11051/13028 back off.

## Nasdaq TotalView-ITCH (`nasdaq_itch`)

Market data from TotalView-ITCH 5.0 over MoldUDP64 multicast (lines A and B), the initial book from GLIMPSE 5.0, and order entry to [fastmm-sim-itch](sim-itch.md) only. No REST, no API keys, no reference data from the venue: tick and lot come from `[[instruments]]`. Code: `include/fastmm/venues/nasdaq/`. Setup: [Receive a multicast feed](../how-to/operations/multicast-feeds.md).

```text
 KernelDatagramSource | XdpDatagramSource ─► moldudp::Receiver ─► RecoveryBuffer (during a snapshot)
   (rx_backend, lines A and B)                A/B, reorder,        │
                                              re-requests          ▼
                                                            ItchL2Bridge ─► BookSnapshot / BookDelta / Trade ─► md ring
                                                            (L3Book per instrument)
```

### Startup and recovery

1. `connect()` joins both lines and buffers every message (up to `recovery_buffer_packets` datagrams) while it logs in to GLIMPSE.
2. The snapshot's Stock Directory, Trading Action and Add Order messages build the L3 books; End of Snapshot gives the sequence number to continue from.
3. The buffered messages from that number on are applied. When the buffer starts after it, the receiver is moved back to it and the missing messages are re-requested; without `rerequest` another snapshot is taken.
4. Each book emits a `BookSnapshotMsg` (top `depth` levels per side) and the venue a `ConnectionStateMsg{Live}`.

The same procedure, after a `ConnectionStateMsg{Resyncing}` that clears the engine's books, follows:

| Trigger | Why |
|---|---|
| a gap the receiver gives up (no `rerequest`, or `max_request_attempts` unanswered requests) | the book misses messages |
| any L3 book error (an unknown or duplicate order reference, a book at `max_orders`) | later messages cannot repair it |
| the recovery buffer filling during a snapshot | the snapshot is taken again |

The buffer filling during two snapshots in a row stops the feed: the venue's kill switch trips with `KillReason::FeedLost`, and the feed state is `lost`. A GLIMPSE connection that fails or closes before End of Snapshot is retried after 1 s. Without `glimpse_url` the books are complete (empty) before sequence 1 and the receiver starts there; a gap given up stops the feed the same way.

### Events

Each datagram yields at most one `BookDeltaMsg` per instrument, with absolute quantities of the top `depth` levels that changed (0 deletes), and a `TradeMsg` per printable execution (E, C with Printable Y, P, Q). T0 (`t0_cycles`) is the `rdtscp` of the receive batch that carried the datagram, also for messages delivered later from the reorder buffer; `recv_ts` is the kernel receive time (the NIC time with `hw_clock = "phc_synced"`, the T0 wall clock on `af_xdp`); `venue_seq` is the MoldUDP64 sequence number of the last message applied; T1 is taken after decoding and the L3 update.

### Orders

| `order_entry` | Orders |
|---|---|
| `none` | refused at once: `OrderRejectMsg` with `RejectReason::VenueReject`, text `order_entry = none` |
| `sim_ouch` | OUCH 5.0 over SoupBinTCP to `ouch_url`: Enter, Replace, Cancel; Accepted, Replaced, Canceled, Executed and Rejected become order events |

With `sim_ouch`, an Enter or Replace Order triggered by market data carries the sequence token of the first ITCH message of its receive batch as ClOrdID (`T` + 13 digits); the simulator times it wire to wire. The engine copies only `t0_cycles` into outbound messages, so the venue maps each batch's `t0_cycles` to that sequence number (4096 recent batches). The simulator cancels a connection's orders when it closes: a lost OUCH connection is reported as `ConnectionStateMsg{Disconnected}` on channel 1 and a reconciliation without open orders, and the connection is retried after 1 s. `cancel_all()` shuts the connection down from the calling thread.

### Status

The venue fills the feed block of its status entry ([Status file](status-file.md#multicast-feed)): per-line packets, duplicates and A/B skew, gaps, recovered and given-up sequences, snapshots, the reorder high-water mark, requests, malformed datagrams, L3 book errors, the kernel-to-T0 histogram, and on `af_xdp` the XDP statistics and mode.

## Configuration keys

Common: `kind`, `ws_url`, `ws_api_url`, `rest_url`, `api_key`, `api_secret` ([secrets](configuration.md#general-rules)), `supports_replace`, `insecure_tls`, `ca_file`, `recv_window_ms`.

Venue-specific keys ([Configuration](configuration.md#connector-specific-keys)):

* Binance: `user_stream` (`ws_api` | `listen_key` | `none`), `key_type` (`hmac` | `ed25519`), `private_key_file`, `private_key_env`, `md_format` (`json` | `sbe`), `sbe_ws_url`, `order_api` (`ws` | `rest`), `depth_limit`, `stale_ms`, `dead_ms`, `position_from_balance`, `allow_offline_reference_data`, `cancel_on_order_channel_loss`, `emit_ack_from_response`.
* Binance USDⓈ-M: `ws_private_url`, `order_api`, `depth_limit` (5, 10, 20, 50, 100, 500 or 1000), `stale_ms`, `dead_ms`, `position_from_account_update`, `allow_offline_reference_data`, `cancel_on_order_channel_loss`, `emit_ack_from_response`, `key_type`, `private_key_file`, `private_key_env`. Example: `configs/binance-usdm-demo.toml`.
* Bybit: `ws_private_url`, `depth`, `order_api`, `stale_ms`, `dead_ms`, `ping_interval_ms`, `orders_per_second`, `position_from_wallet`, `allow_offline_reference_data`, `cancel_on_order_channel_loss`, `emit_ack_from_response`.
* Deribit: `api_key` / `api_secret` are the client id and client secret (`${FASTMM_DERIBIT_CLIENT_ID}` / `${FASTMM_DERIBIT_CLIENT_SECRET}`); extras `ws_private_url`, `currencies` (`"BTC"` or `["BTC", "ETH"]`), `book_interval` / `ticker_interval` / `trades_interval` (`100ms` | `agg2`; `raw` needs an authenticated connection), `heartbeat_interval_s` (>= 10), `reject_post_only`, `cancel_on_disconnect`, `cancel_on_order_channel_loss`, `matching_engine_rate`, `matching_engine_burst`, `stale_ms`, `dead_ms`, `allow_offline_reference_data`, `emit_ack_from_response`. Example: `configs/deribit-testnet.toml`.
* Nasdaq TotalView-ITCH: no `api_key` / `api_secret` (`resolve_venue_env` does not ask for them); `rx_backend`, `interface`, `line_a`, `line_b`, `line_a_interface`, `line_b_interface`, `line_a_source`, `line_b_source`, `queues`, `xdp_mode`, `rcvbuf`, `batch`, `rerequest`, `glimpse_url`, `glimpse_username`, `glimpse_password`, `reorder_packets`, `gap_timeout_ns`, `max_request_attempts`, `request_timeout_ns`, `recovery_buffer_packets`, `depth`, `price_window_ticks`, `max_orders`, `hw_timestamps`, `hw_clock`, `order_entry`, `ouch_url`, `ouch_username`, `ouch_password`. Example: `configs/nasdaq-itch-sim.toml`.

`stale_ms` defaults to 2000 ms for Binance and Bybit and 10000 ms for Deribit. `dead_ms` is raised to at least 45000 ms on Binance (the venue pings every 20 s), to 45000 ms for market data and 240000 ms for the other connections on Binance USDⓈ-M (pings every 3 minutes), twice `ping_interval_ms` plus 5000 ms on Bybit (45000 ms by default) and three heartbeat intervals on Deribit (30000 ms by default). A market-data connection without traffic for `stale_ms` is reported `Stale`: the engine pulls the venue's quotes and clears its books, and the connector fetches a new snapshot when data returns. Testnet BTCUSDT is often silent for more than 2 s, so the testnet and Demo configs set `stale_ms = 10000` (`configs/deribit-testnet.toml`: 15000).

## Open questions (`VERIFY:` in the code)

* Bybit: whether `u` increments by exactly one per delta (recorded testnet deltas did; the sync only requires increase).
* Bybit: whether amend `qty` includes the filled quantity (the testnet config therefore uses cancel + new, `supports_replace = false`).
* Bybit: whether `walletBalance` includes `locked`; balance-related `rejectReason` strings.
* Binance USDⓈ-M: the private payloads (`ORDER_TRADE_UPDATE`, `ACCOUNT_UPDATE`, WS API order responses) follow the documentation and the official connector's models but were not recorded, because the Demo account had no futures margin balance. Whether Demo rejects a crossing GTX order with -5022 or accepts and expires it is not confirmed; both are handled. `order.modify` is covered by the scripted fake exchange only.
* Binance Ed25519 and SBE: `session.logon`, unsigned orders and the SBE streams were tested against fastmm-sim-exchange, a scripted fake exchange and frames built from `stream_1_0.xml`, not against Binance (no Ed25519 key yet). Unconfirmed: whether Demo Mode accepts Ed25519 keys for the SBE streams, the error Binance returns when an HMAC key calls `session.logon` (the simulator answers -4056), and whether a combined SBE stream sends one event per binary frame.
* Binance: `GET /api/v3/time` weight (1 vs 2) and listenKey validity/keepalive figures (the documentation was removed; simulator mode only).
* Deribit: the private payloads (`user.orders`, `user.trades`, order and auth responses, open orders) follow the OpenAPI/AsyncAPI schemas but were not recorded, because that needs testnet keys; the live test covers them when the keys are exported. Whether `reject_post_only` rejections arrive as error 11054 (complete reference) or 11006 (the error page's summary table) is not confirmed; the error map also matches on the message text. The public `trades` `direction` is taken as the taker side, which the current documentation does not state. The account's matching-engine tier is not queried.
