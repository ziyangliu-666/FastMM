# fastmm-sim-exchange

`fastmm-sim-exchange` is a Binance Spot-compatible simulated exchange ([ADR-0008](../adr/0008-sim-exchange-speaks-binance.md)). The unmodified Binance connector (`fastmm::venues::binance::BinanceVenue`, see [venues.md](venues.md)) and `fastmm-live` run against it on localhost: TCP or TLS, REST, market-data WebSockets, the WebSocket API, depth sequence sync, HMAC authentication, order flow with fills, and fault injection.

```
fastmm-sim-exchange ── one net::Reactor thread
  ├─ HttpServer<PlainStream>              REST /api/v3/* + WebSocket upgrades   (:9080)
  ├─ HttpServer<TlsStream<PlainStream>>   the same over TLS (fixture cert)       (:9443)
  ├─ MatchingEngine        price-time, LIMIT_MAKER rejection, STP, per-symbol update ids
  ├─ MarketGenerator       counter-party flow (random-walk mid, Poisson limits/markets/cancels)
  ├─ MdAggregator          depthUpdate U/u batches every depth_update_ms, bookTicker
  └─ venue state           account balances, order index, listen keys, rate-limit windows
```

Code: `include/fastmm/sim/server/` and `src/sim/server/` (target `fastmm::sim_server`), app `apps/fastmm-sim-exchange/`.

## Running

```bash
./build/release/bin/fastmm-sim-exchange --config configs/sim.toml          # 127.0.0.1:9080 / :9443
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret \
  ./build/release/bin/fastmm-live --config configs/sim-local.toml           # or sim-local-tls.toml
./scripts/run-sim.sh --duration 30s [--tls] [--build-dir build/<dir>]      # both, plus a summary
docker compose up --build                                                   # configs/sim-docker.toml
```

Flags and exit codes: [Command lines](cli.md#fastmm-sim-exchange). Every `--stats-interval` the simulator prints a line with connections, orders, rejects, cancels, replaces, fills, open orders, public trades, depth diffs, tickers, snapshots, REST and WS API requests, rate-limited requests, authentication errors, the account position and the touch.

`scripts/run-sim.sh` starts the simulator and waits until port 9080 accepts connections; `--port` and `--tls-port` (or `FASTMM_SIM_PORT` and `FASTMM_SIM_TLS_PORT`) choose other ports and run the engine with a copy of its configuration that uses them. It then runs `fastmm-live --duration <t> --journal runs/<ts>/session.fmj --log runs/<ts>/engine.log`, prints the engine, venue and simulator summaries, and stops the simulator. The key and secret come from `FASTMM_SIM_API_KEY` / `FASTMM_SIM_API_SECRET` (default `sim-key` / `sim-secret`). Engine configs and the simulator both read them; in the simulator they override `[sim.account]`.

## Configuration (`configs/sim.toml`)

The simulator reads `[[instruments]]` (symbol, base, quote, tick, lot, min_qty, max_qty, min_notional) and `[sim]`. It declares `[venues.sim] kind = "sim"` only so that the instruments validate.

| key | default | meaning |
|---|---|---|
| `seed` | 7 | generator seed |
| `start_mid`, `symbols.<SYM>.start_mid` | 60000 | initial latent mid |
| `bind`, `port`, `tls_port`, `tls_cert`, `tls_key` | 127.0.0.1, 9080, 9443, fixture | listeners |
| `depth_update_ms` | 100 | depthUpdate aggregation window |
| `depth_snapshot` | `live` | `GET /api/v3/depth` from the live book, or `flushed` (book as of the last published batch) |
| `driver_tick_ms` | 2 | generator / aggregator poll period |
| `clock_offset_ms` | 0 | server clock = host wall clock + offset |
| `start_time_ms` | 0 | fixed epoch for reproducible timestamps (see Determinism) |
| `ping_interval_ms`, `pong_timeout_ms` | 20000, 60000 | server-initiated WebSocket pings |
| `listen_key_validity_ms` | 3600000 | legacy listenKey lifetime |
| `account.api_key`, `account.api_secret` | sim-key, sim-secret | the HMAC key (env overrides) |
| `account.ed25519_public_key_file` | none | Ed25519 public key (PEM): the account key becomes an Ed25519 key, `session.logon` works |
| `account.balances.<ASSET>` | 100 base, 10,000,000 quote | starting balances |
| `fees.maker_bps`, `fees.taker_bps` | 1, 4 | commission, charged in the quote asset |
| `limits.weight_per_minute` | 6000 | REQUEST_WEIGHT / 1 MINUTE |
| `limits.orders_per_10s`, `limits.orders_per_day` | 1000, 1000000 | ORDERS limits (Binance: 50 / 10 s) |
| `limits.max_recv_window_ms` | 60000 | larger recvWindow answers -1131 |
| `generator.*` | see file | `MarketGeneratorParams` + `enabled`, `seed_levels` |
| `faults.*` | off | see Fault injection |

The default generator places its touch 2990 ticks (29.90 USDT) from a slowly moving latent mid. `BasicMM`'s 5 bps quotes (30 USDT at 60000) therefore rest just behind the best generator levels and are filled by the larger market orders. The book is not realistic.

## What is implemented

Request and response formats are those `BinanceVenue` uses ([What a Binance-compatible simulator must implement](venues.md#what-a-binance-compatible-simulator-must-implement)); this section lists what the simulator serves beyond them and how it behaves.

### REST

Query string or form body; the signature covers query + body.

| endpoint | notes |
|---|---|
| `GET /api/v3/ping`, `GET /api/v3/time` | |
| `GET /api/v3/exchangeInfo[?symbol= / ?symbols=[..]]` | rateLimits, PRICE_FILTER, LOT_SIZE, NOTIONAL, MAX_NUM_ORDERS |
| `GET /api/v3/depth?symbol&limit` | limit 1..5000, weight 5/25/50/250 |
| `GET /api/v3/ticker/bookTicker?symbol` | |
| `GET /api/v3/account` | balances |
| `POST /api/v3/order`, `POST /api/v3/order/test` | LIMIT (GTC/IOC/FOK), LIMIT_MAKER, MARKET; ACK/RESULT/FULL |
| `DELETE /api/v3/order`, `GET /api/v3/order` | by `orderId` or `origClientOrderId` |
| `POST /api/v3/order/cancelReplace` | STOP_ON_FAILURE and ALLOW_FAILURE, -2021/-2022 with `data` |
| `PUT /api/v3/order/amend/keepPriority` | quantity decrease, keeps queue priority |
| `GET /api/v3/openOrders[?symbol]`, `DELETE /api/v3/openOrders?symbol` | -2011 when nothing is open |
| `POST/PUT/DELETE /api/v3/userDataStream` | legacy listenKey (`/ws/<listenKey>`) |

Every response carries `X-MBX-USED-WEIGHT-1M`. Order endpoints also carry `X-MBX-ORDER-COUNT-10S` and `X-MBX-ORDER-COUNT-1D`. 429 responses carry `Retry-After`.

### Market-data WebSockets

`/stream?streams=a/b/c` sends `{"stream","data"}`. `/ws/<stream>` (several streams separated by `/`) and `/ws` send raw payloads, and both accept `SUBSCRIBE` / `UNSUBSCRIBE` / `LIST_SUBSCRIPTIONS`. Streams: `<sym>@depth`, `<sym>@depth@100ms` and `<sym>@depth@1000ms` all use the same `depth_update_ms` batches. `<sym>@bookTicker` is sent when the touch changed during a batch; `<sym>@trade` (which adds `M`) is sent immediately. `U`/`u` are the matching engine's per-symbol update ids, so the stream is contiguous and a REST snapshot's `lastUpdateId` is consistent with it.

### WebSocket API

`/ws-api/v3` echoes the request id verbatim. Methods: `ping`, `time`, `exchangeInfo`, `depth`, `ticker.book`, `userDataStream.subscribe.signature` (returns `{"subscriptionId":N}`), `session.logon` / `session.status` / `session.logout` and `userDataStream.subscribe` (Ed25519 account), `userDataStream.unsubscribe`, `order.place`, `order.test`, `order.cancel`, `order.cancelReplace`, `order.amend.keepPriority`, `order.status`, `openOrders.status`, `openOrders.cancelAll` and `account.status`. A 429 error carries `data.retryAfter`.

### User data events

Events go to WS API connections with a user-data subscription as `{"subscriptionId":N,"event":{...}}`, and raw to `/ws/<listenKey>` connections:

* `executionReport`. `x` is `NEW`, `CANCELED` (`c` = cancel request id, `C` = cancelled order id), `REPLACED` (amend), `REJECTED` (post-acceptance), `TRADE` (`l,L,n,N,t,m`), `EXPIRED` (IOC/FOK/MARKET remainder) or `TRADE_PREVENTION` (self-trade, EXPIRE_MAKER).
* `outboundAccountPosition` after every balance change. Events from one request are sent after its response.
* `listenKeyExpired`.

### Trading model

LIMIT / LIMIT_MAKER orders lock quote notional (buys) or base quantity (sells). A fill releases the lock and moves balances, with commission in the quote asset. The simulator validates filters (`-1013 Filter failure: PRICE_FILTER / LOT_SIZE / NOTIONAL / MAX_NUM_ORDERS`), duplicate client ids (`-2010 Duplicate order sent.`) and insufficient balance (`-2010`). A crossing LIMIT_MAKER answers `-2010 Order would immediately match and take.`.

### Authentication and limits

| condition | answer |
|---|---|
| missing API key / unknown key | 401 `-2014` / 401 `-2015` |
| missing `timestamp` or `signature` | 400 `-1102` |
| bad signature | 400 `-1022 Signature for this request is not valid.` |
| `timestamp >= serverTime + 1000` | 400 `-1021 ... 1000ms ahead of the server's time.` |
| `serverTime - timestamp > recvWindow` | 400 `-1021 Timestamp for this request is outside of the recvWindow.` |
| `recvWindow > max_recv_window_ms` | 400 `-1131` |
| weight window exceeded | 429 `-1003` + `Retry-After` |
| order-count window exceeded | 429 `-1015` + `Retry-After` |
| unknown order on cancel / query / amend | 400 `-2011 Unknown order sent.` / `-2013 Order does not exist.` |

The server also sends WebSocket pings every `ping_interval_ms` and closes connections that have not answered for `pong_timeout_ms`.

## Fault injection

The `[sim.faults]` settings are one-shot and count from the moment the listeners open. The programmatic API is thread-safe (`SimExchangeServer`).

| config key | API | what it exercises |
|---|---|---|
| `drop_md_after_s` | `drop_market_data_connections()` | md Disconnected, quotes pulled, reconnect, re-snapshot |
| `drop_ws_api_after_s` | `drop_ws_api_connections(include_user_streams)` | order-channel loss, REST cancel-all, reconnect |
| `skip_depth_update_after_s` | `skip_next_depth_update()` | `U != prev_u + 1`, Resyncing, re-snapshot |
| `timestamp_error_after_s` | `fail_next_timestamp()` | `-1021` handling, server-time resync |
| `rest_unresponsive_after_s` / `_for_s` | `set_rest_unresponsive(bool)` | REST timeouts (requests are swallowed) |
| `delay_ack_ms` | `set_ack_delay_ms(ms)` | WS API order responses and their user events arrive late |
| `reject_next_orders` | `reject_next_orders(k)` | `-2010` rejects |
| `rate_limit_next` | `rate_limit_next_requests(k)` | 429 / `-1003` / `Retry-After` cooldown |
| — | `set_clock_offset_ms(ms)` | clock skew between client and venue |
| — | `expire_listen_keys()` | legacy listenKey expiry |

`stats()` returns counters for all of the above, plus watermarks since `mark()`: minimum open orders, cancels, cancel-alls, open-order queries and reconnects.

## Determinism

The generator, order ids and trade ids depend only on the configuration and seed and on the order of requests (tested). Event times inside the matching engine come from the reactor clock. Published timestamps (`E`, `T`, `transactTime`, `serverTime`) and recvWindow checks follow the host wall clock plus `clock_offset_ms`; on WSL2 the wall clock steps by more than a second. With `start_time_ms` set, published times are `start_time + elapsed` and reproducible, but clients then rely on their measured clock offset.

## Differences from real Binance

Not implemented:

* RSA keys and more than one account. With `[sim.account] ed25519_public_key_file` (a `-----BEGIN PUBLIC KEY-----` PEM) the account key is Ed25519: signatures are verified with it, `session.logon`, `session.status`, `session.logout` and the unsigned `userDataStream.subscribe` work, and later requests on that connection may omit `apiKey` and `signature`. `session.logon` with an HMAC account answers `-4056`. Session revocation is not simulated.
* SBE market data and `responseFormat=sbe`: JSON only.
* Order types other than LIMIT / LIMIT_MAKER / MARKET: no stop, take-profit, iceberg, trailing, `quoteOrderQty`, OCO/OTO/OPO lists, SOR or pegged orders (`-1014`).
* Other market data: klines, aggTrades, 24 h tickers, avgPrice, `@depth<N>` partial books, `/api/v3/trades`. `@depth` and `@depth@100ms` share one interval, and bookTicker is batched per interval rather than sent on every change.
* The events `balanceUpdate`, `listStatus`, `externalLockUpdate` and `eventStreamTerminated`.
* Self-trade prevention is EXPIRE_MAKER only. Commission is always charged in the quote asset. There is no BNB discount.
* Rate limits are not per IP: one weight window for the whole server and one order-count window for the account. There are no 418 bans, no connection weight and no `X-MBX-ORDER-COUNT-*` on the WS API (counts are in `rateLimits`).
* The legacy listenKey stream is kept ([venues.md](venues.md#binance-spot)).
* The ack delay applies to WS API responses only. REST answers are synchronous.
* Order ids start at 1 for every run. Cancelled and filled orders are forgotten, so querying them answers `-2013`.

## Integration tests

`tests/integration` (label `integration`) runs `BinanceVenue` and the `fastmm-live` wiring against the in-process server over TCP and TLS; `FASTMM_IT_LOG=1` prints the connector and engine log.
