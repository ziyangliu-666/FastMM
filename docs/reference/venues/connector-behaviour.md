# Connector behaviour

Every venue connector follows the rules on this page; each venue's page lists only what differs. The venues and what each supports: [Venue connectors](../venues.md).

## Threads and rings

Each connector runs on its own network thread and writes normalised messages into two rings per venue. Orders and control commands come back on a third.

| Ring | When full |
|---|---|
| market data | drops the delta and forces a book resync |
| order events | spins for a bounded time, then the venue treats the overflow as fatal |

Behind [fastmm-gateway](../../how-to/operations/run-behind-a-gateway.md) the same code runs in the gateway, and the rings are shared memory the strategy process reads.

`cancel_all()` uses a blocking REST connection of its own, so it works from any thread while the network thread is wedged. `Venue::poll()` runs after every network-thread iteration.

## Order admission

| Situation | What is admitted |
|---|---|
| venue-fatal error or REST hard stop | cancels and cancel-all only, on any transport still usable |
| `uncork()` fails | every order of the batch is rejected with `TransportFull`, not counted as sent |
| shadow table full | the order or replace goes back as `OrderTableFull` |

The shadow table holds 7168 orders over every strategy behind the connector: the orders working at the venue and the replaces in flight. A full table logs one error, and one warning when it has room again; `VenueStatus::shadows_refused` counts the refusals. An untracked order could not be amended, and its replies would carry no instrument.

Requests are charged what their endpoint charges, per venue; the venue pages give the figures.

## Account pools

`pool_of = "<primary>"` in `[venues.<member>]` runs a second account of the same exchange behind one venue ([configuration](../configuration.md#account-pools), [strategy API](../strategy-api.md#account-pools)).

| The member has its own | The member uses the primary's |
|---|---|
| network thread, keys, order-count windows, budget publication | instruments and symbol ids |
| order and user sessions, start-up open orders and balances | market data: it opens no stream and holds no books |
| execution replay, `cancel_on_order_channel_loss`, kill-switch `cancel_all` | |

A member's status line reads `md=down books=0/0` while `order` and `user` are live. Every event a member emits carries its own venue id, so the engine keeps each order with its account. The engine sums the members' position reports into the instrument's position.

Binance Spot and Binance USDⓈ-M run as members; any other connector is refused at configuration time. Binance counts request weight per IP, so accounts leaving from one [source address](../configuration.md#source-address) share one weight window. A 429 pause or 418 stop answered to one account holds them all.

## Internal transfers

A pool treasury ([`[venues.<primary>.treasury]`](../configuration.md#venuestreasury)) moves an asset between a pool's accounts. The calls block on a REST connection of their own and run on the treasury's thread.

| Call | Answers |
|---|---|
| `Venue::transfer(TransferRequest)` | `Pending` or `Done` (taken), `Failed` (refused, nothing moved), `Unknown` (ask for the state) |
| `Venue::transfer_status` | the same, or `NotFound` |
| `Venue::transfer_account()` | the account's name in a request |
| `Venue::request_balances()` | asks for a balance snapshot on the network thread |

Binance Spot and USDⓈ-M support transfers, through the master account's universal transfer ([Binance Spot](binance-spot.md#internal-transfers)). Other connectors refuse them.

## Dead man's switch

`cancel_on_order_channel_loss` cancels over REST when the order channel drops, which needs a live process. What survives a SIGKILL, an OOM kill or a dead host is the venue's own switch.

| Venue | Kind | Setting | Default |
|---|---|---|---|
| [Binance USDⓈ-M](binance-usdm.md#dead-mans-switch) | countdown, per symbol | `dead_mans_switch_ms` | 60000 ms |
| [OKX](okx.md#dead-mans-switch) | countdown, whole account | `dead_mans_switch_s`, 10 to 120 | 60 s |
| [Gate](gate.md#dead-mans-switch) | countdown, whole account | `dead_mans_switch_s` | off |
| [Hyperliquid](hyperliquid.md#dead-mans-switch) | scheduled cancel | `dead_mans_switch_s`, at least 10 | off |
| [Lighter](lighter.md#dead-mans-switch) | scheduled cancel-all | `dead_mans_switch_s`, 310 s to 15 days | off |
| [Gemini](gemini.md#dead-mans-switch) | cancel on disconnect; heartbeat countdown | `cancel_on_disconnect`, `heartbeat` | on; heartbeat off |
| [Deribit](deribit.md#session) | cancel on disconnect, per connection | always on | on |
| [Bybit](bybit.md#dead-mans-switch) | cancel after all private connections drop | `dead_mans_switch_s`, 3 to 300 | off |
| Binance Spot, Coinbase, Alpaca | none | | |

A countdown can lapse while the process lives. When the venue refuses every refresh for a whole window, it has cancelled the orders. The connector then trips the venue kill with `DeadMansSwitchLost` instead of quoting again ([Kill switch and shutdown](../../how-to/operations/kill-switch-and-shutdown.md#what-trips-it)).

## Batch order entry

Only the Hyperliquid connector batches order entry; the others send single orders and coalesce an engine batch into one TCP write.

| Venue | Batch the venue offers | Why the connector does not use it |
|---|---|---|
| Binance Spot | none; order lists are contingency orders | |
| Binance USDⓈ-M | REST `batchOrders`, 5 orders | REST only: it would move quotes off the WebSocket API |
| Bybit | WebSocket `order.create-batch` and friends | asynchronous, half-accepted results, no rate-limit saving on spot |
| OKX | WebSocket `batch-orders`, 20 orders | per-order results like Bybit's; cancel-all uses REST `cancel-batch-orders` |
| Lighter | `sendtxbatch`, 15 transactions | each still counts against the limits; failure semantics undocumented |
| Gemini | none | cancel-all uses REST `order/cancel/session` |
| Coinbase | Advanced Trade `batch_cancel`; Exchange batches only on FIX | the kill switch uses `batch_cancel` |
| Deribit | `private/mass_quote` | needs approval and a second order lifecycle; bulk cancels serve the kill switch |
| Hyperliquid | one action, many orders | used: up to 40 orders an action |

## Reconciliation

A reconciliation asks the venue what the account executed, then what is open, and sends the result as one snapshot: `Begin`, `OpenOrder` per order, `Position` per instrument on derivatives, `End`.

| When | What |
|---|---|
| a private or order channel reconnects | a reconciliation |
| the engine sees more than three cancel rejects | `ControlCommand::Reconcile`; the connector calls `request_open_orders()` |
| the first private connect of a session | a sweep: empty watermark, finds only orders the engine does not know, which the engine cancels |

| Rule | Effect |
|---|---|
| a snapshot reaches the engine only when the whole reply parsed | a truncated reply is dropped, not sent as empty |
| `Oms::reconcile_end()` cancels every order the snapshot does not name | bounded by the `sent_watermark` in `Begin`: later orders are kept |
| an order dropped with quantity still working | counted in `EngineStats::unresolved_orders` and logged |
| one snapshot at a time | a request made meanwhile is served by one more straight after |
| a failed snapshot | asked again after 5 s; abandoned after 60 s without an answer |
| `disconnect()` has begun | nothing more is asked |

After each snapshot the connector drops the order shadows the snapshot proves over: an order answered before the request that the venue no longer holds lost its terminal event. `VenueStatus::shadows_swept` counts them. "Before" is by send sequence, so behind `fastmm-gateway` every strategy's shadows are judged.

Orders left by an earlier session carry an earlier session epoch in their client ids, so they cannot be confused with this session's.

A warm standby ([Hand over a running session](../../how-to/operations/hand-over-a-session.md#warm-standby)) calls `defer_private()` before `connect()`. Binance Spot then opens only public market data until `enable_private()`, and sends, cancels and queries nothing of the account. The other connectors refuse it, and the standby connects only once it holds the lock.

## Execution replay

A reconciliation first replays what the account executed, because an open-order snapshot cannot report the fill that finished an order. `Venue::request_executions(since_venue_ms)` runs the venue's trade-history query and sends each execution as an `OrderFillMsg` flagged `kReplayed`.

| Step | Detail |
|---|---|
| deduplication | `Oms::on_fill` drops an execution id, instrument and side it saw among the last 4096 |
| exact replay | `Begin` carries `ReconcileMsg::kExecutionsExact` when every stream answered in full |
| watermark | per stream; moves to the end of a complete read, never past the replay's start less 60 s (OKX 5 min) |
| start | at `connect()`; a restored session passes its watermark with `resume_executions()` ([Recovery at start-up](../storage.md#recovery-at-start-up)) |
| schedule | before each reconciliation, and once a minute |
| failure | the watermark stays; asked again 5 s later; a query held back by the rate limiter for 180 s fails |
| long ranges | walked in the venue's longest window; a start older than the venue keeps makes the replay not exact |

When a reconciliation was not preceded by an exact replay, the engine counts it in `EngineStats::estimated_reconciles`. A quantity no fill covered is then booked at the order's own price with no fee.

A fill first seen only as a larger cumulative quantity stays remembered with its order, for the last 4096 ended orders. When the replay later names it, it corrects the price and fee without adding quantity (`EngineStats::corrected_fills`). The PnL moves by `qty * (px - est_px)`; the average entry price keeps the estimate.

| Counter | Meaning |
|---|---|
| `pending=` on the `estimates` line of fastmm-top, `fastmm_estimates_pending` | orders whose estimate no execution has named yet |
| `forgotten=`, `fastmm_estimates_forgotten_total` | estimates whose order left the 4096 records; a later execution of it is booked again |
| `VenueStatus::execution_queries`, `executions_fetched`, `execution_query_errors` | the replays |

What each venue's history query offers:

| Venue | Query | Scope | Names the client order id |
|---|---|---|---|
| [Binance Spot](binance-spot.md#reconciliation-and-fills) | `GET /api/v3/myTrades` | symbol | no: looked up by `orderId` |
| [Binance USDⓈ-M](binance-usdm.md#reconciliation-and-fills) | `GET /fapi/v1/userTrades` | symbol | no: looked up by `orderId` |
| [Bybit](bybit.md#reconciliation-and-fills) | `GET /v5/execution/list` | account | `orderLinkId` |
| [OKX](okx.md#reconciliation-and-fills) | `GET /api/v5/trade/fills`, `fills-history` | account and instrument type | `clOrdId` |
| [Gate](gate.md#reconciliation-and-fills) | `GET /my_trades_timerange` | account | `text` |
| [Hyperliquid](hyperliquid.md#reconciliation-and-fills) | `userFillsByTime` | account | cloid |
| [Gemini](gemini.md#reconciliation-and-fills) | `POST /v1/mytrades` | symbol | `client_order_id` |
| [Coinbase Advanced Trade](coinbase.md#advanced-trade) | `GET .../orders/historical/fills` | account | no: looked up by `order_id` |
| [Coinbase Exchange](coinbase.md#exchange) | `GET /fills` | product | no: looked up by `order_id` |
| [Deribit](deribit.md#session) | `private/get_user_trades_by_currency_and_time` | currency | `label` |
| [Alpaca](alpaca.md#reconciliation-and-fills) | `GET /v2/account/activities/FILL` | account | no: looked up by order id |
| Lighter, Nasdaq ITCH | none | | |

## Balances

Every connector reports the account's balances as `BalanceMsg` events on the order ring ([Balance check](../../explanation/risk-model.md#balance-check)).

| Source | When |
|---|---|
| snapshot | after each reconciliation snapshot, as one `kSnapshot` ... `kSnapshotEnd` set; an asset it does not name holds nothing |
| stream | the private stream's balance updates, as single messages |
| refetch | after a fill, at most once a second, on a venue without a balance stream |

Only the assets the engine keeps are forwarded: the base and quote of spot instruments, the settlement asset of derivatives. A failed balance fetch is asked again after 5 s and does not hold up the order snapshot. Each row carries free, locked, total, equity and maintenance; the venue pages map the venue's fields.

Money that enters or leaves an account other than by trades is an `AccountFlow` event: Binance Spot `balanceUpdate`, and Binance USDⓈ-M `ACCOUNT_UPDATE` rows whose reason is not `ORDER` or `FUNDING_FEE`. The engine changes nothing for it; the journal keeps it for a backtest ([Journal format](../journal-format.md#messages)). Other connectors send none.

## Mark, index and funding

Connectors of derivatives send the venue's mark price, index price, funding and open interest as `PerpStateMsg` events on the market-data ring ([Perpetuals](../strategy-api.md#perpetuals)). The engine values positions at the mark ([Valuation](../../explanation/risk-model.md#valuation-at-the-venue-mark)).

| Field | Meaning |
|---|---|
| `fields` | what this message carries; a venue with separate channels sends one message per channel |
| `funding_rate` | the rate the venue applies at `next_funding`, per `funding_interval`; positive: longs pay |

| Venue | Mark | Index | Funding rate | Open interest |
|---|---|---|---|---|
| [Binance USDⓈ-M](binance-usdm.md#market-data) | yes | yes | yes | no |
| [OKX](okx.md#market-data) | yes | yes | yes | yes |
| [Bybit](bybit.md#linear-perpetuals) linear | yes | yes | yes | yes |
| [Deribit](deribit.md#market-data) | yes | yes | yes, per 8 h | yes |
| [Gemini](gemini.md#market-data) | yes | no | yes | no |
| spot venues | none | | | |

[Gate](gate.md#market-data) sends its `futures.tickers` channel as `PerpStateMsg`. Funding payments are `Funding` events; each venue page says where they come from.
