# fastmm-sim-exchange

`fastmm-sim-exchange` is a Binance Spot-compatible simulated exchange (plan 8.3,
[ADR-0008](adr/0008-sim-exchange-speaks-binance.md)). The unmodified Binance connector
(`fastmm::venues::binance::BinanceVenue`, see [venues.md](venues.md)) and `fastmm-live` run
against it on localhost: TCP or TLS, REST, market-data WebSockets, the WebSocket API, depth
sequence sync, HMAC authentication, and order flow with real fills. The simulator can also
inject faults.

```
fastmm-sim-exchange ── one net::Reactor thread
  ├─ HttpServer<PlainStream>              REST /api/v3/* + WebSocket upgrades   (:9080)
  ├─ HttpServer<TlsStream<PlainStream>>   the same over TLS (fixture cert)       (:9443)
  ├─ MatchingEngine        price-time, LIMIT_MAKER rejection, STP, per-symbol update ids
  ├─ MarketGenerator       counter-party flow (random-walk mid, Poisson limits/markets/cancels)
  ├─ MdAggregator          depthUpdate U/u batches every depth_update_ms, bookTicker
  └─ venue state           account balances, order index, listen keys, rate-limit windows
```

Code layout: `include/fastmm/sim/server/` holds the public headers.
`binance_json.hpp` builds the JSON bodies, streams and events. `request.hpp` parses query
strings, WS API frames and signatures (simdjson stays inside `request.cpp`).
`venue_state.hpp` holds accounts, orders and rate windows. `sim_server_config.hpp` and
`sim_exchange_server.hpp` define the configuration and the server. The implementation is in
`src/sim/server/`, the library target is `fastmm::sim_server`, and the app lives in
`apps/fastmm-sim-exchange/`.

## Running

```bash
./build/release/bin/fastmm-sim-exchange --config configs/sim.toml          # 127.0.0.1:9080 / :9443
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret \
  ./build/release/bin/fastmm-live --config configs/sim-local.toml           # or sim-local-tls.toml
./scripts/run-sim.sh --duration 30s [--tls] [--build-dir build/<dir>]      # both, plus a summary
docker compose up --build                                                   # configs/sim-docker.toml
```

| flag | meaning |
|---|---|
| `--config <toml>` | `[[instruments]]` + `[sim]` (default: built-in BTCUSDT) |
| `--bind <ip>` | listen address, default `127.0.0.1` (`0.0.0.0` in containers) |
| `--port <n>` / `--tls-port <n>` | override 9080 / 9443 (0 = ephemeral) |
| `--no-tls`, `--tls-cert <pem>`, `--tls-key <pem>` | TLS listener control |
| `--seed <n>` | generator seed |
| `--duration <t>` | stop after `t` (`60s`, `5m`, `1500ms`); default: until SIGINT/SIGTERM |
| `--stats-interval <t>` | periodic statistics line (default 5s, 0 = only at exit) |

Exit codes: 0 ok, 2 bad command line, 3 bad configuration, 4 cannot listen. Every
`--stats-interval` the simulator prints a line with connections, orders, rejects, cancels,
replaces, fills, open orders, public trades, depth diffs, tickers, snapshots, REST and WS API
requests, rate-limited requests, authentication errors, the account position and the touch.

`scripts/run-sim.sh` starts the simulator and waits until port 9080 accepts connections. It
then runs `fastmm-live --duration <t> --journal runs/<ts>/session.fmj --log
runs/<ts>/engine.log`, prints the engine, venue and simulator summaries, and stops the
simulator. The key and secret come from `FASTMM_SIM_API_KEY` / `FASTMM_SIM_API_SECRET`
(default `sim-key` / `sim-secret`). The engine configs reference these variables, and the
simulator reads the same variables, overriding `[sim.account]`.

## Configuration (`configs/sim.toml`)

The simulator reads `[[instruments]]` (symbol, base, quote, tick, lot, min_qty, max_qty,
min_notional) and `[sim]`. It declares `[venues.sim] kind = "sim"` only so that the
instruments validate.

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
| `account.balances.<ASSET>` | 100 base, 10,000,000 quote | starting balances |
| `fees.maker_bps`, `fees.taker_bps` | 1, 4 | commission, charged in the quote asset |
| `limits.weight_per_minute` | 6000 | REQUEST_WEIGHT / 1 MINUTE |
| `limits.orders_per_10s`, `limits.orders_per_day` | 1000, 1000000 | ORDERS limits (Binance: 50 / 10 s) |
| `limits.max_recv_window_ms` | 60000 | larger recvWindow answers -1131 |
| `generator.*` | see file | `MarketGeneratorParams` + `enabled`, `seed_levels` |
| `faults.*` | off | see Fault injection |

The default generator places its touch 2990 ticks (29.90 USDT) from a slowly moving latent
mid. `BasicMM`'s 5 bps quotes (30 USDT at 60000) therefore rest just behind the best generator
levels and are filled by the larger market orders. The book is wide on purpose: it is tuned
for the demo strategy, not for realism.

## What is implemented

**REST** (query string or form body; signed endpoints need `timestamp`, optional
`recvWindow`, hex HMAC-SHA256 `signature` over query + body, and the `X-MBX-APIKEY` header):

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

Every response carries `X-MBX-USED-WEIGHT-1M`. Order endpoints also carry
`X-MBX-ORDER-COUNT-10S` and `X-MBX-ORDER-COUNT-1D`. 429 responses carry `Retry-After`.

**Market-data WebSockets**: `/stream?streams=a/b/c` sends `{"stream","data"}`. `/ws/<stream>`
(several streams separated by `/`) and `/ws` send raw payloads, and both accept
`SUBSCRIBE` / `UNSUBSCRIBE` / `LIST_SUBSCRIPTIONS`. Streams: `<sym>@depth`, `<sym>@depth@100ms`
and `<sym>@depth@1000ms` all use the same `depth_update_ms` batches. `depthUpdate` has
`E,s,U,u,b,a`, and a `"0"` quantity deletes a level. `<sym>@bookTicker` (`u,s,b,B,a,A`) is sent
when the touch changed during a batch. `<sym>@trade` (`E,s,t,p,q,T,m,M`) is sent immediately.
`U`/`u` are the matching engine's per-symbol update ids, so the stream is contiguous and a
REST snapshot's `lastUpdateId` is consistent with it.

**WebSocket API** (`/ws-api/v3`): requests are `{"id","method","params"}` and the id is echoed
verbatim. Responses are `{"id","status","result"|"error","rateLimits"}`. Signed methods verify
HMAC over the alphabetically sorted params (including `apiKey`). Methods: `ping`, `time`,
`exchangeInfo`, `depth`, `ticker.book`, `userDataStream.subscribe.signature` (returns
`{"subscriptionId":N}`), `userDataStream.unsubscribe`, `order.place`, `order.test`,
`order.cancel`, `order.cancelReplace`, `order.amend.keepPriority`, `order.status`,
`openOrders.status`, `openOrders.cancelAll` and `account.status`. A 429 error carries
`data.retryAfter`.

**User data events** go to WS API connections with a user-data subscription as
`{"subscriptionId":N,"event":{...}}`, and raw to `/ws/<listenKey>` connections:

* `executionReport` with the full field set. `x` is `NEW`, `CANCELED` (`c` = cancel request
  id, `C` = cancelled order id), `REPLACED` (amend), `REJECTED` (post-acceptance), `TRADE`
  (`l,L,n,N,t,m`), `EXPIRED` (IOC/FOK/MARKET remainder) or `TRADE_PREVENTION` (self-trade,
  EXPIRE_MAKER).
* `outboundAccountPosition` after every balance change. Events from one request are sent after
  its response.
* `listenKeyExpired`.

**Trading model**: LIMIT / LIMIT_MAKER orders lock quote notional (buys) or base quantity
(sells). A fill releases the lock and moves balances, with commission in the quote asset. The
simulator validates filters (`-1013 Filter failure: PRICE_FILTER / LOT_SIZE / NOTIONAL /
MAX_NUM_ORDERS`), duplicate client ids (`-2010 Duplicate order sent.`) and insufficient balance
(`-2010`). A crossing LIMIT_MAKER answers `-2010 Order would immediately match and take.`.

**Authentication and limits**:

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

The server also sends WebSocket pings every `ping_interval_ms` and closes connections that
have not answered for `pong_timeout_ms`.

## Fault injection

The `[sim.faults]` settings in the table below are one-shot and count from the moment the
listeners open. The programmatic API (`SimExchangeServer`, thread-safe) is used by the
integration tests.

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

`stats()` returns counters for all of the above, plus watermarks since `mark()`: minimum
open orders, cancels, cancel-alls, open-order queries and reconnects. Tests use them to
assert, for example, that quotes were pulled while the market data was down.

## Determinism

The generator, order ids and trade ids depend only on the configuration and seed and on the
order of requests. The generated book is identical for identical seeds (a test checks this).
Event times inside the matching engine come from the reactor clock. Published timestamps
(`E`, `T`, `transactTime`, `serverTime`) and recvWindow checks follow the host wall clock
plus `clock_offset_ms`, as a real venue does. This matters on hosts whose wall clock steps
(WSL2 steps it by more than a second regularly). With `start_time_ms` set, published times
are `start_time + elapsed` and fully reproducible, but clients then rely on their measured
clock offset.

## Differences from real Binance

Deliberately not implemented:

* Ed25519 / RSA keys and `session.logon`: HMAC only, one account. `session.logon` and the
  unsigned `userDataStream.subscribe` answer an error.
* Order types other than LIMIT / LIMIT_MAKER / MARKET: no stop, take-profit, iceberg,
  trailing, `quoteOrderQty`, OCO/OTO/OPO lists, SOR or pegged orders (`-1014`).
* Other market data: klines, aggTrades, 24 h tickers, avgPrice, `@depth<N>` partial books,
  `/api/v3/trades`. `@depth` and `@depth@100ms` share one interval, and bookTicker is batched
  per interval rather than sent on every change.
* The events `balanceUpdate`, `listStatus`, `externalLockUpdate` and `eventStreamTerminated`.
* Self-trade prevention is EXPIRE_MAKER only. Commission is always charged in the quote
  asset. There is no BNB discount.
* Rate limits are not per IP: one weight window for the whole server and one order-count window
  for the account. There are no 418 bans, no connection weight and no `X-MBX-ORDER-COUNT-*`
  on the WS API (counts are in `rateLimits`).
* The legacy listenKey stream is still available here, although Binance removed it on
  2026-02-20.
* The ack delay applies to WS API responses only. REST answers are synchronous.
* Order ids start at 1 for every run. Cancelled and filled orders are forgotten, so querying
  them answers `-2013`.
* The generated book is a wide, synthetic ladder (see Configuration).

## Integration tests (`tests/integration`, label `integration`)

`conformance_test.cpp` drives the real `BinanceVenue` against the in-process server on
ephemeral ports:

* reference data, snapshot plus contiguous diffs, bookTicker and trade;
* depth sync from a live mid-batch snapshot;
* the WS API user stream and the order lifecycle: place, cancel, cancelReplace, a crossing
  LIMIT_MAKER reject, a taker fill, a maker fill against generator flow, openOrders
  reconciliation and the kill-switch `cancel_all`;
* the documented REST errors;
* connector reactions to -1022, -1021, rejects and delayed acks;
* seed determinism.

`e2e_test.cpp` runs the `fastmm-live` wiring
(`Engine<BasicMM, TscClock, LiveTransport, RingFeed>` on its own thread, the venue on a reactor
thread, configs `sim-local.toml` / `sim-local-tls.toml`) over TCP and over TLS. It checks
fills, risk limits, a clean shutdown and that no file descriptors leak. It also covers depth
gap resync, a market-data drop and an order-channel loss. Set `FASTMM_IT_LOG=1` to see the
connector and engine log.

The test servers use `depth_snapshot = flushed`, so the first sync never depends on where a
REST snapshot falls inside a batch. A dedicated conformance case runs with live snapshots and
500 ms batches and requires a clean start without a resync. That needs the BookSyncer
first-delta fix (commit 687a8ae). The e2e cases report two known client-side behaviours as
doctest warnings instead of failures: no openOrders reconciliation after an order-channel
reconnect, and slow quote resumption after a reconnect.
