# Working notes

A running record of what was found, what changed, the evidence, and what is next. Newest first.
This file is for whoever picks the work up, including me after a restart. Keep entries short.

## Next direction (chosen 2026-09-25): split the venue gateway from the strategy

**Why.** Stepping back from recovery work: what a firm needs and FastMM lacks is structural. One
process is one strategy, one account per venue, one risk view. Several strategies cannot share a
venue session or an account; nothing sees risk across processes; a strategy change drops the venue
sessions. Multi-strategy, firm-level risk, alerting and a console all hang off a long-lived gateway.
The other candidates (more venues and correct inverse/multi-currency PnL; calibrating the fill model
on the real market) are listed in the gap list below and come after, or need the user's data.

**What stays.** The engine, its messages, the journal and replay are untouched: the engine consumes
the same rings, only they live in shared memory. Backtest and replay never see a gateway.

**Plan, each step verified before the next.**

1. ~~`ShmRing`~~ Done (`include/fastmm/core/shm_ring.hpp`): MsgRing's protocol with the indices
   and buffer in a `MAP_SHARED` file. A 128-byte hop between processes is 73 ns against 65 ns
   between threads (`BM_ShmRing_PingPong_Process`, busy-poll, 3 runs). Moving MsgRing's own
   indices behind a pointer to share one class cost the in-process engine step ~3%, so the
   protocol is written twice and a test drives the same 200k-step sequence through both.
   Still to do for step 2: the cross-process wake-up (a futex in the mapping) for adaptive spin.
2. ~~`fastmm-gateway`~~ Done except the wake-up (`include/fastmm/live/gateway.hpp`,
   `docs/how-to/operations/run-behind-a-gateway.md`). The gateway runs the venues through the same
   code as fastmm-live (`live/venue_slot.hpp`); on attach it creates three ShmRings per venue and
   switches the sinks to them in a task posted to the venue's reactor (`EventSink` takes a ShmRing).
   Outbound: the network thread's hook copies the strategy's orders into the venue's own outbound
   ring and calls `on_wake()`, so no connector changed. The engine gets the gateway's instrument
   table and ring paths in the reply; `fastmm-live --gateway <socket>` runs everything else as
   before and calls no venue cancel_all. A mid-stream attach needs whole books:
   `Venue::resync_books()` (four connectors; nasdaq_itch cannot).
   **No cross-process wake-up yet, and it matters.** Tick-to-trade against the simulator (release,
   WSL2, unpinned, 45 s x 3 runs, `scripts/bench-gateway.sh`), engine t2t p50 / wire t2t p50:
   busy spin in-process 7.2-7.4 / 41.0 us, through the gateway 7.9-8.2 / 41.0-43.0 us (p99 wire
   61-70 vs 70-74 us): on par. Adaptive in-process 34.8 / 66.4 us, through the gateway 557-590 /
   1376 us: an idle side sleeps up to 1 ms in each direction because nothing wakes it. Next: pass
   the gateway reactor's eventfd with `SCM_RIGHTS` plus a sleeping flag in the ring's mapping for
   engine -> gateway, and a shared futex word the engine's idle wait also watches for gateway ->
   engine. Until then run both sides busy.
3. ~~Attach/detach~~ Done for one strategy. On attach the gateway resyncs the books, replays the
   executions since the strategy's store (the strategy owns the store, so it restores its own
   position, onto its control ring, and sends the replay start and the known trade ids in the
   attach request) and then the open orders. Detach is the connection closing: sinks back to a
   discard ring (counted), cancel_all on every venue, rings removed. `tests/integration/
   gateway_test.cpp`: kill -9 of the strategy leaves no open order at the simulator 5-10 ms later,
   md/api sessions opened stay the same, the next strategy restores, trades, and its stored
   position equals the venue's.
4. Several strategies per gateway: the client order id carries a strategy slot, the gateway routes
   order events by it, and account-level limits (position, exposure, loss, order rate) are checked
   in the gateway before a request leaves.

## 2026-09-25: an execution was booked twice after a long session

The OMS deduplicated executions by `hash(exec_id) ^ cl_ord_id`. A replayed trade history names only
the venue order id, which the Binance connectors map back through a table that silently stopped
accepting entries after 8192 orders; past that, a reconnect replayed streamed fills under no order,
as new keys, and booked them twice. And the replay's watermark only moved when a replay ran, so a
reconnect after hours of quoting replayed more fills than the 4096-entry dedupe window holds. Fixed
at the root: an execution is keyed by its venue id, instrument and side; the order-id tables evict
the oldest pairing (`RecentMap`); every connector replays once a minute, which also books a fill the
stream dropped without disconnecting. The fault soak could not see it: its simulator has no market
flow, so no fill ever arrived both streamed and replayed.

## 2026-09-25: the fill model cannot be calibrated on Demo

`fastmm-data fill-check <session.fmj>` replays the orders a live session actually had resting
through the `l2_queue` model (no strategy re-run) and reports which filled live, which the model
fills, and the gap, per `queue_conservatism`. On backtest journals it agrees (374 of 378). On
Binance Spot Demo it predicts none of today's 9 fills and 13-14% of the fill quantity of the
2026-09-14 hour: Demo fills resting orders ahead of the displayed queue (an order acked behind
1.65 BTC filled after 0.009 BTC traded at its price). Calibrating needs a session on the real
market, which is outside this mandate; the tool is ready for whoever runs one.

## 2026-09-25: a crash comes back into service by itself

`deploy/fastmm-live.service` restarted nothing (`Restart=no`), on the grounds that positions and
open orders did not survive a restart. They do now, so the unit restarts after a crash or exit 4
(`Restart=on-failure`, `RestartPreventExitStatus=2 3 5 6 7`, five starts in ten minutes). Shown
with a user unit against `fastmm-sim-exchange`: `kill -9` mid-quoting, systemd restarted it after
2 s, the new session booked the one resting order that filled in between from the executions and
cancelled the other as unknown, and venue and engine both ended at 0.00044 with no open orders
(120 fills at the venue = 50 + 70 in the two sessions' stores). Exit 4 retried five times and
stopped. Bybit and Deribit did not reconcile on their first connect, so a dead session's orders
rested unmanaged until some later reconnect; both now sweep on first connect with an empty
watermark, like Spot. The production guide's "Nothing survives a restart" section is rewritten
from the code.

## 2026-09-25: restart carry-over checked against Binance Spot Demo

Session A (`configs/binance-demo.toml` at 0.5 bps, 150 s) made 10 fills and stopped at BTCUSDT
−0.0003012. A market buy of 0.0001 placed outside FastMM filled with a 0.0000001 BTC fee. Session B
restored −0.0003012 from the store, replayed exactly one execution (trade 309380872, at its own
price, A's ten skipped by id) and stopped at −0.0002013, which is what the account holds. The
USDⓈ-M check could not run: the Demo futures wallet is unfunded (every order −1109, "no available
USDT margin balance"), Demo has no transfer API, and funding it is a click on demo.binance.com.
Bybit and Deribit have no testnet keys here, so their replays are verified against mocks only.

## 2026-09-25: latency work recovered from 2026-09-15

A latency branch from 2026-09-15 had never been merged. What main lacked was ported against current
code, each piece on a failing test or a benchmark (numbers in the commit messages): timer and
start/finish sends carry no T0; market-data age judged on the engine clock (a WSL2 clock step
made fresh books stale); reactor timers without allocation (6005 allocations per 2000 re-arms → 0);
lock-free posted-task check; no EAGAIN read after a short WebSocket read (TLS too); Oms best-price
scan over live orders only; an idle adaptive engine blocks on a futex (tick-to-trade p99 ~155 → ~75
µs); adaptive network threads spin 200 µs after activity (wire-to-wire p50 ~72 → ~56 µs); idle
journal and log sinks back off (≈8000 → ≈100-900 wake-ups/s). Not ported: epoll once per µs (saves
CPU, adds latency). Producers on a caller's input rings (the Python slow tier)
wake it too, through `LiveStrategy::set_waker`.

## 2026-09-25: the weekly CI matrix

The full matrix (clang, clang-tidy, TSan, ASan, docker) runs only weekly or on dispatch, and had
drifted across the merges: clang rejected `__builtin_cpu_supports("sha")`, the DPDK stub had unused
fields, a fixed-point test relied on signed overflow, and clang-tidy had a dozen findings. All fixed.
Two things it turned up were real. A Binance depth snapshot request that failed synchronously (no
REST connection, no rate budget) retried from inside itself, and with `min_snapshot_interval_ns = 0`
recursed until the stack ran out; the retry is now left to the timer. And the market-data recovery
test read the once-a-second status as proof of a resync. Run the full matrix
(`gh workflow run ci.yml`) after a batch of merges, not only weekly.

## 2026-09-24 (later): reusing mature components

A workflow researched where FastMM should adopt mature components (four independent reports, a
plan, then each step implemented in a worktree, verified by a separate agent and merged only if it
passed). Landed: zlib dropped (unused), the io_uring backend on liburing, every command line on
CLI11, the user-space TCP and the AF_PACKET ring deleted, and the WebSocket client and server gated
on the Autobahn|Testsuite. Deliberately kept, with reasons in the plan: the .fmj journal, rings and
fixed containers, the latency histogram (fixed layout in the status segment), the epoll reactor,
the WebSocket/HTTP/TLS code, the SBE generator (sbe-tool brings a JVM and exceptions), the codecs.
Deferred until there is a reason: Aeron (no process boundary on the trading path yet; start with a
lossy journal tap when a remote consumer is needed), QuickFIX (std::map fields and its own threads;
decide when a FIX venue arrives), the OpenTelemetry SDK (an OTel Collector can scrape the existing
Prometheus endpoint), secrets management.

**Not merged: Quill for logging.** Measured on the step's branch rebuilt on main, interleaved and
pinned with a second core for background threads: BM_TickToOrder_Sim within noise, but
BM_EngineStep_Sim consistently about 3.5% slower (main 1703-1728 ns over six rounds, branch
1758-1838 ns). The change was 524 lines added for 529 removed, so it saved no maintenance. The
first attempt was worse (7-13%) because it also forced StaticVector's insert and erase inline.

**Not merged yet: libbpf/libxdp for AF_XDP.** The second attempt attached through libxdp's
multi-program dispatcher, which libxdp pins: after a crash FastMM's program stays in it, and after
ten crashes the interface cannot be opened. The first attempt (libbpf for loading and UMEM, a
bpf_link attach the kernel removes when the process dies) had the right crash behaviour and was
failed only on an acceptance criterion that asked for the dispatcher, plus formatting and privileged
tests that need root. Deferred rather than finished: it compiles libbpf, libxdp and libelf from source
and brings back zlib (S1 had just removed it) to save about 100 lines (+461/-562), while the
hand-written loader already passed the privileged suite as root (22/22) and the AF_XDP end-to-end
runs. Revisit when AF_XDP runs on production NICs and driver quirks make a maintained loader worth
four source dependencies. Branch deleted; the design to use then is libbpf for loading and UMEM
with a bpf_link attach, not the libxdp dispatcher.

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

**Closed: a restart carries the position over.** `fastmm-live` reads the previous session's
positions from the store before the venues attach, pushes each as a `ReconcileMsg::Kind::Position`
onto that venue's order ring (so the journal records it and a replay starts from the same place),
and points the venue's first execution replay at the store's last fill, 10 s early, skipping the
trade ids the store already holds (`Venue::resume_executions`). Executions after that - fills of
orders that were resting when the process died, trades made on the account outside FastMM - are
booked with their real price and fee. Only venues with `VenueCapabilities::executions` restore; the
others start flat with a warning, because a stored position with nothing to bring it up to date
could be wrong. `[engine] restore_position = false` turns it off. Proved by
`recovery: a restart carries the position over and books what happened while it was down`: a
session trades and stops, an outside market order moves the account, the next session ends at
exactly the venue's position. With the restore off the same test ends 0.001 short.

**Every connector with order entry replays executions now** (2026-09-25): Binance USDⓈ-M
(`GET /fapi/v1/userTrades`, sharing the trade parser and fill mapping with Spot in
`binance/binance_trade_history.hpp`), Bybit (`/v5/execution/list`, one account-wide query) and
Deribit (`private/get_user_trades_by_currency_and_time`, history first when the window reaches past
24 h). Each is proved by venue tests that fail without it: a fill the private stream missed is
booked with its price and fee before the snapshot, `kExecutionsExact` only after a complete replay,
a failed query retried from the housekeeping timer. Untested against the real venues: whether
Bybit's `endTime` and Deribit's `end_timestamp` are inclusive, and Deribit's user-trade `direction`
(documented as the taker's).

**The soak's other two findings.** (2) An order sent right after an order-channel reconnect can be
counted as sent and never reach the venue. The reconciliation now settles it *honestly* - the
execution replay proves it never traded, so `reconcile_end` cancelling it is a fact rather than a
guess - and `VenueStatus::orders_sent` no longer counts the orders of a batch whose write failed
(`unsend()`, 2026-09-25). A frame the kernel took and the peer never read still counts: only the
reconciliation can tell those apart. (3) `cancel_all()` now retries a rate-limited refusal (418/429)
three times before reporting failure, because the kill switch has no other remedy; a genuine
multi-minute IP ban still ends in `false`, and the caller still treats that as final.

**Smaller things seen and left alone.** `VenueStatus::reconnects` counts market-data backoffs only,
so user and order channel reconnects are invisible in the status line. `BinanceVenue::shadows_` leaked
a slot per order whose terminal event was lost (fixed 2026-09-24: a reconciliation sweeps shadows
of orders the venue no longer holds that were sent before the snapshot was asked for). Open-orders replies are matched to their sent
watermark by FIFO on a shared `"oo"` request id; the REST fallback captures its own watermark in
the reply callback and the FIFO is cleared with its connection, so the two cannot drift (checked
2026-09-25).
`OmsAction::ReconcileNeeded` (more than three cancel rejects) was logged and nothing asked for a
snapshot (fixed 2026-09-25: the engine sends `ControlCommand::Reconcile` on that venue's outbound
ring and every connector answers with `request_open_orders()`).

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

**Known gaps (rewritten 2026-09-25).** Landed since this list was written: the control socket and
an engine-owned flatten, the feature/forward-markout extractor, portfolio exposure caps, restart
position carry-over, execution replay on every venue, automatic restart after a crash. Open:

1. The fill model is uncalibrated against the real market (Demo cannot do it, see above).
2. ~~A sweep is a cartesian grid on one dataset with no out-of-sample structure.~~ `fastmm.walk_forward`
   / `bt::walk_forward` (2026-09-25): K time folds, each chosen on the previous one.
3. `Engine<Strategy>` binds one strategy per process; a strategy change drops the venue sessions
   (the gateway split below is the end state).
4. Bybit and Deribit replays are verified against mocks only (no testnet keys here); the USDⓈ-M
   Demo wallet needs funding on demo.binance.com before its restart check can run.

**Flaky.** `integration.store restart: ...` failed once under load average ~40 and passed on rerun.
Watch it; if it recurs, it is a timing assumption, not a store bug.
