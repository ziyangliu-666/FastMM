# Event flow

One market-data update in `fastmm-live` travels from the venue's socket to an order on the wire, and its fill comes back into the strategy. Backtests and replays run the same engine with a simulated clock, feed and transport ([Determinism](determinism.md)); threads and rings are in [Architecture](architecture.md).

## 1. Receive and decode

The network thread reads the socket, decrypts TLS, unframes WebSocket messages and parses the venue's JSON into fixed-size messages. It stamps the receive time (T0) before parsing and the decode time (T1) after.

| Connection | Messages |
|---|---|
| market data | `BookDeltaMsg` (snapshot or delta), `TradeMsg`, `BookTickerMsg` |
| user stream | `OrderAckMsg`, `OrderFillMsg` and the other order events |

The book-sync state machine aligns the REST snapshot with the delta stream by the venue's sequence numbers. A gap puts the book into `Resyncing` and fetches a new snapshot; a connection silent for `stale_ms` is reported `Stale`.

### Multicast venues

`nasdaq_itch` has no TLS or JSON; the receive side is UDP lines A and B, MoldUDP64, ITCH and an L3 book, and orders go out through OUCH.

1. The network thread takes a batch of datagrams from the kernel or an AF_XDP ring and stamps T0 once per batch. The kernel's receive time is recorded too.
2. MoldUDP64 takes the first copy of each packet from line A or B, holds packets ahead of a gap and re-requests the missing ones.
3. Each ITCH message updates the instrument's L3 book; after each datagram the changed top levels go out as one `BookDeltaMsg` per instrument (T1).
4. A gap that cannot be recovered puts the venue into `Resyncing` until a GLIMPSE snapshot rebuilds the books ([Venue connectors](../reference/venues/nasdaq-itch.md#startup-and-recovery)).

## 2. Hand over

Messages cross to the engine through single-producer, single-consumer rings: market data and order events per venue, plus the control ring. The engine visits them round-robin, at most `[engine] max_events_per_step` (default 64) per ring, then runs due timers.

| Ring full | Result |
|---|---|
| market data | the delta is dropped and the book resyncs |
| order events | the kill switch ends the session; order events are never dropped |

## 3. Consume

For each event the engine reads its clock once, which is `ctx.now()` for the hooks. It records the event in the journal with that time, then dispatches on its type.

## 4. Market data

For a book message the engine:

1. applies it to the instrument's L2 book (T2);
2. if the book is valid, marks the position at the mid, updates the price collar and checks `max_loss`;
3. pulls the instrument's quotes if the book stays crossed longer than `crossed_grace_ms`;
4. calls `on_book(ctx, id, book)`.

A trade updates the fat-finger band and calls `on_trade`; a book ticker calls `on_book_ticker`. Messages for instruments outside the table reach no hook.

## 5. Decide

The hook reads what it needs through `ctx` and usually calls `ctx.set_quotes(id, quotes)` with up to 8 levels per side. The call returns at once (T3); nothing has been sent yet.

## 6. From quotes to orders

The quote manager compares each wanted level with the order resting in the same slot:

| Decision | When |
|---|---|
| keep | the price is within `min_requote_ticks` and the remaining quantity covers `min_qty_bps` of the wanted one, or the slot changed less than `min_requote_interval_ms` ago |
| replace | the venue supports it and `supports_replace` is on |
| cancel, then new | otherwise; the new order goes out once the cancel is acknowledged, and is withheld if the old order filled instead and the balance no longer covers it |

An order waiting for a venue response is left alone until it arrives. A side whose new order or replace the venue refused pauses for `reject_backoff_ms`, then quotes again ([Venue-originated rejects](../reference/errors.md#venue-originated)). Orders sent directly with `ctx.send` skip this step.

## 7. Check and record

Each new order and replace passes the pre-trade checks ([Risk model](risk-model.md)). A passing order enters the OMS as `PendingNew` with a client order id made of the session epoch and a sequence number, and joins the event's outbound batch.

## 8. Send

When the event is handled, the engine hands the batch over (T4 to T5). Live, that is a push to the venue's outbound ring and a wake-up of the network thread; in a backtest, the simulated venue's queue. The journal keeps a copy of each message.

The network thread encodes the order in the venue's format, signs it and writes it to the WebSocket API or REST connection. It records the wire tick-to-trade latency from T0.

## 9. The response

The acknowledgement or fill comes back on the user stream and takes steps 1 to 3 through the order ring. The OMS applies the transition, for example `PendingNew` to `Live`, and the quote manager updates the slot. For a fill, the engine books the position and fees, checks `max_loss`, calls `on_fill(ctx, fill)` and then `on_order_update(ctx, u)`.

## 10. Everything else

| Event | What the engine does |
|---|---|
| timer | fires from a timer wheel in engine time, between events; journaled as a `Timer` message, then `on_timer` |
| connection change | for any state but `Live`, pulls the venue's quotes, clears its books for market data, then calls `on_connection` |
| order channel reconnect | the connector replays executions since the last one booked, then reconciles open orders and positions; quotes pause until it ends |
| funding payment | booked as realized PnL of the perpetual and checked against `max_loss`; no hook |
| quoting switched on or off | `on_quoting`, after the event or timer that changed it |
| shutdown | kill switch through the engine, then cancel-all on each venue over REST ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md)) |

Every connector with order entry also replays executions once a minute, which books a fill the stream dropped without a disconnect.
