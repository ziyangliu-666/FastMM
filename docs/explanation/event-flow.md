# Event flow

One market-data update in `fastmm-live`, from the venue's socket to an order on the wire and its fill back into the strategy. Backtests and replay run the same engine with a simulated clock, feed and transport ([Determinism](determinism.md)); threads and rings: [Architecture](architecture.md).

```text
 network thread (per venue)                  engine thread                                           network thread
 ───────────────────────────                 ─────────────                                           ──────────────
 socket ─ TLS ─ WebSocket ─ parser ─ book sync ─► md ring ─► journal ─► book ─► risk marks ─► on_book ─► quote manager ─► risk check ─► OMS ─► outbound ring ─► encoder ─ signer ─ socket
                                                                                                                                                              │
 socket ─ TLS ─ WebSocket ─ user-stream parser ──────────► order ring ─► journal ─► OMS ─► position ─► on_fill ─► on_order_update ◄──── venue ack / fill ◄──────┘

 multicast (nasdaq_itch): UDP A/B ─ MoldUDP64 ─ ITCH ─ L3 book ─► md ring ─► ...                                              ... outbound ring ─► OUCH encoder ─ socket
```

## 1. Receive and normalise (network thread)

Each venue has a network thread running a `net::Reactor` (epoll or io_uring). It reads the socket, decrypts TLS, unframes WebSocket messages and parses the venue's JSON with simdjson into FastMM's fixed-size messages: `BookDeltaMsg` (a snapshot or a delta), `TradeMsg`, `BookTickerMsg` on the market-data connection; `OrderAckMsg`, `OrderFillMsg` and the other order events on the user stream. The receive time is stamped (T0) before parsing and the decode time after (T1).

The book-sync state machine aligns the REST snapshot with the delta stream using the venue's sequence numbers. A gap puts the book into `Resyncing`, sends a `ConnectionStateMsg` and fetches a new snapshot; a connection without traffic for `stale_ms` is reported `Stale`.

A multicast venue (`nasdaq_itch`) has no TLS or JSON. The network thread takes a batch of UDP datagrams from the kernel (`recvmmsg`) or from an AF_XDP RX ring and stamps T0 once per batch, together with the wall clock; `recv_ts` is the kernel's receive time of the datagram, so the kernel-to-T0 time is measured too. The MoldUDP64 receiver takes the first copy of each packet from line A or B, holds packets that arrive ahead of a gap and re-requests missing ones; a message delivered later keeps its datagram's T0. Each ITCH message updates the instrument's L3 book (order by order), and after each datagram the changed top levels go out as one `BookDeltaMsg` per instrument; T1 is taken after the L3 update. A gap that cannot be recovered, or an inconsistent L3 book, puts the venue into `Resyncing` until a GLIMPSE snapshot has rebuilt the books ([Venue connectors](../reference/venues.md#startup-and-recovery)).

## 2. Hand over (rings)

Messages cross to the engine through single-producer, single-consumer rings, two per venue (market data and order events) plus the control ring. Behind `fastmm-gateway` the venue rings are `ShmRing`s in shared memory. A full market-data ring drops the delta and forces a resync; order events are never dropped, and an order ring overflow shuts the session down. The engine polls the rings round-robin, at most 64 messages per ring per visit and `[engine] max_events_per_step` (default 64) per step, then runs due timers.

## 3. Consume (engine thread)

For each event the engine reads its clock once (the engine clock, `ctx.now()`), records the event in the journal with that time, and dispatches on the message type.

## 4. Market data

For a book message the engine:

1. applies it to the instrument's L2 book (T2);
2. if the book is valid, hands the mid to risk (price collar and staleness), marks the position at the mid and checks `max_loss`; if the book stays crossed longer than `crossed_grace_ms`, pulls the instrument's quotes;
3. calls `on_book(ctx, id, book)`.

A trade updates the fat-finger band and calls `on_trade`; a book ticker calls `on_book_ticker`. Messages for instruments outside the table reach no hook.

## 5. Decide (strategy)

The hook reads what it needs through `ctx` (book, position, working orders, parameters) and usually calls `ctx.set_quotes(id, quotes)` with up to 8 levels per side. The call returns at once (T3); nothing has been sent yet.

## 6. From desired quotes to orders (quote manager)

The quote manager compares each desired level with the order in the same slot:

- **keep** it when the price is within `min_requote_ticks` and the remaining quantity covers `min_qty_bps` of the desired one, or when the slot changed less than `min_requote_interval_ms` ago;
- **replace** it when the venue and `supports_replace` allow;
- otherwise **cancel** it and send a **new** order.

Orders waiting for a venue response are never touched until the response arrives, and a side that the venue rejected recently is paused (`reject_backoff_ms`). Direct orders from `ctx.send` skip this step.

## 7. Check and record (risk, OMS)

Each new order and replace passes the pre-trade checks ([Risk model](risk-model.md)). A passing order enters the OMS as `PendingNew` with a client order id (session epoch and sequence), and its `OutNewOrderMsg`, `OutCancelMsg` or `OutReplaceMsg` goes into the event's outbound batch.

## 8. Send (transport, network thread)

When the event is fully handled, the engine hands the batch to the transport (T4 to T5): live, a push into the venue's outbound ring and a wake-up of the network thread; in a backtest, the simulated venue's order queue. The journal records a copy of each message sent. The network thread encodes the order in the venue's format, signs it and writes it to the WebSocket API or REST connection (OUCH over SoupBinTCP for `nasdaq_itch` with `order_entry = "sim_ouch"`), and records the wire tick-to-trade latency from T0.

## 9. The response

The venue's acknowledgement or fill arrives on the user stream and takes steps 1 to 3 through the order ring. The OMS applies the state transition (for example `PendingNew` to `Live`, or a partial fill) and the quote manager updates the slot. For a fill, the engine first books the position and fees, checks `max_loss`, and then calls `on_fill(ctx, fill)`; `on_order_update(ctx, u)` follows for the same execution.

## 10. Everything else

- **Timers** fire from a timer wheel in engine time, between events; each firing is journaled as a synthetic `Timer` message and calls `on_timer`.
- **Connection changes**: for any state other than `Live` the engine pulls the venue's quotes and, for market data, clears its books, then calls `on_connection`. After an order-channel reconnect the connector replays the executions since the last one booked, then sends a reconciliation (Begin, open orders, positions where the venue has them, End) that aligns the OMS with the venue; quotes are paused during it and restored at the end. Every connector with order entry also replays executions once a minute, which books a fill the stream dropped without a disconnect.
- **Funding** payments of perpetuals arrive on the order ring as `Funding` events and are booked as realized PnL of the instrument, once per venue id; `max_loss` is checked at once. There is no strategy hook.
- **Quoting changes**: after each event or timer, if `ctx.quoting_enabled()` changed, the engine calls `on_quoting`.
- **Shutdown**: the control thread requests the kill switch (quotes pulled, orders cancelled through the engine) and independently cancels all orders on each venue over REST, then stops the threads ([Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md)).
