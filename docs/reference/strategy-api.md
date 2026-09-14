# Strategy API

Code blocks come from [`tests/docs/strategy_api_doc_test.cpp`](../../tests/docs/strategy_api_doc_test.cpp) (ctest `docs.strategy_api`). The headers are listed in [Public API](public-api.md); `#include "fastmm/strategy.hpp"` brings in everything a strategy header needs.

## Strategy class

A strategy is a default-constructible class with a static `name()` and a static `schema()` (concept `StrategyLike`). `StrategyBase<Params>` provides `schema()` and the parameters:

| Member of `StrategyBase<Params>` | Meaning |
|---|---|
| `params() -> const Params&` | the current parameters; read-only |
| `configure(const ParamMap&) -> std::optional<std::string>` | applies `name -> value` strings, runs `validate()`, returns the first error; on error the parameters are unchanged. Startup only |
| `describe_params() -> std::string` | `name=value` pairs that parse back to the same values |

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#verify -->
```cpp
static_assert(StrategyLike<AllHooks>);
static_assert(verify_strategy<AllHooks>());
```

`verify_strategy<S>()` checks every hook with stand-in context and book types, so the check runs in the strategy header instead of when an engine is built.

## Hooks

Hooks are public member functions; every hook is optional:

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#hooks -->
```cpp
void on_start(auto& ctx) noexcept {
  hit(kStart);
  static_cast<void>(ctx.every(params().timer_ms, /*tag=*/7));
}
void on_stop(auto& /*ctx*/) noexcept { hit(kStop); }
void on_book(auto& ctx, InstrumentId id, const auto& book) noexcept {
  hit(kBook);
  if (!book.is_valid()) return ctx.pull_quotes(id);
  DesiredQuotes q;
  q.bid(book.best_bid().price, params().quote_qty);
  q.ask(book.best_ask().price, params().quote_qty);
  static_cast<void>(ctx.set_quotes(id, q));
}
void on_book_ticker(auto& /*ctx*/, InstrumentId /*id*/, const BookTickerMsg& /*m*/) noexcept {
  hit(kBookTicker);
}
void on_trade(auto& /*ctx*/, InstrumentId /*id*/, const TradeMsg& /*m*/) noexcept { hit(kTrade); }
void on_option_ticker(auto& /*ctx*/, InstrumentId /*id*/, const OptionTickerMsg& /*m*/) noexcept {
  hit(kOptionTicker);
}
void on_fill(auto& /*ctx*/, const Fill& /*fill*/) noexcept { hit(kFill); }
void on_order_update(auto& /*ctx*/, const OmsUpdate& /*u*/) noexcept { hit(kOrderUpdate); }
void on_timer(auto& /*ctx*/, TimerId /*id*/, std::uint64_t tag) noexcept {
  if (tag == 7) hit(kTimer);
}
void on_connection(auto& /*ctx*/, const ConnectionStateMsg& /*m*/) noexcept { hit(kConnection); }
void on_quoting(auto& /*ctx*/, bool /*enabled*/) noexcept { hit(kQuoting); }
```

| Hook | Called |
|---|---|
| `on_start(ctx)` | once, before the first event; `ctx.quoting_enabled()` is false in a dry run |
| `on_stop(ctx)` | once, when the engine finishes; quotes are not pulled here, shutdown's cancel-all does that |
| `on_book(ctx, id, book)` | after a snapshot or delta is applied, the position is marked and risk has seen the mid; the book may be invalid (empty or crossed) |
| `on_book_ticker(ctx, id, m)` | a top-of-book update without depth |
| `on_trade(ctx, id, m)` | a public trade |
| `on_option_ticker(ctx, id, m)` | an option's mark, implied volatilities and greeks ([Options](options.md)) |
| `on_fill(ctx, fill)` | one of our executions, after the position and fees are updated and before the `on_order_update` of the same execution |
| `on_order_update(ctx, u)` | an OMS state change of one of our orders, including the end of a reconciliation |
| `on_timer(ctx, id, tag)` | a timer from `ctx.every` or `ctx.once` fired |
| `on_connection(ctx, m)` | a venue channel changed state; for any state other than `Live` the engine has already pulled that venue's quotes and, on the market-data channel, cleared its books |
| `on_quoting(ctx, enabled)` | `ctx.quoting_enabled()` changed ([below](#on_quoting)) |

Rules:

- Hooks return `void`. `auto& ctx` and `template <class Ctx> void on_x(Ctx& ctx, ...)` are the same. `noexcept` is recommended; hooks run inside `noexcept` engine code.
- Instrument-scoped hooks (`on_book`, `on_book_ticker`, `on_trade`, `on_option_ticker`, `on_fill`) fire only for instruments in the table, so `ctx.book(id)` and `ctx.instrument(id)` are valid in them.
- `on_fill` fires for every execution on an instrument in the table, including late fills (the order was already terminal) and fills for ids the OMS does not know. Fills on other instruments are counted in `EngineStats::unknown_instrument_fills`.
- Hooks must not block, allocate on every event or read anything that is not an engine input (the system clock, `std::random_device`, files); see [Determinism](../explanation/determinism.md).

### on_quoting

`set_quotes` is ignored while quoting is disabled: an operator pull (`PullQuotes`), the kill switch, a reconciliation, or a dry run. `on_quoting(ctx, enabled)` reports changes, so a strategy can requote as soon as quoting is back.

- The engine compares `ctx.quoting_enabled()` before and after each event, fired timer and `on_start`, and calls the hook after the triggering hook has returned and all flags are final. At the end of a reconciliation that is after the engine has placed the quotes it paused, so a requote from `on_quoting` replaces them.
- It never fires from inside a context call: a kill switch tripped by `set_quotes` or `send` is reported after the hook that made the call returns.
- It does not fire for the initial state; `on_start` reads `ctx.quoting_enabled()`.
- A lost connection does not change `quoting_enabled()`; `on_connection` reports it.

### Signature checks

The engine checks each hook name when it is compiled. A member with a hook's name whose call does not compile stops the build, one error per hook:

```text
error: static assertion failed: fastmm: on_trade has the wrong signature or is not public; expected void on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)
```

- Likely misspellings (`on_fills`, `on_trades`, `on_order_book`, `on_tick`, `on_bbo`, `on_order`, `on_execution`, `on_disconnect`, `onBook`, ...) produce the warning `fastmm: on_fills is not a hook; did you mean on_fill?`, an error with `FASTMM_WERROR`. Silence it with `static constexpr bool fastmm_allow_near_miss_names = true;` in the strategy.
- Limits: a `final` class is only call-checked, so a wrong signature is silently not called; a hook that is ambiguous across two bases is reported as a wrong signature; a hook with a deduced (`auto`) return type must not be checked with `verify_strategy`; private hooks are rejected, also with `friend`; implicit argument conversions are accepted.
- `Engine::start()` logs the implemented hooks: `strategy first_mm hooks: start book fill timer connection quoting`.

## Context

`ctx` is a `StrategyContext<Engine>`, a non-owning view of the engine:

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#context -->
```cpp
// time and reference data
static_assert(std::same_as<decltype(lvalue<Ctx>().now()), Timestamp>);
static_assert(std::same_as<decltype(lvalue<Ctx>().instrument(InstrumentId{})), const Instrument&>);
static_assert(std::same_as<decltype(lvalue<Ctx>().instruments()), const InstrumentTable&>);
static_assert(std::same_as<decltype(lvalue<Ctx>().contains(InstrumentId{})), bool>);
// market data
static_assert(std::same_as<decltype(lvalue<Ctx>().book(InstrumentId{})), const Book&>);
// portfolio
static_assert(std::same_as<decltype(lvalue<Ctx>().position(InstrumentId{})), const Position&>);
static_assert(std::same_as<decltype(lvalue<Ctx>().portfolio()), Portfolio>);
// quoting
static_assert(
    std::same_as<decltype(lvalue<Ctx>().set_quotes(InstrumentId{}, DesiredQuotes{})), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().pull_quotes(InstrumentId{})), void>);
static_assert(std::same_as<decltype(lvalue<Ctx>().pull_all_quotes()), void>);
static_assert(std::same_as<decltype(lvalue<Ctx>().working_quote(InstrumentId{}, Side::Buy, 0)),
                           const Order*>);
// direct orders
static_assert(std::same_as<decltype(lvalue<Ctx>().send(NewOrderRequest{})),
                           Result<ClientOrderId, RejectReason>>);
static_assert(
    std::same_as<decltype(lvalue<Ctx>().cancel(ClientOrderId{})), Result<void, RejectReason>>);
static_assert(std::same_as<decltype(lvalue<Ctx>().replace(ClientOrderId{}, Price{}, Qty{})),
                           Result<void, RejectReason>>);
static_assert(std::same_as<decltype(lvalue<Ctx>().order(ClientOrderId{})), const Order*>);
static_assert(std::same_as<decltype(lvalue<Ctx>().open_qty(InstrumentId{}, Side::Buy)), Qty>);
static_assert(std::same_as<decltype(lvalue<Ctx>().oms()), const Oms&>);
// timers
static_assert(std::same_as<decltype(lvalue<Ctx>().every(Duration{}, std::uint64_t{})), TimerId>);
static_assert(std::same_as<decltype(lvalue<Ctx>().once(Duration{}, std::uint64_t{})), TimerId>);
static_assert(std::same_as<decltype(lvalue<Ctx>().cancel_timer(TimerId{})), bool>);
// control
static_assert(std::same_as<decltype(lvalue<Ctx>().quoting_enabled()), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().killed()), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().venue_killed(VenueId{})), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().request_stop()), void>);
// randomness (seeded from the configuration)
static_assert(std::same_as<decltype(lvalue<Ctx>().rng()), Xoshiro256ss&>);
```

| Method | Notes |
|---|---|
| `now()` | the engine clock of the current event; virtual time in backtests and replay |
| `instrument(id)`, `instruments()`, `contains(id)` | the instrument table; iterate `instruments()` for all instruments |
| `book(id)` | the L2 book of an instrument ([Book](#book)) |
| `position(id)` | `qty` (signed, base units), average price, realised PnL and fees of one instrument |
| `portfolio()` | `realized`, `unrealized`, `fees` and `net` (realised + unrealised - fees) over all instruments, quote currency; a loop, not for every event |
| `set_quotes(id, q)` | the desired ladder; the quote manager sends the difference to the working orders. Returns false when the quotes were ignored: quoting disabled or instrument not in the table |
| `pull_quotes(id)`, `pull_all_quotes()` | cancel the quotes of one or every instrument |
| `working_quote(id, side, level)` | the open order in a quote slot (level 0 is closest to the mid), or `nullptr` |
| `send(req)` | a direct order, risk-checked and journaled. Fails with the `RejectReason` of a risk check, the OMS or the kill switch; `InvalidTag` when `user_tag` lies in the quote manager's range |
| `cancel(id)`, `replace(id, px, qty)` | direct order management; cancels are always allowed |
| `order(id)` | an open order, or `nullptr` once it is terminal; valid until the next context call |
| `open_qty(id, side)` | unfilled quantity of our open orders on one side, quotes included |
| `oms()` | read-only order management state |
| `every(period, tag)`, `once(delay, tag)` | timers; `on_timer` receives the id and tag. They fire in engine time and are journaled |
| `cancel_timer(id)` | false when the timer no longer exists |
| `quoting_enabled()`, `killed()`, `venue_killed(venue)` | quoting state, the global kill switch and one venue's kill switch (new orders to that venue are refused and `set_quotes` ignores its instruments) |
| `request_stop()` | sets the engine's stop flag: a backtest ends after the current engine step; replay always drains the journal |
| `rng()` | a `Xoshiro256ss` seeded from `[engine] rng_seed`, identical in replay |

`set_quotes` applies hysteresis (`[engine] min_requote_ticks`, `min_requote_interval_ms`), skips orders awaiting a venue response and uses replace where the venue supports it. A direct order returns a `Result`:

```cpp
auto id = ctx.send(NewOrderRequest::limit(inst.id, Side::Buy, px, qty).post_only());
if (!id) return FASTMM_LOG_WARN("order refused: {}", id.error());  // e.g. RejectReason::MaxPosition
```

## Book

`on_book` receives the engine's `L2Book`; any type modelling `BookView` has the same read API:

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#book -->
```cpp
static_assert(BookView<Book>);  // best_bid(), best_ask(), mid(), spread(), level(side, i), ...
static_assert(std::same_as<decltype(lvalue<const Book>().best_bid()), Level>);
static_assert(std::same_as<decltype(lvalue<const Book>().mid()), Price>);
static_assert(std::same_as<decltype(lvalue<const Book>().is_valid()), bool>);
```

| Method | Meaning |
|---|---|
| `best_bid()`, `best_ask()` | the touch as `Level{price, qty}`; zero when the side is empty |
| `mid()`, `spread()` | `(bid + ask) / 2` truncated, `ask - bid` |
| `level(side, i)`, `depth(side)`, `top<N>(side)` | levels from the touch outwards |
| `qty_at_or_better(side, px)`, `price_for_qty(side, qty)` | depth queries |
| `is_valid()` | both sides present and not crossed |
| `seq()`, `last_update()` | the venue sequence number and receive time of the last update |

## Fill

`Fill` is built on the engine's stack; `update` and `msg` are valid only during the call.

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#fill -->
```cpp
static_assert(std::same_as<decltype(Fill::instrument), InstrumentId>);
static_assert(std::same_as<decltype(Fill::side), Side>);
static_assert(std::same_as<decltype(Fill::price), Price>);
static_assert(std::same_as<decltype(Fill::qty), Qty>);
static_assert(std::same_as<decltype(Fill::position_delta), Qty>);
static_assert(std::same_as<decltype(Fill::fee), Notional>);
static_assert(std::same_as<decltype(Fill::fee_converted), bool>);
static_assert(std::same_as<decltype(Fill::liquidity), Liquidity>);
static_assert(std::same_as<decltype(Fill::known), bool>);
static_assert(std::same_as<decltype(Fill::late), bool>);
static_assert(std::same_as<decltype(Fill::order_done), bool>);
static_assert(std::same_as<decltype(Fill::update), const OmsUpdate*>);
static_assert(std::same_as<decltype(Fill::msg), const OrderFillMsg*>);
```

| Field | Meaning |
|---|---|
| `instrument`, `side` | the order's, else the fill message's |
| `price`, `qty` | this execution |
| `position_delta` | signed change of the position; differs from `qty` when the commission is charged in the base asset |
| `fee`, `fee_converted` | fee in the quote currency as booked; `fee_converted` is false when the commission was in a third asset (for example BNB) and not booked |
| `liquidity` | `Maker`, `Taker` or `Unknown` |
| `known` | the id matched an order; `update->order` holds its snapshot (for an order that was already terminal: id, instrument, side, state and filled quantity) |
| `late` | the order was already terminal when the fill arrived |
| `order_done` | the order reached a terminal state with this fill |
| `update`, `msg` | the OMS update (non-null when `known`) and the raw `OrderFillMsg` |

## Messages

The messages hooks receive are fixed-size, trivially copyable structs (`core/messages.hpp`):

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#messages -->
```cpp
static_assert(std::same_as<decltype(TradeMsg::price), Price>);
static_assert(std::same_as<decltype(TradeMsg::qty), Qty>);
static_assert(std::same_as<decltype(TradeMsg::aggressor), Side>);
static_assert(std::same_as<decltype(TradeMsg::trade_id), std::uint64_t>);
static_assert(std::same_as<decltype(BookTickerMsg::bid_px), Price>);
static_assert(std::same_as<decltype(BookTickerMsg::bid_qty), Qty>);
static_assert(std::same_as<decltype(BookTickerMsg::ask_px), Price>);
static_assert(std::same_as<decltype(BookTickerMsg::ask_qty), Qty>);
static_assert(std::same_as<decltype(OptionTickerMsg::mark_price), Price>);
static_assert(std::same_as<decltype(OptionTickerMsg::underlying_price), Price>);
static_assert(std::same_as<decltype(OptionTickerMsg::mark_iv), double>);
static_assert(std::same_as<decltype(OptionTickerMsg::delta), double>);
static_assert(std::same_as<decltype(ConnectionStateMsg::state), ConnState>);
static_assert(std::same_as<decltype(ConnectionStateMsg::channel), std::uint8_t>);
static_assert(std::same_as<decltype(EventHeader::instrument), InstrumentId>);
static_assert(std::same_as<decltype(EventHeader::venue), VenueId>);
static_assert(std::same_as<decltype(EventHeader::recv_ts), Timestamp>);
static_assert(std::same_as<decltype(EventHeader::exch_ts), Timestamp>);
static_assert(std::same_as<decltype(OmsUpdate::order), Order>);
static_assert(std::same_as<decltype(OmsUpdate::prev), OrderState>);
static_assert(std::same_as<decltype(OmsUpdate::terminal), bool>);
```

- Every message starts with an `EventHeader`: `type`, `venue`, `instrument`, `exch_ts` (venue event time) and `recv_ts` (receive time), plus `seq` and flags.
- `TradeMsg::aggressor` is the taker's side.
- `OptionTickerMsg`: `mark_price` is in the instrument's price unit, `underlying_price` and `index_price` in the underlying's quote currency, implied volatilities are annualised decimals (0.312 is 31.2 %), greeks are the venue's; NaN marks a field the venue did not send.
- `ConnectionStateMsg::state` is `Disconnected`, `Connecting`, `Live`, `Stale`, `Resyncing` or `Dead`; `channel` is 0 for market data and 1 for the order and user stream; `reason_code` is venue-specific.
- `OmsUpdate` carries the order snapshot after the transition (`order`), the previous state (`prev`), and `known`, `changed` and `terminal` flags.

## Parameters

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#params -->
```cpp
struct AllHooksParams {
  FASTMM_PARAMS(AllHooksParams)
  FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels per side")
  FASTMM_PARAM(double, gamma, 0.1, 0.0, 10.0, "risk aversion")
  FASTMM_PARAM(bool, hedge, false, false, true, "hedge fills")
  FASTMM_PARAM(Qty, quote_qty, 0.001_qty, 0_qty, 1000_qty, "quantity per side")
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 1000_bps, "half spread, bps")
  FASTMM_PARAM_MS(timer_ms, milliseconds(100), milliseconds(1), milliseconds(60000), "timer, ms")

  // Optional: runs after all keys of a configure() call are applied.
  [[nodiscard]] std::optional<std::string> validate() const {
    if (hedge && levels > 4) return "hedge supports at most 4 levels";
    return std::nullopt;
  }
};
```

| Declaration | Field type | Schema type | Configuration value |
|---|---|---|---|
| `FASTMM_PARAM(int, ...)` | any integer type | `int` | whole number: `3`, `3.0`, `3e0` |
| `FASTMM_PARAM(double, ...)` | floating point | `double` | any finite number |
| `FASTMM_PARAM(bool, ...)` | `bool` | `bool` | `true`/`false`, `1`/`0`, `yes`/`no`, `on`/`off` |
| `FASTMM_PARAM(Qty, ...)` | `Price`, `Qty`, `Notional` | `decimal` | exact, up to 8 decimals |
| `FASTMM_PARAM_BPS(name, ...)` | `Ratio` | `bps` | basis points, exact, up to 4 decimals |
| `FASTMM_PARAM_MS(name, ...)` | `Duration` | `ms` | whole milliseconds; bounds are `Duration`s |

- The arguments are the field name, default, minimum, maximum and a description; `FASTMM_PARAMS(Self)` comes first. At most 32 parameters.
- `decimal`, `bps` and `ms` values are parsed as decimal text, never through a double; exponents are accepted ([Fixed point](fixed-point.md#parsing-and-formatting)).
- Ranges are checked on the typed value: `parameter 'quote_qty': value 1000.5 outside [0, 1000]`.
- The schema drives configuration checks, `--list-strategies`, `--param key=value` and `fastmm.strategies()` in Python.

## Fixed-point helpers

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#helpers -->
```cpp
static_assert(100.25_px == Price::from_raw(10'025'000'000));
static_assert(0.01_qty == Qty::from_raw(1'000'000));
static_assert(5_bps == Ratio::from_raw(50'000));  // 1 bp = 10'000 raw, 1.0 = 1e8 raw
static_assert(100_px * 5_bps == 0.05_px);         // Price * Ratio, one truncation toward zero
static_assert(ratio(1_px, 4_px) == Ratio::from_raw(25'000'000));
static_assert(mid(100.00_px, 100.02_px) == 100.01_px);
static_assert(microprice(Level{100.00_px, 3_qty}, Level{100.04_px, 1_qty}) == 100.03_px);
static_assert(spread_ratio(99_px, 101_px) == 200_bps);
static_assert(away_from(100_px, Side::Buy, 0.5_px) == 99.5_px);
static_assert(inventory_allows(Side::Buy, 0.003_qty, 0.001_qty, 0.004_qty));
static_assert(!inventory_allows(Side::Buy, 0.004_qty, 0.001_qty, 0.004_qty));
static_assert(inventory_allows(Side::Sell, 5_qty, 1_qty, Qty{}));  // a zero limit is no limit
static_assert(std::same_as<decltype(lvalue<const Instrument>().ticks(3)), Price>);
static_assert(std::same_as<decltype(lvalue<DesiredQuotes>().bid(Price{}, Qty{})), bool>);
static_assert(std::same_as<decltype(lvalue<DesiredQuotes>().uncross(Price{})), void>);
static_assert(
    std::same_as<decltype(keep_passive(lvalue<DesiredQuotes>(), Price{}, Price{}, Price{})), void>);
static_assert(
    std::same_as<decltype(NewOrderRequest::limit(InstrumentId{}, Side::Buy, Price{}, Qty{})
                              .post_only()
                              .reduce_only()
                              .ioc()
                              .tag(7)),
                 LimitOrder>);
static_assert(std::convertible_to<LimitOrder, NewOrderRequest>);
```

`NewOrderRequest::limit(id, side, px, qty)` builds a GTC limit order. [Fixed point](fixed-point.md) has the types, literals, rounding rules, quoting helpers and ranges.

## Test harness

`fastmm::sim::StrategyHarness<S>` (`fastmm/testing/strategy_harness.hpp`, in `fastmm::sim`) runs a strategy in a real `Engine<S, SimClock, SimTransport, InlineFeed>` against a simulated venue on virtual time. Each call runs the engine until it is idle:

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#harness -->
```cpp
sim::StrategyHarness<AllHooks> h({{"quote_qty", "0.001"}, {"timer_ms", "5"}});  // on_start
h.book("100.00", "100.02");   // on_book; quotes go out
h.advance(milliseconds(10));  // acks (on_order_update), the timer (on_timer)
REQUIRE(h.working_orders().size() == 2);
REQUIRE(h.fill(Side::Buy));              // on_fill, then on_order_update
h.trade(100.02_px, 0.5_qty, Side::Buy);  // on_trade
BookTickerMsg ticker{};
init_header(ticker, EventType::BookTicker, h.instrument(), VenueId{0});
ticker.bid_px = 100.00_px;
ticker.ask_px = 100.02_px;
h.push(ticker.hdr);  // on_book_ticker
OptionTickerMsg option{};
init_header(option, EventType::OptionTicker, h.instrument(), VenueId{0});
h.push(option.hdr);   // on_option_ticker
h.disconnect();       // on_connection (market data lost)
h.reconnect();        // on_connection (live again)
h.pull_quotes();      // on_quoting(false)
h.resume_quotes();    // on_quoting(true)
h.engine().finish();  // on_stop
```

| Call | Effect |
|---|---|
| `StrategyHarness<S>(params, options)` | configures the strategy (`std::invalid_argument` on a bad value) and starts the engine; `HarnessOptions` sets instruments, `EngineConfig`, latency (100 µs each way) and start time |
| `book(bid, ask[, size, id])` | a one-level snapshot each side; prices as strings or `Price` |
| `trade(px, qty, aggressor[, id])` | a public trade |
| `fill(side[, qty, id])` | a taker at the venue fills our best working order on `side` (all of it when `qty` is 0), then advances by the ack latency; false when there is none |
| `disconnect([channel, venue])`, `reconnect(...)` | a `ConnectionStateMsg`; channel 0 is market data |
| `pull_quotes()`, `resume_quotes()` | operator control, `on_quoting(false)` and `on_quoting(true)` |
| `push(msg.hdr)` | any other message at the current time |
| `advance(duration)` | moves virtual time: orders reach the venue, acks and fills return, timers fire |
| `working_orders([id])` | our working orders, bids then asks, best first |
| `engine()`, `strategy()`, `now()`, `instrument()`, `price(s)`, `quantity(s)` | access and conversions |

The default instrument is BTCUSDT on venue 0 with a tick of 0.01 and a lot of 0.001. The harness allocates and is single-threaded.

## Registration

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#registration -->
```cpp
void register_strategies(StrategyRegistry& r) {
  register_strategy<AllHooks>(r);  // Sim + Replay + Live
}
static_assert(std::same_as<decltype(&register_strategies), StrategyModule>);
```

- `register_strategy<S>(r)` adds the Sim, Replay and Live factories of `S`. The file that calls it includes `fastmm/strategies/factories.hpp`, which compiles the three engines; without it the link fails and names the missing factory.
- A strategy library exports one such function (`StrategyModule`); apps pass it to `fastmm::cli::live`, `backtest` or `replay`. The built-in strategies are always registered first.
- Registering the same function twice is a no-op; a name registered by different code throws `StrategyConflict` (the command lines exit with code 3).
- `bt::run_backtest<S>(cfg, source)` and `StrategyHarness<S>` need no registration.

Build setups: [Register a strategy](../how-to/strategies/register-a-strategy.md).

## Logging

`FASTMM_LOG_TRACE`, `FASTMM_LOG_DEBUG`, `FASTMM_LOG_INFO`, `FASTMM_LOG_WARN` and `FASTMM_LOG_ERROR` take a fmt format string (`FASTMM_LOG_INFO("filled {} @ {}", qty, px)`); `Price`, `Qty`, `Side` and the enums format directly. Records go through a per-thread ring to a background thread; when the ring is full the record is dropped and counted. Logs are not journaled: strategy logic must not depend on them.
