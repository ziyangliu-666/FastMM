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
void on_timer(auto& ctx, TimerId /*id*/, std::uint64_t tag) noexcept {
  if (tag != 7) return;
  hit(kTimer);
  // Once: a price off the tick is refused by the engine's own check (on_risk_reject).
  if (count(kTimer) == 1)
    static_cast<void>(ctx.send(NewOrderRequest::limit(
        InstrumentId{0}, Side::Buy, Price::from_decimal("99.995").value(), params().quote_qty)));
}
void on_connection(auto& /*ctx*/, const ConnectionStateMsg& /*m*/) noexcept { hit(kConnection); }
void on_quoting(auto& /*ctx*/, bool /*enabled*/) noexcept { hit(kQuoting); }
void on_params(auto& /*ctx*/) noexcept { hit(kParams); }
void on_balance(auto& /*ctx*/, const BalanceMsg& /*m*/) noexcept { hit(kBalance); }
void on_perp_state(auto& /*ctx*/, InstrumentId /*id*/, const PerpStateMsg& /*m*/) noexcept {
  hit(kPerpState);
}
void on_risk_reject(auto& /*ctx*/, const RiskReject& r) noexcept {
  if (r.reason == RejectReason::InvalidTick && !r.replace) hit(kRiskReject);
}
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
| `on_params(ctx)` | a parameter update was applied; `params()` holds the new values ([Parameter updates](#parameter-updates)) |
| `on_balance(ctx, m)` | a venue reported one asset of the account ([Balances](#balances)); `ctx.balance` already holds it |
| `on_perp_state(ctx, id, m)` | a venue's mark, index or funding of a derivative arrived ([Perpetuals](#perpetuals)); `ctx.mark` and `ctx.funding` already hold it |
| `on_risk_reject(ctx, r)` | the engine's own risk check refused a new order or replace ([below](#on_risk_reject)); nothing was sent |

Rules:

- Hooks return `void`. `auto& ctx` and `template <class Ctx> void on_x(Ctx& ctx, ...)` are the same. Hooks run inside `noexcept` engine code; declare them `noexcept`.
- Instrument-scoped hooks (`on_book`, `on_book_ticker`, `on_trade`, `on_option_ticker`, `on_perp_state`, `on_fill`) fire only for instruments in the table, so `ctx.book(id)` and `ctx.instrument(id)` are valid in them.
- `on_fill` fires for every execution on an instrument in the table, including late fills (the order was already terminal) and fills for ids the OMS does not know. Fills on other instruments are counted in `EngineStats::unknown_instrument_fills`.
- Hooks must not block, allocate on every event or read anything that is not an engine input (the system clock, `std::random_device`, files); see [Determinism](../explanation/determinism.md).

### on_quoting

`set_quotes` is ignored while quoting is disabled: an operator pull (`PullQuotes`), the kill switch, a reconciliation, parameters older than `max_param_age_ms` ([Parameter updates](#parameter-updates)), or a dry run. `on_quoting(ctx, enabled)` reports changes, so a strategy can requote as soon as quoting is back.

- The engine compares `ctx.quoting_enabled()` before and after each event, fired timer and `on_start`, and calls the hook after the triggering hook has returned and all flags are final. At the end of a reconciliation that is after the engine has placed the quotes it paused, so a requote from `on_quoting` replaces them.
- It never fires from inside a context call: a kill switch tripped by `set_quotes` or `send` is reported after the hook that made the call returns.
- It does not fire for the initial state; `on_start` reads `ctx.quoting_enabled()`.
- A lost connection does not change `quoting_enabled()`; `on_connection` reports it.

### on_risk_reject

A `send`, `replace` or `set_quotes` the `[risk]` limits refuse sends nothing: `send` and `replace` return the `RejectReason`, `set_quotes` returns true (the quote manager keeps the level for the next requote) and the engine counts and logs the reject. `on_risk_reject(ctx, const RiskReject& r)` reports each one to the strategy: `instrument`, `side`, `type`, `reason`, `price`, `qty`, `user_tag` (a quote's tag is the `QuoteManager`'s), `replace` with `order` (the id a replace would have changed; invalid for a new order), and `time`.

- It is delivered after the hook that asked has returned, in the order the rejects happened, together with `on_quoting` ([above](#on_quoting)); never from inside the context call that was refused.
- An order refused again from inside `on_risk_reject` is reported in a further round, a few rounds at most: the queue holds 64 rejects per event, and what does not fit is counted in `EngineStats::risk_reject_notices_dropped`.
- A strategy without the hook pays nothing: the queue is compiled out with it.
- Venue rejects are not risk rejects: an order the venue refused comes back through `on_order_update` with `OrderState::Rejected`.

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
static_assert(
    std::same_as<decltype(lvalue<Ctx>().own_qty(InstrumentId{}, Side::Buy, Price{})), Qty>);
static_assert(
    std::same_as<decltype(lvalue<Ctx>().own_qty(InstrumentId{}, Side::Buy, Price{}, Timestamp{})),
                 Qty>);
static_assert(std::same_as<decltype(lvalue<Ctx>().best_ex_self(InstrumentId{}, Side::Buy)), Level>);
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
static_assert(
    std::same_as<decltype(lvalue<Ctx>().queue_ahead(ClientOrderId{})), std::optional<Qty>>);
static_assert(
    std::same_as<decltype(lvalue<Ctx>().order_times(ClientOrderId{})), const OrderTimes*>);
// timers
static_assert(std::same_as<decltype(lvalue<Ctx>().every(Duration{}, std::uint64_t{})), TimerId>);
static_assert(std::same_as<decltype(lvalue<Ctx>().once(Duration{}, std::uint64_t{})), TimerId>);
static_assert(std::same_as<decltype(lvalue<Ctx>().cancel_timer(TimerId{})), bool>);
// control
static_assert(std::same_as<decltype(lvalue<Ctx>().quoting_enabled()), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().killed()), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().venue_killed(VenueId{})), bool>);
static_assert(std::same_as<decltype(lvalue<Ctx>().request_stop()), void>);
static_assert(std::same_as<decltype(lvalue<Ctx>().trip_kill(KillReason::StrategyError)), void>);
// randomness (seeded from the configuration)
static_assert(std::same_as<decltype(lvalue<Ctx>().rng()), Xoshiro256ss&>);
```

| Method | Notes |
|---|---|
| `now()` | the engine clock of the current event; virtual time in backtests and replay |
| `instrument(id)`, `instruments()`, `contains(id)` | the instrument table; iterate `instruments()` for all instruments |
| `book(id)` | the L2 book of an instrument ([Book](#book)) |
| `position(id)` | `qty` (signed, base units), average price, realised PnL (funding included) and fees of one instrument, in its settlement currency |
| `portfolio()` | `realized`, `unrealized`, `fees` and `net` (realised + unrealised - fees) over all instruments, in the `[accounting]` reporting currency when one is set; a loop, not for every event |
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
| `trip_kill(reason)` | trips the global kill switch with a `KillReason`: quoting stops, quotes are pulled and every working order is cancelled |
| `rng()` | a `Xoshiro256ss` seeded from `[engine] rng_seed`, identical in replay |
| `own_qty(id, side, px)`, `own_qty(id, side, px, at)`, `best_ex_self(id, side)`, `queue_ahead(order_id)`, `order_times(order_id)` | our own orders as the market sees them ([Execution view](#execution-view)) |

`set_quotes` applies hysteresis (`[engine] min_requote_ticks`, `min_requote_interval_ms`), skips orders awaiting a venue response and uses replace where the venue supports it. A direct order returns a `Result`:

```cpp
auto id = ctx.send(NewOrderRequest::limit(inst.id, Side::Buy, px, qty).post_only());
if (!id) return FASTMM_LOG_WARN("order refused: {}", id.error());  // e.g. RejectReason::MaxPosition
```

### Fees, risk headroom and venue health

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#venue_state -->
```cpp
static_assert(std::same_as<decltype(lvalue<Ctx>().fees(InstrumentId{})), const FeeRates&>);
static_assert(std::same_as<decltype(lvalue<Ctx>().risk_headroom(InstrumentId{})), RiskHeadroom>);
static_assert(std::same_as<decltype(lvalue<Ctx>().venue_health(VenueId{})), VenueHealthView>);
static_assert(std::same_as<decltype(lvalue<Ctx>().order_budget(VenueId{})), OrderBudget>);
```

| Method | Returns |
|---|---|
| `fees(id)` | `FeeRates`: `maker_cbps`, `taker_cbps` (1 cbps = 0.01 bps; positive is a fee, negative a rebate), `maker_bps()`, `taker_bps()`, `fee(notional, liquidity)`. The instrument's `maker_bps` / `taker_bps`, else its venue's `[venues.<x>.fees]`; with `fetch_fees` the account's own rates ([Binance Spot](venues.md#fee-rates)). A backtest charges these rates; a live venue reports the commission of each fill itself |
| `risk_headroom(id)` | `RiskHeadroom`: what each `[risk]` limit still admits on the instrument now, computed from the inputs the next check uses; an order of exactly a room passes that check and one lot more is refused. `order_tokens` (rate limiter), `open_orders`, `max_order_qty`, `max_order_notional`, `buy_qty` / `sell_qty` (`max_position`, open orders on that side counted, rounded down to the lot), `underlying_buy_qty` / `underlying_sell_qty` (`[risk.underlying]` max_net over the underlying's instruments, in this instrument's contracts; 0 while an inverse contract of it has no mark), `gross_notional`, `net_buy_notional` / `net_sell_notional` (exposure an order that does not reduce its position may add), `exposure_buy_notional` / `exposure_sell_notional` (those two caps as the notional of one buy / sell on this instrument in its settlement currency, converted at the rate the check uses: unlimited for the side that reduces the position, 0 while the currency's rate is unknown), `loss_budget` (`max_loss` plus net PnL; the kill switch trips at 0). A limit that is off reads `RiskHeadroom::kUnlimited` or the type's `max()`. Notionals are in the reporting currency with `[accounting]`. `fastmm-gateway`'s account guards are not included |
| `venue_health(venue)` | `VenueHealthView`: `feed_lag` (`recv_ts - exch_ts` of the venue's latest market-data message with a venue time), `feed_lag_base` (its minimum over the last 8 s), `feed_lag_excess`, `ack_rtt` and `ack_rtt_smoothed` (engine time from sending a new order to consuming its first ack; smoothed as `srtt += (rtt - srtt) / 8`), `md_updated`, `ack_updated`, sample counts, `gate_engagements` and `gated` ([Feed-lag gate](../explanation/risk-model.md#feed-lag-gate)). Zero before the first sample |
| `order_budget(venue)` | `OrderBudget` (`core/order_budget.hpp`): `local_tokens`, the orders the `[risk] orders_per_sec` bucket admits now (`kUnlimited` with the limit off), and `local_wait_ns`, the time until it admits the next one (0 while it admits one now: a strategy refused `RateLimit` waits this long instead of resending on every event); the venue's own counts as `RateWindow`s (`window_ms`, `used`, `limit`, `known()`, `remaining()`): `orders_10s`, `orders_1m`, `orders_1d` (Binance `ORDERS` per 10 s, minute and day, the count the venue last reported plus the orders sent since, those the connector has not taken yet included) and `weight` (request weight over the venue's shortest window: 6000 a minute on Binance Spot, 2400 on USDⓈ-M); `venue_paused` while the connector sends nothing (429 `Retry-After`, 418); `venue_known`; `orders_remaining()`, the least of them |

The first three are computed from journaled inputs, so replay returns the same values. `venue_health` costs one lookup; `risk_headroom` does the work of a risk check. While the feed-lag gate holds a venue, `set_quotes` on its instruments returns false. `order_budget`'s venue fields are the connector's latest publication from its own thread (its rate limiter, `venues/rate_limiter.hpp`, published with its status and after each response that carried the venue's counts), not journaled: a backtest, a replay and a strategy attached to `fastmm-gateway` see `venue_known == false` and every window unlimited; only `local_tokens` is deterministic.

### Balances

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#balances -->
```cpp
static_assert(std::same_as<decltype(lvalue<Ctx>().balance(VenueId{}, "USDT")), Balance>);
static_assert(std::same_as<decltype(lvalue<Ctx>().margin(VenueId{})), Margin>);
static_assert(
    std::same_as<decltype(lvalue<Ctx>().balance_room(InstrumentId{}, Side::Buy, Price{})), Qty>);
static_assert(std::same_as<decltype(Balance::free), Notional>);
static_assert(std::same_as<decltype(Balance::known), bool>);
static_assert(std::same_as<decltype(Margin::available), Notional>);
static_assert(std::same_as<decltype(lvalue<Ctx>().balances_live()), bool>);
```

| Method | Returns |
|---|---|
| `balance(venue, asset)` | `Balance`: `free`, `locked`, `total` (the venue's last report moved by this engine's orders and fills since; [Balance check](../explanation/risk-model.md#balance-check)), `equity` and `maintenance` as reported, `as_of` (the report's venue time), `known` (false until the venue reports the asset, and for an asset no instrument of the venue uses). Amounts are in the asset |
| `margin(venue)` | `Margin`: `available`, `initial`, `maintenance`, `equity`, `wallet`, `asset`: the venue's account-wide margin where it reports one (`account` true, usually valued in USD), else the settlement asset of its first derivative |
| `balance_room(id, side, px)` | the largest quantity of `id` on `side` at `px` the balance covers, rounded down to the lot: a spot buy's quote with the taker fee, a spot sell's base, a derivative's initial margin; `Qty::max()` while the venue has not reported that balance. What this instrument's open orders on that side hold is not counted as room |
| `balances_live()` | a venue has reported balances; before that nothing is estimated or checked |

`RiskHeadroom::balance_buy_qty` / `balance_sell_qty` are `balance_room` at the book's mid.

### Account pools

Several accounts of one exchange behind one venue (`[venues.<x>] pool_of`, [Account pools](configuration.md#account-pools)): the instruments, books, tickers and positions are the primary's; each member has its own `balance()`, `margin()` and `order_budget()` under its own venue id.

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#pool -->
```cpp
static_assert(std::same_as<decltype(lvalue<Ctx>().pool(VenueId{})), PoolMembers>);
static_assert(std::same_as<decltype(lvalue<Ctx>().pool_primary(VenueId{})), VenueId>);
static_assert(std::same_as<decltype(lvalue<Ctx>().account_usable(VenueId{})), bool>);
static_assert(std::same_as<
              decltype(lvalue<Ctx>().balance_room(InstrumentId{}, Side::Buy, Price{}, VenueId{})),
              Qty>);
static_assert(std::same_as<decltype(lvalue<const PoolMembers>().size()), std::size_t>);
static_assert(std::same_as<decltype(lvalue<const PoolMembers>()[0]), VenueId>);
static_assert(std::same_as<decltype(NewOrderRequest::account), VenueId>);
```

| Method | Returns |
|---|---|
| `pool(primary)` | `PoolMembers`: the accounts that take orders for the venue's instruments, the primary first (`size()`, `operator[]`, `contains(venue)`, iterable); just the venue itself without a pool |
| `pool_primary(venue)` | the venue whose instruments `venue` takes orders for: its primary, or itself |
| `account_usable(venue)` | the automatic routing would send to it now: its order link is live and its kill switch is not engaged |
| `balance_room(id, side, px, account)` | `balance_room` on one account of the pool. The three-argument form is what one usable account can hold: the largest over the members of its room (a sell: its free base; a buy: its free quote at the price) plus its own open orders on the side, less `open_qty(id, side)`, the total over every account, and never below zero. `fit_to_balance` (room plus `open_qty`) therefore cuts a ladder to what one account covers, and a quote resting on one account is not resized by another's balance; `balance(member, asset)` shows each account |

A new order goes to one account and stays there: `Order::venue`, the `OmsUpdate` and `Fill` events (their `msg->hdr.venue`) and the store's rows carry the member's id, and `cancel` / `replace` follow it. `NewOrderRequest::account` (`.account(venue)` on the builder) names the account; it must be the primary or one of its members, else `send` fails with `InvalidAccount`. Without one the engine picks: for a spot buy the usable members whose free quote covers the notional, for a spot sell those whose free base covers the quantity (every usable member for a derivative), and among them, skipping those whose 10 s or daily order window is full, the one with the most of the venue's 10 s window left (`order_budget(member).orders_10s`), then of the daily one. The orders sent to an account since its connector last published count against it, so the orders of one burst spread over the pool. Ties take turns: the search starts after the account the last automatic choice with a known budget went to. A backtest without window limits knows no budget and sends everything the balances allow to the primary. When no member covers the order it goes to the primary, which refuses it as the venue does. An order whose account (named, or the primary when no account with window room covers it) has a full window is refused by `send` with `RateLimit`, without reaching the venue. A member whose order link is down, or whose kill switch is engaged, gets no new orders; a kill of the primary stops the pool. Quotes (`set_quotes`) are routed the same way, one order at a time.

### Perpetuals

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#perps -->
```cpp
static_assert(std::same_as<decltype(lvalue<Ctx>().mark(InstrumentId{})), RefPrice>);
static_assert(std::same_as<decltype(lvalue<Ctx>().index(InstrumentId{})), RefPrice>);
static_assert(std::same_as<decltype(lvalue<Ctx>().funding(InstrumentId{})), FundingView>);
static_assert(std::same_as<decltype(lvalue<Ctx>().perp_state(InstrumentId{})), const PerpRow&>);
static_assert(std::same_as<decltype(RefPrice::price), Price>);
static_assert(std::same_as<decltype(RefPrice::stale), bool>);
static_assert(std::same_as<decltype(FundingView::rate), double>);
static_assert(std::same_as<decltype(FundingView::interval), Duration>);
static_assert(std::same_as<decltype(lvalue<const FundingView>().over(Duration{})), double>);
```

| Method | Returns |
|---|---|
| `mark(id)`, `index(id)` | `RefPrice`: the venue's mark or index price of a derivative (`PerpStateMsg`, [venues](venues.md#mark-index-and-funding)), `at` (engine time it arrived), `stale` (older than `[accounting] stale_mark_ms`, or never reported), `usable()` (reported and not stale) |
| `funding(id)` | `FundingView`: `rate` (per `interval`, a decimal; positive: longs pay shorts), `interval`, `next` (venue time of the next payment; zero when funding is continuous), `at`, `stale` (against `stale_funding_ms`), `usable()`, `over(d)` (the rate over a holding time `d`: `rate * d / interval`) |
| `perp_state(id)` | `const PerpRow&`: every field as last reported, open interest included, with the time each arrived and no staleness applied |

A venue that publishes mark, index and funding on separate channels (OKX) sends one message per channel; `PerpStateMsg::fields` names what a message carries and the table keeps the rest. With `[accounting] mark = "venue"` (the default) a position whose instrument has a fresh mark is valued at it ([Valuation](../explanation/risk-model.md#valuation-at-the-venue-mark)); `on_book` then no longer moves its unrealized PnL. `fit_to_balance(ctx, id, inst, q)` (`strategies/quoting.hpp`) cuts a `DesiredQuotes` ladder to what the balance covers, the side's resting orders counted as room, and drops a level under the lot, `min_qty` or `min_notional`; `basic_mm` and `lead_mm` use it.

## Execution view

The engine derives these from the order events and the market data it journals, so a replay reproduces them.

| Method | Returns |
|---|---|
| `own_qty(id, side, px)`, `own_qty(id, side, px, at)` | our resting quantity that the venue's feed shows at `px`, as of the book's last update or of venue time `at` (a `BookTicker`'s `hdr.exch_ts`). An order counts from its ack's venue time to its end's (cancel ack, last fill, expiry, a reconciliation that no longer lists it; with whole-millisecond venue times to the end of that millisecond), less its fills, so a depth update stamped before our cancel still includes it. 0 in the simulator |
| `best_ex_self(id, side)` | the book's best `Level` on one side with `own_qty` taken out; a level that was only ours is skipped; `Level{}` when none is left |
| `queue_ahead(order_id)` | `std::optional<Qty>`: the estimated quantity resting ahead of an open order at its price; `nullopt` before its ack and once it is terminal |
| `order_times(order_id)` | `const OrderTimes*` of an open order, `nullptr` once terminal: `sent` (engine clock), `venue_ack` (the ack's `exch_ts`, whole ms on Binance; 0 when the venue gives none), `local_ack` (the ack's `recv_ts`). After a replace they describe the replacement from its ack on. `OmsUpdate::times` carries them in `on_order_update`, the terminal update included, and through `Fill::update` in `on_fill` |

Whether a venue's feed shows our orders is a transport property (`own_in_feed(venue)`): yes for `LiveTransport` and for the replay of a live journal (one with a TSC calibration), no for `SimTransport`.

`queue_ahead` runs the `l2_queue` fill model's queue model ([`core/queue_model.hpp`](../../include/fastmm/core/queue_model.hpp)) on the market data the strategy sees, with `[engine] queue_conservatism` (`[backtest] queue_conservatism` in a backtest):

| Event | Quantity ahead |
|---|---|
| ack | the displayed quantity at the price less `own_qty`; at the touch of a newer book ticker, the touch's quantity less ours; 0 better than that touch; either less what the trades printed since took from the level |
| depth delta at the price | a shrink from `old` to `new` takes `(old - new) * ahead / old * (1 - conservatism)`, where `old` is less what the trades since the last update took; a level that goes away leaves 0; a snapshot caps it at the level |
| trade at the price | consumed first; a trade through the price leaves 0 (and fills the print's quantity: the aggressor would have taken that much from us before reaching the deeper level). One print is shared by our orders on its side in price priority, best for the aggressor first: each gets what the better ones left, after our maker fills at the print's venue time (live, the print carries them) |
| book ticker newer than the depth (venue update id when both carry one, else venue time) | 0 for an order priced better than its touch; at most the touch's quantity less ours for an order at the touch |
| replace ack | kept at the same price and no more than the leaves; otherwise as for an ack |

A level that shows less than we have resting at its price was published before our order reached it, and all of it counts as others' quantity. The engine tracks queues from the strategy's first `queue_ahead` call, so a strategy that never calls it pays one load per book update and trade; an order already resting at that call starts at the back of its level as the book then shows it. Call it in `on_start` to cover every order from its ack. With `fill_model = "l2_queue"` and no latency the value equals the fill model's ([`tests/sim/exec_view_test.cpp`](../../tests/sim/exec_view_test.cpp)); with latency the simulated venue sees the market before the strategy does. A replay handles events in arrival order, `fastmm-data fill-check` in venue-time order: on a 3 h Binance Spot session the replayed estimate at the ack equals fill-check's for 2077 of 2079 orders, on a session that joined existing levels for 1639 of 1726.

## Hedge executor

`HedgeExecutor` (`strategies/hedge_executor.hpp`, in `fastmm/strategy.hpp`) is a member a strategy owns: it hedges the positions of source instruments with IOC orders on hedge instruments, fails over between them and de-risks ([Hedge with HedgeExecutor](../how-to/strategies/hedge-executor.md)).

<!-- snippet: tests/docs/strategy_api_doc_test.cpp#hedge -->
```cpp
static_assert(std::same_as<decltype(lvalue<HedgeExecutor>().add_source(InstrumentId{})), bool>);
static_assert(
    std::same_as<decltype(lvalue<HedgeExecutor>().add_hedge(InstrumentId{}, Ratio{})), bool>);
static_assert(
    std::same_as<decltype(lvalue<HedgeExecutor>().start(lvalue<Ctx>(), HedgeExecutor::Config{})),
                 bool>);
static_assert(std::same_as<decltype(lvalue<HedgeExecutor>().on_fill(lvalue<Ctx>(), Fill{})), bool>);
static_assert(
    std::same_as<decltype(lvalue<HedgeExecutor>().on_order_update(lvalue<Ctx>(), OmsUpdate{})),
                 bool>);
static_assert(
    std::same_as<decltype(lvalue<const HedgeExecutor>().residual(lvalue<const Ctx>())), Qty>);
static_assert(
    std::same_as<decltype(lvalue<const HedgeExecutor>().can_hedge(lvalue<const Ctx>())), bool>);
static_assert(std::same_as<decltype(lvalue<const HedgeExecutor>().status(lvalue<const Ctx>())),
                           HedgeExecutor::Status>);
```

| Method | Does |
|---|---|
| `reset()`, `add_source(id)`, `add_hedge(id, tolerance)` | name the legs (at most 4 of each), hedges in order of preference; false when full |
| `start(ctx, config)` | checks the legs (configured, linear, each once) and starts; false, logged, otherwise |
| `set_config(config)`, `set_tolerance(i, tolerance)`, `set_target(qty)`, `restart()` | new parameters, a new net target in base units, a halt cleared |
| `on_fill`, `on_order_update`, `on_book`, `on_timer`, `on_connection`, `on_balance`, `update` | the strategy's hooks forwarded; the `bool` ones return whether the event concerned a leg or one of its orders |
| `residual(ctx)` | sources plus hedges minus the target, base units |
| `can_hedge(ctx)` | some hedge instrument can take a hedge now and hedging is not halted |
| `why(ctx, i, now)` | `Reason` hedge instrument `i` is not usable: `Down`, `Killed`, `NoBook`, `Stale`, `Gated`, `Benched`, or `Usable` |
| `held()`, `halted()`, `derisking()`, `state()`, `status(ctx)`, `stats()` | monitoring ([Monitor it](../how-to/strategies/hedge-executor.md#monitor-it)) |
| `hedge_order(inst, residual, bid, ask, tolerance, tag)` | static: the IOC that brings `residual` towards zero on `inst`, or none under its lot, `min_qty` or `min_notional` |

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
| `seq()`, `last_update()` | the venue sequence number and venue time (`exch_ts`, else receive time) of the last update |

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
| `fee`, `fee_converted` | fee in the settlement currency as booked; `fee_converted` is false when the commission was in a third asset (for example BNB) and not booked |
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
static_assert(std::same_as<decltype(BalanceMsg::free), Notional>);
static_assert(std::same_as<decltype(BalanceMsg::asset), FixedString<8>>);
static_assert(std::same_as<decltype(PerpStateMsg::mark_price), Price>);
static_assert(std::same_as<decltype(PerpStateMsg::funding_rate), double>);
static_assert(std::same_as<decltype(PerpStateMsg::next_funding), Timestamp>);
static_assert(std::same_as<decltype(PerpStateMsg::fields), std::uint8_t>);
```

- Every message starts with an `EventHeader`: `type`, `venue`, `instrument`, `exch_ts` (venue event time) and `recv_ts` (receive time), plus `seq` and flags.
- `TradeMsg::aggressor` is the taker's side.
- `OptionTickerMsg`: `mark_price` is in the instrument's price unit, `underlying_price` and `index_price` in the underlying's quote currency, implied volatilities are annualised decimals (0.312 is 31.2 %), greeks are the venue's; NaN marks a field the venue did not send.
- `ConnectionStateMsg::state` is `Disconnected`, `Connecting`, `Live`, `Stale`, `Resyncing` or `Dead`; `channel` is 0 for market data and 1 for the order and user stream; `reason_code` is venue-specific.
- `OmsUpdate` carries the order snapshot after the transition (`order`), the previous state (`prev`), and `known`, `changed` and `terminal` flags.
- `BalanceMsg`: one asset of a venue's account as the venue reports it, absolute amounts in `asset` at `exch_ts`: `free`, `locked`, `total`, `equity`, `maintenance` (for a derivative: available margin, initial margin in use, wallet balance, wallet plus unrealised PnL, maintenance margin). Flags: `kSnapshot` (part of a full snapshot), `kSnapshotEnd` (its last message; an asset it did not name holds nothing), `kAccount` (the account-wide margin).

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

- The arguments are the field name, default, minimum, maximum and a description; `FASTMM_PARAMS(Self)` comes first. At most 64 parameters.
- `decimal`, `bps` and `ms` values are parsed as decimal text, never through a double; exponents are accepted ([Fixed point](fixed-point.md#parsing-and-formatting)).
- Ranges are checked on the typed value: `parameter 'quote_qty': value 1000.5 outside [0, 1000]`.
- A strategy whose parameters name instruments can declare `std::optional<std::string> check_instruments(const InstrumentTable&) const`; it runs after `configure()`, before the engine is built, and an error stops the session the way a bad parameter does. `lead_mm` uses it for its `target`, `leader` and `fx` indices.
- The schema drives configuration checks, `--list-strategies`, `--param key=value` and `fastmm.strategies()` in Python.

## Parameter updates

A running engine takes new parameter values as an input event. `ParamPublisher` (`fastmm/strategies/param_publisher.hpp`) builds the event on another thread and pushes it into a ring of the engine's feed; in a backtest, `sim::ParamSchedule` (`fastmm/sim/param_schedule.hpp`) delivers events at simulated times through `bt::BacktestSession::set_param_schedule`.

```cpp
ParamPublisher pub(ParamSink::to_ring(ring), strategy.params());
bool sent = pub.publish({{"half_spread_bps", "7.5"}, {"quote_qty", "0.02"}});
```

- `publish(values, inst)` parses each value with the parameter's type and range into a copy of the parameters, then runs `validate()`. An unknown name, a name given twice, a value that does not parse or is out of range, a failed `validate()` and more than 32 values throw `std::invalid_argument`; nothing is sent then.
- It returns false when the ring is full or after `close()`; the engine never waits for the publisher.
- The copy starts from the parameters passed to the constructor and follows every update that was sent. A `StrategyBase` keeps one parameter set, so its updates name no instrument and apply to all.
- The engine assigns all values of an update at one event, journals it and then calls `on_params(ctx)`. Parameters the update does not name keep their values.
- `[strategy] max_param_age_ms` (0, the default, is off) disables quoting before the first update and whenever none was applied for that long in engine time: the engine pulls the quotes, `set_quotes` returns false and `on_quoting(false)` fires. The next update fires `on_quoting(true)`.
- Replay applies the journaled updates at the same events and matches their fields to the strategy's parameters by name ([Journal format](journal-format.md#parameter-updates)).

## State across sessions

A strategy that keeps something worth more than one session (an online model, learned sizes) gives the engine its bytes: `std::string_view state()` returns them (the engine copies them at once) and `bool restore(std::string_view)` takes bytes saved before, false when they are not its own. Both or neither; the signatures are checked at registration like the hooks. The format is the strategy's; a version prefix is worth having.

With `[strategy] state_file` set, the engine calls `restore` once, after `on_start` and before the first event, with the file's contents (no file is a first start; an unreadable one is a configuration error), and takes `state()` every `state_interval_s` (300 by default) of engine time and at the end, never from inside another hook. The live session and the backtest write the bytes to the file atomically (a temporary file, then a rename) off the engine thread. A backtest's end state is what the next backtest, or a live session, starts from; a replay of a journal neither reads nor writes the file, so it sees the session as the journal recorded it.

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
                              .tag(7)
                              .account(VenueId{})),
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
h.push(option.hdr);                       // on_option_ticker
h.disconnect();                           // on_connection (market data lost)
h.reconnect();                            // on_connection (live again)
h.pull_quotes();                          // on_quoting(false)
h.resume_quotes();                        // on_quoting(true)
h.publish({{"half_spread_bps", "7.5"}});  // on_params
BalanceMsg usdt{};
init_header(usdt, EventType::Balance, InstrumentId::invalid(), VenueId{0});
usdt.asset.assign("USDT");
usdt.free = Notional::from_int(1000);
h.push(usdt.hdr);  // on_balance
PerpStateMsg perp{};
init_header(perp, EventType::PerpState, h.instrument(), VenueId{0});
perp.mark_price = 100.01_px;
perp.fields = PerpStateMsg::kMark;
h.push(perp.hdr);     // on_perp_state
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
| `publish(values)` | a parameter update for all instruments at the current time: the parameters change, then `on_params` runs; `std::invalid_argument` when a publisher would reject it |
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
