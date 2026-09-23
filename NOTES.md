# Working notes

A running record of what was found, what changed, the evidence, and what is next. Newest first.
This file is for whoever picks the work up, including me after a restart. Keep entries short.

## 2026-09-24

**Recovery is now demonstrated.** `tests/integration/recovery_test.cpp` and
`recovery_restart_test.cpp` break a live session in flight and check it comes back: a market-data
cut and a sequence gap, the order and user channels cut with an order resting / in flight / being
cancelled, fills that happen while the private stream is muted, `kill -9` with orders resting, five
uncertain order outcomes, and venue-side chaos (429, a 418 ban, a revoked key, malformed frames,
clock skew past `recvWindow`). Every case ends on the same invariants: the venue's open orders and
the engine's agree, positions agree, no client order id repeats, nothing is left resting.
The simulator grew the fault controls they need (`docs/reference/sim-exchange.md`): swallow a
WebSocket API reply, duplicate a user event, mute the user stream, fill a named resting order,
418, `-2015`, malformed frames, and a list of the client order ids the venue holds.

**Two bugs the scenarios found, both fixed.**

* A session that crashed left its orders resting and *nothing ever went looking for them*: the
  Binance connector asked for open orders on a re-connect but not on the first one, so the next
  session traded alongside orders it did not manage until its own shutdown cancelled everything.
  `src/venues/binance/binance_venue.cpp` now reconciles on the first connect too; the ids belong to
  an earlier session epoch, so the engine does not recognise them and cancels them
  (`cancelling unknown live order ...`).
* An order that a reconciliation snapshot no longer mentions was silently written off as cancelled,
  even with quantity still working. `Oms::reconcile_end` now reports that quantity
  (`OmsUpdate::unresolved_qty`, `OmsStats::reconcile_unresolved`) and the engine logs it and counts
  `EngineStats::unresolved_orders`.

**A gap too large for this change: a fill that finishes an order while the private stream is down
is lost.** The documented recovery for a missed fill is the `cum_qty` jump a later message carries
(`Oms::absorb_cum` → `Engine::book_missed_fill`), and it works — proved end to end, including that
the synthetic fill is booked at the order's own price with **no fee**, so the engine's fee total
runs short of the venue's by exactly what was charged. But it only works while the order is still
open. If the missed execution *completed* the order, the venue has nothing left to report: the
snapshot omits it, and the quantity is neither booked nor recoverable. Binance Spot has no position
to reconcile against — `ReconcileMsg::Kind::Position` is emitted by the USD-M connector and never by
the Spot one — so the engine's position stays short until a human notices. Today it at least says so
(`unresolved_orders`). The honest fix is to fetch the executions rather than infer them: after a
user-stream gap, `GET /api/v3/myTrades?startTime=<last seen>` per symbol, emitting the trades the
engine never saw as ordinary fills with their real fees. That needs `myTrades` in the simulator
(which currently forgets terminal orders at the end of each request) and a "last trade time" per
instrument in the connector. Until then, treat `unresolved_orders > 0` as "check the position by
hand". Covered by the last third of `recovery: a fill while the private stream is down comes back
as a synthetic fill`.

**Smaller things seen and left alone.** `VenueStatus::reconnects` counts market-data backoffs only,
so user and order channel reconnects are invisible in the status line. `BinanceVenue::shadows_` (a
fixed 8192-entry map) is never swept or cleared on disconnect, so an order whose terminal event is
lost leaks its slot for the life of the process. Open-orders replies are matched to their sent
watermark by FIFO on a shared `"oo"` request id, and the REST fallback never enqueues one.
`OmsAction::ReconcileNeeded` (more than three cancel rejects) is logged and nothing asks for a
snapshot.

## 2026-09-23

**Where it stands.** `main` at 0.2.0, published (PyPI `fastmm-engine`, GitHub release, GHCR image).
955 tests pass with every optional codec on, 887 in the default build. Docs at https://ziy.bio/FastMM/.

**Landed today.** Venue registry (a connector owns its config and declares capabilities; `VenueKind` is
gone; an out-of-tree venue is 282 lines). Connector deduplication (four shared headers, venue tree
21,928 → 21,524 lines, `libfastmm_codecs.a` −52%). FIX and MDP3 behind `FASTMM_CODEC_*` options,
default off: 12,925 lines out of the default build. Pluggable storage (`[storage] backend`, SQLite
first) and pluggable market-data sources (`--data binance:BTCUSDT,2024-03-27`). Markouts in the
backtest. Single-file HTML run report. `fastmm-top --metrics` in Prometheus format.

**The result that matters.** `basic_mm` on real BTCUSDT perpetual data, 2024-03-27, Binance VIP-0
fees: net −499 USDT. Spread captured 0.032 bps against 2 bps of fees, and markouts −0.85 bps at 1 s,
10 s and 1 min — the fills lose money before fees. The shipped strategies are reference
implementations of published rules, not an edge.

**Verified connector gaps (2026-09-23, third review).** Being fixed now: no exchange-side dead man's
switch on any venue but Deribit (`countdownCancelAll`, `set_dcp`), which is the only protection that
survives the process dying; Binance Spot replaces with `order.cancelReplace` only, so every
size-down loses queue position where `PUT /api/v3/order/amend/keepPriority` would keep it; no batch
or mass-quote endpoints, so quote throughput is metered against the wrong limiter.

**Longer-term shape worth knowing.** Roq splits a gateway process per venue from the strategy
process over a Unix socket, so restarting a strategy keeps the authenticated venue session, its
sequence numbers, rate-limit budget and order cache alive, and the reconnecting strategy is replayed
a snapshot and gated on `Ready` before it may send. FastMM is one process (`src/live/session.cpp`),
so any change to strategy or parameters drops the venue session. That is the end state to grow
towards; it is not a patch.

**Found by the recovery soak (2026-09-24, `tests/integration/recovery_soak_test.cpp`).** The soak
breaks a session repeatedly with order flow running and checks after every fault that the venue and
the engine hold the same orders and the same position. 25 rounds over 60 s pass with connection
drops, 418 bans and market-data cuts. Three things it found that are held out of the fault set until
they are fixed, each reproducible by putting the fault back:

1. A fill delivered to nobody does not reach the position when the recovery path is the REST
   cancel-all that follows an order-channel drop: the mirror books no synthetic fill and ends
   short. Execution-history recovery is being built for exactly this.
2. An order sent immediately after an order-channel reconnect can be counted as sent by the
   connector (`VenueStatus::orders_sent` increments) and never appear at the venue, with no reject.
   Reproduce: fault 0 in the soak, then place an order in the next round without waiting for a
   reconciliation.
3. `BinanceVenue::cancel_all()` returns false while the venue has us banned (418). Retrying until
   the ban lapses works, and the soak does that, but the kill path's remedy is cancel-all: a caller
   that treats the first `false` as final leaves the book on.

**Known gaps, in the order I intend to close them.**

1. ~~Recovery is asserted, not demonstrated.~~ Done; see 2026-09-24. What is left of it is the one
   gap recorded there: a fill that finishes an order while the private stream is down.
2. No flatten and no runtime control. A kill pulls quotes and cancels; inventory stays on. Being
   built now: an `AF_UNIX` control socket routed through the control ring so operator actions are
   journaled and replayable, plus an engine-owned flatten.
3. No way to evaluate a signal without running a strategy. `microprice()` and `imbalance()` exist and
   no shipped strategy calls them. A feature/forward-markout extractor over `MdSource` would say
   whether a quote at the touch is adversely selected, before any strategy is written.
4. A sweep is a cartesian grid on one dataset with no out-of-sample structure; the best row is the
   luckiest row.
5. `Engine<Strategy>` binds one strategy; portfolio risk is per instrument plus one `max_loss`.

**Flaky.** `integration.store restart: ...` failed once under load average ~40 and passed on rerun.
Watch it; if it recurs, it is a timing assumption, not a store bug.
