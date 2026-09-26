# How FastMM works

One page on the thread model, the path an event takes and what the engine does and does not guarantee. [Architecture](architecture.md) and [Event flow](event-flow.md) give the detail.

## One engine thread owns the trading state

All trading state (books, orders, positions, risk limits, timers) lives on one thread, `fm-engine`, and nothing else reads or writes it. The decision path takes no locks, and a session is reproducible from its recording.

Everything else is a producer or consumer around that thread:

| Thread | Does | Talks to the engine through |
|---|---|---|
| `fm-net-<i>`, one per venue | sockets, TLS, WebSocket or MoldUDP64 framing, JSON or binary decoding, book sync, order encoding and signing | two SPSC rings in (market data, order events), one out (outbound orders) |
| `fm-engine` | applies events, runs the strategy, diffs quotes, checks risk, updates the OMS | — |
| journal writer | appends the journal ring to the `.fmj` file | the journal ring |
| `fm-store` | writes fills, orders, positions, kills and funding to the store | the record ring |
| log sink | formats and writes log records | a ring per thread |
| main | config, reference data, start and stop, the control socket, the status file, the per-second stats line, TSC recalibration, SIGINT/SIGTERM, `cancel_all` over REST | the control ring, and the venues directly |

Every queue is single-producer, single-consumer. The engine polls the rings round-robin, at most 64 messages per ring per visit, so one busy venue cannot starve another.

With `[engine] threading = "single"` (one venue only) the network thread disappears and the engine thread runs the reactor itself, so no event crosses a core between the packet read and the order write. With `fastmm-live --gateway` the network threads run in `fastmm-gateway` instead, and the rings are in shared memory.

## The path of one event

1. The network thread reads the socket, decodes the venue's format into a fixed-size message and stamps the receive time (T0) and the decode time (T1).
2. It pushes the message into the venue's market-data or order-event ring.
3. The engine takes it, reads its clock once, and writes the event into the journal ring before anything acts on it.
4. It applies the message: a book delta updates the L2 book, marks the position at the new mid and re-checks `[risk] max_loss`; a fill books the position and fees first.
5. It calls the strategy hook (`on_book`, `on_fill`, `on_trade`, `on_timer`, ...). The hook usually calls `ctx.set_quotes`, which only records what the strategy wants.
6. The quote manager diffs the desired ladder against the resting orders and produces new, cancel and replace messages.
7. Each new order and replace passes the pre-trade risk checks ([Risk model](risk-model.md)). A refused order is never sent.
8. Surviving messages are recorded in the journal and pushed into the venue's outbound ring; the network thread encodes, signs and writes them.

The venue's acknowledgement or fill comes back on the order-event ring and re-enters at step 3.

## What the engine guarantees

| Guarantee | How it holds | Where it stops |
|---|---|---|
| A refused order is never sent | the risk check runs on the engine thread between the quote manager and the OMS | limits you did not set are not checked; a limit of `0` is off |
| Cancels always go out | the risk checks skip cancels, also while the kill switch is set | the venue can still refuse or lose one |
| Every consumed event is recorded before it is acted on | `JournalWriter::record_at` runs before `dispatch` in `Engine::process` | the record reaches a ring, not the disk ([durability](../how-to/operations/running-in-production.md#the-journal-is-durable-against-a-crash-not-against-power-loss)) |
| Replaying a journal sends the same order messages | the clock, feed, transport and RNG are compile-time policies fed from the journal ([Determinism](determinism.md)) | needs the same binary and the embedded config |
| Order events are never silently dropped | a full order-event ring trips the kill switch and ends the session (exit code 5) | market-data messages are dropped on a full ring, which forces a resync |
| Client order ids are unique across restarts | `[engine] epoch_file` counts sessions into the upper 32 bits of the id | delete or lose the file and ids repeat |
| The engine thread does not allocate or block | fixed-capacity pools and rings, no heap after start-up (`tests/hotpath` counts global `operator new`) | with `spin_mode = "adaptive"` the engine sleeps when idle and writes the network thread's eventfd when it sends |

## What the engine does not guarantee

| Not guaranteed | What that means for you |
|---|---|
| Everything survives a restart | the position carries over from the store, and the kill switch and `max_loss` budget from `<engine>.kill`; the previous session's open orders are cancelled at the first reconciliation, and the books are rebuilt from the venue's snapshot |
| Fills that arrive while the private stream is down are booked exactly | every connector replays the trade history before reconciling and once a minute; a replay that fails is retried every 5 s, and until then the reconciliation is counted as an estimate |
| PnL adds up across settlement currencies | only with `[accounting]`: inverse contracts are valued in their base coin, linear ones in their quote currency, and the totals, `max_loss` and the exposure caps are converted to one reporting currency at the mid of an FX source instrument; with no current rate, new exposure in that currency is refused. Without it a session or gateway whose instruments settle in more than one currency refuses to start with `max_loss` set, and its PnL totals add unrelated numbers |
| Monitoring beyond a pull | a status file, `fastmm-top` and its Prometheus endpoint, all published every 250 ms; the engine pushes nothing and alerts on nothing |
| Risk across processes | `max_loss`, `max_gross_notional` and `max_net_notional` cover one session; across strategies only `fastmm-gateway`'s `[gateway]` limits see the account |
| The venue's margin, balance or position limits | the venue enforces them; its rejection is a reject like any other |
| Profit | the shipped strategies are reference implementations ([Economics](economics.md)) |

Each of these is spelled out with its mitigation in [Running this in production](../how-to/operations/running-in-production.md).

## Where the same code runs

`Engine<Strategy, Clock, Transport, Feed>` is instantiated three ways, so a strategy's source is identical in all of them:

| | Clock | Events | Orders |
|---|---|---|---|
| Backtest | simulated, driven by event times | a generator, a journal or a CSV file | an in-process matching engine with a latency model |
| Replay | simulated, set to the recorded engine clock | the journal, in recorded order | the recorded acknowledgements |
| Live | TSC calibrated against wall time | the network threads' rings | the venue |

## Next

- [Running this in production](../how-to/operations/running-in-production.md): what breaks and what is not covered.
- [Economics of the shipped strategies](economics.md): what the example backtest and a live session earned, and the fees they have to beat.
- [Architecture](architecture.md): threads, rings, the reactor and the clock, in full.
- [Event flow](event-flow.md): the same path with the venue-specific steps.
