# ADR-0014: US equities

Status: accepted (2026-09)

FastMM trades US-listed stocks on IEX data: first in backtests from IEX HIST, then in paper trading through Alpaca to validate the connector, then two-sided market making through Interactive Brokers with directed routing.

## Context

- FastMM has no equity venue. ITCH 5.0, MoldUDP64, SoupBinTCP, OUCH and FIX codecs (`include/fastmm/codecs/`) are used only by tests and benchmarks. The engine drops L3 events (`engine.hpp`); `L3Book` is fed only in tests and `bench/bench_book.cpp`, rejects off-tick prices, drops orders outside its 65 536-tick window and takes about 75 MB at its default capacity.
- An instrument has one tick. `Instrument::valid_price` and `round_price` are called by risk, the built-in strategies, the hot adapter, the Python binding and the Deribit encoder; `uncross`, `keep_passive` and the QuoteManager's hysteresis take `inst.tick`. Deribit's `TickSchedule` (with `TickStep` and `kMaxTickSteps = 4`) lives in the venue.
- There are no sessions, halts, short-sale controls or buying power. `FeeModel` charges centi-bps on notional. The risk token bucket counts new orders per second only.
- Data:
  - Nasdaq TotalView-ITCH on Databento needs a business entity and a Nasdaq licence. Individuals can buy only other datasets, such as EQUS.MINI and IEX top of book ([Databento](https://databento.com/blog/nasdaq-totalview-live)).
  - Nasdaq's ITCH sample files (5–18 GB per day) state no terms.
  - IEX HIST DEEP (price-level book, trading status, operational halts, short-sale price test) is free and may be redistributed with attribution ([IEX](https://www.iex.io/legal/hist-data-terms)). IEX is the venue of Alpaca's free feed.
- Alpaca:
  - REST order entry at 200 requests per minute per account. A paper account needs only an email address ([paper](https://docs.alpaca.markets/us/docs/paper-trading)).
  - Paper orders fill against the NBBO once marketable, with no queue position, random partial fills 10% of the time and no regulatory fees.
  - Alpaca rejects a buy limit priced at or above the account's open sell limit in the same symbol, in paper too ([protection](https://docs.alpaca.markets/us/docs/user-protection)).
  - Extended-hours orders are Day or GTC limits with `extended_hours=true`. Overnight trading (20:00–04:00 ET) runs on Blue Ocean ATS.
  - The SIP stream ($99/month) has trading status and LULD messages; the free IEX feed is not documented to carry them.
  - Regulatory fees are summed per account per day, rounded up to the cent and posted at the end of the day ([fees](https://docs.alpaca.markets/docs/regulatory-fees)).
- IBKR: the TWS API goes through TWS or IB Gateway, 50 messages/s, at most 20 active orders per contract per side ([IBKR](https://interactivebrokers.github.io/tws-api/order_limitations.html)).
- Rules:
  - Section 31: $20.60 per $1M of sales from 2026-04-04, until 60 days after the FY2027 appropriation ([SEC](https://www.sec.gov/rules-regulations/fee-rate-advisories/2026-2)).
  - FINRA TAF: $0.000195 per share sold, capped at $9.79, in 2026; $0.000232, capped at $11.61, from 2027-01-01. FINRA's rounding does not apply ([TAF](https://www.finra.org/rules-guidance/guidance/trading-activity-fee)).
  - CAT fee: $0.000003 per share, buys and sells ([Alpaca schedule](https://files.alpaca.markets/disclosures/library/BrokFeeSched.pdf)).
  - PDT rule replaced by intraday margin from 2026-06-04 ([FINRA RN 26-10](https://www.finra.org/rules-guidance/notices/26-10)).
  - Rule 612 ticks: $0.01 at or above $1.00, $0.0001 below. The half-penny tick is deferred to November 2027 ([SEC](https://www.sec.gov/files/rules/exorders/2026/34-105656.pdf)).
  - Round lots (100, 40, 10 or 1 share) are set twice a year: the March average sets May's, the September average sets November's.
  - Rule 201: when triggered, a short sale must be priced above the national best bid.
  - LULD bands and pauses apply 09:30–16:00 ET.
  - The SEC approved Nasdaq 23/5 on 2026-04-10; the night session awaits a further filing and SIP readiness.

## Decision

### 1. Scope and stages

1. `IexDeepSource` replays IEX HIST DEEP pcap files into backtests (pcap reader, IEX-TP and DEEP decoder). A trimmed real day is a test fixture with the IEX attribution.
2. Alpaca connector on Alpaca's free IEX feed. Alpaca paper validates the connector, sessions, halts, reconciliation and fees; it is not where market making is judged. Credentials come only from `ALPACA_PAPER_API_KEY` and `ALPACA_PAPER_API_SECRET`, referenced as `${ALPACA_PAPER_API_KEY}` and `${ALPACA_PAPER_API_SECRET}` in `configs/alpaca-paper.toml` as the Binance configs do, never inline. Until the account exists, the connector is developed and tested against recorded fixtures and the in-process fake server (`tests/venues/fake_venue_util.hpp`).
3. IBKR connector, for two-sided market making with routing to a named exchange and per-share tiered pricing.
4. Optional, later: `ItchSource` replays ITCH 5.0 for users with licensed data. Nasdaq sample files are for manual runs only, never CI.

Out of scope: Nasdaq data licensing, a SIP subscription, registered market making, direct exchange access (live ITCH, OUCH, sponsored access), options on equities, fractional shares, hard-to-borrow locates, auction and ISO orders, overnight positions, corporate-action adjustment, overnight quoting on Alpaca, universes over `kMaxInstruments` (256), other brokers.

### 2. Tick schedule and lots

- `TickSchedule` and `TickStep` move to `include/fastmm/core/tick_schedule.hpp`; a step's tick applies at prices >= its `above`. `InstrumentTable` holds up to 16 schedules. Instrument flag `kBandedTick` (bit 3) and cold-line byte `tick_schedule` select one.
- `InstrumentTable::valid_price(inst, px)`, `round_price(inst, px, side)` and `tick_at(inst, px)` use the schedule when `kBandedTick` is set and `inst.tick` otherwise. Callers:
  - risk uses them, and the QuoteManager's hysteresis uses `tick_at` of the resting price;
  - the built-in strategies, the hot adapter and the Python binding round with them and pass `tick_at(mid)` to `uncross`, `keep_passive` and tick thresholds;
  - `ctx.tick_at` and `ctx.round_price` forward to the table.
- For a banded instrument `Instrument::tick` is a tick valid at every price of the schedule: $0.01 for Rule 612. User strategies that use `inst.tick` stay valid, on a coarser grid below $1.00.
- Deribit keeps its schedules in the venue and does not set `kBandedTick`, so crypto journals do not change. The 2027 half-penny tick is a new schedule in reference data.
- Equities have `lot = 1`. Cold-line `std::uint16_t round_lot` comes from DEEP's security directory or ITCH R in backtests and from configuration live, because Alpaca's assets carry no round lot. With `quote_round_lots = true` (default for equities) the QuoteManager rounds quote quantities down to whole round lots; a quote below one round lot is not sent and is counted.
- `ItchSource` gives an instrument an `L3Book` at $0.0001 when the prior close is below $5.00 and at $0.01 otherwise. Off-tick and out-of-window orders are counted and excluded, and the top 10 levels are exact. Books exist only for the run's instruments, and `itch_max_orders` (2^16–2^20) sets their size. Sources run on the backtest runner thread.

### 3. Calendar and sessions

- `enum class SessionState : std::uint8_t { Closed, Pre, Regular, Post, Overnight, Open }`. An instrument without a calendar is `Open`.
- `configs/calendars/us-equities.toml` holds holidays, early closes and session times as UTC nanoseconds.
  - `tools/update_calendar.py` writes it from the NYSE and Nasdaq holiday pages (dates) and Alpaca `GET /v2/calendar` (times); a person reviews the diff.
  - A scheduled CI job fails when it covers fewer than 90 days ahead.
  - The loader builds `SessionSchedule` (up to 4096 transitions), stores it in the journal header and supplies ITCH and DEEP midnight in ET.
- Each transition, and each `pull_before_close_s` and `flatten_before_close_s` point, is an engine timer; firing journals a `TimerMsg` with `engine = 2`, so quiet instruments change state on time and replay is exact.
- `[[instruments]] sessions = ["regular"]` lists where an instrument quotes; outside them the QuoteManager cancels its quotes. `on_session(ctx, inst, state)` joins `hooks.hpp` with near misses `on_sessions` and `on_session_state`.
- A live session compares the host clock with Alpaca `GET /v2/clock` at start and refuses to start beyond `max_clock_skew_ms` (default 500) with exit code 3. The engine clock follows `CLOCK_REALTIME`, which steps on WSL2.
- `OutNewOrderMsg` gains a `session` byte from its padding. Alpaca sends `extended_hours=true` for every order of an instrument whose sessions include an extended session, so an order sent near 16:00 is not rejected. IBKR sets `outsideRth`. Equities default to Day TIF; the simulated venue expires Day orders at the session end.

### 4. Trading status

- `EventType::TradingStatus = 27`, `TradingStatusMsg` (128 bytes): bits `kHalted`, `kLuldPause`, `kSsrActive`, `kShortable`, `kEasyToBorrow`, `kStatusUnknown`; `luld_lo`, `luld_hi`; `reason`. The engine keeps one status per instrument and journals each message.
- Mapping:
  - ITCH H: H to `kHalted`, P to `kLuldPause` (Nasdaq-listed only; others arrive as H), Q to `kHalted`, T clears.
  - ITCH Y: 1 or 2 sets `kSsrActive`. ITCH h sets `kHalted` for Nasdaq.
  - ITCH W: level 3 halts every equity for the rest of the day; levels 1–2 halt for 15 minutes when before 15:25.
  - DEEP trading status, operational halt and short-sale price test map the same way.
  - After a halt, quoting stays off until the first T or, at the open, the first trade after 09:30.
- Live on the IEX feed, LULD bands and trading-status messages may be unavailable. The connector runs `LuldBandCalculator` on IEX trades and maps Alpaca's status and LULD messages when they arrive, the asset endpoint's `tradable` flag at start and on each account poll, and `GET /v2/clock`. An equity starts with `kStatusUnknown`, which pulls quotes like `kHalted`. It clears when a status message arrives, or when the clock says open and an IEX trade or quote for the symbol arrives. It is set again after `status_stale_s` (default 30) without an update in the regular session.
- `LuldBandCalculator` runs inside the source or connector:
  - Tier comes from ITCH R or DEEP's directory. The first reference price is the listing exchange's opening price, and bands apply 09:30–16:00.
  - Every 30 s it takes the mean of the source venue's eligible trades over the last 5 minutes and republishes the reference price only on a move of 1% or more.
  - Percentages: 5% (Tier 1) or 10% (Tier 2) above $3.00; 20% from $0.75 to $3.00; below $0.75, the lesser of $0.15 or 75%. Doubled 15:35–16:00 for Tier 1 and for Tier 2 at or below $3.00.
  - Bands are rounded to the penny.
- Halted, paused or unknown: quotes are cancelled, orders rejected with `RejectReason::Halted = 17`, `on_status(ctx, inst)` fires (near misses `on_trading_status`, `on_halt`). LULD: bids above `luld_hi` and asks below `luld_lo` are clamped before risk.
- A sell is short when it would take position plus open sells below zero; a sell never takes a long position below zero in one order. Short sales are allowed by default and rejected with `NotShortable = 18` when `kShortable` is clear or `[risk] long_only = true`. Live, `kShortable` is clear until Alpaca's asset endpoint says shortable; backtests set it from `assume_shortable`.
- With `kSsrActive`, a short sale priced at or below the best bid is rejected with `ShortSaleRestricted = 19`. The engine holds a venue bid (IEX, Nasdaq or the feed), not always the national best bid; the broker's check is final.

### 5. Account and request budget

- `EventType::AccountUpdate = 28`, `AccountUpdateMsg`: venue, `cash`, `buying_power`, `equity`, and `nets_open_orders` (whether the venue's figure already subtracts open orders). Connectors send it after reconciliation, after fills and from a poll; it is journaled.
- For a venue with equity instruments, orders are rejected with `InsufficientBuyingPower = 20` before its first `AccountUpdate`, while the last is older than `max_account_age_s`, and when order notional (plus open buy notional unless netted) exceeds `buying_power - buying_power_reserve`.
- `[venues.<name>] requests_per_minute` and `request_burst` form a budget that `rest_channel` charges for every REST call. 20% is reserved for cancels, the account poll backs off when the budget is low, and refused orders get `VenueRateLimit`.

### 6. Fees

- `FeeModel` becomes `FeeSchedule` in `include/fastmm/core/fee_schedule.hpp`, with every new field zero by default:
  - `maker_cbps`, `taker_cbps`, `maker_per_share`, `taker_per_share`;
  - `commission_per_share`, `commission_min`, `commission_max_cbps`;
  - `sec_fee_per_million`, `taf_per_share`, `taf_max`, `cat_per_share`;
  - `fee_rounding = none | execution | daily`.
- `[[venues.<name>.fees.periods]]` carry a `from` date; the engine switches rates at the first session timer on or after it. The config comments note the Section 31 expiry rule.
- Alpaca uses `daily`: fills carry unrounded estimates. The connector compares the day's estimate with Alpaca's fee activities the next session and reports the difference in the log and status file; PnL keeps the estimates.
- `PositionTracker` is unchanged; crypto fees and hashes do not change.

### 7. Flat by the close

- From `flatten_before_close_s` (default 60) before the end of the last allowed session, the engine sends flattening orders every second until flat. They are IOC in the regular session and Day limits in extended sessions, priced `flatten_slippage_bps` through the best price and never beyond the LULD band.
- A position left at the end raises an alert, sets a status-file flag and blocks the next start unless `allow_start_with_position = true`. There is no split or dividend adjustment.

### 8. Alpaca connector

- Base URLs `paper-api.alpaca.markets` and `api.alpaca.markets`, order updates from `trade_updates`, market data from `v2/iex`.
- `VenueCaps::rejects_self_cross`: `stp` is forced on and the OMS also counts the pending price of a replace in flight, so no buy is sent at or above an open sell of the symbol or the reverse; the QuoteManager cancels first.
- A paper test records whether Alpaca accepts a short sale while a buy is open. If not, quoting from flat is one-sided on Alpaca, and the venue reference says so.
- Replace is `PATCH`, which creates a new order; the connector follows `replaced_by` and keeps client order ids within Alpaca's length limit. Symbols use the dotted form (`BRK.B`); sources and connectors map theirs through `venues/symbology.hpp` (IBKR `BRK B`).

### 9. Python and invariants

- `FASTMM_HOT_ABI_VERSION = 2` appends after `ask_is_raw`: `session` (int32); `halted`, `luld_paused`, `ssr`, `shortable`, `status_unknown`, `luld_clamped` (uint8); `luld_lo`, `luld_hi`, `buying_power`, `round_lot` (double and `_raw`). `ctx.tick` is `tick_at(mid)`. `on_session` and `on_status` join the hot hooks; `fastmm.SESSION_*` constants work in hot code. Hooks compile at import, so strategies recompile.
- No allocation or virtual call on the hot path: schedules, statuses and account state are fixed arrays; `tests/hotpath/noalloc_test.cpp` covers each new path.
- `kJournalVersion = 4` adds a CRC-checked reference section (tick schedules, session schedule). Readers accept 1–4; existing golden journals and `sample_1000.sha256` replay unchanged.
- Strategies that do not opt in compile and behave as before; crypto instruments set none of the new flags.

## Consequences

- Equities run in backtests, replay and paper trading with the same strategy classes.
- IEX's book is thin, and LULD bands computed from IEX trades differ from the published ones. Live without SIP, a halt may be seen only as missing updates, so quotes are pulled late or conservatively while status is unknown.
- Alpaca paper tests the connector, sessions, halts, reconciliation and fees. Its PnL is no evidence of spread capture: fills ignore queue position.
- At 200 requests per minute for every REST call, requotes are measured in seconds, and engine latency makes no difference on Alpaca. `min_requote_interval_ms` and `min_requote_ticks` need values to match.
- Retail commissions and regulatory fees exceed exchange rebates. IBKR's tiered pricing and directed routing narrow the gap without closing it.
- Alpaca's self-cross rule may force one-sided quoting from flat there; two-sided market making is judged on IBKR.
- The calendar and fee periods are maintained by hand.

## Implementation order

Steps 1–3 can start in parallel; step 4 needs 1–3; steps 5–7 follow in order; step 8 needs step 4. Each step ships its docs.

1. Tick schedule: core types, `InstrumentTable` functions, callers, `[tick_schedules]`. Tests: both sides at $0.9999, $1.00, $1.0001; Deribit tests unchanged; golden hashes unchanged; BasicMM tick-to-order p50 within 2% of main.
2. Engine inputs: session timers, `TradingStatus`, `AccountUpdate`, journal v4, rejects 17–20, clamps, short-sale checks, flattening, hooks, calendar file, tool and scheduled job. Tests: a synthetic journal crossing Pre, Regular and Post with a halt, SSR, band move and account update replays to the same hash on a quiet instrument; v1–3 journals replay; orders fail closed without an account update.
3. Fees: `FeeSchedule`, periods, rounding modes. Tests: hand-computed fees for a 100-share sell at $50.00, a TAF-capped sell, a 2026–2027 period switch and a daily rounding total; crypto results identical.
4. `IexDeepSource` with `LuldBandCalculator` and statuses. Tests: the IEX fixture gives the expected book and statuses; LULD against published examples; two runs give the same hash.
5. Alpaca: request budget, self-cross handling, replace chains, symbology, clock and calendar checks, fee reconciliation, `configs/alpaca-paper.toml`. Tests against the fake server: lifecycle, replace, rejects, reconnection, budget refusal, extended-hours flag, halt pull, replay hash. Tests for unknown status: no status messages and no updates pull quotes. Once the owner's paper account exists: the paper test of short with an open buy, and a paper session.
6. Python ABI 2. Tests: a hot BasicMM port matches C++ on the IEX fixture; ADR-0013 tests pass.
7. IBKR over IB Gateway: routing, `outsideRth`, halt and shortable ticks, 50 messages/s, 20 orders per side. Tests against a fake gateway from recorded fixtures; then a two-sided quoting paper session.
8. Optional: `ItchSource` with the `L3Book` rules. Tests: an ITCH fixture from `generate_fixtures.py` with H, Y, W and prices across $1.00 gives the expected book and statuses; two runs give the same hash.

## Open questions

- IBKR account, commission plan (fixed or tiered) and market data subscriptions, before step 7.
