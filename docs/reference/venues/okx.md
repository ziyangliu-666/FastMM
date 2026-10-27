# OKX

`kind = "okx"` trades OKX v5 USDT-margined perpetual swaps and spot pairs over the WebSocket, in one instance or apart.

| | |
|---|---|
| Markets | linear swaps (`BTC-USDT-SWAP`) in net position mode; spot (`BTC-USDT`) |
| Order entry | WebSocket `order`, `amend-order`, `cancel-order`; REST fallback |
| Environments | production, OKX demo trading (`testnet = true`, the default) |
| Keys | HMAC, with `api_key`, `api_secret` and `api_passphrase` |
| Replace | amend in place; the shipped configs use cancel and new |
| Dead man's switch | `cancel-all-after` countdown, 60 s by default |
| Example config | `configs/okx-demo.toml` (swaps), `configs/okx-spot-demo.toml` (spot) |

An instrument id ending in `-SWAP` is a swap, any other a spot pair.

## Hosts

Keys work only on the domain of the site the account was registered on. `region` sets `ws_url` and `rest_url` when the section leaves them empty.

| `region` (site) | REST | WebSocket | WebSocket, demo |
|---|---|---|---|
| `global`, the default (www.okx.com) | `openapi.okx.com` | `ws.okx.com:8443` | `wspap.okx.com:8443` |
| `eea`, `my` (my.okx.com) | `eea.okx.com` | `wseea.okx.com:8443` | `wseeapap.okx.com:8443` |
| `us`, `app` (app.okx.com) | `us.okx.com` | `wsus.okx.com:8443` | `wsuspap.okx.com:8443` |

REST hosts are `https://`, WebSocket hosts `wss://`.

`www.okx.com` and `my.okx.com` also answer REST for their regions. Demo trading uses the production REST host with the header `x-simulated-trading: 1`. A my.okx.com demo key is refused by `www.okx.com` and `openapi.okx.com` with 401, 50119 "API key doesn't exist".

## Connections

| channel | endpoint | purpose |
|---|---|---|
| md | `ws_url` (`/ws/v5/public`) | `books` (or `books50-l2-tbt` / `books-l2-tbt`, which need VIP4 and a login), `bbo-tbt` as BookTicker, `trades` |
| private | `ws_private_url`, default the `ws_url` host + `/ws/v5/private` | `op: login`, then `orders`, `positions` (events only), `balance_and_position` |
| order | `ws_api_url`, default the private URL on a second connection | `op: login`, then `order`, `amend-order`, `cancel-order` |
| rest | `rest_url` | the requests below |

| request | use | documented limit |
|---|---|---|
| `GET /api/v5/public/instruments` | reference data, `instIdCode` | 20 / 2 s per IP and instType |
| `GET /api/v5/public/time` | clock offset (30 s tolerance) | 10 / 2 s per IP |
| `GET /api/v5/account/config` | position and account mode, at start-up | 5 / 2 s per user |
| `GET /api/v5/trade/orders-pending` | reconciliation, cancel-all; one `instType` a request | 60 / 2 s per user |
| `GET /api/v5/account/positions?instType=SWAP` | reconciliation, swaps only | 10 / 2 s per user |
| `GET /api/v5/trade/fills`, `fills-history` | fill replay: 3 days, 3 months | 60 / 2 s, 10 / 2 s per user |
| `GET /api/v5/account/bills`, `bills-archive` (`type=8`) | funding: 7 days, 3 months | 5 / s, 5 / 2 s per user |
| `POST /api/v5/trade/cancel-batch-orders` | kill switch, order-channel loss, 20 a request | 300 orders / 2 s per user and instrument |
| `POST /api/v5/trade/cancel-all-after` | dead man's switch | 1 / s per user |
| WebSocket `order`, `amend-order`, `cancel-order` | order entry | 60 / 2 s each per user and instrument; 1000 new + amend / 2 s per sub-account |

## Keys and signing

| where | signature |
|---|---|
| REST | `OK-ACCESS-SIGN` = base64 HMAC-SHA256(secret, ISO timestamp + method + path with query + body), with `OK-ACCESS-KEY`, `OK-ACCESS-TIMESTAMP`, `OK-ACCESS-PASSPHRASE` |
| WebSocket `login` | the same HMAC over Unix seconds + `GET/users/self/verify` |

With `testnet = true` every REST request carries `x-simulated-trading: 1`, public ones included, because demo instruments differ. On the demo, BTC-USDT-SWAP has tick 0.01 against 0.1 on production. A demo host with `testnet = false`, or the reverse, is refused. The passphrase comes from `${FASTMM_OKX_API_PASSPHRASE}`.

## Instruments and account

OKX sizes swaps in contracts, and so does FastMM: book, trade, order, fill and position quantities are contracts. Notional is price × contracts × multiplier, in USDT.

| field | swap | spot |
|---|---|---|
| quantity | contracts | base coin (`baseCcy`) |
| `contract_multiplier` | `ctVal` × `ctMult` (0.01 BTC for BTC-USDT-SWAP) | 1 |
| base, quote | `ctValCcy`, `settleCcy` | `baseCcy`, `quoteCcy` |
| tick, lot, minimum | `tickSz`, `lotSz`, `minSz`; maximum `maxLmtSz`; no minimum notional | same; `minSz` the floor |
| accepted | linear SWAP; a `state` other than `live` or `post_only` disables it | every live pair |

[Accounting](../configuration.md#accounting) settles a swap in `settleCcy`. With keys and swaps configured, `load_reference_data()` reads `account/config`. It refuses `posMode` `long_short_mode` and `acctLv` 1, since spot mode trades no swaps. A spot-only instance takes every account mode.

A key the venue refuses makes `fastmm-live` and `fastmm-gateway` exit 3. A `long` or `short` position seen later is fatal for the venue. `td_mode` (cross by default, or isolated) goes on every swap order; leverage and margin are not read or set.

## Spot

Spot orders go with `tdMode` `cash`, which every account mode takes, and never `reduceOnly`. A market order sets `tgtCcy` `base_ccy`, since a market buy is otherwise sized in the quote coin. The private stream subscribes `orders` only.

OKX has no position for a spot pair: the account holds balances, of which the strategy's inventory is a part. As on Binance Spot, the engine's position is the strategy's, zero at start or restored from the store ([Recovery at start-up](../storage.md#recovery-at-start-up)). Reconciliation reports no position for a spot pair, so the engine's stands.

A buy pays its commission in the base coin, which the engine takes off what the fill adds; a sell pays in the quote coin.

## Market data

`books` sends a 400-level snapshot (`prevSeqId` -1), then updates every 100 ms. An update applies when its `prevSeqId` equals the last `seqId`, which covers the documented heartbeat and the reset after maintenance. Anything else emits `Resyncing` and re-subscribes the channel for a new snapshot.

An update can change more levels than the book holds, up to 800 asks on BTC-USDT-SWAP. Past 512 a side the parser keeps the levels nearest the touch. The CRC-32 `checksum` was deprecated on 2026-06-23 and is 0; a non-zero checksum is still checked, and a mismatch resyncs.

| key | default |
|---|---|
| `stale_ms` | 2000 ms |
| `dead_ms` | twice `ping_interval_ms` plus 5000 ms; 45000 ms by default |

### Mark, index and funding rate

Swaps subscribe four public channels in their own subscribe request ([Mark, index and funding](connector-behaviour.md#mark-index-and-funding)).

| channel | carries | rate |
|---|---|---|
| `mark-price` | `markPx` | 200 ms on change, else every 10 s |
| `index-tickers` | `idxPx` of the swap's index (`BTC-USDT` for `BTC-USDT-SWAP`) | at least once a minute |
| `funding-rate` | `fundingRate` for the next settlement at `fundingTime`; interval `nextFundingTime - fundingTime` (8 h, some 4 h) | every 30 to 90 s |
| `open-interest` | `oi`, contracts | every 5 to 10 s |

## Orders

| field | value |
|---|---|
| instrument | `instIdCode` on the WebSocket; REST takes `instId`, and an instrument without an `instIdCode` sends its orders over REST |
| `ordType` | `limit`, `post_only`, `ioc`, `fok`, `market` |
| `reduceOnly` | when the engine sets it, swaps only |
| `posSide` | never sent |
| request `id` | `<kind><id>`, 15 characters |
| `clOrdId` | FastMM's 14-character client id |

A post-only order that would take is accepted and then pushed `canceled` with `cancelSource` 31. It becomes `OrderExpired`, as do IOC and FOK remainders (13, 14). Other cancels are cancel acks.

### Amend

`newSz` is the total including the filled quantity, as the engine's replace quantity is. `sCode` 0 means accepted, not applied. The `orders` push with `amendResult` under the `reqId` is the ack, kept in place so fills stay, or the reject.

Later events for the original `clOrdId` are translated to the new id. The config uses cancel and new until amend has been seen on the demo.

### Private stream

`orders` pushes carry the state and the fill: `tradeId`, `fillPx`, `fillSz`, `fillFee`, `fillFeeCcy` and `execType` M/T. `fillFee` is negative when charged, so the fee is booked with the sign turned. It often has more than 8 decimals and is rounded half away from zero to 8.

The channel may repeat a message. The OMS drops a repeated `tradeId`, and a repeated cancel or amend result finds nothing pending. `positions` is compared with the fills the connector forwarded and corrects the engine only when they differ (`position_from_stream`), as on Bybit.

## Reconciliation and fills

A reconciliation runs the fill replay, then `orders-pending` (paged by `ordId`) and `account/positions`. They become one Begin, OpenOrder*, Position*, End sequence; an instrument absent from the positions is flat.

| aspect | fill replay |
|---|---|
| query | `GET /api/v5/trade/fills` (3 days); `fills-history` (3 months) for an older watermark |
| scope | one stream per `instType` traded: SWAP, SPOT |
| window | `begin` / `end` in ms on `ts`, paged newest first with `after` = `billId`, 100 a page |
| watermark | asks from 1 ms before the newest `ts` forwarded, drops rows below it and skips ids already forwarded |
| quiet replay | moves the watermark to 5 minutes before the query |
| long windows | more than 20 pages: read again up to the oldest row |
| identity | `tradeId` per instrument, `clOrdId`, `billId` |

[Execution replay](connector-behaviour.md#execution-replay) covers the shared rules.

## Balances

`GET /api/v5/account/balance` is the snapshot, the `account` channel the stream, and `acctLv` from `account/config` the mode ([Balances](connector-behaviour.md#balances)).

| mode | free / locked / total / equity / maintenance |
|---|---|
| spot | `availBal` / `frozenBal` / sum / sum / 0 |
| futures | `availEq` / `frozenBal` / `cashBal` / `eq` / `mmr` |
| multi-currency, portfolio margin | per currency `availBal` / `frozenBal` / `cashBal` / `eq` / 0; an account row in USD `availEq - imr` / `imr` / `totalEq` / `adjEq` / `mmr` |

The venue time is the account's `uTime`, or the currency's `uTime` on a push.

## Funding

`account/bills` with `type=8` is the source: subType 173 is an expense, 174 income. Each bill on a subscribed instrument becomes one `Funding` event, `balChg` in `ccy` with positive received, and `billId` as its id.

The query runs with every reconciliation's fill replay and once a minute. It also runs a second after a `balance_and_position` push with `eventType` `funding_fee`, which names no bill.

## Dead man's switch

`cancel-all-after` with `timeOut` = `dead_mans_switch_s` is a countdown refreshed every window/3 from the housekeeping timer ([Dead man's switch](connector-behaviour.md#dead-mans-switch)).

| aspect | behaviour |
|---|---|
| window | `dead_mans_switch_s`, default 60, clamped to [10, 120] |
| scope | every pending order of the account, spot included, not only FastMM's |
| shutdown | `disconnect()` sends `"0"` on its own blocking connection |
| lapse | trips `DeadMansSwitchLost` |

## Rate limits

Client-side `orders_per_second` (default 25) applies to new orders and amends. A 50011 or 50061 reply pauses sending for 2 s, since OKX counts over 2 s windows.

## Errors

| codes | connector action |
|---|---|
| 50011, 50061, 60014 | rate limit |
| 50102, 50112, 60004, 60006, 50036 | clock resync |
| 501xx, 60005, 60007, 60009, 60024, 50007, 50009, 50027, 50029, 51024, 51010, 59113 | fatal: authentication or account block |
| 50121 | hard stop |
| 51400, 51503, 51603, 50004 | reconcile |
| 51008 | insufficient balance |
| 51121, 51020 | lot |
| 51006, 51137 | price limit |
| 51016 | duplicate client id |
| 51511 | post-only amend |

## Not handled

- `books-rpi`, `trades-all` (`trades` aggregates per taker order and price), SBE market data.
- `expTime` and batch operations ([Batch order entry](connector-behaviour.md#batch-order-entry)).
- Leverage and margin settings.

## Unconfirmed

Spot has run on the demo of a spot-mode my.okx.com account, covering login, orders, fills, cancel-all and the countdown. Swaps are implemented from the documentation only.

| topic | open question |
|---|---|
| swaps | the `positions` and `balance_and_position` payloads, amend replies, bills; whether a position closed to zero is pushed with `pos` 0 |
| fill replay | whether `fills` `begin` is inclusive |
| WebSocket errors | whether a rate-limit error comes as `code` or `sCode` |
| depth | the fee tier the demo needs for the tbt depth channels |

## Sources

Read 2026-09-26: [OKX API v5](https://www.okx.com/docs-v5/en/) and its [changelog](https://www.okx.com/docs-v5/log_en/); the regional pages at [my.okx.com](https://my.okx.com/docs-v5/en/) and [app.okx.com](https://app.okx.com/docs-v5/en/).
