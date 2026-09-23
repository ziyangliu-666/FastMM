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

**Closed: a fill that happened while the private stream was down is now fetched, not inferred.**
A reconciliation asks the venue what the account executed before it asks what is open
(`Venue::request_executions`, `VenueCapabilities::executions`, Binance Spot `GET /api/v3/myTrades`).
Each execution reaches the OMS as an ordinary fill carrying the venue's trade id
(`OrderFillMsg::kReplayed`), which `Oms::on_fill` already deduplicates on, so only the unseen ones
are booked - with their real price and their real fee. That covers both halves of the hole: the
partial fill, where the old `cum_qty` estimate was at best priceless and feeless, and the fill that
*finished* the order, where the snapshot has nothing to report at all. The snapshot's `Begin` carries
`ReconcileMsg::kExecutionsExact` when the replay was complete, and without it the engine counts an
estimated reconciliation and says what that costs. A failed query keeps its watermark and is retried
every 5 s from the connector's housekeeping timer, so recovery does not wait for the next reconnect.
A fill the engine had already booked from a `cum_qty` jump is corrected rather than counted twice
when the replay names it (`OmsUpdate::corrected_qty`, `PositionTracker::correct_fill`): the quantity
is already in the position, so the execution replaces the estimate's price and its missing fee.
Proved by `recovery: a fill while the private stream is down is booked from the venue's trades`,
`... is booked when the cancel-all reconciles` (partial and complete, which is what the soak found)
and `... an execution replaces the synthetic fill a cancel ack booked`. The soak now fills an order
in the dark, partially and completely, as one of its faults. Details in
`docs/reference/venues.md#executions-the-private-stream-never-delivered`, including what each of the
four venues can actually answer.

**What execution history does *not* fix: a restart.** The connector replays nothing from before it
connected, on purpose: the engine's position starts at zero, so booking another session's fills into
it would make the number worse, not better. A restarted session needs its position back first (the
store has it - `Recovery::positions` - and only logs it today); once it does,
`request_executions(since_venue_ms)` is where the store's last-fill time goes, and everything after
it comes back exactly. Until then, a restart recovers open orders and not the position.

**Bybit, Deribit and Binance USDⓈ-M declare `executions = false`.** Their endpoints are verified and
written down in `docs/reference/venues.md`; nobody has written the connector side. That is the point
of the capability flag: their reconciliations report themselves as estimates rather than being
assumed exact.

**The soak's other two findings.** (2) An order sent right after an order-channel reconnect can be
counted as sent and never reach the venue. The reconciliation now settles it *honestly* - the
execution replay proves it never traded, so `reconcile_end` cancelling it is a fact rather than a
guess - but `VenueStatus::orders_sent` still counts a WebSocket write that the socket discarded, and
that is worth fixing at the source. (3) `cancel_all()` now retries a rate-limited refusal (418/429)
three times before reporting failure, because the kill switch has no other remedy; a genuine
multi-minute IP ban still ends in `false`, and the caller still treats that as final.

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

1. ~~A fill delivered to nobody does not reach the position when the recovery path is the REST
   cancel-all that follows an order-channel drop.~~ Closed by execution-history recovery; the fault
   is back in the soak's fault set, partial and complete.
2. An order sent immediately after an order-channel reconnect can be counted as sent by the
   connector (`VenueStatus::orders_sent` increments) and never appear at the venue, with no reject.
   Reproduce: fault 0 in the soak, then place an order in the next round without waiting for a
   reconciliation.
3. ~~`BinanceVenue::cancel_all()` returns false while the venue has us banned (418).~~ It now
   retries a rate-limited refusal three times before reporting it. A ban that outlasts that still
   ends in `false`, and a caller that treats it as final still leaves the book on.

**Known gaps, in the order I intend to close them.**

1. ~~Recovery is asserted, not demonstrated.~~ Done; see 2026-09-24, and the fill that finishes an
   order in the dark is closed too. What is left of it is a restart's position.
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
