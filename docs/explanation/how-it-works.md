# How FastMM works

One thread makes every trading decision, and every event it acts on is recorded first. The rest of the program feeds that thread or writes down what it did.

<video class="fastmm-video" controls preload="none" poster="../../assets/film-poster.jpg" src="https://github.com/ziyangliu-666/FastMM/releases/download/v0.5.1/fastmm-film.mp4"></video>

## One engine thread owns the trading state

Books, orders, positions, risk limits and timers live on the engine thread, and no other thread touches them. The decision path takes no locks, and a session can be replayed from its recording.

<video class="fastmm-video" autoplay loop muted playsinline preload="metadata" poster="../../assets/anim/architecture.jpg" src="../../assets/anim/architecture.mp4"></video>

Each venue gets a network thread; behind one venue several accounts can trade. The network threads talk to the engine through queues with one writer and one reader each:

| Thread | Does | Queue to the engine |
|---|---|---|
| network, one per venue | sockets, TLS, WebSocket, decoding, book sync, order encoding and signing | market data and order events in, orders out |
| engine | applies events, runs the strategy, turns quotes into orders, checks risk | |
| journal | appends every event the engine consumed to the `.fmj` file | the journal queue |
| store | writes fills, orders, positions and kills to the trade store | the record queue |
| log | formats and writes log lines | one queue per thread |
| main | configuration, start and stop, the control socket, the status file, signals | the control queue |

The engine takes at most 64 messages from one queue before moving to the next, so one busy venue cannot starve another. Two variants change the layout:

| Setting | Effect |
|---|---|
| `[engine] threading = "single"` | one venue only; the engine thread also runs the network loop, so nothing crosses a core between the packet and the order |
| `fastmm-live --gateway` | the network threads run in `fastmm-gateway`, and the queues are in shared memory ([Run behind a gateway](../how-to/operations/run-behind-a-gateway.md)) |

The thread table with how each thread waits is in [Architecture](architecture.md#threads).

## The path of one event

1. The network thread reads the socket, decodes the message and stamps its receive time.
2. It pushes the message to the engine's queue.
3. The engine reads its clock once and records the event in the journal.
4. It applies the event. A book update moves the book and marks the position; a fill books the position and fees first.
5. It calls the strategy hook, such as `on_book` or `on_fill`. The hook calls `ctx.set_quotes`, which only records the wanted quotes.
6. The quote manager compares the wanted quotes with the resting orders and produces new, cancel and replace messages.
7. Each new order and replace passes the pre-trade risk checks ([Risk model](risk-model.md)). A refused order is never sent.
8. The surviving messages are journaled and handed to the network thread, which encodes, signs and writes them.

The venue's acknowledgement or fill comes back the same way and re-enters at step 3. Each step in detail: [Event flow](event-flow.md).

## Guarantees

| Guarantee | Why it holds | Where it stops |
|---|---|---|
| A refused order is never sent | the risk check runs on the engine thread before the order reaches the network thread | limits you did not set are not checked; a limit of `0` is off |
| Cancels always go out | the risk checks pass cancels, also while the kill switch is set | the venue can still refuse or lose one |
| Every event is recorded before it is acted on | the engine journals an event before it dispatches it | the record reaches a queue, not the disk ([durability](../how-to/operations/running-in-production.md#durability)) |
| A replay sends the same orders | clock, events, orders and randomness come from the journal ([Determinism](determinism.md)) | needs the same binary and the recorded configuration |
| Order events are never dropped | a full order-event queue trips the kill switch and ends the session with exit code 5 | a full market-data queue drops the update and resyncs the book |
| Client order ids are unique across restarts | `[engine] epoch_file` counts sessions into the upper half of the id | lose the file and ids repeat |
| The engine does not allocate while trading | all its memory is reserved at start-up ([Memory](architecture.md#memory)) | with `spin_mode = "adaptive"` the engine sleeps when idle |

## Restarts, accounts and monitoring

| Topic | What happens |
|---|---|
| Restart | the position comes back from the store, the kill switch and `max_loss` budget from `<engine>.kill`; old open orders are cancelled and books rebuilt from snapshots before the first order |
| Fills missed while the private stream was down | each connector replays the trade history before reconciling, and once a minute after that |
| Several settlement currencies | `[accounting]` converts PnL, `max_loss` and exposure caps to one reporting currency; without it, mixed currencies with `max_loss` set refuse to start |
| Monitoring | the status file, `fastmm-top` and its Prometheus endpoint, every 250 ms ([Monitor a session](../how-to/operations/monitor-with-fastmm-top.md)) |
| Risk across processes | `max_loss` and the notional caps cover one session; `fastmm-gateway`'s `[gateway]` limits cover the account |
| Venue margin, balance and position limits | the venue enforces them; its rejection is an ordinary reject |

What to set for each row: [Run in production](../how-to/operations/running-in-production.md).

## Where the same code runs

The engine is one template built three ways, so a strategy's source is the same in all of them:

| | Clock | Events | Orders |
|---|---|---|---|
| Backtest | simulated, from event times | a generator, a journal or a CSV file | an in-process matching engine with a latency model |
| Replay | the recorded engine clock | the journal, in recorded order | the recorded acknowledgements |
| Live | CPU counter calibrated to wall time | the network threads' queues | the venue |
