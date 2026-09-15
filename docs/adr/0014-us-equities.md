# ADR-0014: US equities

Status: proposed

FastMM trades US-listed stocks: first in backtests from Nasdaq order-by-order data, then in paper trading through Alpaca, then through Interactive Brokers with directed routing. This record decides the scope, the engine inputs and model changes, and the order of work.

## Context

- FastMM has no equity venue. Nasdaq TotalView-ITCH 5.0, MoldUDP64, SoupBinTCP, OUCH 4.2/5.0 and FIX 4.4 codecs exist (`include/fastmm/codecs/`) but only tests and benchmarks use them. `src/net` has no UDP socket; the FIX session has no TCP transport and keeps sequence numbers in memory.
- The engine drops L3 events (`engine.hpp`, "not consumed by the L2 engine"). `L3Book` is fed only in tests, and its tick-indexed window is 65 536 ticks.
- `AssetClass::Equity` exists and changes nothing. An instrument has one tick (`Instrument::tick`), checked in `RiskEngine::evaluate`. A price-banded `TickSchedule` exists only in `deribit_order_encoder.hpp`.
- `OutNewOrderMsg` has order type (Limit, Market, PostOnly), TIF (GTC, IOC, FOK, Day) and `reduce_only`, with 36 bytes of padding. Venues map Day to their own TIF.
- There are no calendars or sessions. `ItchDecoder` validates and ignores H (trading action), Y (Reg SHO restriction) and J (LULD auction collar). There are no short-sale controls and no buying power. `FeeModel` charges maker/taker centi-bps on notional. `kMaxInstruments` is 256.
- Nasdaq TotalView-ITCH on Databento, historical or live, is sold only to a business entity with a Nasdaq licence; Nasdaq does not license individuals ([Databento](https://databento.com/datasets/XNAS.ITCH)). Nasdaq publishes whole-day ITCH 5.0 sample files of 5–18 GB with no stated terms ([emi.nasdaq.com](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/)).
- Direct feeds and OUCH need a broker-dealer or sponsored access under SEC Rule 15c3-5.
- Alpaca: REST order entry, WebSocket order updates, 200 trading requests per minute per account. A paper account needs only an email address and has its own keys on the same API ([Alpaca](https://alpaca.markets/learn/start-paper-trading)). Extended-hours orders must be limit orders with Day or GTC and `extended_hours=true`; sessions are overnight 20:00–04:00, pre-market 04:00–09:30 and after-hours 16:00–20:00 ET ([orders](https://docs.alpaca.markets/docs/orders-at-alpaca)). The stock stream has trading status (halt, resume) and LULD band messages; IEX data is free and SIP data costs $99/month ([stream](https://docs.alpaca.markets/docs/real-time-stock-pricing-data)). Paper fills ignore queue position and regulatory fees.
- Interactive Brokers: the TWS API socket goes through a running TWS or IB Gateway, 50 messages/s, at most 20 active orders per contract per side per account ([IBKR](https://interactivebrokers.github.io/tws-api/order_limitations.html)). Orders can be routed to a named exchange.
- Rules in force in September 2026:
  - The pattern day trader rule is replaced by intraday margin requirements, effective 2026-06-04, with broker phase-in to 2027-10-20 ([FINRA RN 26-10](https://www.finra.org/rules-guidance/notices/26-10)).
  - Section 31 fee: $20.60 per $1M of sales from 2026-04-04 ([SEC](https://www.sec.gov/rules-regulations/fee-rate-advisories/2026-2)).
  - FINRA TAF: $0.000195 per share sold, at most $9.79 per trade, from 2026-01-01 ([FINRA](https://www.finra.org/rules-guidance/guidance/trading-activity-fee)).
  - Settlement is T+1.
  - Rule 612 ticks: $0.01 at or above $1.00, $0.0001 below. The half-penny tick is deferred to the first business day of November 2027 ([SEC 34-105656](https://www.sec.gov/files/rules/exorders/2026/34-105656.pdf)).
  - Round lots are 100, 40, 10 or 1 shares by the prior period's average price since November 2025.
  - Nasdaq targets 23/5 trading, with an overnight session 21:00–04:00 ET, for 2026-12-06, pending SEC and SIP readiness ([Markets Media](https://www.marketsmedia.com/nasdaq-aims-to-debut-23-5-trading-on-6-december-2026/)).
  - Rule 201 short-sale price test and LULD bands and halts apply.

## Decision

### 1. Scope and stages

1. Offline. `ItchSource` replays ITCH 5.0 files into backtests. Journals and replay behave as for crypto sources.
2. Alpaca connector for paper trading. It is first because an account costs nothing and needs no deposit, and its REST and WebSocket JSON fits the existing connector structure (`rest_channel.hpp`, `ws_client.hpp`, `blocking_http.hpp`). Tests run against fixtures and the in-process `HttpServer`/`WsServer` used by `tests/venues/fake_venue_util.hpp`, with no account. IBKR would need a running Java gateway, a daily restart and an account before the first test.
3. IBKR connector, for routing to a named exchange and per-share tiered pricing.

Out of scope: registered or designated market making, direct exchange access (live ITCH, OUCH, sponsored access), options on equities, fractional shares, hard-to-borrow locates, auction orders (MOC, LOC, opening and closing cross), ISO orders, positions held overnight, corporate-action adjustment, universes over 256 instruments, a consolidated book from several direct feeds, live TotalView, and other brokers (Schwab, Tradier, tastytrade, Lime, DAS, Sterling).

### 2. Tick schedule and lots

- `TickSchedule` moves from `deribit_order_encoder.hpp` to `include/fastmm/core/tick_schedule.hpp` unchanged, and Deribit uses it from there.
- `InstrumentTable` holds up to 16 schedules. Instrument flag `kBandedTick` (bit 3) and the cold-line field `std::uint8_t tick_schedule` select one; `Instrument::tick` stays the tick at and above the first step ($0.01).
- `valid_price` and `round_price` use the schedule only when the flag is set. Crypto instruments pay one predicted branch on a hot-line byte.
- A new `tick_at(Price)` returns the tick at a price. The QuoteManager's hysteresis, `uncross`, `keep_passive` and the hot `ctx.tick` use `tick_at` of the current mid.
- Configuration: `[[instruments]] tick_schedule = "us-rule612"`. `[tick_schedules.us-rule612]` gives `base = "0.0001"` and `steps = [{above = "1.00", tick = "0.01"}]`. The half-penny tick in 2027 is a new schedule assigned by reference data, with no code change.
- `L3Book` for an equity uses the tick at the day's first reference price; a price move across $1.00 rebuilds the book from its orders off the engine thread. The 65 536-tick window covers $655 at $0.01.
- Equities have `lot = 1` share. Cold-line field `std::uint16_t round_lot` holds 100, 40, 10 or 1 from reference data. With `quote_round_lots = true` (default for equities), the QuoteManager rounds each quote quantity down to a multiple of `round_lot`, because odd lots do not set the protected quote. Flattening orders may be odd lots.
- The cold-line padding has room for both fields, so `sizeof(Instrument)` stays 128, and zero keeps today's behaviour in old journals.

### 3. Calendar and sessions

- `enum class SessionState : std::uint8_t { Closed = 0, Pre = 1, Regular = 2, Post = 3, Overnight = 4, Open = 5 }`. Instruments without a calendar are `Open` and never change.
- `configs/calendars/us-equities.toml` lists, per year, holidays, early closes and session times, with transitions stored as UTC nanoseconds. `tools/update_calendar.py` writes it from the NYSE and Nasdaq holiday pages; a person reviews the diff. Overnight sessions appear only from a date written in the file after Nasdaq confirms 23/5.
- A CI test fails when the file covers fewer than 90 days past the build date.
- At start the loader builds `SessionSchedule`: a sorted fixed array of up to 4096 (timestamp, state) pairs covering the run. The live Alpaca connector compares the next five days with `GET /v2/calendar` and refuses to start on a mismatch (exit code 3).
- The schedule goes into the journal header, so a replay uses the file of the original run. The engine derives state by comparing engine time with the next transition on each event and timer, so replay reproduces every transition without a new message.
- `[[instruments]] sessions = ["regular"]` lists the sessions an instrument quotes in. Outside them, quoting is off for that instrument and the QuoteManager cancels its quotes. `pull_before_close_s` (default 5) moves the cancel earlier.
- New hook `on_session(ctx, inst, SessionState)` joins the hook table in `hooks.hpp`, with near misses `on_sessions` and `on_session_state`.
- The engine fills the new `OutNewOrderMsg::session` byte (from the padding) with the state an order is sent in. Alpaca sets `extended_hours` from it, and IBKR sets `outsideRth`. Crypto encoders ignore it.
- Equities default to Day TIF. The simulated venue expires Day orders at the end of the calendar session they were sent in, with `OrderExpiredMsg`.

### 4. Trading status: halts, LULD and short sales

- New `EventType::TradingStatus = 27` with `TradingStatusMsg` (128 bytes):
  - `status` bits: `kHalted`, `kLuldPause`, `kSsrActive`, `kShortable`, `kEasyToBorrow`.
  - `luld_lo` and `luld_hi` (`Price`, zero when unknown) and a `reason` code.
- The engine keeps one fixed status per instrument and journals each message. Instruments that never receive one have `kShortable` set and no bands.
- Sources:
  - `ItchSource` maps H to `kHalted` and `kLuldPause`, and Y to `kSsrActive`.
  - ITCH has no band message, so `LuldBandCalculator` computes bands from the trades per the LULD plan, off the engine thread, and emits a message when a band moves by a tick or more.
  - Alpaca maps its trading status and LULD channels.
  - Shortable and easy-to-borrow come from Alpaca's asset endpoint at start and on each account poll.
- Halted or paused: the instrument's quotes are cancelled, new orders are rejected with `RejectReason::Halted = 17`, and `on_status(ctx, inst)` fires. Quoting resumes on the message that clears the bit.
- LULD: after the strategy and before risk, bid prices above `luld_hi` are clamped to `luld_hi`, and ask prices below `luld_lo` to `luld_lo`. The ctx reports the clamp.
- A sell is a short sale when it would take position plus open sells below zero. Short sales are rejected with `RejectReason::NotShortable = 18` when:
  - `[risk] allow_short = false` (default for equities), or
  - `kShortable` is clear.
- When `kSsrActive` is set, a short sale must be priced above the best bid of the book the engine holds; otherwise it is rejected with `RejectReason::ShortSaleRestricted = 19`. With IEX-only data that bid is not the national best bid, and the check is advisory; the broker's check is final.

### 5. Buying power

- New `EventType::AccountUpdate = 28` with `AccountUpdateMsg`: venue, `cash`, `buying_power` and `equity` (`Notional`). Connectors send it after reconciliation, after each fill, and from a poll (Alpaca `GET /v2/account`, default every 5 s). It is journaled.
- Risk rejects a buy with `RejectReason::InsufficientBuyingPower = 20` when order notional plus the venue's open buy notional exceeds `buying_power - [risk] buying_power_reserve`.
- The check is off until a venue's first `AccountUpdate`, so crypto venues are unaffected. FastMM does not compute margin.

### 6. Fees and PnL

- `FeeModel` moves to `include/fastmm/core/fee_schedule.hpp` as `FeeSchedule`. Every added field defaults to zero:
  - existing `maker_cbps`, `taker_cbps`;
  - `maker_per_share`, `taker_per_share` (signed `Notional`, for exchange pass-through fees and rebates);
  - `commission_per_share`, `commission_min`, `commission_max_cbps`;
  - `sec_fee_per_million` (on sell notional);
  - `taf_per_share`, `taf_max`.
- Each regulatory fee is rounded up to the cent per execution.
- Configuration keys go under `[venues.<name>.fees]`. Rates are configuration and are not updated automatically. `configs/alpaca-paper.toml` and `configs/ibkr-paper.toml` state each rate with its effective date.
- The simulated venue charges the schedule on each fill. Alpaca and IBKR do not report regulatory fees per fill, so the connector fills `OrderFillMsg::fee` from the same schedule and marks the estimate in the fill's padding byte `fee_estimated`.
- `PositionTracker` and `PnLLedger` are unchanged: a fee is still a `Notional` in quote currency. Crypto fees, results and hashes are unchanged, since every added field is zero.

### 7. Positions across days and corporate actions

- Equity positions are flat at the close. From `flatten_before_close_s` (default 60) before the end of the last allowed session, the engine sends marketable limit IOC orders, priced `flatten_slippage_bps` through the best price, every second until flat. Each is journaled as a normal order.
- A live session refuses to start with a reconciled equity position unless `allow_start_with_position = true`.
- There is no split or dividend adjustment. Backtests run one trading day per `ItchSource` file, with fresh books and locates.

### 8. Instrument limit

`kMaxInstruments` stays 256: a market maker quotes a few symbols, and every per-instrument array in the engine is sized by it. `ItchSource` drops messages for symbols not in the instrument table, as `ItchDecoder` does today.

### 9. Constraints

- Hot path: schedules, session schedule, trading status and buying power are fixed arrays filled at load or copied from messages. No new virtual call. `tests/hotpath/noalloc_test.cpp` covers each new message and hook.
- Determinism: each new input is either in the journal header (tick schedules, session schedule) or a journaled event (`TradingStatus`, `AccountUpdate`). `kJournalVersion = 4` adds header fields for a reference section (schedules and calendar) with its CRC32C. Readers accept versions 1–4.
- Strategies: existing strategies compile and behave the same. `on_session` and `on_status` are optional; session gating, LULD clamps, short-sale checks and flattening are engine behaviour configured per instrument.
- Crypto: instruments without a schedule, calendar or status keep today's code path. Existing golden journals and `sample_1000.sha256` replay unchanged.

### 10. Python

- `FASTMM_HOT_ABI_VERSION = 2` appends inputs to `fastmm_hot_ctx`:
  - `session` (int32);
  - `halted`, `luld_paused`, `ssr`, `shortable`, `luld_clamped` (uint8);
  - `luld_lo`, `luld_hi`, `buying_power`, `round_lot` (double, with `_raw` int64 forms).
- `ctx.tick` is the tick at the mid.
- `fastmm.SESSION_CLOSED` … `fastmm.SESSION_OPEN` are integer constants usable in hot code. `on_session` and `on_status` join the hot hook set.
- `ctx.snapshot()` carries the same fields.
- `fastmm.data.itch(path, date, symbols)` returns an `ItchSource` for `run_backtest`.

## Consequences

- US equities run in backtests, replay and paper trading with the same strategy classes. Crypto results do not change.
- Through Alpaca, orders go over REST at 200 requests per minute for the whole account, which is about 3 requests/s. Round-trip time is set by the broker and the internet path, in milliseconds, so FastMM's microsecond engine latency makes no difference there. Quotes must requote rarely; `min_requote_interval` and `min_requote_ticks` need values in seconds and several ticks.
- Retail economics: commissions and regulatory fees on sells exceed exchange rebates, and wholesaler routing gives no control over the venue. A backtest with the fee schedule shows the spread capture a strategy needs to break even. IBKR with directed routing and tiered pricing narrows the gap and does not close it.
- Backtests need ITCH data. Without a licence it is Nasdaq's sample files, whose terms are unstated; a Databento licence needs a business entity.
- Backtests fill against L2 levels from `L3Book` with the existing `L2Queue` model, not exact queue position by order reference. With IEX-only live data, backtest (Nasdaq) and live (IEX) books differ.
- The LULD bands in backtests are computed, not published, and can differ from the SIP's.
- The calendar file needs a yearly update, and CI enforces it.
- Hot-hook ABI version 2 breaks Python strategies compiled against version 1; they recompile at import.

## Implementation order

Steps 1–3 can start in parallel. Step 4 needs steps 1–3. Step 5 needs steps 1–3. Step 6 needs steps 3 and 5. Step 7 needs step 5.

1. Tick schedule and lots: `core/tick_schedule.hpp`, `Instrument` fields, `tick_at`, QuoteManager and risk changes, Deribit on the core type, `[tick_schedules]` configuration. Tests: rounding and validation at $0.9999, $1.00 and $1.0001 for both sides; Deribit encoder tests unchanged; golden hashes unchanged; C++ BasicMM tick-to-order p50 on crypto within 2% of main.
2. Engine inputs: `SessionSchedule`, `TradingStatusMsg`, `AccountUpdateMsg`, journal version 4, reject reasons 17–20, session gating, LULD clamp, short-sale checks, flattening, `on_session`, `on_status`, `configs/calendars/us-equities.toml`, `tools/update_calendar.py` and the coverage test. Tests: a synthetic journal crossing Pre, Regular and Post with a halt, an SSR trigger, a band move and an account update replays to the same outbound hash; journals of versions 1–3 replay; the no-allocation test covers the new paths.
3. Fees: `FeeSchedule` and the configuration keys. Tests: hand-computed commission, SEC fee and TAF for a 100-share sell at $50.00 and a 60 000-share sell hitting the TAF cap; crypto backtest results identical to main.
4. `ItchSource`: `ItchDecoder` to `L3Book` to `BookDeltaMsg` and `TradeMsg`, H and Y to `TradingStatusMsg`, `LuldBandCalculator`, Day expiry in the simulated venue. Tests: an ITCH fixture from `tests/fixtures/nasdaq/generate_fixtures.py` with add, execute, cancel, replace, halt and Reg SHO messages gives the expected top of book and statuses; two backtests of it give the same hash; a manual run over one Nasdaq sample day reports events/s.
5. Alpaca connector in `include/fastmm/venues/alpaca/` and `src/venues/alpaca/`: REST order entry and replace, `trade_updates` stream, stock data stream (IEX or SIP), status and LULD channels, account poll, calendar check, `cancel_all` on a blocking connection, rate limit 200/min, `configs/alpaca-paper.toml`. Tests against the fake server with recorded fixtures: order lifecycle, replace, rejects, reconnection, rate-limit refusal, extended-hours flag by session, halt pulls quotes, and a live session against the fake server that replays to the same hash. Then a paper session once the owner has an account.
6. Python ABI version 2, `fastmm.data.itch`, the new hooks. Tests: a hot BasicMM port reproduces the C++ hash on the ITCH fixture; ADR-0013 `python/tests` pass.
7. IBKR connector over the TWS API socket to IB Gateway: directed routing (`route = "SMART"` or an exchange), `outsideRth`, tick types for halts and shortability, 50 messages/s, `max_open_orders` capped at 20 per side. Tests against a fake gateway replaying recorded wire fixtures; then an IBKR paper session.
8. Docs: an equities how-to under `docs/how-to/venues/`, configuration and venue references, calendar maintenance, the go-live checklist entry for equity risk and fees.
