# Working notes

A running record of what was found, what changed, the evidence, and what is next. Newest first.
This file is for whoever picks the work up, including me after a restart. Keep entries short.

## Next direction (chosen 2026-09-30): what a multi-venue desk needs from the engine

The user's list for running real cross-venue market making on Binance, OKX, Gemini and Coinbase,
done one at a time, each verified before the next:

1. Balances, margin and collateral reach the engine, risk and the strategy. Spot inventory and
   perp margin are the usual limit of cross-venue market making; today only `max_position` stands
   in for them. Each connector's balance and margin updates become journaled events, the engine
   keeps a per-venue table (`ctx.balance`, `ctx.margin`), risk refuses an order the account cannot
   cover, xmm stops quoting a side it cannot fill, and the gateway serves the account's balances
   to every strategy.
2. Funding rate, mark and index prices reach the strategy (ticker/funding streams of Binance
   USD-M, OKX, Bybit, Gemini, Deribit), journaled, so xmm prices basis and funding into a perp leg.
3. Fill-model calibration from real fills: `fastmm-data fill-check` over the real-money soak's
   journals, the queue model's parameters fitted, the backtest compared with the session.
4. A reusable hedge executor out of xmm: sizing from positions, one-in-flight, splitting across
   the hedge venue's size limits, failing over to a second hedge venue, stepwise de-risking.

**Real money, two exchanges, crash recovery (2026-10-01).** xmm on the Tokyo box (c7i.large):
quotes on Binance spot BTCU (maker 0), hedges on Binance USD-M BTCUSDT (run 1) and on OKX
BTC-USDT-SWAP, my.okx.com, 265 ms away (runs 2 to 4). Loss limit 10 USDT, the engine's at 8; the
whole day cost about 2.2 USDT. Configs, `chaos.sh` (kill -9 after a fill and on a timer, restart
2 s later) and `reconcile.py` (store against both venues' trade lists) are in `~/fastmm-aws/live/`.

| Run | What | Result |
|---|---|---|
| 1 | Binance spot + USD-M, 1 h | 1 quote fill, hedged in the same second; both in Binance's records |
| 2 | Binance + OKX, 1 h | 1 quote fill, the OKX hedge 208 ms later, in both records |
| 3 | Binance + OKX, kill -9 x 8 | first 8 min clean; then three kills 5 s apart booked 3 OKX fills twice |
| 4 | the same after the fixes, 31 min, kill -9 x 9 (5 after a fill) | 87 Binance trades, 95 OKX fills (83 orders), 10 sessions: every one in the store once |

Found and fixed, in the order they showed:
- An FX source that is quiet was stale after `[risk] stale_md_ms`: UUSDT goes more than 5 s without
  an update, so BTCU orders were refused `FxRateUnknown` (217 in run 1). `[accounting] stale_fx_ms`.
- USD-M (and spot) resynced the book once on every start: the snapshot was requested when the
  stream opened and came back older than the stream's first event. Requested after the first event.
- `BalanceShort` on a requote of a side the balance covers once: Binance's acks from order
  responses had no venue time, so a balance report that arrived first left the order held twice.
  The acks of Binance, OKX, Bybit, Deribit and Coinbase Exchange carry the venue's time.
- OKX "channel -> Live" logged every 20 s (the return from Stale on a pong). Logged on transitions.
- An OKX funding bill has 16 decimals in `balChg`; the bills request failed and funding was never
  booked. Rounded.
- Executions booked twice across consecutive crashes: the start-up replay knew only the newest
  session's execution ids while its window reached older sessions. It knows every session's now,
  and a session that died inside its replay does not move the start. `fastmm-pnl duplicates` and an
  ERROR at start report a store that holds any (the test store keeps the three rows of run 3).
- A hedge larger than `max_order_notional` was refused forever (84 USDT against a cap of 60;
  0.001 BTC stayed open for 8 minutes and was closed by hand). The hedge executor cuts each order
  to the risk headroom; run 4 sent 0.07 + 0.03 contracts.

The engine corrected the position of run 3 from OKX's position report by itself; the phantom hedge
it wanted in between was stopped only by the notional cap.

Open after this: failover to a second hedge venue and stepwise de-risking have not run with real
money (the spot inventory that feeds the asks is hedged outside the engine's view by the USD-M
short, which the engine would count if USD-M were its fallback). Hedge orders share the quote
orders' rate limit (2 hedges refused `RateLimit` in run 4, sent on the retry).

**Fill causality, lead_mm sizing, simulated margin (2026-10-01).** The open items of "Backtest vs
live: balances and the extra orders" below.
Fill causality. A recording is in receive order and its streams arrive with different delays: in
A, 46 % of trades come after a book ticker stamped later (p50 0.5 ms, p99 40 ms, max 72 ms). The
simulated venue took them in that order, so an order that arrived in between was filled by a
trade printed before it (first fills before arrival: A 33 of 232, B 18 of 159, V 58 of 595), and
cancelled before a trade printed while it rested. The rule: the venue sees recorded events in venue
time. `VenueOrderSource` (`sim/venue_order.hpp`) holds events until none unread can be earlier,
assuming none was received more than `[backtest] reorder_window_ms` (1000) after its venue time;
`SimTransport` forwards them to the engine in recorded order (`hdr.seq`), so the strategy's feed
and its recorded arrival times are what they were. For an event later than the window, an order
counts from its arrival: a trade stamped before it does not fill it (its print still takes from
the queue if it came after the book view the queue was taken from: what the tape would have done
at placement), and under `matching` a crossing recorded level fills at the later of the two
arrivals. The three sessions have no event later than the window (23874 / 22417 / 51756
reordered). fill-check already sorted by venue time; the backtest now agrees with it on its own
orders (fill-check over the backtest's journal, orders filled one way only: A 11 -> 1, B 8 -> 2).
The 8.1 / 11.0 / 38.1 ms were the backtests measured from the ack with the early fills in; live
from the ack is 11 / 13 / 32 (calibrate prints it).
`calibrate --backtest` (A, B embedded config, V with its balances and check_balance off; before =
main, after = this branch, fitted keys; live first): orders A 2088 / 2138 / 2071, B 1726 / 1822 /
1729, V 4094 / 4157 / 4262 (V before the lead_mm change); fills 234 / 232 / 231, 161 / 170 / 159,
628 / 595 / 613; time to fill p50 11.0 / 15.5 / 13.7, 13.0 / 29.5 / 17.4, 32.0 / 52.8 / 45.0 ms;
net A -0.122 / -0.119 / -0.122, B -0.037 / -0.040 / -0.033. The extra orders went with the
reordering: the feed keeps its recorded order and times; the fills, and our orders in the feed,
moved. Golden hashes unchanged: their sources are synthetic, in venue-time order, with no
fill before its order's arrival (sample_1000 0 of 5, synthetic l2_queue 0 of 103).
lead_mm sizing. `requote` runs the shared `fit_to_balance` (basic_mm's) and a balance report
requotes; `fit_side` also drops a level under `min_notional`. That surfaced an engine bug: on a
last fill the strategy's `on_fill` ran before the order's hold update, and the holds are kept by
pool slot, so an order placed from `on_fill` into the freed slot lost its hold state and the
estimate leaked (V: 0.0005 BTC locked with no order open). The hold now moves before the hook.
V with `check_balance = true`: BalanceShort about 464500 -> 0, fills 52 -> 666, orders 500 -> 4480,
venue rejects 0 -> 2 (live 309: that strategy did not size), the same as with the check off.
Simulated margin: a derivative row's free and equity count unrealised PnL at the recorded mark
(`PerpState`), else the book's mid.
Tests: sim.arrival (3), sim.venue_order (2), sim.queue_model arrival (1), sim.account (2),
core.balance slot reuse (fails on main), strategies.lead_mm sizing (1). Full ctest (werror) 1649
passed.
Open: account-wide margin rows of a journal snapshot are not used (the simulated account has no
cross-asset margin to put them in); time to fill p50 is still 3-13 ms above live, while
fill-check on the live orders matches it (the backtests' own orders and times differ); the latency
model has no 40 ms tail.

**Item 1 done: balances, margin and collateral (2026-09-30).** One event, `BalanceMsg`
(`EventType::Balance`, 128 bytes, on the order ring, so journaled and replayed): one asset of a
venue's account, absolute free / locked / total / equity / maintenance at the venue's time,
`kSnapshot` / `kSnapshotEnd` (an asset a snapshot does not name holds nothing), `kAccount` (the
account-wide margin in USD). Absolute amounts only: a delta would turn one lost message into a
lasting error. Connectors: `ReconcileDriver` runs a balance leg after every snapshot it emits
(`fetch_balances`, rows filtered by `VenueAssets`: spot base and quote, a derivative's settlement
asset), `request_balances()` (at most 1/s) for venues without a stream or with partial updates, and
`emit_balance()` for stream updates. Sources per venue with the docs read: docs/reference/venues.md
"Balances" (Binance Spot `outboundAccountPosition` + `account.status`; USD-M `/fapi/v3/account`,
`ACCOUNT_UPDATE` triggers a refresh because `B` has the wallet only, multi-assets mode an account
row; OKX `account` channel + `/account/balance` per `acctLv`; Bybit `wallet` + wallet-balance, an
account row unless isolated margin; Deribit `user.portfolio` + `get_account_summaries`; Gemini
`balances@account` (it exists) + `/v1/balances`, `/v1/margin` as the account row; Coinbase Advanced
`/accounts` paged, refreshed after every execution; Coinbase Exchange `/accounts` + the `balance`
channel; Nasdaq none). The connectors were done by four parallel agents on the core commit.
Engine (`core/balance_book.hpp`): rows per venue and asset, resolved at start; nothing runs until
the first report (`balances_live`). The estimate rule: a report is the account at its venue time
and counts the orders the venue had acknowledged; an unacknowledged order holds on top of every
report until its ack (stamped at or before the report: the report had it, the extra comes back);
after a report the engine's own events move the row unless the venue stamped them at or before it.
Holds: spot buy notional + taker fee (quote), spot sell qty (base), derivative notional x
`[[instruments]] initial_margin`, larger side of the instrument; fills move the assets (derivative:
position margin at the fill price, the fee). Check (`[risk] check_balance`, default on,
`BalanceShort`, before the rate limit): what the order adds to its row's holds <= free; a reducing
or reduce-only derivative passes; no `initial_margin`: passes while available > 0; unreported rows
refuse nothing. `ctx.balance`, `ctx.margin`, `ctx.balance_room`, `ctx.balances_live`, `on_balance`
(C++ and Python), `RiskHeadroom::balance_{buy,sell}_qty`. Gateway: each router keeps the account's
BalanceBook over every strategy's orders and fills (`[gateway] check_balance`,
`GatewayBalanceShort`); balances go to every attachment. basic_mm cuts its ladder
(`fit_to_balance`); xmm drops a side the quote venue cannot fill and holds a hedge the hedge venue
cannot cover (logged, `Stats::hedges_held` per episode, only the reducing side quoted). Status v13
balances, `fastmm_balance_*` metrics, `journal_dump.py` decodes the event.
Found on the OKX demo and fixed: a balance push made while a post-only order was in flight held
none of it, and the post-only's reject released a hold the estimate no longer had (USDT locked
-1.69); the unacknowledged-order rule above (test `core.balance: an order in flight holds on top of
a report until its ack`, fails without it). Found by the benchmark: `fit_to_balance` on every
requote cost +4.7 % on `BM_EngineStep_Sim` (every book event of a step reaches requote); it is one
flag test until a venue reports.
Tests: core.balance (12), hotpath.noalloc engine with balances, strategies basic_mm / xmm (side
pulled, hedge held), reconcile_driver balance leg (4), per-connector parser and fake-venue tests,
`sim_exchange: BinanceVenue reports the simulator's balances`, `balance_replay_test` (a sim session
with balances replays to the identical outbound hash), `gateway_balance_test` (a's bid and b's bid on
one USDT balance, b's refused by the gateway, no venue reject), status/Prometheus, python. Golden
hashes unchanged (no balance events in them). Full ctest (werror) 1533 passed; gateway_*, xmm_* and
the balance integration cases (51) green 3 times; clang-tidy-18: no bugprone or performance finding
in the changed src files. Bench (`bench_tick_to_order`, werror release, base main at a path of the
same length, taskset -c 2, 8 x 2 interleaved runs of 3, load < 1.5, medians of 24):
`BM_TickToOrder_Sim` 168.1 -> 159.8 ns, `BM_EngineStep_Sim` 2305.7 -> 2315.4 ns.
Demo verification, the engine's status-file balances against a raw REST query of the same moment (scripts in /tmp, not kept):
Binance Spot demo (75 s basic_mm, 30 replaces): BTC and USDT free / locked equal to the digit at
all 5 samples (e.g. USDT 3664.36157846 / 24.978654). USD-M demo (60 s): without `initial_margin` the
engine's free ignores the resting orders' margin (4.17 USDT at the venue); with 0.05 it tracked
the venue within 0.0006 USDT after 4 fills. OKX demo spot (75 s): BTC 0.99989918 / 0.00002 equal;
USDT equal (the demo refuses these bids with 51137). Gemini sandbox btcusd (60 s): equal at all 4
samples except USD's stream in cents (99884.66 vs REST 99884.66092). Coinbase Advanced live:
read-only fetch through the connector equal to the raw reply (USD 0 / 0; no BTC account).
Open: USD-M and OKX multi-currency / Bybit UTA account rows and Deribit are from documented frames
only; Gemini `/v1/margin` with a derivatives account; Coinbase Advanced `hold` for open orders is
undocumented; the leverage per symbol is configuration (`initial_margin`), not read from the venue;
REST snapshots stamped with the connector's venue clock can precede a stream event they contain by
a few ms (counted twice until the next report); backtests carry no balances.

**Item 2 done: mark, index and funding reach the strategy (2026-09-30).** One event, `PerpStateMsg`
(`EventType::PerpState`, 128 bytes, market data: the md ring, journaled, replayed): a derivative's
mark, index, funding rate per interval (decimal, positive: longs pay), the interval, the next
funding time and open interest at the venue's time, with a `fields` mask. A venue with separate
channels (OKX) sends one message per channel; Bybit's deltas are merged in the connector and every
push carries all fields. Engine (`core/perp_book.hpp`): each field kept with its arrival time on the
journaled engine clock; `ctx.mark` / `ctx.index` (`RefPrice`: price, at, stale, usable),
`ctx.funding` (`FundingView`: rate, interval, next, `over(d)`), `ctx.perp_state`, `on_perp_state`
(C++ and Python). `[accounting] stale_mark_ms` 15 s (mark and index), `stale_funding_ms` 180 s.
Valuation, decided: `[accounting] mark = "venue"` (default) values a position at its venue's
fresh mark (unrealized PnL, `max_loss`, the exposure caps); a new mark checks `max_loss` at once;
book updates stop moving it; a stale mark falls back to the mid at the next book update (logged,
`PerpStats::mark_fallbacks`). `"mid"` keeps the mid. Spot has no marks, so nothing changes there;
the collar, fat finger, `[risk.underlying]` and FX rates keep the mid. The gateway's `AccountBook`
runs the same `PerpBook` on its steady clock, so `[gateway] max_loss` sees the strategies' values;
PerpState goes to every attachment (the md drain) and an attaching strategy gets the fresh fields
with the books. xmm: `funding_horizon_s` (default 0): fair += hedge ref * (hedge funding - quote
funding) over the horizon (a filled bid is hedged by a short that earns the hedge's funding; the
ask side pays the same, so one shift); `mark_basis`: basis = (quote mark - index) - (hedge mark -
index) instead of the EWMA; a non-perpetual leg counts 0; stale inputs pull the quotes. Status v14
`perps`, `fastmm_perp_*`, `journal_dump.py`, JournalSource and the sim driver carry it (multi-venue
backtests from recorded journals); CSV, Tardis, Binance archive and numpy sources do not (Tardis
`derivative_ticker` has mark, index, funding and OI but no interval: not done).
Sources per venue, read 2026-09-30, detail in docs/reference/venues.md "Mark, index and funding";
connectors done by four parallel agents on the core commit, each recording production public frames
with `fastmm-live --dry-run --record-raw` for its fixtures:
* Binance USD-M: `<sym>@markPrice@1s` on `/market` (the change notice lists markPrice there):
  `p`, `i`, `r`, `T`, `E`. Interval: `GET /fapi/v1/fundingInfo` at start (803 rows: 469 4 h, 333
  8 h, 1 1 h; unlisted = 8 h), corrected when `T` steps by whole hours within 5 min after a funding
  (captured across 08:00: LPTUSDT 4 h, BTC/ETH 8 h). 1.00/s per symbol.
* OKX: `mark-price`, `index-tickers` (index = swap id less `-SWAP`), `funding-rate`,
  `open-interest` on `/ws/v5/public`, in their own subscribe request (a refusal leaves the books).
  `fundingRate` is the predicted rate for `fundingTime`; interval = `nextFundingTime -
  fundingTime` (397 swaps 8 h, 320 4 h). Docs say mark every 10 s and index once a minute without a
  change; production pushes mark every 200 ms and index every 0.2-2 s regardless.
* Bybit: `tickers.<sym>` (linear perpetuals), snapshot + deltas of changed fields, cache cleared on
  reconnect; `fundingIntervalHour` (else instruments-info `fundingInterval`); OI from
  `singleOpenInterest` (one side; not in the docs table, present in every frame).
* Deribit: the subscribed `ticker.<name>.100ms`, perpetuals and futures: funding =
  `current_funding` per 8 h, next 0 (continuous; support article "Inverse Perpetual"; on production
  830/862 tickers matched (mark - index)/index clamped, `funding_8h` is the trailing 8 h); OI in
  contracts (USD / contract size).
* Gemini: the new WebSocket has `{sym}@markPrice` (every 5 s) and `{sym}@fundingAmount` (each
  minute: `f` = estimated funding of one long contract over the hour, rate f/p per 60 min, `T` next),
  neither in streams.md nor the AsyncAPI spec (only the 2025-10-31 revision history names "Mark
  Price WebSocket API"); `i` of markPrice is the index times an undocumented per-symbol factor, not
  used. The earlier note that these were v2-only was wrong. No funding snapshot on subscribe.
* Coinbase: none (spot).
Tests: core.perp_state (6), status/Prometheus (2), per-connector parser/feed tests on recorded
frames (binance_usdm md parser + fake venue, okx_perp 8, bybit_perp 7, deribit_perp_state 6,
gemini_perp_state 4), strategies.xmm (3: carry, the funding shift and requote, mark basis),
hotpath.noalloc (engine with marks and funding; Xmm with both terms; one per connector),
`perp_state_replay_test` (a live sim session whose position a venue mark takes past `max_loss`
replays to the identical outbound hash), `gateway perp state` (two strategies on a shared Bybit
perpetual both journal the marks; the account's venue position is valued at them:
-0.015 x (60000 - 60123.45) = 1.85175), python `test_on_perp_state_and_the_venue_mark`, the strategy
API doc test. Golden hashes unchanged (no PerpState in them). Full ctest (werror) 1587 passed; clang
werror build clean; clang-tidy-18: no bugprone/performance finding in the changed src files. Found on
the way: the Python hot-bench module did not build with FASTMM_BUILD_PYTHON (fixed).
Bench (`bench_tick_to_order`, werror release, main at a path of the same length, taskset -c 2, 8 x 2
interleaved runs of 3, load < 1, medians of 24): `BM_TickToOrder_Sim` 158.7 -> 158.7 ns,
`BM_EngineStep_Sim` 2317.7 -> 2323.9 ns. The book path adds one flag test (`PerpBook::marking`).
Dry run on production public data, 15 min, one fastmm-live with all five perp venues (10
instruments; 36059 PerpState events in 927 s; 0 resyncs, 0 malformed, 0 dropped, no ERROR). The
table filled 0.4-0.8 s after start for every instrument except Gemini (mark +3.2 s, funding +46 s).
Per symbol: Binance mark/index/funding 1.00/s, longest gap 2.7 s; OKX mark 4.9/s (1.9 s), index
2.8-3.6/s (2.8 s), funding 16 pushes (82.5 s), OI 0.12/s (16.9 s); Bybit all fields 6.9-7.1/s
(2.1 s); Deribit perps 2.0-2.7/s (5.0 s), the future 1.4/s (7.6 s); Gemini mark 0.17/s with gaps
of 69.5, 45.7 and 35.0 s in the first 4 min (the other Gemini streams kept flowing), so the mark
went stale three times and the position fell back to the mid as designed; funding 14 in 15 min
(one minute missed). A separate 10 min websocket probe right after (Python, same streams) saw none: 121 marks, longest gap 7.0 s, funding every 59-61 s; whether the gaps were the venue's or the connector's is not settled (no raw frames were recorded in that run).
Open: Gemini index and OI (REST `/v1/riskstats` polling, lagging the socket by up to ~6 s) and a
REST funding fetch at connect; Gemini's markPrice gaps; OKX around a settlement (`settState`
processing) unverified; Bybit inverse category; Tardis `derivative_ticker` mapping; demo/testnet
hosts serving the new streams not checked (production only).

**Item 3 done: fill-model calibration (2026-09-30).** `fastmm-data calibrate <journal>...`
(`backtest/calibrate.hpp`): per session fill-check over a `queue_conservatism` grid and without
the ticker, the trade tape or both; hit / miss / false rates, time to fill, the model's queue at
the live fills, each venue's round trips; the conservatism cross-validated (fit on one session,
score on the others), the latency fitted to the round trip's p5 and p50 (the simulator's lognormal
shape), a `[backtest]` snippet; `--backtest` re-runs each session as a `strip_own` backtest as
configured and with the fitted keys, beside the live session, all marked against the journal's
ticker mids. Method: docs/explanation/backtesting.md "Calibrating against live sessions".
Data: the three real-money Binance BTCU sessions (A improve, 3 h; B join the touch, 3 h; V, 1 h).
Fill-check found four systematic errors, each traced to single orders in the journals:
* The ticker cap only lowered the queue. An order at a newer ticker's touch took
  min(depth, ticker), and when the throttled depth had no level there yet, 0: filled at once by the
  model. Now it joins behind the touch's quantity (`queue_join`).
* A ticker stamped up to ~1.6 ms after an order's transactTime did not show it yet; stripping our 1
  from its 0.8 left 0 ahead. A level showing less than our resting quantity predates our order and
  keeps all of it (`others_shown`; engine, fill-check, `strip_own`).
* Trades between depth updates: a level traded away before our ack still showed in the depth
  (B: 0.02377 swept 6 ms before the ack; live filled 1 ms after it). `TradeTape`: prints after the
  depth's (or ticker's) venue time take their quantity from its levels, at placement, in the old
  quantity of the next delta (no longer also counted as cancels ahead when conservatism < 1) and
  under the ticker's cap; in the simulator, fill-check and `ctx.queue_ahead` alike.
* Measurement: fills reported after the order's end were dropped (Binance's cancel response
  arrives before the execution report; a reconciliation missed an order that had just filled: 3 V
  orders), and a trade in a cancelled order's last millisecond counted (6 false fills; the venue
  did not fill the order with it, so it came after the cancel).
Fill-check (both / live only / model only), main -> now: A 233/0/6 -> 233/0/3, B 151/2/3 -> 151/2/0,
V 621/0/7 -> 624/0/2; error (live only + model only over filled either way) 0.025 -> 0.013,
0.032 -> 0.013, 0.011 -> 0.003. The 5 false fills left are trades in the ack's millisecond
(ms order times cannot order them); B's 2 misses are 0.00595 ahead (one bot's size) cancelled in
the ~1 ms before a sweep, shown by no update before the trade. B at the fit: without the ticker
144/9/1, without the tape 150/3/0, without both 127/26/1 (the 25 misses reported on 09-26).
Conservatism: every value 0..1 scores the same on every session (A and V improve the touch, B's
queues are set by the ticker), so it is not identified and 1 is kept; cross-validated held-out
error 0.005 (fit A, test B+V), 0.006 (fit B), 0.013 (fit V). Time to fill p50 live / model:
11.0 / 10.9, 13.0 / 13.3, 32.5 / 32.6 ms. Latency: send to transactTime p50 0.91 / 0.95 / 0.71 ms
(+0.5 ms for the ms truncation), round trip p50 1.39 / 1.44 / 1.64 ms, p99 41 / 39 / 25 ms;
fitted `latency_fixed_us = 792`, `latency_ack_us = 0`, `latency_ack_jitter_us = 817` (the round
trip's fixed part is below the one-way median, so the one-way leg is capped at it).
Backtest vs live (fills, net U, markout 10 s bps; before = the hand-set 400/100/1100/300 us on
main's model, after = fitted keys on this one): A live 234, -0.122, -0.30; before 235, -0.102,
-0.36; after 237, -0.101, -0.34. B live 161, -0.037, -0.01; before 167, -0.026, -0.03; after 160,
-0.035, -0.10. V live 628, -0.146, -0.22; before 814, -0.151, -0.20; after 824, -0.150, -0.19.
The fill model is no longer the gap. V's is balances: live got 309 venue rejects (-2010,
insufficient balance) from minute 20, a backtest has none. A and B send 17-19 % more orders in
the backtest with the same fills; not explained yet (the latency model has no 40 ms tail).
Tests: fill-check (7, each fails on main's model: run through main's fastmm-data), engine queue
estimate (2), calibrate (pick rule, a 0.5-generated grid fitted to 0.5 in both folds, latency on
us and ms venue times and a two-quantile solve, a flat grid kept at 1, a backtest journal
re-run through `--backtest` to identical orders, fills and net), `apps.fastmm-data.calibrate` on
two committed synthetic fixtures (`FASTMM_REGEN_GOLDEN=1` rewrites them). Golden
`basic_mm/l2_queue` re-baselined in its own commit: the tape alone moves it (1014 -> 1169 fills;
without the tape the old hash comes back); the other golden hashes and sample_1000's are unchanged.
Open: ms order times on Binance (the ws-api `timeUnit=MICROSECOND` parameter would remove the ack
ties); the round trip's tail; balances in backtests; the extra backtest orders; queue ahead cancelled
just before a sweep; the ticker's venue time can run up to ~1 ms ahead of what it shows (B: a
ticker stamped 30 us after a trade showed the book before it).

**Item 4 done: a reusable hedge executor (2026-09-30).** `HedgeExecutor`
(`strategies/hedge_executor.hpp`, in `fastmm/strategy.hpp`, tier 1): a member a strategy owns,
configured in `on_start` with up to 4 source instruments (the exposure), up to 4 hedge instruments
in preference order with their own tolerance, and a target in base units; the strategy forwards
`on_fill`, `on_order_update`, `on_book`, `on_timer`, `on_connection`, `on_balance` (the bool ones
say whether the event was the executor's). No timer of its own: the waits end at the strategy's next
call. Decisions:
* Residual = sources + hedges - target, base units via the contract multiplier; inverse legs refused
  at start (not handled).
* One order in flight overall, not per instrument: the residual is one number, two orders sized from
  it on two venues can both fill; per-instrument would mean sizing from positions plus open orders,
  which an unreported outcome breaks. An open order on any hedge instrument (a dead session's, once
  reconciliation adopts it) blocks the next; `max_qty` pieces go one after the other.
* Failover: the first usable instrument takes the hedge. Unusable: an order channel down, venue
  killed, book invalid or older than `stale`, feed-lag gate, benched (`max_failures` in
  `failure_window`, for `bench`). Balance short: the next usable one that covers it; none: held, as
  xmm did. The last instrument not benched failing halts, `restart()` clears. A residual under the
  first usable instrument's minimum waits there and is not moved to a venue with a smaller minimum.
* De-risk: the executor sends it rather than signalling the strategy, because it needs the same
  sizing, in-flight rule and holds (an uncertain de-risk end holds hedges too). After `derisk_after`
  with no instrument taking the residual (unusable, held or halted): reduce-only IOC on a source whose
  position has the residual's sign, at most `derisk_step`, `derisk_interval` apart, `derisk_tolerance`
  through its touch; stops when a hedge goes out, the residual is under the minimum, or flat. The
  strategy keeps off its side by pulling quotes while `!can_hedge()` and quoting the reducing side
  while `held()`.
* Monitoring: `status()` (state, residual, active instrument, in flight, held, halted, de-risking,
  hold end), `stats()` (+ failovers, benches, de-risk episodes and orders), `why(i)`; each transition
  logged once.
xmm is its quoting plus the executor (-75 lines), with `fallback_instrument`,
`fallback_tolerance_bps`, `failover_bench_ms`, `derisk_after_ms` (0: off), `derisk_step_qty`,
`derisk_interval_ms`, `derisk_tolerance_bps`. Equivalence: a first commit pins, on main's xmm, the
FNV hash of every strategy call over three 20000-event seeded sessions on a fake context (every kind
of hedge end, reconciles, channel drops, balance shortfalls, halts and restarts) and the outbound
SHA-256 of a 3000-step engine session; the rewrite gives the same four values with gcc and clang. No
golden or replay hash involves xmm. Deliberate differences: no hedge to a killed or gated hedge
venue or on a book older than `stale_ms` (main sent it, priced from that book, or had it refused by
risk, counting towards a halt), and the quotes come off while the hedge venue is killed.
`examples/cpp/hedged_mm.cpp`: a one-level quoter on A hedged on B then C, run through
StrategyHarness (fill hedged on B; B killed, next on C; both killed, a fill de-risked on A; B back).
Tests: hedge_executor (14: sizing, pieces, minimums, in flight, uncertain hold, failover on each
trigger and back, held, halt and restart, de-risk caps and spacing, halted de-risk, and xmm through
the engine with three venues: B's kill switch, C hedges, B back), xmm_equivalence (2), hotpath
noalloc (the executor with logging on through failover, benches, halts, holds, de-risk), integration
`xmm failover` (fastmm-live on three simulators, B refusing every order: two refusals, C; SIGKILL
with C holding the hedge's reply; the restart books it, sends nothing, and fails over again for the
next fill; one accepted hedge per maker fill, venues net zero), the strategy API doc test. Full ctest
(werror) 1623 passed; xmm and executor tests (52, 7 integration) 6 runs green; clang werror clean
and the pinned values equal; clang-tidy-18 (tidy.sh) no error, the two headers clean under every
check; lint ok. Bench (`bench_tick_to_order`, werror release, main at a path of the same length,
taskset -c 2, 8 x 2 interleaved runs of 3, load < 1.4, medians of 24): `BM_TickToOrder_Sim` 157.0 ->
158.1 ns, `BM_EngineStep_Sim` 2293.1 -> 2302.3 ns; the two binaries' `.text` are byte-identical
(basic_mm does not use the executor), so that is noise.
Open: the fair value stays on the hedge instrument's book, so xmm pulls its quotes when that
instrument's market data dies even with the fallback usable; `[risk] max_order_qty` is not a
splitting limit (only `max_qty`); benches and halts are in memory (a restart relearns them: two
refusals in the test); a residual held in a hedge leg (an overshoot) is not de-risked; C++ only (no
Python binding); executor state is not in the status file or metrics; verified on simulators only.

**Backtest vs live: balances and the extra orders (2026-10-01).** The two gaps item 3 left.
Balances: `SimAccounts` (`sim/sim_account.hpp`), one account per simulated venue from
`[backtest.balances]` / `[backtest.venues.<name>.balances]` or the journal's first balance snapshot
(`balances_from_journal`). Spot orders hold notional (buy, quote) or quantity (sell, base), a
derivative its initial margin on the larger side (reducing orders nothing); fills move the assets,
realise a derivative's PnL and pay the fee. Refused: `InsufficientBalance` (-2010); a replace that
does not fit cancels the order and refuses the new leg. `BalanceMsg`: a snapshot at the start, an
update behind each order event that moved an asset, on the order wire with no latency draw of its
own. No keys: no account, no message, golden hashes unchanged. `calibrate --backtest` prints the
venue rejects. V's starting BTC is not in its journal (it predates `BalanceMsg`): 0.00024, from the
sells (all 304 balance rejects were sells): none refused needed less, 4 of 3410 accepted needed more
(in flight). With `[risk] check_balance = false`, as V's engine had, and the fitted keys: fills 824
-> 587 (live 628), venue rejects 0 -> 294 (309), orders 6256 -> 4601 (4094).
The extra orders: the simulated feed did not show our own resting orders; a live one does. lead_mm
never improves on its own order (live books show it as the touch); when the level it had improved on
went, the backtest's book showed the next level as the touch and lead_mm moved its quote down to it,
and back when the level returned: a cancel and a new order each time. A's orders by what ended the
previous one on that side and where the next went, live / backtest: cancel then a worse price within
10 ms 100 / 296, cancel then the same price within 10 ms 517 / 690 (369 of the 422 extra). Fix:
`[backtest] own_orders_in_feed` (default on). Recorded depth levels and tickers are forwarded with
our resting quantity added, the next depth update carries our levels that changed since the last (0
for a level that was only ours), and a book ticker goes out when our orders move the top of book
(flagged synthetic, update id 0, so lead_mm compares it with the depth by time; `strip_own` drops it
from a backtest journal). Orders at or through the recorded opposite touch are not shown (a live
venue would have matched them). The engine's own-quantity view is on for simulated venues; header
bit `kHeaderOwnInFeed` tells replay and `strip_own` that a backtest journal's feed shows its orders.
Orders, live / main / now (fitted keys; V with its balances): A 2088 / 2510 / 2138, B 1726 / 2031 /
1822, V 4094 / 4601 / 4157; fills 234 / 237 / 232, 161 / 160 / 170, 628 / 587 / 595. Golden hashes:
`basic_mm/l2_queue` (1169 -> 1143 fills) and `sample_1000` (54 messages both) change, in their own
commit: `basic_mm` prices off the book's mid, which now holds its own quotes; the coupled goldens,
`options_mm/scripted` and A-S are unchanged.
Tests: sim.account (7), backtest.balances (5: BalanceShort vs the venue's refusal, basic_mm sized,
xmm's ask pulled on the quote venue, configuration, replay with balances and the journal's snapshot
as the next start), sim.own_feed (2), backtest.own_feed (2: lead_mm keeps a bid that became the
touch, 1 buy and no cancel, against 2 and 1 without; replay), the queue-estimate equality now with
our orders in the feed too. Full ctest (werror) 1639 passed; clang werror build clean, its sim,
backtest and hotpath tests pass; clang-tidy-18 (tidy.sh) no error and no finding in the new .cpp
code; the new tests pass under ASan; python 162 passed.
Open: lead_mm does not size to the balance: V with `check_balance` on (fitted keys) refuses about
460000 sells (`BalanceShort`, one per requote) and fills 48 times. About 10 % of the backtests'
first fills are stamped before their order reached the venue (A 33 of 232, B 18 of 159, V 56 of
591): the recorded streams' venue times are not monotone, and a trade processed after an order
arrived fills it at its earlier time; the backtest's time-to-fill p50 excludes those and reads 15.5
/ 29.5 / 52.8 ms against live 11 / 13 / 32 (from the ack, the journals give 8.1 / 11.0 / 38.1).
Unrealised PnL is not in a simulated derivative account's free margin; account-wide margin rows of a
journal snapshot are not used.

**Gemini connector, `kind = "gemini"` (2026-09-30).** Perpetuals (`btcgusdperp`, linear, 1 BTC a
contract) and spot (`btcusd`) on one API, for the sandbox as a third venue. Docs read 2026-09-30:
docs.gemini.com now redirects to developer.gemini.com, which serves markdown pages and the specs
`specs/openapi/rest.yaml` and `specs/asyncapi/websocket.yaml` (0.10.7); copies in `/tmp/gem` (not
kept). Facts the connector follows, with their page:
* WebSocket API `wss://ws.gemini.com` (`websocket/introduction.md`): the archived `v2/marketdata`
  and `v1/order/events` "have been replaced"; connection parameters `snapshot=-1` (full book on
  subscribe) and `cancelOnDisconnect=true` ("all open orders placed via the WebSocket session will
  be cancelled upon disconnection"). No heartbeats from the venue; `ping`, `time`, `conninfo`.
* Book (`websocket/streams.md`, AsyncAPI `DepthUpdate`): `{sym}@depth@100ms`, "the FIRST frame after
  subscribing is the snapshot ... no separate snapshot message ... if a frame's U skips ahead of the
  last applied u, discard the book and resubscribe". Production: diffs overlap at `U` = previous `u`,
  the snapshot has `U` = `u`, ids sparse; a resubscribe's replies came before its snapshot, a fresh
  connection's snapshot before its reply (checked with a script, `/tmp/gem/resub.py`). v2 `l2` has no
  sequence number at all.
* Auth (`authentication/api-key.md`, `websocket/authentication.md`): REST payload base64 in
  `X-GEMINI-PAYLOAD`, `X-GEMINI-SIGNATURE` = hex HMAC-SHA384 of it, empty body; the WebSocket signs
  the upgrade (`X-GEMINI-NONCE`, payload = base64(nonce)), "Only account-scoped keys with time-based
  nonces", seconds within +/- 30 s. Master keys refused.
* Orders (AsyncAPI `OrderPlaceParams`): `order.place` {symbol, side, type LIMIT|MARKET, timeInForce
  GTC|IOC|FOK|MOC, price, quantity, clientOrderId}, `order.cancel` {orderId} (venue id only),
  `order.cancel_all`, `order.cancel_session`; no amend, no reduce-only (REST `order/new` has neither
  either). Result fields "not enumerated". MOC/IOC/FOK are "accepted, then cancelled — never
  REJECTED".
* Events (`streams.md`): `orders@account` `orderUpdate` X NEW/OPEN/PARTIALLY_FILLED/FILLED/
  CANCELED/REJECTED/MODIFIED, `Z` = last execution on fills and cumulative on CANCELED, `n` fee on
  FILLED only, no sequence number.
* History: `POST /v1/mytrades` per symbol, `timestamp` (ms), limit 500, newest first, `tid` and
  `client_order_id`; `/v1/orders`; `/v1/positions` (`openPositions` in the schema, a bare array in
  its example); `/v1/perpetuals/fundingPayment?since&to` (hourly transfers, no id).
* Dead man's switch: `cancelOnDisconnect`, and the key setting "Requires Heartbeat" (30 s without
  an authenticated request cancels the session's orders; `POST /v1/heartbeat` every 15 s).
  `order/cancel/session` cancels the key's orders; there is no per-symbol cancel-all.
* Rate limits (`rate-limit.md`): private REST 600/min, 5/s recommended, a burst of 5 queued, then
  429; `conninfo` on production: ORDERS 3500 per 10 s, REQUEST_WEIGHT 7000 per 10 s.
* Specs (`symbols/details`): `tick_size` is the quantity step, `quote_increment` the price step;
  `product_type` swap, `contract_type` linear; "Each contract has an underlying of 1 BTC"
  (gemini.com/artemis/legal/contract-specifications). Sandbox: `api.sandbox.gemini.com`,
  `ws.sandbox.gemini.com`, 53 perpetuals against 13 on production, same BTCGUSDPERP details.
  Singapore: derivatives are Gemini Artemis Pte. Ltd.; eligibility (retail vs accredited) not
  confirmed (support page 403). Irrelevant to the code.
Design, on the shared machinery: `ReconcileDriver` (orders + positions, sweep on the first live order
connection), `ReplayScheduler` twice (mytrades, one stream per symbol, ascending pages from the
newest row; funding, one stream, key symbol:time), `BlockingControl` (kill-path
`order/cancel/session`, 429 and `RateLimit` waited out), `SentWatermark` (`connection_lost` on the
order connection; no REST order entry, so nothing `sent_over_rest`), `StreamBookSync` with
`GeminiSyncTraits` (overlap rule) and the feed marking the snapshot frame, `LevelSpill` for deep
snapshots, `CountdownDriver` for the heartbeat (window 30 s, refresh 1/2). `cancelOnDisconnect` is
wired like Deribit's cancel-on-disconnect: a URL parameter, nothing to refresh. One order connection
carries order entry and `orders@account`. Shared changes: `net::ConnectionConfig::make_headers`
(headers built for every upgrade, a fresh nonce), `WsSessionHandler::accept_upgrade_with_headers`
(the fake checks the signed upgrade), `net::hmac_sha384_hex`, `FakeVenueServer::on_upgrade`.
Found on production: the btcusd book has asks at 1e10 to 9e12, past the 8-decimal fixed point; the
first version refused the snapshot as malformed. Such levels are now left out (`out_of_range`) without
marking the side cut.
Tests: `gemini.md_parser`/`md_feed` (4, on 205 recorded frames: every frame decodes, three books sync
with no resync, snapshot marking, overlap, stale drop, gap and resubscribe), `gemini.auth`/
`encoder`/`error_map`/`rest_decoder`/`private_parser` (7, RFC 4231 vector, documented payloads),
`gemini.venue` (11, fake server: config, reference data, key and account refusal exit 3, signed upgrade with
`cancelOnDisconnect=true` and a fresh nonce on reconnect, start-up sweep, order lifecycle with reject,
post-only expiry and cancel before the ack, a fill made while the order connection was down booked
once, funding once, heartbeat lapse kills, kill-path cancel-all through a 429, book gap resync),
`hotpath.noalloc: Gemini ...`.
Dry run on production public data, 15 min (btcgusdperp, ethgusdperp, btcusd): books 3/3 synced 2 s
after start, 25587 md messages, 0 resyncs, 0 malformed, 0 dropped, 0 reconnects, no ERROR; an
independent check of the recorded frames found no `U`/`u` gap and no stale frame; clock offset
-624 ms (the WSL clock). Not run: anything with keys.
Sandbox, with the user's sandbox keys (exchange account "primary", Singapore, test funds), spot
btcusd only. Found and fixed: (a) a time-based nonce must also increase: two REST requests in one
second got `InvalidNonce` ("Nonce '1790735176' has not increased since your last call"), which
failed the snapshot and the shutdown cancel-all; milliseconds are taken, so every nonce (REST and
the upgrade) is now venue-time ms, strictly increasing (one atomic counter). (b) `order.cancel`
refuses a numeric `orderId` (`-1013 Invalid parameters`; 183 cancel rejects in 40 s, the engine's
retry loop); it is sent as a string. (c) A refused placement comes back twice, a 400 reply and a
`REJECTED` event: one reject now (test fails without it). (d) With a perpetual configured, start-up
asks `/v1/positions`: an exchange account answers `AccountNotOfTypeRequired` and `fastmm-live` exits 3
("orders need the assessment": perpetuals need a derivatives account, opened after the Derivatives
Knowledge Assessment, support.gemini.com "How do I open a Gemini derivatives account?"; a perpetual
`order.place` from the exchange account is refused `-2010 InsufficientFunds` plus a `REJECTED`
event; `/v1/positions`, `/v1/margin` and `fundingPayment` answer `AccountNotOfTypeRequired`).
Verified: signed upgrade with `cancelOnDisconnect=true` and `orders@account`; `live.gemini`
(opt-in, `FASTMM_LIVE_TESTS=1`, sandbox hosts only): post-only placed and cancelled, an IOC buy of
0.0001 filled, a second connector's sweep and replay, the order's trades per `/v1/order/status`
equal to the fills booked. `fastmm-live` basic_mm: 24 orders / 24 cancels, clean stop; 180 s near
the touch, 3 maker fills (one partial); a restart restored -0.000124 BTC and `fastmm-ctl flatten`
sent one IOC that filled; kill -9 with two quotes resting and `cancelOnDisconnect` on: the sandbox
had cancelled both within 2 s; with it off they stayed, and the restart's start-up sweep reported
2 open orders of the earlier session and the engine cancelled them. Against the sandbox's own
record (`/tmp/gem/verify.py`, order status per order): 257 orders, no client id twice, 4 fills,
each equal to the venue's trades, nothing open; the account's BTC moved by exactly the fills.
Sandbox gaps: `/v1/mytrades` and `/v1/orders/history` return `[]` for this account (5 min polling,
also a REST-placed order), so the replay finds nothing there; the stream's fee `n` was 0 on every
WebSocket fill (a REST IOC was charged 0.033 USD). Replies are not ordered with the subscribe ack.
Open: perpetual order entry (no derivatives account yet); production `mytrades` paging (desc list,
forward walk-through); `fundingPayment` query vs payload; how fast `cancelOnDisconnect` acts without
a FIN; whether `orders@account` covers REST orders; `btcusdcperp` and `btcgusdperp` looked like one
book; the fee schedule (`configs/gemini-sandbox.toml` sets none).

## OKX spot and regional hosts, run on the demo (2026-09-30)

The OKX connector trades spot pairs (`BTC-USDT`) next to or instead of swaps, and `region` picks the hosts. `configs/okx-spot-demo.toml` ran on the demo of the user's spot-mode my.okx.com account.

API facts, read 2026-09-30 from <https://www.okx.com/docs-v5/en/> (Overview, "Regional API Domain Requirement"; Trade: place order, fills, fills-history, cancel-all-after; WS order channel; Account: positions), <https://my.okx.com/docs-v5/en/> (Overview: production and demo trading services) and <https://app.okx.com/docs-v5/en/>:

* Hosts. my.okx.com accounts use REST `https://eea.okx.com`, WebSocket `wss://wseea.okx.com:8443` and, for demo, `wss://wseeapap.okx.com:8443`; app.okx.com accounts `us.okx.com`, `wsus`, `wsuspap`; global REST is now documented as `https://openapi.okx.com` (`ws`, `wspap`). Demo REST is the production host with `x-simulated-trading: 1`. Measured: the demo key works on `my.okx.com` and `eea.okx.com`, and `www.okx.com` and `openapi.okx.com` answer 401 50119. Python's default User-Agent gets Cloudflare 403 1010 on all of them.
* Spot orders: `tdMode` `cash` (the non-margin mode; every account mode takes it); `sz` in the base coin; `reduceOnly` only for margin and net-mode futures and swaps; `posSide` "do not send" for spot; a market buy is sized in the quote coin unless `tgtCcy` is `base_ccy`. The WebSocket order takes `instIdCode` (demo BTC-USDT: 3; tick 0.1, lot 1e-8, `minSz` 0.00002).
* Fees: `fee` and `fillFee` are negative when charged. The fee currency is `feeCcy`: a buy pays in the base coin and a sell in the quote coin (measured). A fee is the fill times the rate, so it often has more than 8 decimals: 0.001680388 USDT on a 0.00002 BTC sell, and the docs' example is -0.00000192834 BTC.
* Balances and positions: `account/positions` takes MARGIN, SWAP, FUTURES and OPTION, but not SPOT. A spot holding is a balance (`account/balance`). As on Binance Spot, the engine's position is the strategy's: it is restored from the store (`restore_position`), then the execution replay from the store's last fill books what is missing. Reconciliation reports no position for a spot pair. The engine has no balance input, and the demo account started with 1 BTC and 5000 USDT, so the balance is not read into it.
* Replays: `fills-history` requires `instType` (and `fills` does not), so there is one replay stream per `instType` traded. `orders-pending` without `instType` returns every type.
* cancel-all-after applies to "all trading symbols through order book (except Spread trading)", spot included. Measured: a resting spot order was cancelled 10 s after arming, with `cancelSource` 20 "Cancel all after triggered", and `triggerTime` is in ms.
* `orders-history` with `begin` and no `end` returns rows oldest first, and `after` then pages backwards over the same rows. `fills` with `begin` is newest first, as the connector expects. The connector does not read `orders-history`.

Bugs found and fixed:

* A fee with more than 8 decimals made the whole `orders` push Malformed (`parse_notional`), so the fill was lost from the stream. On the demo this happens on every spot sell. The REST replay dropped the same fee silently (fee 0). Now `parse_fee` rounds half away from zero to 8 decimals in both paths. Tests: `okx.private_parser: a spot fill with a base-coin fee past 8 decimals is booked` (fails without the fix), a recorded sell frame, `okx.rest_decoder` spot fill, `venues.decimal: parse_fee`.

Demo verification (my.okx.com demo, spot mode, BTC-USDT at 0.00002 BTC, `basic_mm`, cancel-all-after 60 s), against OKX's `fills`, `orders-history`, `orders-pending` and `account/balance`:

| step | what happened | result |
|---|---|---|
| a. read-only | `account/config` acctLv 1 net_mode; balance 1 BTC, 5000 USDT; 0 open orders, 0 fills; private and order WebSocket login on `wseeapap` | ok |
| b. 10 min run | 183 orders, 38 fills, 145 cancels; clean stop, `cancel_all ok` | 38/38 tradeIds in the store once; 0 open; position -0.00000038 = OKX net fills less BTC fees = BTC balance change |
| c. kill -9 with 2 resting orders, restart 5 s later | position restored from the store; start-up sweep found the 2 earlier-epoch orders and cancelled them | 0 open after the sweep |
| c'. kill -9, one resting order filled while down, restart 48 s later | cancel-all-after cancelled the other (`cancelSource` 20); the replay booked the fill before the sweep; sweep found 0 | fill booked once |
| d. SIGTERM, and the end of `--duration` | `kill-switch cancel-all ok`, `cancel-all-after stopped`, exit 0 | 0 open |
| all 5 sessions | 327 orders, 86 fills | no duplicate clOrdId or tradeId; store position -0.00008082 = OKX net fills less base fees (exact; the 8-decimal rounding did not bite at minimum size) = BTC balance change |

Every cancel that races a fill gets 51400 "filled, canceled or does not exist", which is mapped to `VenueAction::Reconcile`, as Bybit 110001 and Binance -2011 are. Each race costs a fill replay and an `orders-pending` read, and the engine pulls its quotes between Begin and End. This is not wrong, but it is expensive when quoting at the touch (about once a minute here). A cancel reject for an order whose fill is already on its way needs no snapshot. Not changed: it is shared behaviour across the connectors.

Next: swaps on a futures-mode demo account (positions, balance_and_position, amend, bills are still docs-only); the `openapi.okx.com` default for `region = "global"` is from the docs and has not been run with keys.

**Coinbase: Advanced Trade and Exchange spot connectors (2026-09-30).** Two kinds, both spot, on
the shared machinery (ReconcileDriver, ReplayScheduler with order lookups, BlockingControl,
SentWatermark, StreamBookSync with LevelSpill). Choice, from the official docs read today:
(a) Advanced Trade (`coinbase_advanced`) is the API a Singapore individual gets (Coinbase
Singapore Pte. Ltd., MAS MPI licence; the SG user agreement covers "Advanced Trading" and "the
Coinbase API for Advanced Trading"), CDP keys with ES256 JWTs; its sandbox
(https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/sandbox) returns "static and
pre-defined" responses, so it runs against production only. (b) The Exchange
(`coinbase_exchange`) "allow[s] institutions to place orders" (individuals only through HNWI
onboarding) but has the only self-serve sandbox with real matching (subset of books, fake funds;
https://docs.cdp.coinbase.com/exchange/introduction/sandbox). (c) International Exchange: "Only
non-US based institutions"; its API stops trading on 2026-10-01. The Deribit claim holds:
"On October 1, 2026, Advanced Trade is moving international derivatives from INTX onto a
Deribit-powered gateway" (drb.coinbase.com, JSON-RPC 2.0, BTC_USDC-PERPETUAL, hard cutover,
open orders cancelled; https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/guides/
derivatives/overview). Whether a Singapore account may trade those perpetuals is not published
(no country list; coinbase.com/en-sg/derivatives-trading is a 404): check in the app. I built the
Exchange first for its sandbox; the user then supplied a live CDP key (EC, ES256) and asked for
Advanced Trade, which is now the primary. Neither has a venue-side dead man's switch on the path
used (Exchange: only FIX Logon 8013; Advanced spot: none).
Protocol facts found on the way: Exchange `level2_batch` is acknowledged as `level2_50` and sends
the whole book (22662 bids, 1.3 MB for BTC-USD), with no sequence number: the feed numbers book
messages itself and treats a skipped `match` trade_id, or a heartbeat `last_trade_id` still unseen
a heartbeat later, as loss (resync). Advanced `level2` snapshots are 4.6 MB (recv buffer 32 MB);
`sequence_num` counts every message of the connection, a gap resyncs every book. The Advanced
`user` channel reports order states with `cumulative_quantity` and no executions: the connector
reads `fills?order_ids=` when it grows and forwards each trade_id once (a fill arrives one REST
round trip late). `api.coinbase.com` sends more than 32 response headers (kMaxHttpHeaders now 64)
and both hosts refuse requests without User-Agent. A snapshot still arriving made the md
connection Stale (> 2 s silent) and the Stale handler resubscribed, fetching the book twice: an
unsynced book now keeps waiting. Found by the round-trip test: two executions in the same
millisecond, listed newest first, were sorted into the wrong order and one went out twice; now
dedupe by trade id.
Live, read only (`tests/venues/live_coinbase_test.cpp`, the user's key from
`~/crypto_quant/.env`, FASTMM_LIVE_TESTS=1): JWT accepted; `/accounts` 200 (2 accounts,
`{"accounts":[..],"has_next":false,..}`), open orders 200 (`{"orders":[],"sequence":"0",
"has_next":false,"cursor":"",..}`), fills 200 (`{"fills":[],"cursor":"",..}`), user channel
subscribed (snapshot with empty orders, then `subscriptions {"user":[<user id>]}` and heartbeats),
the connector's start-up sweep complete with 0 REST errors. Nothing placed, amended or cancelled.
Once, before any key was involved, I sent one unauthenticated POST to the Advanced *static*
sandbox (api-sandbox.coinbase.com) to see whether it echoes input; it returned its canned order.
Dry runs on production public data, `fastmm-live --dry-run --record-raw`, 15 min each, BTC-USD +
ETH-USD: Exchange 35990 md messages, books 2/2, 0 resyncs, 0 malformed, 0 dropped, 0 reconnects,
0 trade-id or heartbeat gaps; Advanced 35608 messages (31528 l2_data, 3178 market_trades, 899
heartbeats), books 2/2, 0 resyncs, 0 sequence gaps, 0 malformed/dropped/reconnects. Tests: crypto
(base64url RFC 4648, ES256 against the RFC 6979 A.2.5 vector and a PyJWT token), 7 Advanced unit
cases, 8 Advanced fake-exchange cases, 29 Exchange cases (unit, book sync, fake exchange), both in
the no-allocation suite, registry. Mutations: parked cancel not sent, per-order read not retried,
a read forwarding rows twice: each fails its test. Left: Exchange private side unrun (no sandbox
key yet); Advanced order payloads unrecorded (no funds, no orders allowed); Advanced REST rate
limits undocumented (client cap 8 new orders/s); `batch_cancel` maximum undocumented (50 used);
perpetual hedging on Coinbase needs the drb.coinbase.com JSON-RPC gateway (close to the Deribit
connector) once Singapore eligibility is known.

**End to end over veth is slower on this host today (2026-09-29).** `scripts/bench-e2e.sh`, kernel, busy, 3 x 60 s: wire to wire p50 51 to 74 µs, T0 to T5 p50 7 to 20 µs, against 23.6 to 25.6 and 2.4 to 2.6 on 2026-09-23. The 2026-09-23 `release-native` build and the 2026-09-26 `release` build measure the same today (57 and 51 µs, one 30 s run each), so it is the host, not the code; `BM_TickToOrder_Sim` (release) is unchanged at 151 ns p50. The published e2e numbers stay those of 2026-09-23.

## Benchmark history, moved from bench/README.md (2026-09-29)

bench/README.md now carries current results only. The measurements below were its dated sections.

### Hot-path changes, 2026-09-23

Before: 6d43d8e. After: the commits listed. `release-native`, pinned to one core, 5 repetitions,
median; ns per operation (p50 where the benchmark records one).

Both columns were measured with the harness as it was on that day, so they are comparable with each
other but not with the table above: `BM_TickToOrder_Sim` then included the simulator's outbound
SHA-256 in its timed region, and `BM_ItchL2Bridge_Message_DefaultBook` ran a working set that fit
L2. The two rows affected say so.

| benchmark | before | after | change |
|---|---:|---:|---|
| `BM_Ouch50_EncodeColdMap` p50 | 1770 | 12.8 | UserRefMap tables resident at construction (no page fault per new order), direct-mapped |
| `BM_Ouch50_EncodeNewIds` p50 | 26.6 | 14.8 | same, and the message written in place (no store-forwarding stall) |
| `BM_Ouch50_EncodeEnterOrder` p50 | 19.5 | 9.7 | in place, SWAR ClOrdID hex |
| `BM_Ouch42_EncodeNewIds` p50 | 14.8 | 7.4 | in place, SWAR ClOrdID hex |
| `BM_Encode_BinanceOrderPlace` | 1440 | 522 | HMAC key schedule done once; bulk JSON and query appends |
| `BM_Encode_BybitOrderCreate` | 313 | 223 | same |
| `BM_Encode_DeribitBuy` | 319 | 207 | bulk JSON appends |
| `BM_TickToOrder_Sim` p50, hash included | 991 | 247 | see below: most of this is the benchmark's own SHA-256, not engine work |
| `BM_EngineStep_Sim` (32 events) | 9200 | 2900 | running PnL totals (the max-loss check summed 256 positions per event) |
| `BM_ItchL2Bridge_Message` p50 | 61.4 | 55.3 | L3 tables on huge pages, prefetch of the named order |
| `BM_ItchL2Bridge_Message_DefaultBook` p50 | 82 to 86 | 55.3 | same; the working set was the small one, so this row says nothing about the large book |
| one-day synthetic backtest, wall time | 21.5 s | 17.4 s | same outbound SHA-256 |

End to end (`scripts/bench-e2e.sh`, settings as below), the builds interleaved, 3 runs of 20 s
each; p50 in µs, range over the runs. The OUCH encode row is the network thread's `encode` figure
from the `final order latency` log line.

| build | wire to wire | T0 to T5 | T0 to OUCH write returned | OUCH encode |
|---|---:|---:|---:|---:|
| before (6d43d8e) | 34.8 to 38.9 | 2.9 to 3.2 | 24.4 to 28.3 | 4.40 to 4.64 |
| after | 24.6 to 25.6 | 2.6 to 2.7 | 16.6 | 0.09 to 0.11 |
| after, PGO (`scripts/build-pgo.sh`) | 23.6 to 28.7 | 2.6 to 3.2 | 16.6 to 18.6 | 0.09 to 0.15 |
| after, PGO + BOLT (`--bolt`) | 24.6 to 25.6 | 2.7 to 2.8 | 16.6 to 17.6 | 0.07 to 0.09 |

### Build variants, 2026-09-23

Same machine, 3 interleaved rounds of 5 repetitions, median; ns per operation (p50 where
recorded). All gcc 13 unless noted; `release-native` is `-O3 -march=native` with LTO.

Measured with the harness of that day: the `BM_TickToOrder_Sim` row includes the simulator's
SHA-256 (about 180 ns of it) and the `_DefaultBook` row ran the small working set. The columns are
comparable with each other, not with the table at the top of this file.

| benchmark | release-native | PGO | PGO + BOLT | `-O2` | no LTO | `x86-64-v2` | clang 18 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `BM_TickToOrder_Sim` p50 | 247 | 215 | 215 | 231 | 247 | 271 | 271 |
| `BM_EngineStep_Sim` | 2877 | 2748 | 2751 | 2784 | 2896 | 3397 | 4182 |
| `BM_Encode_BinanceOrderPlace` | 522 | 462 | 464 | 553 | 513 | 570 | 541 |
| `BM_ItchL2Bridge_Message` p50 | 55.3 | 53.2 | 53.2 | 57.3 | 57.3 | 63.5 | 57.3 |
| `BM_ItchL2Bridge_Message_DefaultBook` p50 | 57.3 | 53.2 | 55.3 | 57.3 | 57.3 | 63.5 | 59.4 |
| `BM_Ouch42_EncodeNewIds` p50 | 7.4 | 7.4 | 7.4 | 7.4 | 8.7 | 7.7 | 6.4 |
| `BM_Ouch50_EncodeNewIds` p50 | 14.8 | 12.3 | 11.8 | 15.4 | 13.8 | 14.8 | 12.8 |

`release-native` stays gcc `-O3 -march=native` with LTO: `-O2` and no-LTO are within the noise, the
portable `x86-64-v2` build is 9 to 18 % slower, and clang 45 % slower on the engine step (faster on
the OUCH encoders). PGO gains another 4 to 17 % on the micro-benchmarks and nothing measurable end
to end, where the OUCH `write` system call dominates; BOLT adds nothing on top (the hot code fits
the instruction cache). `scripts/build-pgo.sh [--bolt]` builds both.

### Code alignment, 2026-09-26

Edits that execute nothing in a benchmark moved `BM_EngineStep_Sim` and `BM_TickToOrder_Sim` by 3
to 5 %. The test: base, an identical copy, and four edits off the benchmarked path (8 or 40 bytes of
`nop` in `Engine::on_funding`, 24 in `calibrate_tsc`, two `EngineConfig` members swapped), each
built with every setting; `bench_tick_to_order` pinned to one core, the binaries interleaved, 8
processes of 3 repetitions each. A variant's figure is the median over its processes of the
process's median; the table gives the median of the six variants in ns and their spread,
(max - min) / min. All gcc 13 portable `release` (`x86-64-v2`, LTO) unless noted; the alignment
rows set the flags on every target (`CMAKE_CXX_FLAGS`).

| setting | `BM_EngineStep_Sim` | `BM_TickToOrder_Sim` | `BM_TickToOrder_SimHash` | `.text` |
|---|---:|---:|---:|---:|
| `release` (gcc defaults, 16 B) | 2390, 3.8 % | 161, 7.4 % | 330, 5.4 % | 410 KB |
| `-falign-functions=64` | 2317, 1.3 % | 159, 4.9 % | 335, 3.9 % | 422 KB |
| functions 64, loops 64 | 2346, 2.3 % | 157, 8.5 % | 329, 5.0 % | 433 KB |
| functions 64, loops and jumps 32 | 2337, 3.0 % | 156, 1.2 % | 334, 2.3 % | 446 KB |
| clang 18 | 3448, 1.3 % | 178, 4.2 % | 342, 1.9 % | 413 KB |
| `release-native` | 1791, 6.3 % | 120, 1.8 % | 302, 3.9 % | 402 KB |
| PGO (`scripts/build-pgo.sh`, native) | 1685, 5.0 % | 105, 6.8 % | 282, 2.4 % | 361 KB |

The identical copy alone differed from base by up to 5 %: the process matters as much as the code,
so one run of each side says nothing about a 3 % change. PGO is the fastest and not stable: the
identical copy, profiled again, came out 7 % slower on the tick; it also needs an instrumented
build and a training run in every build that ships (CI, the release tarball, the wheels).

`FASTMM_ALIGN_CODE` (default ON, gcc) puts functions 64 and loops and jump targets 32 on the
fastmm targets only. Confirmed on four new edits (16 and 56 bytes in `on_funding`, 64 in
`calibrate_tsc`, 32 in `on_kill`), 18 processes per variant, a busier machine than above:

| setting | `BM_EngineStep_Sim` | `BM_TickToOrder_Sim` | `BM_TickToOrder_SimHash` |
|---|---:|---:|---:|
| `-DFASTMM_ALIGN_CODE=OFF` | 2730, 7.8 % | 188, 5.6 % | 393, 3.6 % |
| `FASTMM_ALIGN_CODE=ON` | 2610, 2.1 % | 182, 4.7 % | 393, 2.9 % |

The engine step no longer moves with layout and both tick benchmarks got faster or stayed. The
tick's remaining spread is the size of the process-to-process difference of one binary (2 to 3 %
here), so it cannot be attributed to layout; compare such changes over several interleaved
processes, not one run each.

The other benchmarks, `release-native` on against off, 8 interleaved processes: within 3 % except
`BM_L2_PriceForQty` -22 %, `BM_L2_ApplyDelta/20` -13 %, `BM_L2_ApplyDelta/100` -5 %, and
`BM_Json_BybitExecution` +10 %, `BM_L3_OverflowAddCancel` +5 %, `BM_L3_AddCancelExecMix` +4 %.

### End to end: fastmm-sim-itch to fastmm-live over veth

Measured by `scripts/bench-e2e.sh` on 2026-09-23: WSL2 (Linux 6.6, 8 cores), `fastmm-sim-itch` and `fastmm-live` in two network namespaces joined by a veth pair, `kernel` receive backend, `spin_mode = "busy"` in both processes, simulator on core 2, engine on core 4, network thread on core 6, no CPU isolation (`isolcpus` not set). BasicMM on FMAA and FMBB (`configs/nasdaq-itch-sim.toml`, `half_spread_bps = 1`), generator at `--speed 4`, ITCH on lines A and B, OUCH 5.0 over TCP. 3 runs of 30 s; each cell is the range over the runs, in µs. The wire-to-wire and OUCH rows have 275 to 358 samples per run, so their p99.9 is the largest sample. In runs 1 and 3 a few orders had T0 to T5 near 3 ms while every engine hop stayed below 1.2 ms at p99.9 (as in run 3 of the 2026-09-22 measurement); they set the upper end of the T0 to T5 and OUCH p99 columns.

| hop | samples per run | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| wire to wire: simulator `sendmmsg` to the order read | 312 / 354 / 275 | 23.6 to 25.6 | 61.4 to 139.3 | 76.1 to 217.1 |
| kernel receive timestamp to T0 | 137934 / 139164 / 139986 | 3.1 to 3.2 | 11.8 to 22.5 | 61.4 to 688.1 |
| T0 to T1: MoldUDP64, ITCH decode, L3 update | 79020 / 81569 / 82623 | 0.8 | 2.4 to 5.9 | 6.7 to 15.4 |
| T1 to T2: ring hand-off, L2 book apply | 79020 / 81569 / 82623 | 0.2 | 5.9 to 19.5 | 63.5 to 1179.6 |
| T2 to T3: strategy | 69044 / 69662 / 70143 | 0.1 | 0.2 | 0.2 to 11.3 |
| T3 to T4: quote manager, risk, OMS | 488 / 548 / 1112 | 0.4 to 0.5 | 1.3 to 1.7 | 1.6 to 49.2 |
| T4 to T5: outbound ring push (no eventfd write when busy) | 536 / 594 / 1243 | 0.2 | 0.4 to 0.6 | 0.6 to 1.5 |
| T0 to T5: tick to trade (engine) | 186 / 215 / 184 | 2.4 to 2.6 | 32.8 to 3183.9 | 46.0 to 3183.9 |
| T0 to OUCH write returned (network thread) | 316 / 358 / 279 | 16.6 | 52.8 to 3194.4 | 63.6 to 3194.4 |

The network thread writes all orders of one drain of the outbound ring with one `write`; the write (p50 7 to 9 µs) runs the veth and the simulator's TCP receive path inside the system call. With `spin_mode = "adaptive"` (one 30 s run, 2026-09-21) wire to wire is 98.3 / 262.1 / 263.7 µs and kernel to T0 21.5 / 53.2 / 163.8 µs (p50 / p99 / p99.9): the network thread wakes from `epoll_wait`. `af_xdp` was not measured (it needs root: `sudo scripts/bench-e2e.sh --backend af_xdp`).

`[engine] timer_slack_ns` with `spin_mode = "adaptive"` (2026-09-23, `--spin adaptive
--timer-slack N`, 2 interleaved runs of 20 s each, µs, range over the runs). The engine sleeps
50 µs when idle; the network thread waits in `epoll_wait`, which the slack does not delay, so
wire to wire does not change. In busy mode nothing sleeps.

| timer slack | wire to wire p50 | T0 to T5 p50 | T0 to T5 p99 |
|---|---:|---:|---:|
| 0 (the kernel's 50 µs) | 69.6 to 77.8 | 11.8 to 13.3 | 147.5 to 155.6 |
| 1 ns | 73.7 to 81.9 | 7.4 to 9.2 | 98.3 to 127.0 |

Order send path, 2026-09-22 (before the hot-path changes above), same machine and settings, 3 runs of 20 s per row (9 for the first and third), p50 in µs, range over the runs:

| network thread | wire to wire | T0 to T5 | T0 to OUCH write returned |
|---|---:|---:|---:|
| before: two writes per order (SoupBinTCP header, then OUCH message), one order after the other; eventfd write per wake | 47.1 to 55.3 | 4.6 to 5.4 | 30.3 to 39.1 |
| one write per drain | 32.8 to 34.8 | 4.9 to 5.1 | 24.4 |
| one write per drain, no eventfd write when busy | 32.8 to 34.8 | 2.7 to 3.1 | 23.4 to 26.4 |
| `IORING_OP_SEND` + `io_uring_enter` instead of `write` | 32.8 to 34.8 | 2.7 to 2.9 | 24.4 to 26.4 |
| `IORING_OP_SEND` with SQPOLL, thread on core 0 | 34.8 | 2.8 to 3.1 | 12.7 to 13.2 |
| `IORING_OP_SEND` with SQPOLL, thread unpinned | 163.8 to 172.0 | 2.9 | 12.2 to 13.2 |

The io_uring rows were a prototype and are not in the code: the plain submission costs what `write` costs, and SQPOLL only moves the send to another core (the call returns in 0.3 µs) without shortening wire to wire.

```bash
scripts/bench-e2e.sh --duration 30 --runs 3            # --backend af_xdp needs root
```

#### Receive backend

Same machine and settings, 2026-09-22 (load average 2 to 4), one 20 s run per cell and round, three rounds with the configurations interleaved; p50 in µs, range over the rounds. `dpdk` is EAL with `--no-huge --no-pci --in-memory` and the `net_af_packet` vdev on the veth. OUCH runs on kernel TCP.

| rx_backend | wire to wire | T0 to T5 | T0 to OUCH write returned |
|---|---:|---:|---:|
| kernel | 32.8 to 34.8 | 2.8 to 2.9 | 23.4 to 24.4 |
| dpdk | 30.7 to 31.7 | 2.8 to 3.1 | 24.4 to 25.4 |

`bench_order_tcp` isolates the send call over a veth (one thread, the server in a second namespace, 64-byte messages): `write` on a `TCP_NODELAY` socket 3.97 µs p50, 7.9 µs until the server's `read` returns. The kernel's TCP send path is about 0.5 µs of that; the rest of the ~23 µs OUCH write in the table is the veth and the simulator's receive path and wake-up, which run inside whatever system call puts the frame on the veth. A user-space TCP client over an `AF_PACKET` ring measured the same (7.7 µs until the server's `read`) and was removed. `af_packet` (for `dpdk`) is a copy of the kernel path, not kernel bypass: these numbers bound what the code adds, not what a NIC would give.

Kernel-bypass options:

| option | status | needs |
|---|---|---|
| DPDK receive (`rx_backend = "dpdk"`) | implemented; measured on virtio below | a NIC DPDK can own: AWS ENA (c6in, c7gn), Azure mlx5 (Accelerated Networking), GCP gVNIC, Intel E810/X710 |
| AF_XDP receive (`rx_backend = "af_xdp"`) | implemented; measured on virtio below | root or `CAP_NET_ADMIN`+`CAP_BPF`; zero-copy on ice, i40e, mlx5, ENA |
| Onload (`LD_PRELOAD`, TCP and UDP) | documented ([Low-latency TCP](../docs/how-to/operations/low-latency-tcp.md)), not measured here | AMD Solarflare (bare metal, colocation) |
| NVIDIA XLIO (`LD_PRELOAD`, TCP and UDP) | documented, not measured here | NVIDIA ConnectX, BlueField (Azure Accelerated Networking VMs, OCI bare metal) |
| F-Stack (DPDK + FreeBSD TCP) | not tried: owns the event loop and the port, hugepages | same NICs as DPDK |

### End to end across two hosts (Vultr VMs)

`scripts/bench-2host.sh`, 2026-09-23: fastmm-sim-itch and fastmm-live on two Vultr `vhf-2c-4gb` VMs in Tokyo (2 vCPU, Ubuntu 24.04, Linux 6.8, virtio_net), joined by a VPC (MTU 1450, ping round trip 0.27 to 0.85 ms). Unicast market data, `threading = "single"`, busy spin, the engine on CPU 1 and interrupts on CPU 0. `dpdk` binds the VPC NIC to `vfio-pci` (no-IOMMU) with the virtio PMD; `af_xdp` runs native with a socket on each RX queue. Three 30 s runs per cell; p50 in µs, range over the runs.

| rx_backend | wire to wire | T4 to T5 (encode + write) | T0 to T5 |
|---|---:|---:|---:|
| kernel | 327.7 to 344.1 | 27.6 to 47.1 | 61.4 to 69.6 |
| af_xdp | 327.7 to 344.1 | 30.7 to 59.4 | 77.8 to 81.9 |
| dpdk | 311.3 to 360.4 | 29.7 to 86.0 | 77.8 to 110.6 |

The VPC dominates wire to wire (p99 0.6 to 15 ms in every cell). OUCH runs on kernel TCP in every cell. The removed user-space TCP client, sending on the XDP socket and the DPDK port, measured 8.7 to 27.6 µs for T4 to T5 in the same setup and 278.5 to 311.3 µs wire to wire, inside the VPC's spread. The kernel backend's kernel-to-T0 was 10.8 to 15.9 µs p50 (61 to 107 µs p99), against 3 µs on the WSL2 machine above.


**Binance Demo, USD-M smoke and a spot + USD-M gateway soak (2026-09-29).** Scripts and logs in
`~/fastmm-usdm-soak`. Smoke: `fastmm-live` basic_mm on USD-M BTCUSDT at 0.002 BTC; 56 trades, each
booked once, store = venue = positionRisk; countdownCancelAll armed (a 75 s SIGSTOP: the venue
cancelled both quotes 60 s after the last refresh, the session killed the venue on resume) and
stopped at shutdown (an order placed after the exit survived 78 s). The smoke died of max_loss at
8 min: the reconciliation's positionRisk `entryPrice` "83954.61851851852" failed to parse and the
short was valued at an average of 0 (fixed, 7936228). Soak: gateway + X (xmm, spot BTCUSDT quotes,
USD-M BTCUSDT hedges, 0.002, max_unhedged 0.004) + M (basic_mm USD-M ETHUSDT, 0.04), a fault every
20 min (kill -9 X after a spot fill, 20 s SIGSTOP of the gateway, kill -9 of the gateway 90 s down,
kill -9 M). Two more bugs stopped the first runs: xmm halted on a hedge under USD-M's 50 USDT
minimum notional (a 0.0002 partial fill; 8d426d7), and a BTCUSDT depth resync cleared every USD-M
book in the engine and the gateway's copies, so M never got an ETHUSDT book (5dc3829; xmm's
hedge-venue md bit had the same stickiness). Third run, 2 h 28 min, all six faults: spot 3011
orders / 33 trades, USD-M BTCUSDT 25 hedge IOCs all filled, ETHUSDT 1482 orders / 1316 trades;
every trade booked once by its epoch's strategy, no duplicate ids, 0 open, positions = venue,
balances exact (`verify.py`: ALL CHECKS PASS). X's unhedged at most 0.00209 (a fill while its hedge
was out, ~75 ms), 0.0000453 at the end; the X killed 76 ms before its hedge reached the venue had
sent it, and its successor booked the fill and sent nothing. A gateway kill -9 left 12.6 MB of
rings per attached strategy in /dev/shm (fixed, 212d40a). Strategy RSS grows about 0.8 MB/min:
the journal's mapped file (46 MB after 72 min of M), not the heap; the gateway stays at 90 MB.

**Binance Spot Demo soak, 4 h, a fault every 30 min (2026-09-29).** Gateway + A/B sharing
BTCUSDT; cycling kill -9 of A, a 20 s SIGSTOP of the gateway, kill -9 of the gateway with 90 s
down (twice each, eight faults). Against the venue (`~/fastmm-soak/verify.py`): 10697 orders
(distinct ids), 852 trades each booked once by its strategy, 0 open, positions and BTC exact:
ALL CHECKS PASS. Resources sampled each minute (`resources.log`): gateway RSS at most 43.8 MB,
strategies at most 43.0 MB, none growing over a process's life; fds 15 (gateway) and 10
(strategy) from the first minute to the last; threads 6. ERROR lines are the expected ones (the
gateway went away; an order left at the venue by a kill -9, then swept).

**Binance Spot Demo, third run with the order lookup (2026-09-28, 47 min).** Same scenario after
the unacknowledged-order work: 1850 orders (distinct ids), 137 trades each booked once by its
strategy, 0 open, positions and BTC exact, unattributed 0 (`verify.py`: ALL CHECKS PASS). Three
open-orders snapshots in the first 11 s failed with -1021 (timestamp 1 s ahead: the WSL host clock
steps by up to 1 s every ~10 s); `ReconcileDriver`'s 5 s retry took the fourth.

**Binance Spot Demo rerun after the fix (2026-09-28, 47 min).** Same scenario, the gateway down
90 s after its kill -9 with orders resting. 4789 orders (all client ids distinct), 709 trades, all
booked exactly once by the strategy whose epoch they carry, 0 open at the end, each store's
position equal to its trades less BTC fees, the account's BTC exact (`verify.py`: ALL CHECKS
PASS). 4 fills landed while the gateway was down (2 of each strategy): each booked by its own
strategy after the restart; the gateway's unattributed stayed 0.

**Binance Spot Demo, gateway + two strategies sharing BTCUSDT, 45 min with faults (2026-09-28).**
The user approved a demo run (demo keys only; the USD-M demo wallet is empty, so spot only).
`fastmm-gateway` with `[gateway.shared."binance:BTCUSDT"]`, `basic_mm` A (1.5 bps) and B (3 bps).
Faults: kill -9 A and restart; SIGSTOP of the gateway for 20 s; kill -9 of the gateway (both
strategies exited within 1 s, all back within 8 s); kill -9 B and restart; clean stop.
Checked against the venue's own allOrders/myTrades (`~/fastmm-demo-run/verify.py`): 2165 orders,
2165 distinct client ids; 233 trades; 232 booked exactly once by the strategy whose epoch they
carry; no booked fill the venue lacks; 0 open at the end; the account's BTC moved by exactly the
trades less BTC fees; B's position equals its trades less fees. **One fill lost to its strategy:**
A's order (epoch 5) filled 2 s after the gateway's kill -9, while nothing ran; the new gateway's
connect-time replay booked it to the account as unattributed, and A's attach never claimed it
(A's position short by exactly that fill). Fixed: the attach carries the store's open orders
(Binance's myTrades names only the venue order id); see the entry above.

## Next direction (chosen 2026-09-28): one connector machinery, written once

**Why.** The five exchange connectors are ~9,500 lines, of which ~5,100 are the same machinery
written five times: execution replay, reconciliation, dead man's switch, blocking shutdown
requests, funding windows, reference data. The copies have drifted and the drift is where
recovery bugs live (the USD-M countdown stop was fixed in OKX's copy only). A read-only survey
(2026-09-28) found, among others: Bybit, OKX and Deribit never retry a failed open-orders
snapshot and drop a request while one is in flight; USD-M skips the start-up sweep of orders
nobody holds; only Spot sweeps its order shadows (the others leak up to 8192 entries);
`SentWatermark::connection_lost()` also forgets REST-sent orders, during exactly the outage they
are used in; Spot's replay counts emitted rows, not returned ones, for a full page, and can open a
REST connection during shutdown; USD-M and Bybit give up on 418/429 in the kill-path cancel-all;
USD-M's offline reference-data path skips its account checks (hedge-mode refusal).

**Plan.** Each extraction keeps the connector tests green and adds a test per closed divergence.
All three done (2026-09-28), entries below.
1. Done. `ReconcileDriver`: open-orders snapshot vs the OMS, generation, in-flight/again, retry,
   start-up sweep, shadow sweep, Begin/End, the exact flag. Venue hooks: fetch snapshot, rows.
2. Done. Blocking control helper (kill-path cancel-all, countdown stops, bounded 418/429 retry),
   `CountdownDriver` over `CountdownSwitch`, and `SentWatermark` losing only WebSocket-sent orders.
3. Done. `ReplayScheduler`: windows, cursors, edge ids, watermark settle, retry/sweep timers, shared by
   execution replay and funding. After 1.
Keep separate: order replies and amend semantics, instrument mapping, Bybit DCP and Deribit
cancel-on-disconnect, venue cancel-all bodies, Deribit's WebSocket transport.

**Performance: BM_TickToOrder_Sim +8 % since f5f7461, bisected (2026-09-29).** Found: 152.6 ns at
f5f7461 (the OKX merge) against 164.6 at main. Each first-parent merge built with the `release`
preset under `/tmp/bench-*` (same path length); `bench_tick_to_order` pinned (`taskset -c 2`),
interleaved, load < 1.5, 3 byte-identical copies of each binary, 3 processes each (median of the
file medians, the file range for t2o). "instr" is the instructions of one timed tick (push +
`Engine::step`, cancel of both quotes), counted by single-stepping it in gdb (no PMU under WSL2).

| first-parent commit | t2o ns | t2o+hash | engine step | instr |
|---|---:|---:|---:|---:|
| f5f7461 OKX merge (df175aa: same `.text`) | 155.6 (152.8-156.0) | 324.4 | 2290 | 2092 |
| cab2aa1 fees, headroom, venue health, feed-lag gate | 156.9 | 325.7 | 2297 | 2088 |
| d386044 execution view (QueueTracker) | 157.8 | 328.2 | 2304 | 2089 |
| 54f78f9 research merge (71708bf: same `.text`) | 157.3 | 329.7 | 2303 | 2089 |
| d2cea64 multi-venue backtest | 159.2 | 327.5 | 2319 | 2110 |
| e88e4d0 net limit per underlying | 160.8 | 329.6 | 2321 | 2117 |
| 8252b40 xmm recovery, restart gate | 167.8 (161.3-168.3) | 333.8 | 2319 | 2120 |
| eea4a24 shared instruments | 158.2 | 331.6 | 2333 | 2122 |
| 205f03b per-venue replace (5e35fc0: same `.text`) | 160.2 | 328.4 | 2337 | 2125 |
| bfbfd40 .. main (same `.text` as each other) | 164.3 (163.9-186.2) | 330.7 | 2331 | 2125 |
| this fix | 159.8 (157.5-161.4) | 326.9 | 2324 | 2109 |

The work added is 33 instructions per tick (+1.6 %): 21 in `SimTransport::send` (d2cea64: the
per-venue latency model is found by an instrument -> byte index -> `links_[k]` multiply, twice per
tick), 7 in e88e4d0 (gcc stopped inlining `record_md_hops`, which returns at once in the
simulator), 4 in `set_quotes` (cab2aa1's gate check), 3 at 8252b40 (`awaiting_`), 3 at 205f03b
(per-instrument replace flag), 2 at eea4a24 (the truncated-side test in `L2Book::apply_delta`);
cab2aa1 moved `on_book_delta` out of `step` and saved 4. The xmm/OMS work (095d5b8, inside
54f78f9) and the connector refactors execute nothing on the tick. The time is mostly layout, not
work: 8252b40 costs 7 ns with 3 more instructions and eea4a24 gives it back with 2 more; four
byte-identical copies of main's binary measure 159.7 to 164.0 ns and bfbfd40's build (26 bytes
differ from main's: its path) 157.7, so one file is not a sample of a build.
Fixed: `SimTransport` keeps a `Link*` per instrument (one load); `record_md_hops` tests for a wire
timestamp inline and records out of line; the market-data handlers test `queue_on_`, a copy of
`queue_.enabled()` next to the engine's flags, before the tracker's per-instrument heads. 2109
instructions per tick. Same method, 5 copies x 5 processes per side, base f5f7461 / main / fix:
t2o 154.1 / 163.3 / 159.2 ns (file medians 153.2-158.1 / 160.5-165.0 / 158.1-160.8), t2o+hash
323.4 / 330.0 / 327.1, engine step 2297 / 2336 / 2318. Golden and replay hashes unchanged (full
ctest). Tried and dropped: a hot `feed_gate_seen_` flag in front of the gate check (+17
instructions from register spills in `BasicMM::requote`), `on_book_delta` forced inline (+15:
`dispatch` went out of line). What remains (+17 instructions, about 1 ns; the rest layout) is the
gate check, the reconcile gate, per-venue replace, the book's truncation test and the sim's venue
lookup. `Oms_Submit` and `QuoteManager_Reconcile` pay for `OrderTimes` (a 32-byte slot per order
in its own array, written on submit, replace and ack, and copied into every `OmsUpdate`): kept,
it is what `ctx.order_times` and `OmsUpdate::times` read.

**A fill of an order the venue never acknowledged names its order (2026-09-29).** Left by the
entry below: an order whose ack never came before a kill has no venue id in the store, and a
replayed Binance trade names only `orderId`, so its fill named no order (shared: nobody's; owned:
the owner's, naming no order). Per venue: Binance Spot (`myTrades`) and USD-M (`userTrades`) give
`orderId` only; Bybit (`orderLinkId`), OKX (`clOrdId`) and Deribit (`label`) rows carry the client
id, no lookup needed. Fix, in `ReplayScheduler` (optional `Hooks::lookup` and the typed
`unnamed`): before a window's rows go out, the orders the connector cannot name are asked for, one
per order, at most `max_lookups` (16) per replay, and the window waits (`kQueryTimeoutNs` at most).
Spot asks `GET /api/v3/order?orderId=` (weight 4), USD-M `GET /fapi/v1/order` (weight 1), only
with rate-limit room; the answer goes into the connector's orderId map; an order not FastMM's is
not asked again. A row still unnamed goes out naming no order, as before, and a copy is kept (256
at most): the next replay (5 s later, 5 attempts) asks again and sends it once more naming its
order, same trade id, so the OMS and the account keep one; the gateway drops a parked unnamed copy
when the named one arrives. Replay rows own their text now (`binance::MyTradeRow`). Simulator: a
filled order that ended is still answered by `GET /api/v3/order` (`OrderIndex::past`), as on
Binance; `set_open_orders_delay_ms` holds `openOrders.status`. `wait_gateway_up` also waits for the
order channel's session.
Tests, each failing with the lookup disabled: `gateway_restart_test` "... never acknowledged ..."
shared (a first) and owned (b first): a's requote is placed with every reply swallowed and the user
stream muted, kill -9 of the gateway and a, the order filled while nothing runs, both restarted:
a's store holds the fill once naming it (disabled: shared never booked, account short; owned
booked naming no order). Connector: Spot "never saw acked names it" and "a failed order lookup is
asked again at the next replay", USD-M "never saw acked names it"; `replay_scheduler_test`: budget,
not-ours, re-send, 5 failures then dropped, a lookup never answered, a close while waiting.
The retag path has its test: "a streamed fill of a session nobody has claimed yet ..." (the new
gateway's snapshot held 8 s, a's order from before the restart filled meanwhile): the account books
it unattributed, and a's attach moves it to a's share. With the retag disabled it stays
unattributed and the test fails.
gateway_*, recovery_*, xmm_* (68) green 3 times; full ctest (werror) 1359 passed; clang-tidy-18:
no bugprone or performance finding in the changed code (the analyzer's NewDeleteLeaks on the new
REST callbacks, as on every existing one).
Then closed (same day): with a `primary` attached, a fill whose lookup failed went to the primary
unnamed and later, named, to its owner as well (two stores held it). Now such a fill carries
`OrderFillMsg::kUnresolved` (`ReplayScheduler::emitting_unresolved()` while it is emitted); the
gateway books it for nobody and parks it, routing it to no strategy even at an attach. The copy
naming its order drops the parked one and goes to its strategy (the account retags it); when the
connector gives up (5 attempts, or not FastMM's order) it sends a last copy without the flag, which
drops the flagged one and follows the primary/unattributed rule. An engine ignores the flag.
Test: `gateway_restart_test` "a fill whose order lookup failed first reaches its strategy only, not
the primary" (b primary attaches first, the simulator's first `GET /api/v3/order` answers 503,
`fail_next_order_queries`); every restart case now also checks no trade id is in both stores. With
the gateway's hold-back disabled: b's store holds a's fill (trade 13) unnamed, a + b != venue, the
account 0.005 against the venue's 0.004. gateway_*, recovery_*, xmm_* (69) green 3 times; full
ctest 1360 passed; lint ok; clang-tidy-18: nothing new.
Left: a flagged fill whose stream goes away (unsubscribe) stays parked for nobody; an instrument no
strategy ever owns has its executions kept, not booked.

**A fill made while the gateway was down reaches its strategy (2026-09-28).** Binance Spot Demo
run (`/home/rufus/fastmm-demo-run`, BTCUSDT shared, no primary): kill -9 of the gateway at
12:10:27 with demo-a's buy `fm0005000001de` resting; it filled at 12:10:29.2 (venue time, trade
311389073); demo-a's store never got it and the account showed `unattributed=0.0001998` to the end.
Root cause: Binance's `myTrades` gives the venue's `orderId`, not the client order id; the
connector names a replayed trade's order from its own orderId map, which a new gateway process
starts empty (its start-up sweep learns only orders still open). The fill named no order: epoch 0,
so on a shared instrument without a primary it was nobody's, and the epochs demo-a's attach sent
(1, 3, 5) were never consulted; on an owned instrument it went to the owner naming no order
(position right, the order's row left Live). It was booked at all because the account's "older
than the gateway" start was host time and the trade time venue time (offset +1.27 s): the fill,
0.8 s before the gateway started, counted as after it. Not the cause: the account's dedupe,
demo-a's replay start (its last fill), the "order events for no attachment" (the sweep's cancels).
Found on the way, same restart: (i) an owned instrument's account booked a fill another strategy's
replay met first, then its first owner's seed overwrote the position and the dedupe kept the fill
out for good (account short by it); (ii) `ReplayScheduler::resume()` from a second attach while
the first's replay ran replaced that start and joined the running replay, so after a failed one
the retry began at the later store's end and the earlier gap was never read (the demo's first two
`myTrades` failed with -1021, the retry ran from demo-b's start); (iii) connectors published the
clock offset their reference data measured only at their first timer tick after connecting.
Fix: (a) the attach request (protocol 7) carries the store's orders open at their last record with
their venue ids (`Recovery::past_orders`: newest first, at most 256, from the chain's last 64
sessions); the gateway names a fill that names no order by them (per venue, instrument and venue
order id) before routing it. (b) `VenueRouter::parked`: a fill no attached strategy receives
(epoch nobody has claimed, strategy detached, no owner or primary attached, owned instrument not
seeded yet) is kept, at most 4096 per venue (oldest dropped, both counted in the log line), and
routed again at each attach once its routes, seeds and past orders are in; a replay's copy of one
given that way is not given again (`Route::unparked`), and the strategy's OMS dedupe stays behind
it. One booked for nobody and claimed later moves to the claimant's share by the existing retag.
(c) An owned instrument's account books nothing before its first owner's store seeds it. (d) The
"older than the gateway" start adds the venue's clock offset; every connector publishes the offset
from its reference data. (e) `ReplayScheduler`: a restart while a replay runs goes on from the
earliest start at the next tick, as the same replay (`finished` after it).
Tests, each failing on the old code: `integration/gateway_restart_test.cpp` (kill -9 of the
gateway with a's order resting, the order filled while nothing runs, a new gateway up and swept,
then the strategies one after the other): shared with a first; shared with b first and the venue
3 s ahead (the demo's timing); owned with a first; owned with b first, venue 3 s ahead. Each: a's
store holds the fill once naming its order, no other store has it, stores = venue, the gateway's
position = venue with unattributed and unexplained 0, logged shares = stores. Old code: shared, a
never booked it (account short, or 0.002 unattributed with the skew); owned a first, booked naming
no order; owned b first, the account short by it. `replay_scheduler_test` "a restart while a
replay runs goes on from its start, as one replay" fails on the old scheduler. gateway_*, recovery_*,
xmm_* (65) green 3 times; full ctest (werror) 1350 passed; clang-tidy-18: no bugprone or performance
finding in the changed files.
Left: an order the venue never acknowledged before the kill has no venue id in the store, so its
fill stays ownerless (asking the venue for an unknown orderId would name it); a fill booked for
nobody that a store also holds (a clock error beyond the offset) counts twice; the retag path (a
streamed fill of an unclaimed epoch between a new gateway's connect and its sweep) has no test; an
instrument no strategy ever owns has its executions kept, not booked.

**Replay queries bounded: a query never answered fails after 30 s (2026-09-28).** Left by step 3.
What hung: `ReplayScheduler` had no deadline and `ReconcileDriver` none while it waits for the
replay (its 60 s bound covers the snapshot fetch only). A Deribit trade-history query lost on a
private WebSocket that stays up kept the replay active, the snapshot unasked, and after a restart
the engine (`await_reconcile`) sending nothing until the connection dropped. REST: a written
request fails after `http_timeout_ms` (5 s) and so did the replay, but `HttpClient` had no timer
while connecting: a peer that accepts TCP and never answers the ClientHello held every queued
request (replay, snapshot, listen key) for good, and a black-holed SYN for the kernel's ~2 min.
Fix: (a) `ReplayScheduler::kQueryTimeoutNs` = 30 s per query, checked from `on_timer`: the query
is `failed(q)`, the replay ends incomplete, the retry follows 5 s later, a late answer fails
`expects(q)`. 30 s: above the REST timeout, which fails a REST query first; the scheduler's bound
is for the WebSocket. (b) Deribit's reply id named only the currency, so a late answer was taken
for the retry's query; the id now carries a per-query number. (c) `HttpClient` bounds the connect
and the TLS handshake by `timeout_ms`. HA: a failed replay already gave a non-exact snapshot (the
REST behaviour), the engine's gate opens on it and counts `estimated_reconciles`; the timeout now
does the same. Kept: Deribit's snapshot carries the venue's positions, so what is estimated is the
fill attribution of the gap, not the position, and the retry books the missed fills when the
venue answers (the OMS dedupes by trade id); staying idle would hang the session on one lost
reply. Tests failing on the old code: scheduler "a query never answered fails after
kQueryTimeoutNs", Deribit "a trade-history query never answered ends the replay incomplete" (no
End before; the engine gate is checked with a real `Engine`; with the deadline but the old ids,
the late T1 was booked for the retry), http "a TLS handshake the peer never answers times out".

**Docs followed as written, from a fresh clone (2026-09-28).** Pages: getting-started (install,
quickstart), the tutorial (1-9), operations (gateway, deploy, fastmm-top, operate, journals,
query, kill switch, runbook), strategies (xmm, python-live, register), backtesting on Binance
data. Venues were `fastmm-sim-exchange` or `--dry-run`; systemd pages ran as user units
(kill -9 of the gateway: restarted, the strategy reattached and restored its position).
Code fixed, each with a test: a latched `max_loss` kill was read after reference data, so with the
venue unreachable a restart exited 4 (restarted by the unit) instead of 6, and the docs' "before
contacting any venue" was false; `fastmm-backtest --journal-out` did not create its directory
(the journals page's command exited 5); `fastmm-pnl` without `--engine` said only `open
runs/fastmm.db: unable to open database file`, now names the stores it finds; the tutorial's
`first_mm` logged `quoting disabled (dry run)` in every live session (quoting is off until the
start-up reconciliation), now logs `quoting on/off` from `on_quoting`. Docs fixed: the Binance day
backtest stops at 09:37 on `max_loss = 500` (the page read its 36.9 % uptime as the spread);
`fastmm-pnl` examples lacked `--engine`; deploy's `fastmm-top` lacked `--name` and its
`--clear-kill` line would start a keyless session (now: remove the kill file); the 0.2.0 tarball
holds only live, top, replay and sim-itch; runbook's `/opt` path lacked `-x86_64`; the gateway
intro lacked the simulator and keys, and its unit needs the gateway named `gateway`; stale
outputs (tutorial 7-8, cl_ord_id format). Not runnable here: Docker (no docker), Binance Demo
keyed sessions, `host-setup.sh tune`. The first full ctest had 21 gateway/recovery failures while
a reconfigure relinked the test binary under it; all pass rerun.

**Order shadows: swept in send order, a full table refuses (2026-09-28).** Left by step 1: the
sweep compared ids of the watermark's epoch only, so behind the gateway every other strategy's
shadows whose end was lost stayed in the 8192-slot table (7168 usable) for good. What a full table
did, per connector (inserts ignored `assign()`'s result): the order went out untracked, its replies
had no instrument (Spot, Bybit, OKX, Deribit), a later amend of it was refused as unknown; a
Bybit/OKX amend with no room went to REST and came back "no order channel"; a Deribit order without
a shadow reports its first partial fill with leaves 0; a USD-M modify with no room was sent and
acked, and the venue's later fills (still under the first client id) were booked to the replaced id
with the cumulative quantity not rebased (seen with the refusal removed: fill for fm000100000001,
cum 0.0007, after the ack of fm000100001c02). Design: `SentWatermark` numbers every New and
Replace it notes (connector-local, all engines), `mark()` returns the watermark id and its
sequence, each shadow stores `sent_seq` (`last_seq()` in the send path: one increment and one store
per order), the driver sweeps by sequence. An order or replace the table has no room for is refused
as `RejectReason::OrderTableFull` (39), marked answered so it holds no snapshot back, counted in
`VenueStatus::shadows_refused`, one ERROR when the table fills and one WARN when it has room
(`ShadowOverflow`, checked from `on_timer`). `VenueStatus::shadows` is the table's size; the
once-a-second venue line prints `shadows= swept= refused=`. Size kept at 8192: with the sweep the
table holds working orders plus replaces in flight; one engine has at most 4096, venues cap 200-500
per symbol, and overflow is now a refusal. Aliases (link id -> engine id) are at most one per live
shadow, so they cannot fill first. Tests failing on the old code: driver "several engines' shadows
are judged by send order", USD-M two epochs swept, gateway_shadows_test (two raw clients, 4 rounds
of 60+60 orders cancelled with the answers swallowed and the user stream muted: 120 shadows, 0
after the snapshot; old: 60 left after round 1, 180 at round 2), full-table refusal for all five
connectors (7168 orders through each fake). Left: a shadow whose end was lost stays until the
next snapshot (reconnect, the engine's request). `bench_tick_to_order` (sim transport, so no
connector code; base main at a path of the same length, werror release, taskset -c 2, 2 x 12
interleaved runs, load under 3): `BM_TickToOrder_Sim` 159.5/159.1 -> 161.2/159.4 ns,
`BM_EngineStep_Sim` 2337/2327 -> 2333/2344 ns (medians): noise. Connector tests and integration
recovery_*, gateway_*, xmm_* (61) green 3 times; full ctest (werror) 1338 passed; clang-tidy-18:
no bugprone or performance finding in the changed src files.

**Connector machinery step 3: `ReplayScheduler` (2026-09-28).** `venues/replay_scheduler.hpp`
runs the execution replay of all five connectors and the USD-M income and OKX bills funding
queries. It owns the streams (a symbol, a currency, the account), windows (length, history floor,
Deribit's history window ending where the recent one starts), paging (oldest first from the newest
row or the id after the highest; newest first by the venue's token, a window re-read up to its
oldest row after 20 pages), the watermark and the keys read at or after it, the known ids of a
restart, one generation (`close()`, `abort()`, `expects(q)`), retry, sweep and `due_in()`. Hooks:
ready, venue time, `query(q)` answered by `answer(q, page)` or `failed(q)`, `emit(row)`, `finished`.
Endpoints, parsing and what a row becomes stay in the connectors. Lines, venue cpp + hpp: Spot
-166 +103, USD-M -322 +192, Bybit -245 +140, OKX -343 +163, Deribit -194 +109; scheduler +584.
Decisions: (a) Watermark: moves to the end of what a replay read in full, never past the replay's
start less a settle margin (60 s, OKX its 5 min), rows found or not; the rows read at or after it
are known by key. The margin is for an execution the history shows only after the query ran,
with a time before it. Before, Bybit, OKX and USD-M jumped to the newest row (a late row older
than it was never asked for), Bybit and USD-M funding stayed put when nothing came (a quiet account
asked from its connect time for ever, in 7-day windows after a week), OKX and Deribit settled only
the empty case. Cost: the last margin's rows are read again each replay (on Bybit and OKX a page
more at over 100 fills a minute); Binance's fromId cursor is not affected. (b) Retry 5 s after the
replay ended, OKX's rule: the others asked again on the next tick, into the rate limit that may
have failed it. (c) A page is full by the rows returned (Spot counted forwarded ones, so a known id
made a full page pass for the last), and the next page is asked in the same replay (Spot and USD-M
waited for the retry). (d) Spot's 24 h is a window, not a lookback: an older start walks 24 h
windows (it was clamped to 24 h ago, never exact, the trades before lost); the encoder takes
`endTime`. (e) Deribit's recent query starts after the history window: no overlap, no per-replay
seen set. (f) Generation: step 1 had closed Spot's REST connection at shutdown, but an aborted
myTrades reply still counted as an error and scheduled a retry; every reply now checks
`expects(q)`. (g) Funding has its own scheduler: with every reconciliation's replay, once a minute
(it rode the execution sweep), 1 s after a stream event. Tests: scheduler units (10); failing on the
old code (6): Spot full page with a known id and the second page failing (old: exact, no second
page), Spot 24 h windows, Spot aborted reply not an error, Bybit no retry on the next tick, Bybit
and USD-M funding empty replays move the watermark. Changed tests: the Spot harness's exchangeInfo
`serverTime` is now (the 2026-09-13 fixture put the replay start 15 days back); Bybit, OKX, Deribit
and USD-M funding "next query starts at the newest row" now check "no later than" (settle); Bybit's
retry test ticks 6 s later and its window ends at start + 7 d - 1 ms; OKX's second funding bill is
dated now, not 15 days back; Deribit's history test has no overlap. Full ctest (werror): 1330
passed; integration recovery_*, xmm_*, gateway_*, store restart and gateway funding (63)
green 3 times; clang-tidy-18: no bugprone or performance finding in the changed files. Left: a
Deribit query never answered keeps the replay active until the connection drops (as before; closed
above); Binance allows 10 pages a symbol per replay, so a long outage is read over several retries 5 s
apart.

**Connector machinery step 2: blocking control, countdown driver, watermark (2026-09-28).**
(a) `BlockingControl` (venues/blocking_control.hpp): one BlockingHttp from the connector config,
one request or one per target, a 418/429 (plus Bybit retCode 10006, OKX 50011) retried up to 3
times after Retry-After or 400 ms, never waiting past a 10 s deadline for the whole call. Used by
the five `cancel_all`s (OKX's paged batch and Deribit's per-instrument GET keep their bodies) and
both countdown stops. Before, only Spot retried; USD-M and Bybit left orders resting on a 429.
(b) `CountdownDriver` (dead_mans_switch.hpp) over `CountdownSwitch`, used by USD-M and OKX.
Decisions: a refresh round counts only when every part is confirmed (USD-M's countdown is per
symbol, and one refused symbol still has its old countdown running; it used to count as armed if
any symbol answered), measured from when the round went out. A lapse is reported once, the
connector kills the venue, and nothing is refreshed after it until a reconnect: the venue's own
timer is left to clear the account whatever the local cancels do (USD-M used to re-arm on the same
tick). The shutdown stop is sent once any refresh went out, confirmed or not, since a lost reply
may have armed it (OKX stopped only a confirmed one, USD-M always). A fatal error other than a
lapse no longer stops OKX's refresh: the switch is the backstop for exactly that case.
(c) `SentWatermark::connection_lost()` settles only the WebSocket-sent entries (a `rest` flag per
entry, set by `sent_over_rest()` where the four REST fallbacks queue an order); REST-sent orders
keep holding the snapshot back. (d) USD-M's offline reference data now runs the clock probe and
`account_checks` (hedge-mode refusal); Bybit, OKX and Deribit log the exception they swallow when
offline data is allowed. `rest_channel_config()` / `rest_queue_for()` size every connector's REST
queue as 32 + 4 per subscribed symbol (was 8, OKX 32, Deribit 264), resized on a later subscribe.
Tests, each failing with its piece reverted: USD-M and Bybit kill-path 429 then success (and a
limit that never lifts gives up after 4 requests); USD-M no refresh after a lapse, one symbol's
refused refresh kills; OKX stop after an unreadable arm reply; Spot: order connection dropped, a
reconciliation's replay held on REST, an order queued behind it over REST, the connection back,
then the snapshot's watermark (taken after the replay, ReconcileDriver) stays below that order
(WS API on a second fake server so it answers while REST is held); USD-M offline hedge-mode
refusal; `CountdownDriver` units. With ReconcileDriver taking the watermark after a replay over
the same REST connection, a REST order sent before the replay is answered first; the flag matters
for one sent during it. `bybit_linear.venue: order round trip` now waits for the trade connection
(`BybitVenue::order_channel_live()`). On top of step 1: integration recovery_*, xmm_*, gateway_*
(60) green 3 times; full ctest (werror) 1316 passed; clang-tidy-18: no bugprone or performance
finding in the changed files.

**Step 1 done: `ReconcileDriver` (2026-09-28).** `venues/reconcile_driver.hpp` is the open-order
snapshot of all five connectors: generation, one at a time with one more queued, retry 5 s after a
failure and 60 s without an answer, start-up sweep, shadow sweep, Begin/End, sent watermark,
`kExecutionsExact`, closed by `disconnect()` before the REST reset. Hooks: `fetch_snapshot` (REST
pages, or WebSocket requests for Spot's `openOrders.status` and Deribit), `replay_executions`,
`shadow_ids`/`drop_shadow`; rows through `add_order`/`add_position`; `transport_lost()` when the
connection a fetch went out on drops. Lines, cpp + hpp: Spot -121 +68, USD-M -146 +101, Bybit
-159 +115, OKX -123 +77, Deribit -89 +124 (+55 for its positions request); driver +357.
Watermark rule: taken when the snapshot is asked for, after the replay (Spot and Deribit took it
at the request): an order answered while the replay ran is then judged too, and it is what
`SentWatermark::value` describes. The survey held, except that USD-M did find orders nobody holds
at start-up (its first user-stream snapshot); it judged this session's orders with a real
watermark while doing so. Deribit now has a positions leg (`private/get_positions` per currency, a
row per subscribed instrument, absent = flat): its caps say `positions`, and nothing on its private
stream reports one, so a delivery, liquidation or other client's trade never reached the engine.
Divergences closed, each with a test that fails on the old code: Deribit retries a failed snapshot
(Bybit and OKX share the path); USD-M's first snapshot is a sweep (empty watermark, an earlier
session's order reported, the engine's Oms cancels it); Spot's shutdown during a replay opens no
REST connection; USD-M and Deribit drop the shadow of an order whose end was lost; Deribit's
positions. Driver unit tests: coalescing, replay first, retry, transport loss and timeout,
close/open generation, sweep, shadow sweep, row order. The Deribit end-to-end test now counts the
two position rows. Integration recovery_*, xmm_restart, gateway_* (67 tests) green 3 times; full ctest
(werror): 1306 passed. clang-tidy-18: no bugprone or performance finding in the changed files.
Left: the shadow sweep only compares ids of the watermark's epoch (ids of several engines behind
the gateway are not in send order), so a gateway with several strategies still leaks the others'
shadows; a send-order stamp on the shadow would fix it. `bybit_linear.venue: order round trip`
failed once under a parallel run, 60 runs alone pass: the test waits for the sweep, not for the
trade channel, and an order sent before that is Live goes over REST, which the fake does not
serve. The race is older than the driver (same steps before the snapshot).

**Replace per venue (2026-09-28).** The quote manager replaced only if every venue the engine
traded could (the gap left by quote/hedge step 1): one venue without replace, say an IOC hedge
venue, sent cancel + new on all of them. Now the engine resolves each instrument's venue at start
(`QuoteManager::set_replace`: a per-instrument `replace` bool, resolved with `supports_replace` at
start, replaces the `params_.supports_replace` test);
`[engine] supports_replace = false` still turns it off everywhere. Replay: the journal already had
`replace_venues` per venue; new session journals set header bit `kHeaderReplacePerVenue`, and one
without it replays with the old all-or-nothing rule (`QuoteParams::replace_all_venues`), so old
multi-venue journals still verify. Single-venue runs cannot differ: golden and replay hashes
unchanged. Tests: a two-venue engine test (replace on venue 0, cancel + new on venue 1; the
override; the old rule), `multi_venue_test` now asserts replaces on a and none on b, and a replay
of that session, plus the old rule recorded, replayed without the bit (ok) and with it (mismatch).
Benchmarks (release, base 19ebc2e at a path of the same length, 12 interleaved runs, taskset -c 2,
load under 1): `BM_TickToOrder_Sim` 159.4 -> 159.7 ns, `BM_EngineStep_Sim` 2344 -> 2349 ns (medians).
A first version testing `supports_replace && !no_replace` in the hot path was +7 ns on tick to
order (GCC inlined `cancel` differently), hence the precomputed flag. Full ctest: 1292 passed.

**Long depth updates, the two items left (2026-09-28).** (a) Deribit lists book levels best first
in snapshots and changes: every one of 2114 snapshot sides and 104726 change sides with two or more
levels, production 100ms books of 108 instruments (every BTC/ETH future, 80 BTC options, resubscribed
every minute) plus BTC-PERPETUAL through `fastmm-live --dry-run --record-raw` (20 min, 0 resyncs).
The docs do not say. The largest change had 121 levels a side, snapshots up to 1210 bids. A side
past 1024 now goes through `LevelSpill` like the others (selection by price, so the order is not
relied on), for changes and snapshots; past 16384 it counts as `dropped`. Cost, 1200 bids + 850
asks: 108 us against 99 us for the old cut snapshot (the extra 176 levels are parsed now), and a
change that long used to resync the book; short changes unchanged (636 / 640 ns). Test: the
recorded 1195-bid snapshot as a fixture, and a change made of its levels, then one chained on it
(fails before: Overflow, resync). (b) The gateway's 1024-level books: parsers set
`EventHeader::kTruncatedBids/Asks` (bits 6, 7) on a delta side they cut, and `L2Book::apply_delta`
drops its levels behind the last one that side carries (cold path behind one flag test;
`BM_L2_ApplyDelta` 269 / 270 ns). Trim rather than a depth mark or a resync: the copy stays exact
up to the carried range, no consumer has to know about it, no book time lost; a no-op for the
engine's 256 levels (argument in `level_spill.hpp`). `gateway.cpp` unchanged. Test: 25 new bids
between the best and 1000 deletes, the 1000th left out; before, the gateway kept that bid and an
attaching strategy got it. Full ctest (werror): 1276 passed.

**Several strategies on one instrument behind the gateway (2026-09-28).** Before: an attach
claiming an instrument a live attachment traded was refused (asserted by `gateway_test`,
`gateway_multi_test`); still so unless `[gateway.shared."venue:symbol"]` names it (optional
`primary = "<engine name>"`). Decisions:
(a) Routing. Own orders by the client order id's epoch, unchanged. A fill of an earlier session's
order goes to that session's strategy by `[engine] name`: the gateway keeps the epochs it gave
each name, and the attach request (protocol 6) carries the store's session epochs
(`Recovery::session_epochs`, last 64), so a restart after kill -9 or after a gateway restart gets
its own (after a gateway restart it did not on Binance, whose replay names no order: see "A fill
made while the gateway was down" above). Events naming no order (liquidation/ADL, orders placed elsewhere, funding) go to the
primary, else the account alone. The venue's position records go to no strategy: the engine sets
its position from them, and on a shared instrument they are the account's. One attachment per
`[engine] name` (a name is a store and a share).
(b) Positions. The account's is the venue's, as before. Every execution the account books counts
once towards its strategy's share or towards nobody (unattributed); a strategy's store joins the
account the first time it attaches in a gateway run (summed, where an unshared instrument keeps
its first owner's). One booked for nobody that later reaches a strategy (its session claimed after
the fill) moves to it, via a tag in the account's dedupe window (not exercised by a test).
unexplained = account − shares − unattributed, nonzero when a venue position record disagrees:
status v12 (position: shared, traders, unattributed, unexplained; attachment: its instruments, as
there is no single owner), `fastmm_account_unattributed`/`_unexplained`, a WARN line a second.
(c) Self-trade: refused at the gateway (`GatewaySelfTrade`) when an order would trade with another
strategy's resting (GTC/Day) order. Holding needs timers and state for an order that is stale by
then; venue STP alone differs per venue and, on one account, cancels the other strategy's quote
(the simulator's CancelMaker did exactly that in a first run). `RestingOrders`
(core/self_trade.hpp), a linear scan: 0.8 / 1.0 / 3.2 / 11.1 ns with 0 / 2 / 8 / 32 resting
(`bench_self_trade`); an unshared instrument skips it. Test 1 hit it once for real: a's skewed
quote would have crossed b's.
(d) Risk: `[gateway]` limits already sum over strategies; per-strategy `[risk]` in each engine.
(e) Detach and restart: unchanged; tested below.
Evidence: `integration/gateway_shared_test.cpp` (quiet simulator, every fill made by the test):
two strategies trading, stores = own fills, venue = a + b + an outside trade left unattributed,
the gateway's shares = the stores; kill -9 of a while b trades, restart, nothing booked twice; a's
order filled while a was dead (gateway SIGSTOPped so it cannot cancel first) is booked by a's
restart, never by b; a raw third client's crossing limit buy, limit sell and market order refused,
a non-crossing one rests and is cancelled on its detach; an execution naming no order booked by
the primary alone. `gateway_funding_test.cpp` (fake Bybit linear): funding without a primary booked
once by the account and by no strategy; a venue position no store holds reaches no strategy and
shows as unexplained (status and log). Each fails with its piece broken (5 mutations: no routing
by past epoch or to the primary, no check, no attribution, funding to a trader, a position row to
a trader). The 7 new cases 10 times each in a row (all passed); the integration suite 3 times at
-j8 (below); full ctest. Gateway t2t (`scripts/bench-gateway.sh`, new `--shared`; adaptive, 45 s,
base 1eb45c6 and this built at the same path length, interleaved, load under 2): gateway p50
engine/wire 36.9/70.3 µs for base (6 runs), this (6) and this with BTCUSDT shared (3), one run
each of base and this at 66.4 wire; wire p99 105-121 (base), 109-133 (this), 105-109 (shared).
Left: no real venue; the venue's position is compared only where the connector reports one (not
spot: there the account is its seeds plus the fills); after a gateway restart a strategy that has
not reattached leaves its old sessions' fills unattributed until it does (wrong as written: they
were never re-assigned, and a Binance Demo run lost one; fixed, see "A fill made while the gateway
was down" above). `fastmm-ctl --gateway pull --instrument` on a shared instrument reaches every strategy trading it.

**Every connector on production public data (2026-09-28).** `fastmm-live --dry-run --record-raw`,
6 connectors in parallel, 15 min each, load 1 to 6, before (r1) and after (r2) the fixes. Configs
were the demo/testnet ones with production hosts (`/tmp/aw/p-*.toml`, not kept).

| connector, instruments | md msgs r1 / r2 | resyncs r1 / r2 | other |
|---|---|---|---|
| binance spot BTCUSDT ETHUSDT | 381k / 335k | 0 / 0 | largest depth side 1009 |
| binance_usdm BTCUSDT ETHUSDT | 1.00M / 927k | 14 / 0 | 17 / 2 updates past 1024 levels a side (max 2144) |
| bybit linear BTCUSDT ETHUSDT | 128k / 132k | 0 / 0 | |
| bybit spot BTCUSDT ETHUSDT | 81k / 86k | 0 / 0 | |
| okx BTC- ETH-USDT-SWAP | 143k / 130k | 6 / 0 | updates up to 612 / 800 levels a side |
| deribit BTC-PERPETUAL, 4 options 30SEP26 84000/84500 | 15k / 16k | 0 / 0 | did not start before fix (a) |

No reconnects, malformed or dropped frames anywhere; every book synced within 1 s of start.
Clock offsets 8 to 116 ms (the WSL host clock also steps by a few ms every 10 s; by 0.8 to 1.8 s at
load 38 in the earlier xmm recording). Found and fixed:
(a) `settlement_mix()` treated an inverse and a linear contract as different currencies even when
both settle in BTC, so `configs/deribit-testnet.toml` (perpetual + options, `max_loss` set) exited 3
at start. Only the currency counts now.
(b) The USD-M resyncs, including the 5 of the 30-minute xmm recording, were ours, not Binance's and
not load: the raw frames have no `pu` gap. In a burst one 100 ms update changes more than 1024
levels a side (`kMaxBookLevelsPerMsg`); the parser refused it (`Overflow`, counted as `ignored` and
never shown), and the next update's `pu` did not chain. OKX the same past 512 a side. The parsers
now keep the levels nearest the touch (`LevelSpill`; exact for books up to 512 deep, the engine's
are 256, argument in `level_spill.hpp`); a side past 16384 levels still overflows and now counts as
`dropped`. Pausing the process 1 to 5 s (SIGSTOP) left no gap in the raw stream.
(c) A resync dropped the update that revealed the gap, so a snapshot taken inside it looked older
than the buffer (`SnapshotTooOld`) and waited out the 2 s request interval: 2.1 s without a book
against 85 to 240 ms (median 92) for the other USD-M resyncs (OKX's resubscribe: 170 to 400 ms).
That update and, after a gap in a replayed buffer, the updates behind it now start the next attempt.
(d) Resyncs were logged nowhere (the journal has only the reason code): every sync now logs its
reason with the update ids, and the book's return with the time it was away.
Left: Deribit deltas past 1024 levels a side still overflow (its snapshots are cut to the best);
the gateway's 1024-deep `AccountBook` can keep a stale level behind its 512th after a cut update.

**Quote/hedge step 4 done (2026-09-28): xmm through a real crash.** `integration/
xmm_restart_test.cpp`: `fastmm-live` as a child, two in-process simulators outliving it, SIGKILL
(1) with the hedge IOC executed at the venue but its reply and execution report held (sim ack
delay), (2) with a quote left resting and filled while down, with and without an earlier hedged
round, (3) behind `fastmm-gateway`, the strategy killed with its hedge unanswered and the held
events reaching the gateway after the restarted strategy attached, (4) `[risk.underlying.BTC]
max_net = 0.0015`, the down-time fill left unhedgeable (hedge venue rejects) while the test fills
every quote at once. Venue facts: one hedge order per maker fill, venues net to zero, store
position per venue = venue position, no exec id stored twice, no repeated client order id,
|net| never past max_net; the gateway's account ends at the venues' positions too. Three bugs, each
seen failing first: (a) restored positions were matched by symbol only, so BTCUSDT on two venues
got each other's (hedge venue restored +0.001: 3 hedges); `Recovery::PositionState` now names the
venue. (b) A session that died before storing a fill left no resume point, so the next one
replayed from its connect and lost the down-time fill; recovery now reads the chain of the engine's
sessions: per venue and symbol the newest recorded position (a fill row counts, as the store can
commit a fill without the position row after it), per venue the newest session with a stored fill,
and a venue with none replays from the newest clean shutdown's start (else the oldest) less 10 s.
(c) After a restart each venue's replay books on its own thread, so a quote fill replayed before
the hedge venue's replay made xmm hedge twice (and risk checked stale positions). A live session
now sends nothing and keeps quoting off until every venue that replays executions has finished its
first reconciliation (`EngineConfig::await_reconcile`, journal header field, orders refused
`NotReconciled`); `reconciling_` is a per-venue mask; xmm does not hedge while
`ctx.reconciling()`. Evidence: 10 of 10 runs of the 5 cases in a row (47 s each), 3 copies x 3
runs in parallel with the other 111 integration tests (all passed), store and xmm unit cases,
full ctest. Not measured: the extra branch in `submit_new` on the benchmarks.

**Quote/hedge direction done (2026-09-28), CI green.** Steps 1-4 on main: multi-venue backtest,
`xmm`, `[risk.underlying]`, recovery through kill -9 mid-hedge (three bugs fixed, see the entry
below). The restart gate (no order before every venue has reconciled) costs nothing measurable:
6 interleaved pinned rounds, both built at the same path length, t2o 160.6 / 160.7 ns, t2o+hash
332.8 / 326.6, engine step 2333 / 2320. Test harness: children of a failed integration case are
killed at exit (15 orphaned gateways up to 45 h old had kept load at 30-40). Next: every connector
against its production public data (dry run), then choose the next direction.

**xmm on 30 minutes of real Binance USD-M + Bybit BTCUSDT (2026-09-28).** Recorded with
`fastmm-live --dry-run` against production public streams (both books synced; 733k and 101k md
messages; USD-M 5 resyncs, Bybit 0, no reconnects, machine at load 38; reason not checked).
Backtest `l2_queue`, `md_arrival = recorded`, Binance 1 ms, Bybit 1 ms or 35 ms (Tokyo to
Singapore), 0.001 BTC. Retail fees (Binance maker 2, Bybit taker 5.5 bps): quotes 10.5 bps out,
0 fills. Market-maker fees (maker -0.5, taker 2): edge 0 / 0.5 / 1 bps gives 15 / 6 / 5 maker
fills, net -0.16 / -0.04 / -0.01 USDT at 1 ms and -0.26 / -0.09 / -0.06 at 35 ms (10 s markout
+0.17 / +0.20 / +0.51 bps at 1 ms, negative at 35 ms). Fills come when the Binance book sweeps
through the quote; the hedge races Bybit following it (one fill: +21.9 USDT/BTC at 1 ms, a loss at
35 ms). Every run hedged each fill once and ended flat. Too few fills to judge the strategy; the
engine question (two venues, hedging, latency per venue in a backtest) is answered.

**USD-M could not start on production (fixed 2026-09-28).** Its exchangeInfo (no symbol filter) is
1.1 MB, past the 1 MiB receive buffer the blocking client inherited from the streaming client.
Every earlier run was on Demo, whose list is smaller. Blocking requests now take 16 MiB.

**Flaky under load.** `integration.gateway: kill -9 of one strategy ...` failed once at load ~30
(2026-09-27); 10 of 10 alone and 4 x 101 integration runs in parallel passed.

**Quote/hedge step 1 done (2026-09-27): backtests across venues.** Every venue an instrument names
is simulated with its own latency model (order, ack and md paths), wires, `md_arrival`, replace and
STP (`[backtest.venues.<name>]`, `SimTransportConfig::venues`); books, matching engine and queue
model stay shared (every queued order belongs to one instrument, so venues never touch each
other's queues). The first venue
keeps `seed`, so single-venue runs are unchanged: every golden and replay hash is byte-identical.
Feeds merge by event time with `;` (`--data "a; b"`, Python lists). Equity per instrument
(`equity.csv` `pnl_<id>` ..., `result.equity_by_instrument`). Evidence: a two-venue test checks every
order, ack, fill and md event against its venue's latency; merge order; determinism; config errors;
a two-venue no-allocation run. `BM_TickToOrder_Sim` 150.8 -> 156.0 ns (+3.5 %, 10 interleaved
processes; first version with heap-allocated venues was +17 %, now inline), `BM_EngineStep_Sim` and
the matching benchmarks within 2 %. Open: the engine's quote manager replaces only when every
traded venue supports it (global `QuoteParams`), so a per-venue `supports_replace = false` turns
replace off for all; the synthetic generator drives instrument 0 only.

**Quote/hedge step 3 done (2026-09-27): net position per underlying.** `[risk.underlying.BTC]
max_net = 0.5` (base units) in the engine, `[gateway.underlying.BTC]` in the gateway's account
(every strategy and venue). An instrument counts towards the underlying its `base` names
(case-insensitive): `qty * contract_multiplier`, inverse `qty * multiplier / mark` (mark = last
valid mid, stale with `stale_md_ms`); options excluded (a delta needs a model). Worst case as
`max_position`: net + every same-side open order over the underlying's instruments + the order
(a replace excludes its leaves); refused only when |worst| > max_net and > |net now|. An inverse
contract with a position or open orders and no mark refuses every order on the underlying
(`UnderlyingMarkUnknown`; gateway `GatewayUnderlyingMarkUnknown`); a flatten is not checked.
`UnderlyingPlan` (core/underlying.hpp) is built after reference data loads: fixed tables indexed
by instrument, at most 8 underlyings; a base no instrument has exits 3. Runtime: `fastmm-ctl
limits underlying.BTC.max_net=...` → `ControlCommand::SetUnderlyingLimit` (journaled, replays;
only for an underlying the config names, `max_net = 0` tracks one without a limit). The gateway
publishes per instrument the leaves working per side and the mark (atomics, like `qty`); `GwOrder`
now tracks leaves, and a reconciled order gets its real side (it was Buy). Status version 11
(net per underlying, engine and gateway; 9 gateway refusal counters), metrics
`fastmm_underlying_net`/`_max_net`, `fastmm_account_underlying_*`. Evidence:
`core/underlying_test.cpp` (plan, conversions, RiskEngine incl. replace/stale mark, engine across
four venues incl. OKX contracts and an inverse contract, reducing orders, runtime limit),
`config_test.cpp` (round trip, errors), `integration/gateway_underlying_test.cpp` (two strategies:
b refused while a's orders work, b's netting side passes after a's fill), control socket, status,
Prometheus, no-allocation engine step with a limit. Bench (`release` + werror, WSL2, pinned,
interleaved x6, quiet machine; base, an identical base build, this built at the same path length):
t2o 155.3 / 159.2 / 161.4 ns, t2o+hash 334.9 / 332.9 / 334.4, engine step 2303 / 2403 / 2331;
new `BM_TickToOrder_SimUnderlying` (a limit set, the check runs on every order) 161.0, the same as
without. Within the identical-copy spread. A variant passing the inputs by pointer (RiskInputs 16
bytes smaller) measured worse (179 / 350): layout, not work. Not done: balances/collateral in the
base coin are not counted; options' delta; the gateway limit is not adjustable at run time.

**Quote/hedge step 2 done (2026-09-27): built-in `xmm`.** Quotes one instrument, hedges on another
with IOCs; the hedge comes from the two positions in base units, one at a time, never from a count
of fills. Guards: stale or invalid books and a hedge venue down pull the quotes, `max_unhedged`,
failed hedges back off and N of them halt (cleared by a new `restart` value), an unreported outcome
holds hedging `uncertain_hold_ms`. Found on the way, in the OMS: Bybit's `order` topic can end an
IOC with its cumExecQty before the `execution` topic delivers the fill. The end booked the
quantity as a synthetic fill and the live execution was booked again (only replayed executions
corrected the estimate), so the position counted it twice and xmm would have hedged the phantom.
A live execution whose quantity lies under the booked cum_qty (or, without a cum_qty, arrives
after the order ended) now names the estimate. OKX carries fills on the orders push itself, so it
is hit only when pushes arrive out of order; the same rule covers it. Evidence: OMS unit cases,
Bybit and OKX fake-exchange cases (end first, then executions: booked once; each fails without the
change), xmm unit and engine-harness cases, and a live session against two simulators with a hedge
lost in a hedge-venue drop (one hedge per maker fill, venues net to zero). Not verified: any of it
on real venues; profitability, basis behaviour and hedge delay need the multi-venue backtester.
Configs: `configs/xmm-demo.toml`; docs: `docs/how-to/strategies/xmm.md`.

## 2026-09-26: live Binance Spot sessions from AWS Tokyo, and what strategies can now see

**Sessions.** `lead_mm` on BTCU (0 maker fee for the account), priced off BTCUSDT / UUSDT, from
c7i.large in ap-northeast-1a (apne1-az4; TCP connect 0.53 ms to ws-api/stream vs 1.7 / 2.3 ms from
1d / 1c; api.binance.com is CloudFront, 1.7 ms). SBE market data, ws-api orders, Ed25519.
Session A (3 h, improve one tick): 2088 orders, 234 fills, net -0.149 U. Session B (3 h, join the
touch): 1726 orders, 161 fills, net -0.037 U. `cancel_all` ok, exit 0, 0 reconnects, both.
Journals and scripts: `~/fastmm-aws/research/` on the dev host.

**Latency (session A).** Send to venue transactTime 0.41 ms p50; first ack 1.39 / 2.48 / 41 ms
p50/p90/p99; cancel ack 1.17 / 1.58 / 32 ms. The execution report arrives before the ws-api reply
in 79 % of orders, and 0.06-6.8 ms before the public trade that filled us. SBE vs JSON, same trades,
both recorded by fastmm-live: SBE first in 80 %, 0.65 ms earlier at the median (a Python JSON
recorder had shown 1.3-3.3 ms: its own overhead). Engine tick-to-trade p50 5.9 us.

**Congestion.** Excess feed lag (recv_ts - exch_ts over its baseline) explains log ack latency
with R^2 0.41 (t 18.9); realised volatility adds nothing (R^2 0.03 alone). Around slow acks the
lag goes 0.2 -> 36 ms within 100 ms of a price burst and is gone in ~300 ms. Fills while lag > 5 ms:
1 s markout -0.31 bps vs -0.005 (95 % CI of the difference [-0.59, -0.06]). The arrival-based
`stale_md_ms` never fires then: messages keep arriving, each tens of ms old. -> `max_feed_lag_ms`.

**Backtest vs live.** `fill-check` bounded orders by receive time and matched 0 of 13 live fills
(the public trade arrives after our execution report); on venue time it matches 233/233 (session
A, 6 false of 1847). Session B, orders behind others: 109 of 135 before the BookTicker queue cap,
133 of 135 after (2 false). Strategy re-run on session A's own journal with our orders stripped:
229 fills vs 234 live, realized -0.113 vs -0.118 U.

**Fixed on the way.** fill-check on venue time; `journal:...,strip_own=1`; pnl_report and
`fastmm report` booked replayed execution reports twice; inside-the-touch fills were counted as
behind; `latency_ack_us`; the reconcile watermark counted an unanswered order as sent (13:10:59:
an order cancelled as unknown; Binance Spot, USD-M, Bybit, Deribit, OKX); bounded shutdown (a
stop exits within 60 s, a second signal after 5 s at once, exit 5).

**Added for strategies.** `own_qty` / `best_ex_self` (the feed includes our orders live, not in the
sim), `queue_ahead` (the l2_queue model, capped by a newer BookTicker), `order_times`, `fees`
(`fetch_fees` on Binance Spot), `risk_headroom`, `venue_health`; `[risk] max_feed_lag_ms`;
`[backtest] md_arrival = "recorded"`. Not done: spot balances (a new event type per venue), a book
quality flag (`is_valid`, `last_update` and `on_connection` cover it). Hot hooks do not see them.

**For the quote/hedge plan below.** `xmm` can take the hedge cost from `ctx.fees`, the hedge
venue from `venue_health`, and the quote leg's queue from `queue_ahead`; a multi-venue backtest
should keep per-venue `md_arrival` and the ack latency split.

**Step 4 done (2026-09-26): OKX v5 USDT-margined swaps (`kind = "okx"`).** Modelled on Bybit
linear. OKX docs and changelog read 2026-09-26; three recent changes the connector follows: the
book `checksum` is deprecated (0 since 2026-06-23; the seqId chain is the check, a non-zero checksum
is still verified against a shadow of the level texts), WebSocket order operations take
`instIdCode` and ignore `instId` (2026-03/04), and a crossing post-only order is accepted, then
pushed `canceled` with cancelSource 31 (booked as expired). Quantities are contracts
(`contract_multiplier` = ctVal x ctMult; notional and PnL linear in USDT). Net mode only: long/short
or spot account mode exits 3. Amend is acked from the orders push (`amendResult` under `reqId`, the
new client id), not from the reply. Funding from `account/bills` type 8. Dead man's switch
`cancel-all-after` (60 s, refresh 20 s), stopped on disconnect over a blocking connection. New
generic key `api_passphrase`. Evidence: 33 test cases (unit, fake exchange, no-allocation, exit 3 at
the process level); 32 mutations of the covered code each make their test fail. The public stream
ran against the demo and production hosts (book synced, 0 resyncs, checksum 0 on both; demo
instIdCode and tickSz differ from production). Untested against the venue (no keys): login, every
private payload and reply, account/fills/bills REST, cancel-all-after on demo.

**Flaky under load, found on the way (2026-09-26).** `integration.recovery: shadows of orders ...`
placed an order before the user stream was back after a drop (3 of 16 under load); it now waits for
both connections (16 of 16). `binance_usdm.venue: countdownCancelAll ... stopped on shutdown` failed
once in a full run: `disconnect()` queued the stop on the REST channel and reset it, which drops a
queued request. It now sends the stop over a blocking connection, as OKX does (35 of 35, 5 runs).

**Performance: code alignment on by default (2026-09-26).** `FASTMM_ALIGN_CODE` (ON, gcc): the
fastmm targets get `-falign-functions=64 -falign-loops=32 -falign-jumps=32` (`.text` +4 %).
Test: base, an identical copy and four edits that execute nothing in the benchmark (nops in
`on_funding`, `calibrate_tsc`, `on_kill`; two `EngineConfig` members swapped), built per setting,
`bench_tick_to_order` interleaved and pinned, 8 to 18 processes per binary. Median of the six
variants, ns, and their spread (max - min) / min:

| setting | EngineStep_Sim | TickToOrder_Sim | TickToOrder_SimHash |
|---|---:|---:|---:|
| `release` today | 2390, 3.8 % | 161, 7.4 % | 330, 5.4 % |
| functions 64 | 2317, 1.3 % | 159, 4.9 % | 335, 3.9 % |
| functions 64, loops 64 | 2346, 2.3 % | 157, 8.5 % | 329, 5.0 % |
| functions 64, loops and jumps 32 | 2337, 3.0 % | 156, 1.2 % | 334, 2.3 % |
| clang 18 | 3448, 1.3 % | 178, 4.2 % | 342, 1.9 % |
| `release-native` | 1791, 6.3 % | 120, 1.8 % | 302, 3.9 % |
| PGO, native | 1685, 5.0 % | 105, 6.8 % | 282, 2.4 % |
| second set of edits, busier machine: OFF | 2730, 7.8 % | 188, 5.6 % | 393, 3.6 % |
| same, `FASTMM_ALIGN_CODE=ON` | 2610, 2.1 % | 182, 4.7 % | 393, 2.9 % |

The engine step no longer moves with layout and the engine benchmarks got faster. Elsewhere
(`release-native`, on against off): L2 book -5 to -22 %, `BM_Json_BybitExecution` +10 %, L3 +4 to
+5 %, the rest within 3 %. What remains on the tick is the size of the process-to-process
difference of one identical binary (2 to 5 %), so a 3 % change needs several interleaved
processes per side to mean anything; the funding measurement below had one. PGO is the fastest but not stable (the identical copy re-profiled: +7 % on the tick) and
needs instrumented builds plus training in CI, the tarball and the wheels: not adopted.
`-fno-semantic-interposition` and LTO were already on. Details: "Code alignment, 2026-09-26" in the benchmark history above.

**Performance: the hot path is sensitive to code layout (2026-09-26).** Booking funding cost
BM_EngineStep_Sim +3.5% (2263 → 2338 ns) and BM_TickToOrder_Sim +3–5% with no new work on the
benchmarked path: moving the funding state off the hot data did not recover it, and an edit that
executes nothing in the benchmark (naming padding in a store record) reproduced it (2370 ns). Base
rebuilt at the same path length stayed at 2258 ns, so it is not build noise. Accepted, because funding
is a correctness fix. Next performance item: make the build insensitive to layout (function
alignment, `scripts/build-pgo.sh`), measured, before layout drift accumulates.

**Open after the alignment change (2026-09-26).** `BM_Json_BybitExecution` +10% with
`FASTMM_ALIGN_CODE` on (venue decode, off the engine path; not investigated); the budgets in
`bench/ci_budget.toml` are not re-measured. (The gateway tests failing in a deep worktree were the 107-byte AF_UNIX path limit, which the
gateway already refuses at startup with exit 3 and the path in the message.)

## Next direction (chosen 2026-09-27): quote on one venue, hedge on another

**Why.** The common crypto market-making setup is to quote on the venue that pays or charges less
for making and hedge each fill at once with a taker order on the deepest venue. The live engine can
already trade two venues from one strategy (split threading, gateway included), but nothing prices
from another venue's book, nothing hedges, the backtester models a single venue with one latency,
and risk cannot net BTC on one venue against BTC on another except by summing all notional.

**Plan.**
1. Backtest across venues: the sim runs several venues in one run, each with its own latency,
   fees, replace and STP settings; recorded feeds from two venues merge by timestamp.
2. A built-in `xmm` strategy: fair value is the hedge venue's book plus a tracked basis; maker
   quotes on the quote venue are that fair value minus/plus the edge and the hedge cost. The hedge
   target is derived from positions (quote position plus hedge position in base units, contract
   multipliers applied), not from a count of fills, so a restart, a dropped fill or an uncertain
   hedge outcome converges to the same place. One hedge IOC in flight; a stale or down hedge venue
   pulls the quotes; `max_unhedged` pulls the side that would grow the gap.
3. Risk per underlying: `[risk.underlying.BTC] max_net = ...` in base units, across venues, in the
   engine and the gateway's account book. Done 2026-09-27 (top).
4. Evidence: backtest on recorded Binance USD-M + Bybit public data; kill -9 and venue drops in the
   middle of hedging against fake venues (no lost hedge, no double hedge, limits hold).

## Next direction (chosen 2026-09-26): a crypto desk can run on this

The gateway's first version is complete. Next, what a crypto market-making desk would hit first. Step 1
is accounting across settlement currencies: a desk holds BTC-settled Deribit options next to USDT
perpetuals, and today one session or gateway with instruments in two settlement currencies refuses
`max_loss` (fail closed; inverse PnL itself is correct, `Instrument::inverse_pnl`). Design:
* `reporting_currency` and, per other settlement currency, the instrument whose mid prices it
  (`BTC = "binance:BTCUSDT"`). Positions, PnL and fees stay in their own currencies; totals,
  `max_loss` and the exposure caps are converted at the current rate.
* A rate that is unknown or stale is unknown: an order that increases exposure in that currency is
  refused. Same accounting in the engine and the gateway; backtest and replay share it.
Step 2, Bybit linear perpetuals: done 2026-09-26 (mock and docs only, no testnet keys).
Step 3, funding: done 2026-09-26 (below).
Step 3 funding: done 2026-09-26 (USDⓈ-M, Bybit linear; Deribit has aggregates only). Alerting:
done 2026-09-26 as shipped Prometheus rules (`deploy/prometheus/fastmm-alerts.yml`, checked against
the exporter by a test), plus systemd units for the gateway and its strategies (a gateway crash
brings the strategies back via WantedBy; tested). Step 4, OKX swaps: done 2026-09-26 (above;
public data against the venue, private side mock only).

**Step 3 done (2026-09-26): perpetual funding is booked.** `EventType::Funding` (27) /
`FundingMsg` (128 B: signed amount in the settlement asset, venue id, venue time, kReplayed) on the
order-event ring, so it is journaled and replays; journal format unchanged (v3: records are
self-describing, an older reader skips the type), old fixtures replay byte-exact. Engine: realized PnL
of the instrument (not a fee: carry of holding the position, paid by a position that never trades;
fees stay the trading cost), `PositionTracker::on_funding` with per-instrument/total/per-currency
funding (part of realized), converted by `[accounting]`, `max_loss` checked at once; booked once per
(venue id, instrument), 4096-entry window; an asset other than the settlement currency or an unknown
instrument is counted (`unbooked_funding`), not booked. No strategy hook (realized PnL is readable).
Store schema 4: `funding` table, `funding_raw` in positions/pnl_daily/views/sessions;
`fastmm-pnl funding`, funding column in `pnl`/`positions`/`recover`; Python `store.funding()`.
A restart's resume point counts stored funding as venue events, known as `funding:<id>`.
Gateway: routed to the instrument's owner (same replay/known filter as fills), booked once by
`AccountBook`. Binance USD-M: `/fapi/v1/income?incomeType=FUNDING_FEE` is the source (tranId); the
`ACCOUNT_UPDATE` FUNDING_FEE (symbol in `a.S` since 2026-08-07, no id) only triggers a query 1 s
later; the query also runs with every execution replay (own watermark, edge ids, 7-day windows,
retry). Bybit linear: `execType Funding` rows from the topic and `execution/list`, amount =
-execFee (sign inferred from the transaction-log page; the execution pages say nothing). Deribit:
not booked (continuous accrual, no per-payment event). Docs read 2026-09-26.
Evidence: `core/funding_test.cpp`, `account_book_test.cpp`, `store/funding_store_test.cpp`,
USD-M/Bybit venue and parser tests, `integration/funding_replay_test.cpp` (live sim session with
funding tripping max_loss replays to the identical outbound hash), `integration/gateway_funding_test.cpp`
(gateway + strategy processes on a fake Bybit linear: owner and account book one payment delivered
twice, store holds one row). Each fails with its piece broken (26 mutations: dispatch, dedupe,
max_loss check, asset check, conversion, record, store rows/day roll-up/resume/v3 reader, account
dedupe, gateway routing/account/owner, USD-M stream flag/replay query/edge ids/known ids/retry,
Bybit stream/sign/replay/known ids/order-field relaxation).
Release, WSL2, interleaved x6 (base, base rebuilt at a path of the same length, this): t2o
153.6 / 150.7 / 158.5 ns median, t2o+hash 324.3 / 323.8 / 328.4, engine step 2263 / 2258 / 2338.
Bisected with the headers copied onto base one at a time: the step's +3.5% and most of t2o's
+5 ns come and go with edits that change no executed code in the bench (two named fields in place
of `PositionRecord::pad_`, which the bench never writes, reproduces step 2370 vs 2252), so it is
code layout, not work; funding state lives off the hot lines anyway (heap `FundingBox` in the
tracker, `FundingStats` and the dedupe window behind one pointer at the end of the engine).
Left: no testnet run (Binance income row shape for FUNDING_FEE and Bybit Funding on the WS topic
are undocumented); stream-missed funding is booked up to a minute late; Deribit funding; the status
file shows funding only inside realized; pnl_report counts it as cash.

**Step 1 done (2026-09-26): `[accounting]`.** `reporting_currency` and `[accounting.fx] BTC =
"venue:symbol"` (an instrument of `[[instruments]]`, `enabled = false` if untraded; a USDTBTC-style
pair is inverted). `FxPlan` (`core/fx.hpp`) is built after reference data (inverse is only known
then): each instrument's currency slot, each currency's source. `PositionTracker` keeps totals per
currency and converted ones (updated where the totals change: fill, mark, new rate; out of line);
its `total_*`/exposure accessors are in the reporting currency, so max_loss, the caps, the status
file, the store's session totals and the kill file carry are too. The engine takes a rate from the
source's book mid; `RiskEngine` converts the order's notional for the caps. Fail closed: a rate is
unknown until the source's book is valid and not current while it is invalid (disconnect, crossed)
or older than `stale_md`; then an order adding exposure in that currency is refused
(`FxRateUnknown`, gateway `GatewayFxRateUnknown`), reducing ones and flattens pass. Booked PnL stays
at the last valid rate; a currency never priced counts as zero (only a restored position can be
there). Only while max_loss or a cap is set; with neither the rate refuses nothing. Uncovered
currency: exit 3 with a limit set, warning and no conversion otherwise; without `[accounting]`
everything is as before. Gateway: per-venue totals per currency in the Account, rates published by
the source's venue thread, converted when read (net PnL, caps, kill file, log `in=USDT`); status
segment v9 (a 7th refusal slot). Backtest ledger converts at the venue mid; replay rebuilds the plan
from the journal's table (a replay without `[accounting]` of such a journal mismatches, tested).
Evidence: `core/fx_test.cpp` (plan, orientation, tracker linear+inverse, gate unknown/stale/down,
max_loss tripping on a BTC loss only after conversion, an engine with BTCUSD inverse + ETHUSDT +
BTCUSDT source), config tests, `backtest/accounting_test.cpp`, `integration/gateway_fx_test.cpp`
(sim BTCUSDT + ETHBTC: gateway refusal and start, gross cap in USDT, max_loss on a BTC fee,
`GatewayFxRateUnknown` at stale_md 1 ms, fastmm-live in-process). Each fails with its piece broken
(13 mutations: no per-currency booking, no gate, no staleness, no inversion, no engine rate, no
disconnect, gateway not converting / ignoring the plan / order notional / staleness, ledger, replay,
session). Release, WSL2, interleaved x6, base 3bfc065 vs this: t2o 152.0 vs 153.5 ns (p50 151 both),
t2o+hash 326.2 vs 322.7, engine step 2258 vs 2295, risk check 6.52 vs 6.49, OMS 106.5/28.4 vs
106.6/28.3. Inlining the conversion into `mark` first cost t2o +6%; it is out of line now.
Left: `[gateway] max_open_notional` stays per venue in settlement currency (unconverted); the
status file does not name the reporting currency; the rate is the source's mid, not the venue's
conversion.

**Step 2 done (2026-09-26): Bybit linear perpetuals**, `[venues.x] category = "linear"` on the
Bybit connector (default spot, unchanged). Built from the v5 docs read 2026-09-26
(instruments-info, create/amend/cancel, open-order, cancel-all, execution, position, switch
position mode, DCP, private order/execution/position/dcp, rate-limit, ws/connect); no testnet key,
so mock and docs only. Reference data: LinearPerpetual only, perpetual, multiplier 1, reduce-only,
quote = settleCoin (what `[accounting]` settles in). Orders: positionIdx 0, reduceOnly. Hedge mode:
Bybit has no mode getter; `position/list?symbol=` returns positionIdx 1/2 rows in hedge mode, so
start-up refuses it (or an unreadable mode); `Venue::refused_account_settings()` makes fastmm-live
and the gateway exit 3, not 4 (Binance USD-M's hedge refusal still exits 4). A hedge row later is
venue-fatal. Reconciliation: replay, then open orders and positions per settle coin (absent =
flat) into Begin/OpenOrder*/Position*/End, the start-up sweep included. Position topic compared
with the forwarded fills after 1 s settle, as USD-M does (`position_from_stream`). Linear fee =
settle coin (Quote), rebates negative. DCP: product DERIVATIVES + `dcp.future`. Evidence:
`tests/venues/bybit_linear_test.cpp`, `bybit_linear_venue_test.cpp`,
`integration/bybit_linear_startup_test.cpp`; each of 30 mutations (category, positionIdx,
reduceOnly, settleCoin, fee rule, signs, hedge detection, exit code, Position records, DCP
product/topic, position check, fill tracking, ...) fails at least one of them. Spot tests untouched.
`integration.gateway books` failed its 1 s pause bound (1.05-1.09 s) in 2 of 7 runs here, unrelated
(no Bybit in it), not checked against the base.
Left: no testnet run; tickers (mark, funding) not subscribed, funding not booked; LinearFutures and
inverse refused; leverage/margin mode not read.

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
2. ~~`fastmm-gateway`~~ Done (`include/fastmm/live/gateway.hpp`,
   `docs/how-to/operations/run-behind-a-gateway.md`). The gateway runs the venues through the same
   code as fastmm-live (`live/venue_slot.hpp`); on attach it creates three ShmRings per venue and
   switches the sinks to them in a task posted to the venue's reactor (`EventSink` takes a ShmRing).
   Outbound: the network thread's hook copies the strategy's orders into the venue's own outbound
   ring and calls `on_wake()`, so no connector changed. The engine gets the gateway's instrument
   table and ring paths in the reply; `fastmm-live --gateway <socket>` runs everything else as
   before and calls no venue cancel_all. A mid-stream attach needs whole books:
   `Venue::resync_books()` (four connectors; nasdaq_itch cannot).
   Wake-ups (2026-09-26): the attach reply passes a memfd wake page and each reactor's eventfd
   (`SCM_RIGHTS`). The engine's feed Waker is shared into the page (non-private futex) and the
   gateway's net thread notifies it after pushing; the net thread's sleeping flag is in the page
   and the engine's wake hook writes the eventfd only when it was set. Without them an idle side
   slept up to 1 ms each way (adaptive through the gateway: engine t2t p50 557-590 us, wire 1376
   us). Now, release, WSL2, unpinned, 45 s x 3 runs, engine t2t p50 / wire t2t p50: adaptive
   in-process 34.8 / 66.4 us, through the gateway 36.9 / 70.3 us (wire p99 105-113 vs 113-117 us);
   busy in-process 7.2-7.7 / 41.0 us, through the gateway 7.9-9.2 / 43.0 us (unchanged).
   `integration.gateway.wake:` tests both directions across a fork.
3. ~~Attach/detach~~ Done for one strategy. On attach the gateway resyncs the books, replays the
   executions since the strategy's store (the strategy owns the store, so it restores its own
   position, onto its control ring, and sends the replay start and the known trade ids in the
   attach request) and then the open orders. Detach is the connection closing: sinks back to a
   discard ring (counted), cancel_all on every venue, rings removed. `tests/integration/
   gateway_test.cpp`: kill -9 of the strategy leaves no open order at the simulator 5-10 ms later,
   md/api sessions opened stay the same, the next strategy restores, trades, and its stored
   position equals the venue's.
4. ~~Several strategies per gateway~~ Done (2026-09-26, up to 16). The gateway hands out epochs
   from its epoch file; attach names the instruments, a clash is refused. The sinks write into
   local rings drained on every commit: md to every attachment (a full ring drops for that one
   only; it then gets its own Resyncing), order events by the epoch in the id, dead-epoch fills and
   Position records to the instrument's owner, a snapshot to whoever asked with its own rows and
   its own watermark (`SentWatermark` now keeps the last id taken, the same as the highest for one
   engine, which locates the snapshot in the gateway's forwarding history). Dead rows and late
   dead acks are cancelled by the gateway; detach cancels its epoch one order at a time
   (connectors only have venue-wide cancel_all) and sweeps. `[gateway]` guards the order rate and
   open-order notional per venue, not positions (the gateway does not know the account's).
   `gateway_multi_test.cpp` (BTCUSDT and BTCUSDC on the simulator): stores agree per symbol, no
   journal holds another epoch's event, kill -9 clears one strategy in 5 ms and the other's
   orders stay. Single strategy vs the step-3 build, release, 45 s x 2: adaptive engine/wire p50
   36.9/70.3 us both; busy 8.7/43.0 vs 7.9-8.7/43.0-44.9. Left: two strategies on one
   instrument; positions and loss across strategies.
5. ~~Account risk in the gateway~~ Done (2026-09-26). Each venue's network thread books every
   execution once into an `AccountBook` (`core/account_book.hpp`: OMS dedupe key, the engine's fee
   booking, marks at its own L2Books' mids); an instrument's position starts with what its first
   owner restored (sent in the attach request, protocol v4), replays older than that owner's
   replay start or in its store's ids are skipped, and the position stays on detach. `[gateway]`
   `max_gross_notional` / `max_net_notional` refuse increasing orders back to the sender
   (`GatewayGrossNotional` / `GatewayNetNotional`); `max_loss` over the account's net PnL, carried
   in the gateway's kill file (KillStateStore), trips: orders refused (`GatewayAccountKilled`),
   `TripVenueKill(GatewayMaxLoss)` to every attachment, every known order and every row of the
   next snapshot cancelled, `cancel_all`, attaches refused, latched (start exits 6) until
   `--clear-kill`. `gateway_account_test.cpp` (BTCUSDT and ETHUSDT: the limits need one settlement
   currency); each case fails with its piece broken (no booking, no seed, no exposure check, no
   trip, no latch). Applying the book deltas in the md drain put a bucket on the adaptive engine
   t2t (36.9 -> 38.9 us), so the drain copies them aside and the hook applies them after the
   wake-ups. Release, WSL2, one strategy, 45 s x 2, gateway engine/wire p50, base (8802339) vs
   limits on: adaptive 36.9/70.3 vs 36.9/70.3 us; busy 8.7/43.0-44.9 vs 7.9-8.2/44.9 us.

**Gateway: an attach froze the others' books (found and fixed 2026-09-26).** On attach, and when
one attachment fell behind on market data, the gateway called `resync_books()` on the venue; the
depth sync then forwarded no deltas until a new snapshot, which waited out the connector's 2 s
interval, so every other strategy quoted on a frozen book. Now the attaching (or lagging, after its
own `Resyncing`) attachment gets a `BookSnapshot` of the gateway's copy of each book
(`write_book_snapshot`, `core/book/book_snapshot.hpp`) in its md ring alone. Ordering: all on the
venue's network thread, and the md drain routes each event to every ring and to `acct_md` in one
step, so applying `acct_md` first makes the copy the book after exactly the events routed so far.
Depth: the copy is now 1024 levels a side (`kMaxBookLevelsPerMsg`, as deep as any snapshot
message); with 256 an engine book from it differs from one built from the venue's snapshot
(`truncated`, then levels). `resync_books()` stays only for `acct_md` overflow. A book the gateway
does not hold gets no snapshot (the venue's next one reaches everyone). Evidence:
`book_snapshot_test.cpp` (gateway snapshot + 6000 deltas == venue snapshot + same deltas, attach at
6 points; fails with a 256-level copy); `gateway_books_test.cpp` (a second strategy attaching twice
within 2 s, and one SIGSTOPped until its 64 KiB ring drops: no depth snapshot at the simulator,
gateway `books` N/N throughout, a has one snapshot per book and <= ~100 ms between BTCUSDT updates,
b's books equal a's delta by delta and the simulator's top at sampled update ids). On the old
gateway: 2-4 extra depth snapshots, `books` below N in ~200 status samples, 3 snapshots per book
in a, gaps of 1.3-2.0 s. Two tests assumed attaches took that long (the ops test's first status
had no live venue yet, the account test read the log before it was written); they wait now.
Release, WSL2, adaptive, one strategy, 45 s x 2, gateway engine/wire p50, base (421c0d5) vs this:
36.9/70.3 vs 36.9/70.3 us (one noisy run 38.9/74.3, rerun 36.9/70.3; in-process 34.8/66.4).

**Resume point in venue time (fixed 2026-09-26).** A restart's execution replay started at the
store's last fill minus 10 s in the engine's clock (the host's), which the venue compared with its
own; the 20 s of known ids only hid it. Now the store keeps each fill's `exch_ts` (schema 3:
`fills.exch_ns`, `session_venues`) and `Recovery::venue_resume` gives, per venue by name, the venue
time of its last stored fill minus 1 s (delivery reordering across symbols is ms) and the ids stored
from there. Binance Spot and USD-M resume each symbol at the stored max trade id + 1
(`Venue::resume_trade_ids`), in-process; Bybit, Deribit and every venue behind a gateway use the
time path. The gateway's attach carries it per venue (protocol 5), and a fresh attach's history
starts at the venue's time rather than the host's. At most 128 ids a venue (1024 over 8 per
attach): more in the overlap moves the start later by whole ms, logged, instead of dropping ids.
Stores from before schema 3 fall back to the old 10 s/20 s engine-clock start. The 20 s widening
is gone from the new path. Evidence: `recovery_restart_test` and `gateway_test` restart with the
simulator's clock 15 s ahead and behind, 65 outside trades in the last 26 s before the stop and one
while down: pass; with the HEAD binaries (f8ed1c4) all four fail (68-70 executions stored by both
sessions ahead, the down-time trade missed behind), and in-process with only the start reverted to
the engine clock both fail too. `store/resume_test.cpp` covers the round trip, the shrink and a v2
store. Left: Binance's exact start does not recover a fill the stream missed before the last
stored one (the time path covers 1 s of that); the gateway's pre-seed filter (`seed_from_ms` at
gateway start) still uses the host clock.

**Gateway books (fixed 2026-09-26).** An attaching or lagging strategy now starts from a snapshot
of the gateway's own 1024-level book copy, so no attach pauses anyone else (tests failed on the old
gateway with 1.3–2.0 s gaps in the other strategy's updates). Open: a lagging strategy with a small
md ring and many deep books may never fit the snapshots it needs and keep resyncing; size each
attachment's ring for the snapshots of the instruments it receives.

**Gateway follow-ups (2026-09-26).** Steps 4 and 5 landed (several strategies, epochs from the
gateway, instrument ownership, per-epoch detach, `[gateway]` rate and open-notional guards, account
positions, exposure and loss). Open:
* ~~The replay start in the engine's clock~~ Fixed: see "Resume point in venue time" below.
* ~~1024 known ids per attach could overflow~~ Fixed there too.
* ~~Two strategies on one instrument~~ Done 2026-09-28 (`[gateway.shared]`, entry at the top).
* The account's position of an instrument is seeded once per gateway run, by its first owner's
  store; the gateway books a strategy's missed fill from the replayed execution, where the engine
  first books an estimate from cum_qty, so the two can differ until that replay.
* Its kill file refuses a strategy with the gateway's `[engine] name` while `max_loss` is set (one
  config for both, as in the docs' first example, then needs a second name).

**Gateway operability (2026-09-26).** The gateway writes the status segment (v8: `kind`, a gateway
block with attachments, the account, a position per instrument, routing counters) to
`/dev/shm/fastmm-<name>.gw.status` every 250 ms from its main thread, out of the totals the network
threads published already; `fastmm-top` picks the frame, JSON and Prometheus families by `kind`
(`--gateway <name>`). A control socket at `<attach socket>.ctl` (`fastmm-ctl --gateway <name>`,
the same listener): `pull`/`resume` put the engine's own ControlMsg on the owners' order rings
from a task on the venue's network thread (journaled by the strategies), `kill` trips the account
as max_loss does (`KillReason::GatewayOperator`, latched in the kill file when max_loss is set),
`clear-kill` clears it and re-arms the budget (carry = -(realized - fees) so far), refused while
any strategy is attached (each was killed by the trip). The `.gw` in both paths keeps them apart
from a strategy that shares the config. `gateway_ops_test.cpp`; each part fails with its piece
broken (no publish, pull pushes nothing, kill does not trip, clear-kill leaves it tripped, no
gateway metrics). Release, WSL2, adaptive, one strategy, 45 s x 2, gateway engine/wire p50, base
(14ad5d4) vs this: 36.9-38.9/70.3-74.3 vs 36.9/70.3 us (in-process 34.8/66.4 both). Left: per-instrument PnL (only
qty is published per instrument; the rest is per venue), a `pull` does not reach a strategy that
attaches later.

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
