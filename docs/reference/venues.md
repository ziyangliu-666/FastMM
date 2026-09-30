# Venue connectors

FastMM ships seven connectors behind the control-path `fastmm::venues::Venue` interface (`include/fastmm/venues/venue.hpp`): Binance Spot (testnet, Demo Mode or the local Binance-compatible simulator), Binance USDⓈ-M perpetual futures (Demo Trading), Bybit v5 spot and linear perpetuals (testnet), OKX v5 USDT-margined swaps (demo trading), Gemini perpetuals and spot (sandbox), Deribit options and futures (testnet) and Nasdaq TotalView-ITCH market data (with order entry to fastmm-sim-itch). Each registers itself in the venue registry (`include/fastmm/venues/registry.hpp`) under the `kind` a `[venues.<name>]` section names, and `make_venue()` resolves through it:

| kind | connector |
|---|---|
| `binance_spot`, `binance`, `sim` | `binance::BinanceVenue` |
| `binance_usdm` | `binance_usdm::BinanceUsdmVenue` |
| `bybit`, `bybit_spot` | `bybit::BybitVenue` |
| `okx` | `okx::OkxVenue` |
| `gemini` | `gemini::GeminiVenue` |
| `deribit` | `deribit::DeribitVenue` |
| `nasdaq_itch` | `nasdaq::NasdaqItchVenue` |

A project can register its own connector, with its own configuration keys, without changing FastMM ([Add a venue](../how-to/venues/add-a-venue.md), `examples/external-venue/`). Operating them: [Run in production](../how-to/operations/running-in-production.md).

Every connector runs on its own `net::Reactor` thread and writes normalised messages into two rings per venue: market data (lossy: a full ring drops the delta and forces a resync) and order events (never dropped: bounded spin, then an overflow the venue treats as fatal). Behind [fastmm-gateway](../how-to/operations/run-behind-a-gateway.md) the same code runs in the gateway and the rings are shared memory the strategy process reads. `cancel_all()` uses an independent blocking REST connection, so it works from any thread even if the reactor is wedged; Binance Spot retries a 418/429 refusal three times before reporting failure (`nasdaq_itch` shuts the OUCH connection down instead, see below). `Venue::poll()` runs after every reactor iteration; `nasdaq_itch` polls its sockets there in `spin_mode = "busy"`.

### Venue-side dead man's switch

`cancel_on_order_channel_loss` is the connector cancelling over REST when its order channel drops, which needs a live process. What survives a SIGKILL, an OOM kill or a dead host is the venue's own switch:

| venue | request | shape | window | default |
|---|---|---|---|---|
| Binance USDⓈ-M | `POST /fapi/v1/countdownCancelAll` (IP weight 10, per symbol) | countdown the connector refreshes every window/3 | `dead_mans_switch_ms`, ms; `countdownTime=0` stops it | 60000 |
| Deribit | `private/enable_cancel_on_disconnect`, scope `connection` | the venue watches its own socket | detection is the `public/set_heartbeat` interval; without heartbeats a socket that dies without a FIN takes the 10-minute inactivity timeout | on |
| Bybit v5 | `POST /v5/order/disconnected-cancel-all` (product `SPOT`, or `DERIVATIVES` for linear) + the `dcp.spot` / `dcp.future` private topic | account setting; Bybit starts the clock when every private connection subscribing `dcp.*` is gone | `dead_mans_switch_s`, seconds, clamped to [3, 300] | off |
| OKX | `POST /api/v5/trade/cancel-all-after` (1 request/s) | countdown the connector refreshes every window/3, for every pending order of the account | `dead_mans_switch_s`, seconds, clamped to [10, 120]; `timeOut` "0" stops it | 60 |
| Gemini | `cancelOnDisconnect=true` on the order connection; with `heartbeat`, `POST /v1/heartbeat` | the venue watches its own socket; the heartbeat is a countdown the connector refreshes every 15 s, for the API key's orders | disconnect; 30 s without an authenticated request | on; heartbeat off |
| Binance Spot | — | none exists | — | — |

Bybit's is off by default because Bybit grants it only to institutional accounts, configured by an account manager ("DCP feature is only available for Ins clients"); the connector arms it and subscribes `dcp.spot` (`dcp.future` for linear) only when `dead_mans_switch_s` is set, and logs a refusal without stopping. Binance Spot has none: `rest-api.md`, `web-socket-api.md` and `fix-api.md` have no countdown, session auto-cancel or cancel-on-disconnect as of the 2026-09 docs, and the FIX `News` "countdown" counts down to a maintenance logout without touching orders.

Only a countdown can lapse while the process lives. If Binance USDⓈ-M, OKX or the Gemini heartbeat refuses the refresh for a whole window, the venue has cancelled those orders, so the connector sets its fatal flag and trips the venue kill with `DeadMansSwitchLost` instead of requoting ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md#what-trips-it)). `CountdownSwitch` (`include/fastmm/venues/dead_mans_switch.hpp`) keeps two clocks apart: when to retry, and when the venue last confirmed the countdown.

### Batch order entry

No connector batches order entry:

* Binance Spot has no batch endpoint. The order lists (OCO/OTO/OTOCO/OPO/OPOCO) are contingency structures, not a way to send two independent quotes.
* Binance USDⓈ-M has `POST /fapi/v1/batchOrders` (5 orders, IP weight 5, 1 against the 1-minute order limit instead of 5) and `DELETE /fapi/v1/batchOrders` (10 orders, IP weight 1 instead of 1 each). Both are REST-only, so using them would move quotes and cancels off the WS API onto an HTTP round trip. The connector already coalesces an engine batch into one TCP write between `cork()` and `uncork()`, so a batch request would not save a round trip on the WS path either.
* Bybit v5 has `order.create-batch` / `order.amend-batch` / `order.cancel-batch` on the WebSocket trade endpoint, with their own rate-limit pool. They are asynchronous ("please use the websocket to confirm the order status") and report per-order outcomes positionally in `retExtInfo.list[]` while the top-level `retCode` stays 0, so a two-sided quote can come back half-accepted. Two single orders each get their own response under their own `reqId` and the per-order error mapping. Spot batches are charged per order, so there is no rate-limit saving.
* OKX has `batch-orders` / `batch-amend-orders` / `batch-cancel-orders` (20 orders) on the same WebSocket, reported per order like Bybit's, and `mass-cancel` only for options in portfolio margin mode. The connector sends single orders; cancel-all uses REST `cancel-batch-orders`.
* Gemini has no batch order entry; `order.cancel_all` and `order.cancel_session` exist on the WebSocket, and the connector cancels over REST `order/cancel/session`.
* Deribit has only `private/mass_quote`, which is a separate pathway: it needs administrator approval per user and currency (`13902 mass_quotes_disabled`), cancel-on-disconnect (`13042 cod_not_enabled`) and an MMP group that reserves initial margin; it allows one quote per instrument per side per group, is not available for spot, and its quotes are not orders (`private/buy`, `private/sell` and `private/edit` do not apply; they are cancelled with `private/cancel_quotes`). Supporting it means a second order lifecycle beside the OMS's. Its bulk cancels (`private/cancel_all_by_instrument`, `private/cancel_all_by_currency`) are what the kill switch and the channel-loss path use.

Requests are charged what their endpoint charges. On Binance USDⓈ-M's WebSocket order path a place and a modify cost 0 IP weight and a cancel 1, with the order limits taking the other side (charging 1 per request would use half the 2400/minute IP budget at the venue's 1200 orders/minute). Binance Spot's `order.amend.keepPriority` costs IP weight 4 and nothing against the ORDERS bucket ("Unfilled Order Count: 0").

A venue-fatal error and a REST hard stop stop new orders and replaces; Cancel and cancel-all are always admitted, on whatever transport is still usable. Between `cork()` and `uncork()` a frame is only encoded, so a failed `uncork()` rejects every order of the batch (`RejectReason::TransportFull`) rather than counting them as sent. A reconciliation snapshot reaches the engine only when the whole reply parsed: `Oms::reconcile_end()` cancels every order the snapshot does not name, so a rejected or truncated reply is dropped rather than emitted as an empty snapshot. What it may cancel is bounded by the `sent_watermark` its `Begin` carries: orders the engine sent after the snapshot was asked for cannot be in it. An order the snapshot drops while it still had working quantity is reported (`OmsStats::reconcile_unresolved`, `EngineStats::unresolved_orders`) and logged, because the venue ended it without saying whether it filled or was cancelled.

A connector reconciles after a private or order channel reconnects, and when the engine asks: after more than three cancel rejects the engine sends `ControlCommand::Reconcile` on the venue's outbound ring and the connector calls `request_open_orders()`. The first snapshot of a session is a sweep (Binance Spot: when the order channel first goes Live; USDⓈ-M: the user stream; Bybit, OKX, Deribit: the private channel), with an empty watermark, so the snapshot says nothing about the engine's own orders: it finds only orders the engine does not know, which a session that died without cancelling left resting, and the engine cancels them. Their client order ids carry an earlier session epoch, so they cannot be confused with this session's.

Every connector runs its snapshot through one `ReconcileDriver` (`venues/reconcile_driver.hpp`): one snapshot at a time, a request made meanwhile served by one more straight after it; the execution replay first; the watermark taken when the snapshot is asked for, after the replay; a snapshot that failed asked again after 5 s and one that got no answer abandoned after 60 s; nothing asked once `disconnect()` has begun. After each snapshot the connector drops the order shadows it proves over: an order answered before the snapshot was asked for that the venue no longer holds ended with its terminal event lost (`VenueStatus::shadows_swept`). "Before" is by send sequence: every New and Replace takes the connector's next number, stamped on its shadow, and the watermark carries the number of the id it names, so behind `fastmm-gateway` every strategy's shadows are judged (their ids are in send order within one session epoch only).

The shadow table holds 7168 orders (8192 slots, `kShadowSlots`): the orders working at the venue and the replaces in flight, over every strategy behind the connector. An order or replace it has no room for is not sent; it goes back to the engine as `OrderTableFull`, counted in `VenueStatus::shadows_refused`, with one error line when the table fills and one warning when it has room again. Sent untracked, its replies would carry no instrument and it could not be amended; a USD-ⓈM modify would leave the venue's later fills booked to the order it replaced.

### Executions the private stream never delivered

An open-order snapshot cannot report a fill that finished an order: the venue no longer holds the order, so its quantity, price and fee are absent. Inferring the quantity from a `cum_qty` the venue reports later (`Oms::absorb_cum` → `Engine::book_missed_fill`) works only while the order is still open, and is an estimate at the order's own price with no fee.

So a reconciliation asks what the account executed before it asks what is open. `Venue::request_executions(since_venue_ms)` runs the venue's trade-history query for every subscribed instrument and emits each execution into the order sink as an ordinary `OrderFillMsg` carrying the venue's execution id and `OrderFillMsg::kReplayed`. `Oms::on_fill` deduplicates on the venue execution id, instrument and side over the last 4096 executions, so only the ones the engine never saw are booked, with their real price and fee. An order the snapshot then drops has been proved cancelled. The snapshot's `Begin` carries `ReconcileMsg::kExecutionsExact` when every instrument answered in full.

Every connector runs its replays (and USDⓈ-M's and OKX's funding queries) through one `ReplayScheduler` (`venues/replay_scheduler.hpp`). A replay reads each stream (a symbol on Binance and Gemini, a currency on Deribit, the account on Bybit and OKX) from its watermark to now. On Binance Spot and USDⓈ-M the watermark is the trade id after the last one read for that symbol (`fromId`) once there is one; otherwise, and on the other venues, it is a venue time, inclusive, and the rows read at or after it are remembered by id and not forwarded again. It moves to the end of what a replay read in full, but never past the replay's start less a settle margin (60 s; OKX 5 min), whether or not the replay found anything: an execution the venue shows in its history only after the query ran, with a time before it, is still inside the next query, and a quiet account does not keep asking from its connect time. It starts at `connect()`: a connector replays nothing from before it connected, because the engine's position starts at zero, unless a restarted session that restores its position says where to start with `resume_executions(since_venue_ms, known)` (venue time, the ids the store already holds) and, on Binance, `resume_trade_ids` (the next trade id per symbol) ([Recovery at start-up](storage.md#recovery-at-start-up)).

A range longer than the venue takes in one query is walked in windows (Binance Spot 24 h, USDⓈ-M and Bybit 7 days), and a start older than the venue keeps is moved up to what it keeps (USDⓈ-M and OKX 3 months, Bybit 2 years), which makes that replay not exact. A page is full when the venue returned as many rows as asked for, whether or not they were forwarded; the next page is asked for in the same replay, from the newest row (Binance: the id after the highest; Bybit and OKX, which answer newest first: the venue's page token, the window emitted oldest first once read). A window needing more than 20 pages is read again up to its oldest row seen; more pages than a replay allows (Binance 10 per symbol, 20 for USDⓈ-M funding, 100 otherwise), or a full page that cannot move past its start, leaves the replay not exact.

`VenueCapabilities::executions` says whether a connector can ask at all. When a reconciliation was not preceded by a complete replay:

* the snapshot's `Begin` carries no `kExecutionsExact`; the engine counts it (`EngineStats::estimated_reconciles`) and logs that a quantity no fill covered will be booked at the order's own price with no fee;
* an order the snapshot drops with quantity still working is reported (`EngineStats::unresolved_orders`), and the message says which case it is: the executions were replayed first, so the order was cancelled, or the venue could not be asked and the position may be short;
* a query that failed (a rate limit, a ban, a reply that did not parse) leaves the watermark where it was, and the replay is asked again 5 s after it ended. A reply to a query a `disconnect()` (or a lost Deribit connection) abandoned is ignored.

The replay also runs once a minute on every connector with executions. The watermark moves only when a replay runs, and the OMS deduplicates over a bounded window, so without it a reconnect after hours of streaming would replay more fills than it can deduplicate; it also books a fill the private stream dropped without disconnecting. `VenueStatus::execution_queries`, `executions_fetched` and `execution_query_errors` count the replays.

A fill the venue reported as cumulative quantity before any execution named it stays remembered (`Oms`'s synthetic-fill pool). When the replay later names it, the execution does not add its quantity again: it corrects the price and the fee (`OmsUpdate::corrected_qty`, `PositionTracker::correct_fill`, `EngineStats::corrected_fills`). Correcting `qty` from `est_px` to `px` moves the position's PnL by `qty * (px - est_px)`, given up on a buy and received on a sell; the average entry price keeps the estimate.

What each venue can answer, from their documentation (2026-09-24):

| venue | query | scope | window | identity | implemented |
|---|---|---|---|---|---|
| Binance Spot | `GET /api/v3/myTrades` | per symbol (`symbol` required) | `fromId` (ascending, inclusive) or `startTime`/`endTime` no more than 24 h apart; the two cannot be combined | `id` (int64, per symbol); the order only as `orderId` (no `clientOrderId`), and `side` as `isBuyer` | yes; weight 20, limit ≤ 1000 |
| Binance USDⓈ-M | `GET /fapi/v1/userTrades` | per symbol | `fromId` or a range up to 7 days, within the last 3 months; neither given returns 7 days | `id` (int64, per symbol, the user stream's `t`); the order only as `orderId`, and `side` | yes; weight 5, limit ≤ 1000 |
| Bybit v5 | `GET /v5/execution/list` | per account (only `category` is required) | a range up to 7 days, 2 years of history, opaque `nextPageCursor`, newest first | `execId` (string), and `orderLinkId` is the client id | yes; one account-wide query, limit 100 |
| OKX | `GET /api/v5/trade/fills` (3 days), `fills-history` (3 months) | per account and `instType` | `begin`/`end` (ms, on `ts`), paged newest first with `after` = `billId`, limit 100 | `tradeId` (per instrument), `clOrdId`, `billId` | yes; `instType=SWAP`, fills-history for a watermark older than 3 days |
| Gemini | `POST /v1/mytrades` | per symbol | `timestamp` (ms, trades at or after it), `limit_trades` ≤ 500, listed newest first | `tid` (int64), `order_id`, `client_order_id` | yes; the next page from the newest row |
| Deribit | `private/get_user_trades_by_currency_and_time` (WebSocket) | per currency (`kind` optional), or `..._by_instrument_and_time` per instrument | `start_timestamp`/`end_timestamp` (ms), `count` ≤ 1000, `sorting`, `has_more`; `historical: false` covers the last 24 h only and `true` older records (indexed after a short delay, recent ones excluded); a single call cannot span both | `trade_id` (string, unique per currency), `order_id`, `label` (the client id); `direction` is documented as the taker's, see below | yes; per currency, `kind: any` |

On Binance, Spot's and USDⓈ-M's history names the order by `orderId` only. The connector maps it through the order ids its acks and snapshots carried; one it has not seen (an order whose acknowledgement never came, or any order when the process started since) it asks for with `GET /api/v3/order?orderId=` (weight 4) or `GET /fapi/v1/order?orderId=` (weight 1) before the window's fills go out, so the fill names the order's `clientOrderId`, and with it the session and strategy. At most 16 lookups per replay, one per order, and none while the rate limiter has no room. An order that is not FastMM's is not asked for again. A failed or skipped lookup sends the fill naming no order with `OrderFillMsg::kUnresolved`, and the next replay (5 s later, 5 times at most) asks again and sends the fill once more, naming its order; after the last attempt, or when the order is not FastMM's, the last copy goes without the flag and still names none. An engine keeps one of the copies (same trade id); `fastmm-gateway` gives a flagged fill to no strategy until the copy that settles it. Bybit, OKX, Gemini and Deribit rows carry the client id (`orderLinkId`, `clOrdId`, `client_order_id`, `label`) and need no lookup.

On Deribit, a page with `has_more` is followed from its last row's timestamp; the replay is exact when every currency ends with `has_more: false`. A watermark older than 23 h is first read with `historical: true` up to 23 h ago, then with `historical: false` from there (the recent endpoint covers 24 h). The open-ended query ends 60 s past the replay's start, the venue's clock not being ours.

The schema (checked 2026-09-25) describes a user trade's `direction` as "Trade direction of the taker", the same text as for public trades, while the same row's `liquidity` (M/T) and `fee` are the account's own. The connector takes the side from the order it still holds and otherwise reads `direction` as the account's side, as it does for `user.trades`; neither reading has been checked on testnet.

## Binance Spot

| channel | endpoint | purpose |
|---|---|---|
| md | `<ws_url>?streams=<sym>@depth@100ms/<sym>@bookTicker/<sym>@trade` | combined stream, dispatch by stream suffix |
| md (`md_format = "sbe"`) | `<sbe_ws_url>?streams=<sym>@depth/<sym>@bestBidAsk/<sym>@trade` | binary SBE frames, dispatch by template id |
| user | `<ws_api_url>` + `userDataStream.subscribe.signature` (HMAC) or `session.logon` + `userDataStream.subscribe` (Ed25519) | `executionReport`, `outboundAccountPosition` |
| order | `<ws_api_url>`: `order.place` / `order.cancel` / `order.amend.keepPriority` / `order.cancelReplace` / `openOrders.status` / `openOrders.cancelAll` | order entry; REST fallback |
| rest | `<rest_url>` | `exchangeInfo`, `depth`, `time`, `openOrders`, `account/commission` (`fetch_fees`), REST order entry, kill-switch cancel-all |

The listenKey user stream (`POST/PUT /api/v3/userDataStream` + `/ws/<listenKey>`) remains as `user_stream = "listen_key"` for the simulator only; Binance removed it on 2026-02-20.

### Keys and session logon

* HMAC keys (`key_type = "hmac"`, the default) sign every request: apiKey + hex HMAC-SHA256. The key pads are hashed once (`net::HmacSha256Key`), so a signature costs about 170 ns.
* Ed25519 keys (`key_type = "ed25519"`, `private_key_file` or `private_key_env`) log on once per WS API connection with `session.logon` (the only signed request) on the order and user connections. Later requests carry neither `apiKey` nor `signature`, only `timestamp` and `recvWindow`. The order channel is Live only after the logon reply. A failed logon is fatal for bad key, signature or permission errors; timestamp, rate-limit and server errors retry the logon after 2 s. A revoked session (`{"id":null,"status":401,...}`, key deleted or IP not whitelisted) is logged and acted on through the error map (-2015 is fatal). REST requests (fallback, reconciliation, kill switch) are signed with Ed25519 per request, about 30 µs each, so use `order_api = "ws"` with Ed25519 keys.
* Binance supports `session.logon` with Ed25519 keys only, on production, the Spot testnet (`wss://ws-api.testnet.binance.vision/ws-api/v3`) and Demo Mode (`wss://demo-ws-api.binance.com/ws-api/v3`); RSA keys are not supported by FastMM.

Order encode with HMAC signing: 523 ns (`bench/bench_order_encoders.cpp`, [bench/README.md](../../bench/README.md), release-native, one pinned core, 2026-09-23). `BM_Encode_BinanceOrderPlace_Session` and `_Ed25519` are not in `bench/results/latest`.

### Fee rates

`fetch_fees = true` makes `fastmm-live` ask for the account's rates on each configured symbol at start-up, after `exchangeInfo`: `GET /api/v3/account/commission?symbol=S` (signed, weight 20 per symbol; HMAC and Ed25519 keys). `/sapi/v1/asset/tradeFee`, the bulk query, is not on the Spot testnet or in Demo Mode. The rate of a fill is `standardCommission` + `specialCommission` + `taxCommission`, maker or taker, plus the larger of each group's `buyer` and `seller` rate (a warning when they differ); the BNB `discount` is left out, because a commission paid in BNB is not booked (`FeeAsset::Other`). Rates are rounded to 1 cbps (1e-6).

The fetched rates replace `[venues.<x>.fees]` and `[[instruments]] maker_bps` / `taker_bps` for those symbols: `ctx.fees` returns them, and a line per symbol logs them, as a warning when they differ from the configuration. They are written as `maker_bps` / `taker_bps` into the configuration the journal and the store embed, so a replay and a backtest over the journal use them. A refused request stops the start (exit code 4). Fills are still booked with the commission each `executionReport` carries. `fastmm-gateway` does not fetch them; a strategy attached to it reads its own configuration.

### SBE market data

`md_format = "sbe"` reads the SBE market-data streams (<https://developers.binance.com/docs/binance-spot-api-docs/sbe-market-data-streams>): `wss://stream-sbe.binance.com[:9443]`, Demo Mode `wss://demo-stream-sbe.binance.com`, testnet `wss://stream-sbe.testnet.binance.vision`. `sbe_ws_url` defaults to `ws_url` with `stream.` / `demo-stream.` replaced by `stream-sbe.` / `demo-stream-sbe.`. The connection needs an Ed25519 API key in the `X-MBX-APIKEY` upgrade header (no signature; the server answers `400 No X-MBX-APIKEY header` without it), so the setting requires `key_type = "ed25519"` and works in a dry run without the private key.

| stream | template | message |
|---|---|---|
| `<sym>@depth` | `DepthDiffStreamEvent` 10003 | `BookDelta` (`firstBookUpdateId` / `lastBookUpdateId` as U / u) |
| `<sym>@bestBidAsk` | `BestBidAskStreamEvent` 10001 | `BookTicker` (`bookUpdateId` as sequence) |
| `<sym>@trade` | `TradesStreamEvent` 10000 | one `Trade` per group entry |
| `<sym>@depth20` | `DepthSnapshotStreamEvent` 10002 | decoded, not subscribed |

The schema is `tools/sbe/binance_spot_stream_1_0.xml` (schema id 1, version 0, from the binance-spot-api-docs repository); `tools/sbe_gen.py` generates `include/fastmm/venues/binance/generated/binance_stream_sbe.hpp` (CI checks it is current). Timestamps are microseconds; prices and quantities are int64 mantissas with a per-message exponent, converted exactly to 1e-8 fixed point (a value finer than 1e-8 makes the frame malformed). The depth snapshot still comes from REST (JSON), and depth sync is unchanged. JSON decode per message ([bench/README.md](../../bench/README.md), release-native, 2026-09-23): `BM_Json_BinanceDepth20` 608 ns, `BM_Json_BinanceDepth100` 2.73 µs, `BM_Json_BinanceBookTicker` 111 ns, `BM_Json_BinanceTrade` 109 ns. The `BM_Sbe_*` benchmarks are in `bench/bench_json.cpp` (`--benchmark_filter=Sbe_Binance`) but not in `bench/results/latest`.

WS API responses stay JSON: `responseFormat=sbe` would move order acks and execution reports to SBE schema 3 (retired every few months, currently version 5), for a saving of about 300 ns per order event.

Depth sync follows "How to manage a local order book correctly": buffer deltas, fetch `GET /api/v3/depth?symbol=S&limit=1000`, drop `u <= lastUpdateId`, first applied delta must have `U <= lastUpdateId+1 <= u`, then `U == prev_u + 1`. A gap emits `ConnectionState{Resyncing}` and re-snapshots (at most once per `min_snapshot_interval`, 2 s by default). A delta that changes more than 1024 levels on a side (production USDⓈ-M BTCUSDT does in a fast market; see `level_spill.hpp`) keeps the 1024 nearest the touch instead of being dropped, which would break the chain. The update that reveals a gap starts the buffer for the next snapshot. Every resync is logged with its reason and update ids, and the book's return with the time it was away.

### What a Binance-compatible simulator must implement

`BinanceVenue` uses only the following.

#### REST

Parameters go in the query string. Signed requests carry `timestamp`, `recvWindow`, `signature` = lowercase hex HMAC-SHA256(secret, query without `&signature=...`) and the `X-MBX-APIKEY` header.

| method + path | request | response the connector reads |
|---|---|---|
| `GET /api/v3/exchangeInfo?symbols=["S"]` (percent-encoded) | public | `serverTime`, `rateLimits[]{rateLimitType,interval,intervalNum,limit}`, `symbols[]{symbol,status,baseAsset,quoteAsset,orderTypes[],filters[]}`; filters `PRICE_FILTER.tickSize`, `LOT_SIZE.{stepSize,minQty,maxQty}`, `NOTIONAL.{minNotional,maxNotional}` or `MIN_NOTIONAL.minNotional` |
| `GET /api/v3/time` | public | `{"serverTime": ms}` |
| `GET /api/v3/depth?symbol=S&limit=1000` | public | `{"lastUpdateId":L,"bids":[["px","qty"]...],"asks":[...]}` |
| `GET /api/v3/account/commission?symbol=S` | signed, with `fetch_fees` | `standardCommission`, `specialCommission`, `taxCommission` `{maker,taker,buyer,seller}` |
| `GET /api/v3/openOrders[?symbol=S]` | signed | array of `{symbol,orderId,clientOrderId,price,origQty,executedQty,status,timeInForce,type,side}` |
| `DELETE /api/v3/openOrders?symbol=S` | signed | 200 with an array; `400 {"code":-2011,...}` is treated as "nothing open" |
| `GET /api/v3/order?symbol=S&orderId=N` | signed | `{orderId,clientOrderId}`, also for an order that has ended and traded |
| `POST /api/v3/order`, `DELETE /api/v3/order`, `POST /api/v3/order/cancelReplace`, `PUT /api/v3/order/amend/keepPriority` | signed; same parameters as the WS API methods below | same bodies as the WS API `result` / `error` objects |
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
| `order.amend.keepPriority` | newClientOrderId, newQty, orderId or origClientOrderId, symbol | `{transactTime,executionId,amendedOrder{symbol,orderId,origClientOrderId,clientOrderId,price,qty,executedQty,status}}` |
| `openOrders.status` | recvWindow, timestamp (no symbol = all) | array as in REST openOrders (request id `"oo"`) |
| `openOrders.cancelAll` | symbol | array (request id `"ca"`) |

Request ids for orders are `n|c|r|a` + the 14-character client id (`fm` + 12 hex), so the simulator only has to echo `id`. `a` is an amend, because its response has a different shape from a cancel-replace's.

#### User data events

User data events arrive on the subscribed WS API connection as `{"subscriptionId":0,"event":{...}}` (legacy `/ws/<listenKey>` raw events are also accepted):

* `executionReport`: `E,s,c,S,q,p,C,x,X,r,i,l,z,L,n,N,T,t,m`, with `x` in `NEW`, `CANCELED` (the cancelled id is in `C`), `REPLACED`, `REJECTED`, `TRADE`, `EXPIRED`, `TRADE_PREVENTION`
* `outboundAccountPosition`: `E,B[]{a,f,l}` (forwarded only with `position_from_balance = true`)

### Replaces: amend where the venue allows it, cancel-replace otherwise

`order.cancelReplace` cancels and places again, so the order goes to the back of the queue at its price. `order.amend.keepPriority` keeps the order, its `orderId` and its time priority, and only reduces the remaining quantity; it has no price parameter. The connector sends an amend when the replace is a size-down at the shadow's price (`cmd.qty < shadow.qty`; `newQty` at or above the current quantity is refused with -2038), and a `cancelReplace` for anything else: a price change, a size-up, a replace whose original has no shadow.

* The amended order gets the engine's new client id (`newClientOrderId`) and keeps its venue order id and its `executedQty`, where a cancel-replace produces a different venue order whose fills start at zero. The connector marks the ack with `OrderAckMsg::kAmendedInPlace` (from the response, and from the user stream, where `x` is `REPLACED`), and `Oms::on_ack` rekeys the order without resetting `cum_qty`. `QuoteManager` holds a handle, not an id, and needs nothing.
* `exchangeInfo`'s `MAX_NUM_ORDER_AMENDS` filter caps amendments per order (10 where published) and returns -2038 past it. The connector counts them in the order's shadow and falls back to `cancelReplace` at `max_order_amends`.

`amend_keep_priority = false` puts every replace back on `cancelReplace`.

Error codes the connector acts on: -1003/-1015 (cool down), -1021 (resync clock), -1022/-2014/-2015 (fatal), -2010/-2011 message texts (`Order would immediately match and take.`, `Unknown order sent.`, `Account has insufficient balance for requested action.`, `Duplicate order sent.`), -2013, -2021/-2022, -2038 (amend refused: the order is gone, the quantity did not go down, or `MAX_NUM_ORDER_AMENDS` is used up: nothing changed on the venue, so the original keeps working and the replacement id is rejected), HTTP 418/429.

## Binance USDⓈ-M futures

Perpetual contracts in one-way position mode, with HMAC or Ed25519 keys. Sources: the USDⓈ-M documentation at <https://developers.binance.com/docs/derivatives/usds-margined-futures/general-info> (read 2026-09-15), the field names of the official connector's generated models (`binance-connector-python`, `derivatives_trading_usds_futures`) and recorded Demo Trading market data (`tests/fixtures/binance_usdm/fixtures.meta.json`).

| channel | endpoint | purpose |
|---|---|---|
| md | `<ws_url>/public/stream?streams=<sym>@depth@100ms/<sym>@bookTicker` | depth diffs, best bid and offer |
| trades | `<ws_url>/market/stream?streams=<sym>@aggTrade` | aggregate trades |
| user | `<ws_private_url>/ws/<listenKey>`, default `<ws_url>/private` | `ORDER_TRADE_UPDATE`, `ACCOUNT_UPDATE`, `listenKeyExpired` |
| order | `<ws_api_url>`: `order.place` / `order.cancel` / `order.modify` | order entry; REST fallback `POST` / `DELETE` / `PUT /fapi/v1/order` |
| rest | `<rest_url>` | `exchangeInfo`, `depth`, `time`, `listenKey`, `userTrades`, `openOrders`, `positionRisk`, account checks, kill-switch `DELETE /fapi/v1/allOpenOrders`, dead man's switch `POST /fapi/v1/countdownCancelAll` |

The `/public`, `/market` and `/private` paths come from the 2026-03-05 URL split; the unrouted `/ws` and `/stream` URLs were decommissioned on 2026-04-23 ("Important WebSocket Change Notice"). Demo Trading hosts: REST `https://demo-fapi.binance.com`, streams `wss://demo-fstream.binance.com`, WebSocket API `wss://testnet.binancefuture.com/ws-fapi/v1`.

Ed25519 keys (`key_type = "ed25519"`) log on to the WS API order connection with `session.logon` (request id `"logon"`; the USDⓈ-M WS API documents it for Ed25519 keys only) and send `order.place` / `order.cancel` / `order.modify` without `apiKey` and `signature` after that, as on Spot. REST requests are signed with Ed25519. There are no SBE streams for USDⓈ-M.

The listenKey comes from `POST /fapi/v1/listenKey` and is kept alive with `PUT` every 30 minutes (valid for 60). On `listenKeyExpired` or a failed keepalive the connector requests a key, reopens the user connection and reconciles.

### Book sync

`GET /fapi/v1/depth?symbol=S&limit=1000` (weight 20). Deltas with `u` below `lastUpdateId` are dropped, the first applied delta has `U <= lastUpdateId <= u`, and each later delta's `pu` must equal the previous `u` (`BinanceFuturesSyncTraits`). A mismatch emits `ConnectionState{Resyncing}` and fetches a new snapshot, at most once per 2 s. As on Spot, a delta longer than 1024 levels a side keeps the levels nearest the touch (production BTCUSDT: up to 2144 on a side, 17 frames in 15 minutes on 2026-09-28). The trades connection does not report its state to the engine, so a quiet `aggTrade` stream never clears the book. Mark price is not subscribed: the engine has no message for it.

### Orders

* LIMIT maps `timeInForce` GTC, IOC and FOK directly; post-only is LIMIT with `GTX`; MARKET has no price and no `timeInForce`. `reduceOnly=true` is sent when the engine sets it; `positionSide` is omitted (BOTH). A crossing GTX order is rejected with -5022 or expires (`OrderExpired`).
* `order.modify` keeps the venue's `clientOrderId` and takes the total quantity. The connector sends the new remaining quantity plus the filled quantity, reports later events for that order under the engine's new client id, and counts their fills from the modify. A modify answered with status `CANCELED` or `EXPIRED` becomes a cancel of the original and a reject of the replacement. A modified order loses its queue position, so `configs/binance-usdm-demo.toml` uses cancel and new (`supports_replace = false`).
* Rate limits come from `exchangeInfo.rateLimits`, the WS API `rateLimits` and the REST headers `X-MBX-USED-WEIGHT-1M` and `X-MBX-ORDER-COUNT-10S`. Requests are charged what their endpoint charges, over the WS API as over REST: a place and a modify cost 0 IP weight and one against the 10 s and 1-minute order limits, a cancel costs 1 IP weight and no order.

### Dead man's switch

`POST /fapi/v1/countdownCancelAll?symbol=S&countdownTime=<ms>` (signed, IP weight 10). "If this endpoint is not called within 120 seconds, all your orders of the specified symbol will be automatically canceled". The timer is per symbol, sending it again replaces the running one, and `countdownTime=0` stops it. Binance publishes no minimum or maximum for `/fapi` (the 5000 ms floor belongs to the options endpoint `/eapi/v1/countdownCancelAll`) and notes only that it checks countdowns about every 10 ms. There is no WebSocket API method and no way to read the armed state back.

The connector arms it on every subscribed symbol from the housekeeping timer and refreshes at `dead_mans_switch_ms`/3, at most every 500 ms. The default 60 s window costs 10 IP weight per symbol per 20 s: 120 a minute for four symbols, 5 % of the 2400/minute budget; a shorter window raises that in proportion. `disconnect()` sends `countdownTime=0`, because a clean shutdown cancels its own orders.

The refresh clock advances only when the venue answers. A refused refresh is logged and retried; if none succeeds for a whole window, the connector trips `DeadMansSwitchLost`.

### Positions and reconciliation

* On every user-stream connect and order-channel reconnect, `GET /fapi/v1/userTrades` per symbol replays the executions since the last one forwarded ([above](#executions-the-private-stream-never-delivered)), then `GET /fapi/v1/openOrders` (weight 40) and `GET /fapi/v3/positionRisk` (weight 5) become one `ReconcileMsg` sequence: Begin, one `OpenOrder` per order, one `Position` per configured instrument (flat when absent), End.
* `userTrades` names the order by `orderId` only; the connector maps it back through the order ids its acks and snapshots carried (to the engine id a modify moved it to), keeping the last 8192 pairings (`RecentMap`, oldest evicted; Binance Spot does the same); an order not mapped is looked up ([Binance: the order a trade names](#executions-the-private-stream-never-delivered)). The replay asks `fromId` = the last trade id + 1 once it has one, else `startTime` = connect time or the restored watermark. A start older than 7 days is walked forward a week per query (bounded by `endTime`) and older than 3 months is clamped; either way that replay is not exact.
* The engine books every fill, including liquidations and ADL (`autoclose-*` client ids). The connector compares each `ACCOUNT_UPDATE` position with the fills it forwarded once no fill has arrived for 1 s, and sends a `PositionUpdate` only when they differ (`position_from_account_update`). The two event types are not ordered against each other, so forwarding every position would count fills twice.
* `load_reference_data()` refuses an account in hedge mode and logs the position mode, the leverage and margin type of each symbol and the margin balance. It changes no account setting.
* Funding: `GET /fapi/v1/income?incomeType=FUNDING_FEE` (weight 30, every symbol in one query) is the source. Each row on a subscribed symbol becomes a `Funding` event: `income` (negative paid) in `asset`, `time`, `tranId` as its id. The query runs with every reconciliation's execution replay and once a minute, from its own watermark ([as the executions](#executions-the-private-stream-never-delivered)), in windows of at most 7 days and no further back than 3 months; a full page is followed by the next, a failed query asked again 5 s later. An `ACCOUNT_UPDATE` with reason `FUNDING_FEE` names the symbol (`a.S`, since 2026-08-07) and the balance change (`B[].bc`) but carries no id, so it only triggers the query, a second later. User Data Streams and Get Income History pages read 2026-09-26.

### Errors

`binance_usdm_error_map.hpp` follows the USDⓈ-M error code page: -5022 post-only reject, -2018/-2019/-2027 insufficient balance or margin, -2022/-4118 reduce-only rejects, -4164 minimum notional, -4014/-4023 tick and lot, -1003/-1015 rate limit, -1021/-5028 clock resync, -1022/-2015/-4109 and -4061 (hedge mode) fatal, -2011/-2013/-4116 reconcile, HTTP 418 (stop REST), 429 (cool down) and 503 (execution status unknown, reconcile).

## Bybit v5 spot and linear perpetuals

One connector, one `category` per `[venues.<name>]` section: `spot` (the default) or `linear`, the USDT- and USDC-margined perpetuals. Every request that takes a category gets it, and the private topics are filtered by it. The table and the paragraphs below describe spot; [Linear perpetuals](#linear-perpetuals) lists what changes.

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`/v5/public/spot`) | `orderbook.<depth>.SYM` snapshot/delta, `orderbook.1.SYM` to BookTicker, `publicTrade.SYM` |
| private | `ws_private_url` (default: ws_url host + `/v5/private`) | `op: auth`, then `order`, `execution`, `wallet`, and `dcp.spot` when `dead_mans_switch_s` is set |
| trade | `ws_api_url` (`/v5/trade`) | `op: auth`, then `order.create` / `order.amend` / `order.cancel` with `reqId` and `X-BAPI-TIMESTAMP` / `X-BAPI-RECV-WINDOW` headers |
| rest | `rest_url` | `/v5/market/instruments-info`, `/v5/market/time`, `/v5/order/realtime`, `/v5/execution/list`, `/v5/order/create|amend|cancel|cancel-all`, `/v5/order/disconnected-cancel-all` |

Signing: REST `X-BAPI-SIGN` = hex HMAC-SHA256(secret, timestamp + api_key + recv_window + payload), where the payload is the exact query string (GET) or JSON body (POST) sent. WS auth: `{"op":"auth","args":[key, expires_ms, hex HMAC("GET/realtime" + expires)]}`. Every channel sends `{"op":"ping"}` every 20 s.

Book sync: the stream's `snapshot` resets the book; each `delta` must have a larger `u`; `u == 1` or a missing snapshot (10 s) re-subscribes the depth topic to get a fresh snapshot. Amend keeps the venue's `orderLinkId`: the engine receives an ack for its new client id and later `order`/`execution` events for the old link id are translated to it.

Disconnect-Cancel-All: `POST /v5/order/disconnected-cancel-all` with `{"product":"SPOT","timeWindow":<3..300>}`, sent once per private connect when `dead_mans_switch_s` is set. The window is an account setting, not a countdown the client refreshes: Bybit starts the clock once every private connection that subscribed a `dcp.*` topic is gone and resets it when one reconnects ("your private websocket connection must subscribe 'dcp' topic in order to trigger DCP successfully"), so the connector subscribes `dcp.spot` only when the switch is on. Nothing is disarmed on shutdown; the session cancels its own orders.

Reconciliation: `GET /v5/order/realtime?category=spot&limit=50` is paged with `cursor` = `result.nextPageCursor` until the cursor is empty (at most 40 pages). Bybit answers a rate limit (10006/10018) and a clock or signature error (10002/10004) with HTTP 200 and a non-zero `retCode`, so the snapshot is gated on `retCode == 0` as well as the HTTP status.

Execution replay: `GET /v5/execution/list?category=spot&startTime=<watermark>&limit=100`, one query for the account, rows for unsubscribed symbols dropped. A range may span at most 7 days, so a watermark older than that is walked in 7-day windows (`endTime` = `startTime` + 7 days - 1 ms); the last window leaves `endTime` off. Each window is paged with `cursor` to its last page and then emitted oldest first, since the rows come newest first. A window that needs more than 20 pages is read again up to its oldest row seen; a replay that needs more than 100 requests stops, reports itself incomplete and continues from the housekeeping retry. Only `execType` `Trade` is a fill; `Funding` is a funding payment on linear ([below](#linear-perpetuals)); `AdlTrade`, `BustTrade`, `Settle` and `Delivery` are skipped, as on the private stream. The fee asset is `feeCurrency`, or the spot rule when it is absent (buy pays base, sell pays quote, reversed for a negative maker rate). On the first private connect the replay runs before the start-up sweep's snapshot, from the resumed watermark when there is one.

### Linear perpetuals

`category = "linear"`, with `ws_url` on `/v5/public/linear` (`configs/bybit-linear-testnet.toml`). Sources: the v5 pages for instruments-info, place/amend/cancel order, open orders, cancel-all, trade history, position info, switch position mode, set DCP, and the private `order`, `execution`, `position` and `dcp` topics (read 2026-09-26).

* Reference data: `GET /v5/market/instruments-info?category=linear&symbol=S`. Only `contractType` `LinearPerpetual` is accepted (`LinearFutures` is refused). Tick is `priceFilter.tickSize`, lot `lotSizeFilter.qtyStep`, minimum quantity `minOrderQty`, minimum notional `minNotionalValue`. `qty` is in the base coin, so the multiplier is 1. The instrument becomes a perpetual that supports reduce-only, with base = `baseCoin` and quote = `settleCoin` (USDT for `BTCUSDT`, USDC for `BTCPERP`): [accounting](configuration.md#accounting) settles a linear instrument in its quote, so a configured quote that differs is replaced, with a warning.
* Position mode: one-way only. Bybit has no call that reads the mode; it is set per symbol (symbol > coin > default). `GET /v5/position/list?category=linear&symbol=S` answers whether or not there is a position, with one row (`positionIdx` 0) in one-way mode and one per side (1 and 2) in hedge mode. With keys, `load_reference_data()` reads it for each symbol and refuses a symbol in hedge mode, or one whose mode it cannot read. `fastmm-live` and `fastmm-gateway` exit 3 for this (`Venue::refused_account_settings()`), not 4: a retry does not fix it. A hedge-mode row that appears later, on the position topic or in a reconciliation, is fatal for the venue.
* Orders: `order.create` adds `"positionIdx":0` and `"reduceOnly":true` when the engine sets reduce-only. A market order has no `marketUnit` ("Perps, Futures & Option: always order by qty"). Amend and cancel take `category` and nothing else. The default `orders_per_second = 10` is the per-UID limit for linear create, amend and cancel.
* Private topics: `order`, `execution` and `position` (no `wallet`; `position_from_wallet` is spot only), and `dcp.future` when `dead_mans_switch_s` is set. DCP arms `{"product":"DERIVATIVES","timeWindow":N}`, which covers USDT and USDC perpetuals and futures and inverse contracts, with the same semantics and refusal handling as spot.
* Fees: a linear `execFee` is in the settlement coin, and a maker rebate is negative. With `feeCurrency` empty or equal to the quote, the fill's fee asset is `Quote`; another coin is `Other`. The spot base/quote rule does not apply.
* Reconciliation: after the execution replay (`GET /v5/execution/list?category=linear`, which needs no symbol), `GET /v5/order/realtime?category=linear&settleCoin=C&limit=50` and `GET /v5/position/list?category=linear&settleCoin=C&limit=200`, paged, for each settle coin of the subscribed instruments, become one `ReconcileMsg` sequence: Begin, `OpenOrder`*, one `Position` per subscribed instrument, End. Listed by settle coin, Bybit returns only non-zero positions, so an absent symbol is flat. The start-up sweep does the same, so a restart converges to the venue's position. A failed or unparsable position list emits nothing, as for open orders.
* Positions between reconciliations: Bybit publishes the position "every time when you create/amend/cancel an order ... regardless if there's any actual change". The connector compares its value with the sum of the fills it forwarded once neither has changed for 1 s, and sends a `PositionUpdate` only when they differ (a liquidation, ADL, another client), as on Binance USDⓈ-M; `position_from_stream = false` turns this off. The two topics are not ordered against each other, so forwarding every position would count fills twice.
* Cancel-all: `POST /v5/order/cancel-all` with `{"category":"linear","symbol":S}` per subscribed symbol (linear requires a symbol, base coin or settle coin).
* Funding: an execution with `execType` `Funding`, on the private `execution` topic or from `execution/list`, becomes a `Funding` event under its `execId`, with the amount `-execFee` in `feeCurrency` (the settle coin when empty). The sign is inferred: the transaction log page says its `funding` field ("positive fee value means receive funding") "is opposite to the execFee from Get Trade History"; the execution pages say nothing about funding. Whether the `execution` topic pushes Funding rows is not documented (`execution.fast` does not), so the replay's one-minute sweep books what the stream does not. The transaction log (`/v5/account/transaction-log`, `type` `SETTLEMENT`) carries the same payments with an explicit sign, but only for unified accounts and 50 rows a page. Pages read 2026-09-26.
* Not handled: the `tickers` topic (mark price, funding rate) is not subscribed, because no engine message carries it; `AdlTrade` and `BustTrade` rows are not fills of our orders and reach the engine only through the position correction or the next reconciliation. Leverage and margin mode are not read or set.

## OKX v5 USDT-margined swaps

`kind = "okx"`: USDT-margined perpetual swaps (`instType` SWAP, `ctType` linear, e.g. `BTC-USDT-SWAP`) in net position mode (`configs/okx-demo.toml`). Sources: <https://www.okx.com/docs-v5/en/> and the changelog <https://www.okx.com/docs-v5/log_en/> (entries to 2026-09-15), read 2026-09-26; public data recorded from the demo and production hosts the same day (`tests/fixtures/okx/fixtures.meta.json`).

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`/ws/v5/public`) | `books` (or `books50-l2-tbt` / `books-l2-tbt`, which need VIP4 and a login), `bbo-tbt` to BookTicker, `trades` |
| private | `ws_private_url` (default: ws_url host + `/ws/v5/private`) | `op: login`, then `orders`, `positions` (`updateInterval` 0: events only), `balance_and_position` |
| order | `ws_api_url` (default: the private URL, a second connection) | `op: login`, then `order` / `amend-order` / `cancel-order` |
| rest | `rest_url` | the requests below |

| request | use | documented limit |
|---|---|---|
| `GET /api/v5/public/instruments?instType=SWAP&instId=I` | reference data, `instIdCode` | 20 / 2 s per IP and instType |
| `GET /api/v5/public/time` | clock offset (30 s tolerance) | 10 / 2 s per IP |
| `GET /api/v5/account/config` | position and account mode, at start-up | 5 / 2 s per user |
| `GET /api/v5/trade/orders-pending?instType=SWAP` | reconciliation, cancel-all | 60 / 2 s per user |
| `GET /api/v5/account/positions?instType=SWAP` | reconciliation | 10 / 2 s per user |
| `GET /api/v5/trade/fills`, `fills-history` | fill replay: 3 days, 3 months | 60 / 2 s, 10 / 2 s per user |
| `GET /api/v5/account/bills`, `bills-archive` (`type=8`) | funding: 7 days, 3 months | 5 / s, 5 / 2 s per user |
| `POST /api/v5/trade/cancel-batch-orders` | kill switch, order-channel loss (20 a request) | 300 orders / 2 s per user and instrument |
| `POST /api/v5/trade/cancel-all-after` | dead man's switch | 1 / s per user |
| WebSocket `order`, `amend-order`, `cancel-order` (REST `trade/order` etc. as fallback) | order entry | 60 / 2 s each, per user and instrument; 1000 new + amend / 2 s per sub-account |

* Signing: REST `OK-ACCESS-SIGN` = base64 HMAC-SHA256(secret, ISO timestamp + method + path with query + body), with `OK-ACCESS-KEY`, `OK-ACCESS-TIMESTAMP`, `OK-ACCESS-PASSPHRASE`. WebSocket `login`: the same HMAC over Unix seconds + `GET/users/self/verify`. Three credentials: `api_key`, `api_secret`, `api_passphrase`. `testnet = true` (the default) is demo trading: every REST request, public ones included, carries `x-simulated-trading: 1`, because demo instruments differ (BTC-USDT-SWAP: tick 0.01 against 0.1, `instIdCode` 2021032601102993 against 10459). A demo host with `testnet = false`, or the reverse, is refused.
* Units: OKX sizes swaps in contracts of `ctVal` × `ctMult` of `ctValCcy` (0.01 BTC for BTC-USDT-SWAP), and so does FastMM: book, trade, order, fill and position quantities are contracts, `contract_multiplier` = `ctVal` × `ctMult`, so notional is price × contracts × multiplier in USDT and PnL is linear in it. Base = `ctValCcy`, quote = `settleCcy`, which [accounting](configuration.md#accounting) settles in. Tick `tickSz`, lot `lotSz`, minimum `minSz`, maximum `maxLmtSz`; no minimum notional. Only linear SWAP is accepted; a `state` other than `live` or `post_only` disables the instrument.
* Account: with keys, `load_reference_data()` reads `account/config` and refuses `posMode` `long_short_mode`, `acctLv` 1 (spot mode trades no swaps) and a key the venue refuses; `fastmm-live` and `fastmm-gateway` exit 3. A `long` or `short` position seen later is fatal for the venue. `td_mode` (cross by default, or isolated) goes on every order; leverage and margin are not read or set.
* Book sync: `books` sends a 400-level snapshot (`prevSeqId` -1), then updates every 100 ms. An update applies when its `prevSeqId` equals the last `seqId`: the documented heartbeat (no change for 60 s, empty sides, `seqId` = `prevSeqId`) and the reset after maintenance (`seqId` below `prevSeqId`) both do. Anything else emits `Resyncing` and unsubscribes and subscribes the channel for a new snapshot (`OkxSyncTraits`, `okx_book_sync.hpp`). An update can change more levels than the book holds (up to 800 asks on BTC-USDT-SWAP); past 512 a side the parser keeps the levels nearest the touch (`level_spill.hpp`). The CRC-32 `checksum` of the top 25 levels was deprecated on 2026-06-23 and is 0 on both hosts; the connector keeps each level's text and checks a non-zero checksum (a mismatch resyncs, `SyncReason::ChecksumMismatch`), and costs nothing when it is 0.
* Orders: `instIdCode` names the instrument (the WebSocket ignores `instId` since 2026-03-26 for `order` and since 2026-04-07 for amend and cancel); REST still takes `instId`, and an instrument without an `instIdCode` sends its orders over REST. `ordType` `limit`, `post_only`, `ioc`, `fok` or `market`; `reduceOnly` when the engine sets it; no `posSide`. The request `id` (`<kind><id>`, 15 characters) and `clOrdId` (FastMM's 14) fit the "up to 32 alphanumerics" rule. A post-only order that would take is accepted and then pushed `canceled` with `cancelSource` 31 (since 2026-08-20): it becomes `OrderExpired`, as do IOC and FOK remainders (13, 14); other cancels are cancel acks.
* Amend: `newSz` is the total including the filled quantity, as the engine's replace quantity is. `sCode` 0 means accepted, not applied: the `orders` push with `amendResult` under the `reqId` (the engine's new client id) is the ack (`kAmendedInPlace`, so the fills stay) or the reject. Later events for the original `clOrdId` are translated to the new id. The config uses cancel and new until amend has been seen on the demo.
* Private stream: `orders` pushes carry the fill (`tradeId`, `fillPx`, `fillSz`, `fillFee` negative when charged, `fillFeeCcy`, `execType` M/T) and the state; the fee is booked with the sign turned. The channel may repeat a message (2025-07-08): the OMS drops a repeated `tradeId`, and a repeated cancel or amend result finds nothing pending. `positions` is compared with the fills the connector forwarded, as on Bybit and Binance USDⓈ-M, and corrects the engine only when they differ (`position_from_stream`).
* Reconciliation: fills, then `orders-pending` (paged by `ordId`) and `account/positions`, as one Begin / OpenOrder* / Position* / End; an instrument absent from the positions is flat. The start-up sweep has an empty watermark. The fill replay asks from one millisecond before its watermark (the newest `ts` forwarded; whether `begin` is inclusive is not documented), drops rows below it and skips the ids forwarded at it; with no rows it moves to 5 minutes before the query. A window of more than 20 pages is read again up to its oldest row.
* Funding: `account/bills` with `type=8` (subType 173 expense, 174 income) is the source: one `Funding` event per bill on a subscribed instrument, `balChg` (positive received) in `ccy`, `billId` as its id. It runs with every reconciliation's fill replay, once a minute, and a second after a `balance_and_position` push with `eventType` `funding_fee`, which names no bill.
* Dead man's switch: `cancel-all-after` with `timeOut` = `dead_mans_switch_s` (default 60, [10, 120]), refreshed from the housekeeping timer every window/3; it cancels every pending order of the account, not only FastMM's. `disconnect()` sends `"0"` on its own blocking connection (a request queued on the REST channel would be dropped by its reset). A lapsed countdown trips `DeadMansSwitchLost`.
* Rate limits: client-side `orders_per_second` (default 25) on new orders and amends; a 50011 or 50061 reply pauses sending for 2 s (OKX counts over 2 s windows).
* Errors (`okx_error_map.hpp`): 50011/50061/60014 rate limit; 50102/50112/60004/60006/50036 clock resync; authentication and account blocks (501xx, 60005/60007/60009/60024, 50007, 50009, 50027, 50029, 51024, 51010, 59113) fatal; 50121 hard stop; 51400/51503/51603/50004 reconcile; 51008 balance, 51121/51020 lot, 51006/51137 price limit, 51016 duplicate id, 51511 post-only amend.
* Not handled: `books-rpi`, `trades-all` (one trade a message, on `/business`; `trades` aggregates per taker order and price), SBE market data, `expTime`, batch operations, the mark price and the funding rate.

## Gemini perpetuals and spot

`kind = "gemini"`: linear perpetuals (`product_type` swap, e.g. `btcgusdperp`) and spot (`btcusd`), one API for both (`configs/gemini-sandbox.toml`). Sources: <https://developer.gemini.com> (formerly docs.gemini.com), its OpenAPI spec `specs/openapi/rest.yaml` and AsyncAPI spec `specs/asyncapi/websocket.yaml` (0.10.7), read 2026-09-30; public data recorded from production the same day (`tests/fixtures/gemini/fixtures.meta.json`). The WebSocket API at `wss://ws.gemini.com` replaces the archived `v2/marketdata` and `v1/order/events` streams.

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` + `?snapshot=-1` | `{sym}@depth@100ms`, `{sym}@bookTicker` to BookTicker, `{sym}@trade`; `time` for the clock offset |
| order | `ws_api_url` (default `ws_url`) + `?cancelOnDisconnect=true` | authenticated at the upgrade; `orders@account`, `order.place`, `order.cancel` |
| rest | `rest_url` | the requests below, all `POST` except the first |

| request | use |
|---|---|
| `GET /v1/symbols/details/{symbol}` | reference data |
| `POST /v1/orders` | key check at start-up, reconciliation |
| `POST /v1/positions` | reconciliation, with a perpetual subscribed |
| `POST /v1/mytrades` | trade replay, per symbol |
| `POST /v1/perpetuals/fundingPayment?since=&to=` | funding |
| `POST /v1/order/cancel` | a cancel while the order connection is down |
| `POST /v1/order/cancel/session` | kill switch, order-connection loss |
| `POST /v1/heartbeat` | with `heartbeat` |

* Hosts: production `api.gemini.com` and `ws.gemini.com`, sandbox `api.sandbox.gemini.com` and `ws.sandbox.gemini.com`. The URLs default to the sandbox; `testnet` must match the hosts. The sandbox lists more perpetuals (53 against 13 on 2026-09-30) and returns nothing from the public `fundingamount`.
* Signing: REST requests are `POST`s with an empty body; the JSON payload (`request` = the path, `nonce`, parameters) goes base64-encoded in `X-GEMINI-PAYLOAD`, with `X-GEMINI-SIGNATURE` = hex HMAC-SHA384 of it and `X-GEMINI-APIKEY`. The order connection sends `X-GEMINI-NONCE`, `X-GEMINI-PAYLOAD` = base64 of the nonce and its signature on the upgrade request, with a new nonce for every connection. The WebSocket takes only account-scoped keys (`account-...`) with a time-based nonce, Unix seconds within 30 s of the venue; a `master-` key is refused at start-up.
* Units: a perpetual is 1 base unit per contract, so quantities are base units and `contract_multiplier` is 1. Tick is `quote_increment`, lot is `tick_size` (Gemini's name for the quantity step), minimum `min_order_size`; base `base_currency`, quote `quote_currency` (GUSD or USDC for perpetuals, the currency PnL is booked in). An inverse perpetual is refused; `status` other than `open`, `post_only` or `limit_only` disables the instrument.
* Book sync: with `snapshot=-1` the first `depthUpdate` after subscribing is the full book (`U` = `u`), the rest are diffs. A diff applies when `U` ≤ the last `u` < `u` (consecutive frames overlap at `U` = previous `u`), one with `u` ≤ the last is dropped, and one whose `U` is past the last `u` is a gap: `Resyncing`, then an unsubscribe and subscribe of the depth stream (`GeminiSyncTraits`, `gemini_book_sync.hpp`). A snapshot looks like any diff, so the feed takes the first frame after the connection opens, or after the unsubscribe's reply, as the snapshot. Levels past 512 a side are cut to those nearest the touch (`level_spill.hpp`); the production spot book lists asks at 1e10 and above, past the 8-decimal fixed point, which are left out.
* Orders: `timeInForce` `GTC`, `IOC`, `FOK`, or `MOC` for post-only; `type` `LIMIT` or `MARKET`. There is no reduce-only (a `reduce_only` order goes out plain) and no amend, so `supports_replace` is off. `order.cancel` takes the venue's id only: a cancel of an order not yet named waits for its `NEW` event. With the order connection down, a new order is refused (`TransportFull`) and a cancel goes over REST.
* Order events: `orders@account` `orderUpdate`: `NEW` is the ack; `PARTIALLY_FILLED` and `FILLED` carry the execution (`t`, `L`, `Z` this execution, `z` left, fee `n` on `FILLED` only, `m` maker); `CANCELED` carries the cumulative `Z` and is `OrderExpired` for the venue's own reasons (`MakerOrCancelWouldTake`, `ImmediateOrCancelWouldPost`, `FillOrKillWouldNotFill`, `SelfCrossPrevented`, `ExceedsPriceLimits`); `REJECTED` names its reason in `r`. The stream has no sequence number and the WebSocket sends no heartbeat: the connector sends `ping` every `ping_interval_ms` and reconciles after every reconnect.
* Reconciliation: trades, then `/v1/orders` and `/v1/positions` as one Begin / OpenOrder* / Position* / End; a perpetual absent from the positions is flat, and spot has no position rows. The start-up sweep has an empty watermark. A broken trade (`break` = `full`) is not booked.
* Funding: `perpetuals/fundingPayment` (hourly transfers, `Debit` paid, `Credit` received, in `assetCode`), with every reconciliation's replay and once a minute; a transfer has no id, so its symbol and time are its id.
* Dead man's switch: `cancelOnDisconnect=true` (`cancel_on_disconnect`, on by default) has the venue cancel the orders placed on the order connection when it closes. A key with "Requires Heartbeat" cancels its orders after 30 s without an authenticated request; `heartbeat = true` sends `POST /v1/heartbeat` every 15 s, and a lapse trips `DeadMansSwitchLost`. The kill switch and an order-connection loss send `order/cancel/session`, the key's orders on every symbol (Gemini has no per-symbol cancel-all), a 429 waited out (`BlockingControl`).
* Rate limits: REST 600 requests a minute for private endpoints, 5 a second recommended, a burst of 5 queued, then 429; the WebSocket's `conninfo` reports 3500 orders and 7000 request weight per 10 s. Client-side `orders_per_second` (default 20); a 429 or `-1003` pauses sending for 1 s.
* Errors (`gemini_error_map.hpp`): `RateLimit`, 429 and `-1003` rate limit; `InvalidNonce` clock resync; `InvalidSignature`, `InvalidApiKey`, `MissingRole`, `EndpointMismatch`, 401/403 and `-1002` fatal; `RemoteAddressForbidden` hard stop; `InsufficientFunds`, `InvalidPrice`/`LimitPriceOffTick`, `InvalidQuantity`, `DuplicateOrder`, `MakerOrCancelWouldTake` and `ExceedsPriceLimits` as rejects.
* Not handled: the archived v1/v2 streams, stop orders, the mark price and funding-rate streams (v2 only), `positions@account` (documented for prediction markets), `balances@account`, OAuth, master keys and their `account` field.

## Deribit (options and futures)

Deribit speaks JSON-RPC 2.0 over one WebSocket endpoint (`wss://test.deribit.com/ws/api/v2`), and the same methods are available over REST (`https://test.deribit.com/api/v2/<method>`). The sources are the official documentation at <https://docs.deribit.com> (articles, OpenAPI and AsyncAPI specs) and recorded testnet responses (`tests/fixtures/deribit/fixtures.meta.json`).

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url`, unauthenticated | `public/set_heartbeat`, `public/subscribe` `book.NAME.100ms`, `ticker.NAME.100ms`, `trades.NAME.100ms` |
| private | `ws_private_url` (default `ws_url`), a second connection | `public/auth` (client_credentials, refresh_token), `public/set_heartbeat`, `private/enable_cancel_on_disconnect`, `private/subscribe` `user.orders.KIND.CURRENCY.raw` + `user.trades.KIND.CURRENCY.raw`; order entry `private/buy`, `private/sell`, `private/edit`, `private/cancel`, `private/cancel_by_label`; reconciliation `private/get_user_trades_by_currency_and_time`, then `private/get_open_orders_by_currency` and `private/get_positions` |
| rest | `rest_url` | `public/get_time`, `public/get_instruments?currency=C&kind=option|future`; kill switch `private/cancel_all_by_instrument` with `Authorization: Basic base64(client_id:client_secret)` |

### Messages

Requests are `{"jsonrpc":"2.0","id":..,"method":..,"params":{..}}`. Order requests use the string id `n|c|r` + the 14-character client id (the testnet echoes string ids); control requests use small integers. Notifications are `{"method":"subscription","params":{"channel","data"}}`, and responses carry `result` or `error {code, message, data}`. Prices and amounts are JSON numbers, often with exponents (`1.0002e6`), and are parsed exactly from the raw token.

### Units

Engine quantities are contracts: `contract_multiplier` = Deribit `contract_size` (1 BTC for BTC options, 10 USD for BTC-PERPETUAL). Fills carry `fee_currency`: a fee in the quote coin is `FeeAsset::Quote` (BTC options are quoted in BTC, so base == quote), a base-coin fee on a linear instrument is `FeeAsset::Base`, and the base-coin fee of an inverse future is `FeeAsset::Other`: it is neither a quote amount nor a number of contracts, so the engine counts it (`unconverted_fees`) instead of booking it. Orders are sent with `contracts`, and book, trade and fill amounts are divided by the contract size. The label is the FastMM client id, and `post_only` is always sent (Deribit defaults it to `true`). Post-only orders set `reject_post_only` (config `reject_post_only`), so a crossing order is rejected (11054) rather than repriced. Prices are rounded passively onto `tick_size_steps` (BTC options: 0.0001, and 0.0005 from 0.005).

### Book sync

The first `book.NAME.interval` notification is a full snapshot. Every later change must have `prev_change_id` equal to the previous `change_id` (the ids are not consecutive integers). A mismatch emits `ConnectionState{Resyncing}` and sends `public/unsubscribe` then `public/subscribe` for that book channel, which yields a new snapshot. A missing snapshot (10 s) does the same, at most once per 2 s per instrument. A snapshot or change longer than 1024 levels on a side keeps the 1024 nearest the touch (`level_spill.hpp`); production BTC-PERPETUAL snapshots have about 1200 bids. Deribit lists levels best first in both (every one of 2114 snapshot sides and 104726 change sides with two or more levels, 108 books recorded on 2026-09-28; the docs do not say), but the parser selects by price and does not rely on it.

### Options data

Each option ticker yields a `BookTicker` and an `OptionTicker` (mark, IVs, greeks, underlying price; see [options.md](options.md)).

### Session

Both connections enable heartbeats (interval >= 10 s; smaller values are refused with -32602) and answer every `test_request` with `public/test`. The access token goes into `params.access_token` of every private request. It is refreshed with `grant_type=refresh_token` at 80 % of `expires_in`, and an order answered with 13009 re-authenticates. After the first private connect (the start-up sweep) and after every reconnect, the connector replays the executions and then reconciles open orders and positions across all configured currencies (one `ReconcileMsg` sequence: Begin, `OpenOrder`*, one `Position` per subscribed instrument, flat when `get_positions` does not list it, End). Nothing on the private stream reports a position, so a delivery, a liquidation or another client's trade reaches the engine only this way. When the private connection drops it cancels every subscribed instrument over REST (`cancel_on_order_channel_loss`), in addition to the venue-side cancel-on-disconnect.

Cancel-on-disconnect fires "when the TCP connection is properly terminated, when the connection is closed due to 10 minutes of inactivity, or when a heartbeat detects a disconnection", and not after `private/logout`. A host that dies without sending a FIN triggers neither of the first two quickly, which is why the connector always enables `public/set_heartbeat`. Scope is `connection`, so it cancels only that socket's orders.

### Edits

`private/edit` keeps the order id and label, and its `contracts` is the new total including fills. The engine receives an ack for its new client id. While the edit is in flight, a `user.orders` "open" update for the old label is not acked; later updates and fills for the old label map to the new id. Fills get `cum_qty`/`leaves_qty` from the venue's order shadow, because `user.trades` has no cumulative quantity.

### Rate limits

The credit model is from the rate-limits article: order requests use a leaky bucket sized by `matching_engine_rate` / `matching_engine_burst` (Tier 4 default 5/s, burst 20); check `private/get_account_summary` `limits` for the account's tier. New orders and edits are refused locally when the bucket is empty; cancels are always sent. A 10028 `too_many_requests` (after which the venue drops the session) drains the bucket.

`private/mass_quote` has its own buckets, outside this model entirely ("Rate limits described in this article do not apply to Mass Quotes"): `limits.matching_engine.maximum_quotes`, `maximum_mass_quotes` and `guaranteed_mass_quotes`. The connector does not use it ([Batch order entry](#batch-order-entry)).

### Funding

Perpetual funding is not booked. Deribit has no per-payment event: funding accrues continuously into the session PnL and is realized at the 08:00 UTC settlement. What reports it is aggregate or per interval: `realized_funding` on a `user.changes` position (the session so far), `interest_pl` on `private/get_transaction_log` entries of type `settlement` and on each trade or position change (1 request/s), `funding` in `private/get_settlement_history_by_instrument`. Booking it would mean deriving payments from session totals. Pages read 2026-09-26.

### Errors

`deribit_error_map.hpp` follows the "Complete RPC Error Codes Reference" table. Among others: 11054 post_only_reject, 10009/10039 insufficient funds, 10004/11044 unknown order (10004 reconciles), 10043/10026 tick, 10005-10007 price bands, 10028 rate limit, 13004/13021 fatal, 13009 re-authenticate, and 10040/10041/10047/11051/13028 back off.

## Nasdaq TotalView-ITCH (`nasdaq_itch`)

Market data from TotalView-ITCH 5.0 over MoldUDP64 (lines A and B, multicast or unicast), the initial book from GLIMPSE 5.0, and order entry to [fastmm-sim-itch](sim-itch.md) only. No REST, no API keys, no reference data from the venue: tick and lot come from `[[instruments]]`. Code: `include/fastmm/venues/nasdaq/`. Setup: [Receive a multicast feed](../how-to/operations/multicast-feeds.md).

```text
KernelDatagramSource | XdpDatagramSource      rx_backend, lines A and B
  ─► moldudp::Receiver                        A/B, reorder, re-requests
  ─► RecoveryBuffer                           during a snapshot
  ─► ItchL2Bridge                             an L3Book per instrument
  ─► BookSnapshot / BookDelta / Trade ─► md ring
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

Generic, parsed by FastMM for every venue: `kind`, `ws_url`, `ws_api_url`, `rest_url`, `api_key`, `api_secret`, `api_passphrase` ([secrets](configuration.md#general-rules)), `testnet`, `supports_replace`, `insecure_tls`, `ca_file`, `recv_window_ms`, `fees`.

Everything else in a `[venues.<name>]` section belongs to its connector, which declares, validates and documents it: the tables under [Connectors](configuration.md#connectors) are generated from those declarations, one per `kind`. A key the connector does not own is a warning naming its line; a key of the wrong type stops the session.

Not in the tables:

* Deribit's `api_key` / `api_secret` are the client id and client secret (`${FASTMM_DERIBIT_CLIENT_ID}` / `${FASTMM_DERIBIT_CLIENT_SECRET}`). `raw` book intervals need an authenticated connection.
* OKX needs a third credential, `api_passphrase` (`${FASTMM_OKX_API_PASSPHRASE}`), a generic key like `api_key`.
* Nasdaq TotalView-ITCH declares that it needs no credentials, so `fastmm-live` never asks for `api_key` / `api_secret`; OUCH order entry logs in with `ouch_username` / `ouch_password`.

Example configurations: `configs/binance-usdm-demo.toml`, `configs/bybit-linear-testnet.toml`, `configs/okx-demo.toml`, `configs/gemini-sandbox.toml`, `configs/deribit-testnet.toml`, `configs/nasdaq-itch-sim.toml`.

`stale_ms` defaults to 2000 ms for Binance, Bybit, OKX and Gemini and 10000 ms for Deribit. `dead_ms` is raised to at least 45000 ms on Binance (the venue pings every 20 s), to 45000 ms for market data and 240000 ms for the other connections on Binance USDⓈ-M (pings every 3 minutes), twice `ping_interval_ms` plus 5000 ms on Bybit, OKX and Gemini (45000 ms by default on Bybit and OKX; 30000 ms on Gemini) and three heartbeat intervals on Deribit (30000 ms by default). A market-data connection without traffic for `stale_ms` is reported `Stale`: the engine pulls the venue's quotes and clears its books, and the connector fetches a new snapshot when data returns. Testnet BTCUSDT is often silent for more than 2 s, so the testnet and Demo configs set `stale_ms = 10000` (`configs/deribit-testnet.toml`: 15000).

## Open questions (`VERIFY:` in the code)

* Bybit: whether `u` increments by exactly one per delta (recorded testnet deltas did; the sync only requires increase).
* Bybit: whether amend `qty` includes the filled quantity (the testnet config therefore uses cancel + new, `supports_replace = false`).
* Bybit: whether `walletBalance` includes `locked`; balance-related `rejectReason` strings.
* Binance USDⓈ-M: the private payloads (`ORDER_TRADE_UPDATE`, `ACCOUNT_UPDATE`, WS API order responses) follow the documentation and the official connector's models but were not recorded, because the Demo account had no futures margin balance. Whether Demo rejects a crossing GTX order with -5022 or accepts and expires it is not confirmed; both are handled. `order.modify` is covered by the scripted fake exchange only.
* Binance Ed25519 and SBE: `session.logon`, unsigned orders and the SBE streams were tested against fastmm-sim-exchange, a scripted fake exchange and frames built from `stream_1_0.xml`, not against Binance (no Ed25519 key yet). Unconfirmed: whether Demo Mode accepts Ed25519 keys for the SBE streams, the error Binance returns when an HMAC key calls `session.logon` (the simulator answers -4056), and whether a combined SBE stream sends one event per binary frame.
* Binance USDⓈ-M `countdownCancelAll`: the documentation gives no minimum or maximum for `countdownTime` on `/fapi` and no way to read the armed state back, so `dead_mans_switch_ms` is only clamped to be non-negative. Nor does it say what, if anything, the user stream carries when the countdown fires: futures `ORDER_TRADE_UPDATE` has no field for a cancel reason, so a countdown cancel is expected to look like any other `x: "CANCELED"`. Covered by the scripted fake exchange only.
* Binance Spot `order.amend.keepPriority`: the documentation does not say what happens when `newQty` is at or below what the order has already filled. The connector never sends it (the quote manager's target is a leaves quantity and the engine's replace quantity is a total above `cum_qty`), and the sim exchange refuses it with -1013. Also unstated, though strongly implied by the fields: that `C` on a `REPLACED` `executionReport` carries the pre-amend client id, which is what the connector reads.
* Bybit linear: implemented from the documentation (2026-09-26) and tested against the scripted fake exchange only. Unconfirmed: the private `order`/`execution`/`position` payloads for linear on testnet, whether `feeCurrency` is filled for linear executions (the documented example has `USDT`; empty is treated as the settle coin), and whether a hedge-mode symbol with no position returns both rows by symbol as the documentation implies.
* Bybit Disconnect-Cancel-All: no test account has it enabled, so the arming request and the `dcp.spot` / `dcp.future` subscriptions are covered by the scripted fake exchange only. Bybit documents no way to turn the window off again other than asking an account manager.
* OKX: implemented from the documentation (2026-09-26); the public stream ran against the demo and production hosts. Unconfirmed, with no keys here: login, the `orders` / `positions` / `balance_and_position` payloads, order, amend and cancel replies, `account/config`, fills, bills, `cancel-batch-orders` and `cancel-all-after` (whether demo trading honours it, and the unit of `triggerTime`), whether `fills` `begin` is inclusive, whether a position closed to zero is pushed with `pos` 0, whether a WebSocket rate-limit error comes as `code` or `sCode`, and the fee tier the demo needs for the tbt depth channels.
* Gemini: implemented from the documentation (2026-09-30); the public stream ran against production. Unconfirmed, with no keys here: the signed upgrade and `orders@account` payloads, the `order.place` result (the spec does not list its fields; the connector reads `orderId` when present and acks from the `NEW` event), whether a REST nonce may repeat within a second on a time-based key, whether `fundingPayment` takes `since`/`to` in the query with `request` = the path, whether `mytrades` pages forward from `timestamp` as its walk-through says (the list is newest first), whether a fee is reported on `PARTIALLY_FILLED` events and in which currency, how soon `cancelOnDisconnect` acts on a connection that dies without a FIN, whether `orders@account` covers orders placed over REST, and whether `btcusdcperp` and `btcgusdperp` are one book (their public data was identical on 2026-09-30).
* Binance: `GET /api/v3/time` weight (1 vs 2) and listenKey validity/keepalive figures (the documentation was removed; simulator mode only).
* Deribit: the private payloads (`user.orders`, `user.trades`, order and auth responses, open orders) follow the OpenAPI/AsyncAPI schemas but were not recorded, because that needs testnet keys; the live test covers them when the keys are exported. Whether `reject_post_only` rejections arrive as error 11054 (complete reference) or 11006 (the error page's summary table) is not confirmed; the error map also matches on the message text. The public `trades` `direction` is taken as the taker side, which the current documentation does not state. The account's matching-engine tier is not queried.
