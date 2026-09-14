# Adding a strategy

A strategy is one header: a parameter struct, a class with a static `name()`, and whichever engine
hooks it needs. Hooks are ordinary public member functions, called with the engine's concrete
context and book types, so everything inlines. There are no virtual calls, and a hook with a wrong
signature is a compile error.

```cpp
// include/fastmm/strategies/my_quoter.hpp
#pragma once
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cstdint>
#include <string_view>

namespace fastmm {

struct MyQuoterParams {
  FASTMM_PARAMS(MyQuoterParams)
  FASTMM_PARAM(double, half_spread_bps, 5.0, 0.0, 1000.0, "half spread around mid, basis points")
  FASTMM_PARAM(double, quote_qty, 0.001, 0.0, 1e9, "quantity per side, base units")
};

class MyQuoter : public StrategyBase<MyQuoterParams> {
 public:
  static constexpr std::string_view name() noexcept { return "my_quoter"; }

  // Convert config doubles to fixed point once. Nothing below touches floating point.
  void on_start(auto&) noexcept {
    half_cbps_ = static_cast<std::int64_t>(params().half_spread_bps * 100.0 + 0.5);
    qty_ = Qty::from_double(params().quote_qty);
  }

  void on_book(auto& ctx, InstrumentId id, const auto& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);
    const Instrument& inst = ctx.instrument(id);
    const Price mid = book.mid();
    const Price half =
        Price::from_raw(static_cast<std::int64_t>(Int128{mid.raw} * half_cbps_ / 1'000'000));
    DesiredQuotes q;
    static_cast<void>(q.bids.push_back(Level{inst.round_price(mid - half, Side::Buy), inst.round_qty(qty_)}));
    static_cast<void>(q.asks.push_back(Level{inst.round_price(mid + half, Side::Sell), inst.round_qty(qty_)}));
    ctx.set_quotes(id, q);  // false while quoting is disabled
  }

  // Quoting paused or resumed: requote at once when it is back, even if the mid has not moved.
  void on_quoting(auto& ctx, bool enabled) noexcept {
    if (!enabled) return;
    for (const Instrument& inst : ctx.instruments()) on_book(ctx, inst.id, ctx.book(inst.id));
  }

 private:
  std::int64_t half_cbps_ = 0;  // centi-bps keeps two decimals of the config value
  Qty qty_{};
};

static_assert(StrategyLike<MyQuoter>);
static_assert(verify_strategy<MyQuoter>());  // checks the hooks now, not when an engine is built

}  // namespace fastmm
```

## Hooks

All hooks are optional and return `void`. `auto& ctx` and `template <class Ctx> ... Ctx& ctx` are
the same thing. The reference is `include/fastmm/strategies/hooks.hpp`.

| Hook | Called when |
|---|---|
| `on_start(auto& ctx)` | once, before the first event; read `ctx.quoting_enabled()` here (a dry run starts disabled) |
| `on_stop(auto& ctx)` | once at shutdown; quotes are not pulled for you here, shutdown's cancel-all does that |
| `on_book(auto& ctx, InstrumentId id, const auto& book)` | after a snapshot or delta is applied, the position is marked and risk has seen the mid; the book may be invalid (crossed or empty) |
| `on_book_ticker(auto& ctx, InstrumentId id, const BookTickerMsg& m)` | top-of-book update without depth |
| `on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)` | public trade |
| `on_option_ticker(auto& ctx, InstrumentId id, const OptionTickerMsg& m)` | an option's mark, implied vols and greeks (see `docs/options.md`) |
| `on_fill(auto& ctx, const Fill& fill)` | one of our executions; the position and fees are already updated |
| `on_order_update(auto& ctx, const OmsUpdate& u)` | any OMS state change, including the end of a reconciliation |
| `on_timer(auto& ctx, TimerId id, std::uint64_t tag)` | a timer from `ctx.every` or `ctx.once` fired |
| `on_connection(auto& ctx, const ConnectionStateMsg& m)` | a venue channel changed state; for any state but `Live` the engine has already pulled that venue's quotes and, on the market-data channel (0), cleared its books |
| `on_quoting(auto& ctx, bool enabled)` | `ctx.quoting_enabled()` changed (see below) |

Instrument-scoped hooks (`on_book`, `on_book_ticker`, `on_trade`, `on_option_ticker`, `on_fill`)
fire only for instruments in the table, so `ctx.book(id)` and `ctx.instrument(id)` are always safe.

### Fill

`on_fill` fires for every execution on an instrument in the table, before the `on_order_update` of
the same execution, including late fills (the order was already cancelled) and fills for ids we
never issued. Fills for instruments outside the table are counted in
`EngineStats::unknown_instrument_fills` and not passed to the strategy.

| Field | Meaning |
|---|---|
| `instrument`, `side` | the order's, else the fill message's |
| `price`, `qty` | this execution |
| `position_delta` | signed change of the position; differs from `qty` when the commission is charged in the base asset |
| `fee`, `fee_converted` | fee in quote currency as booked; `fee_converted == false` when the commission is in a third asset (BNB) and was not booked |
| `liquidity` | maker or taker |
| `known` | the id matched an order; `update->order` holds its snapshot (for an order that was already terminal: id, instrument, side, state and filled quantity) |
| `late` | the order was already terminal when the fill arrived |
| `order_done` | the order reached a terminal state with this fill |
| `update`, `msg` | the OMS update (non-null when `known`) and the raw `OrderFillMsg` |

`Fill` is built on the engine's stack: `update` and `msg` are valid only during the call.

### on_quoting

`set_quotes` is ignored while quoting is disabled: an operator pull (`PullQuotes`), the kill switch
or a reconciliation. `on_quoting(ctx, enabled)` tells the strategy when that changes, so it can
forget its "last quoted" state and requote as soon as quoting is back.

- The engine compares `ctx.quoting_enabled()` before and after each event, fired timer and
  `on_start`, and calls the hook after the triggering hook has returned and all flags are final.
  At the end of a reconciliation that is after the engine has put back the quotes it paused, so a
  requote from `on_quoting` replaces them.
- It never fires from inside a context call: if `set_quotes` or `send` trips the kill switch, the
  strategy hears `on_quoting(false)` after its hook returns.
- It does not fire at start; `on_start` reads `ctx.quoting_enabled()`.
- A connection drop does not change `quoting_enabled()`; `on_connection` covers it.

### When a hook signature is wrong

The engine checks every hook name when it is instantiated, and
`static_assert(fastmm::verify_strategy<MyQuoter>())` checks them in the strategy header. A member
with a hook's name whose call does not compile stops the build with one error per hook:

```text
error: static assertion failed: fastmm: on_trade has the wrong signature or is not public; expected void on_trade(auto& ctx, InstrumentId id, const TradeMsg& m)
```

Likely misspellings (`on_fills`, `on_tick`, `on_order`, `onBook`, ...) give a warning,
`fastmm: on_fills is not a hook; did you mean on_fill?`, which `FASTMM_WERROR` builds turn into an
error. A strategy that uses such a name on purpose declares
`static constexpr bool fastmm_allow_near_miss_names = true;`. Known limits: a `final` class is only
call-checked (a wrong signature is then silently not called); a hook with a deduced `auto` return
type must not be checked with `verify_strategy`; implicit argument conversions are accepted.

## What the context gives you

| Group | Methods |
|---|---|
| Time, reference data | `now()`, `instrument(id)`, `instruments()`, `contains(id)` |
| Market data | `book(id)` |
| Portfolio | `position(id)`, `portfolio()` (`realized`, `unrealized`, `fees`, `net` over all instruments) |
| Quoting | `set_quotes(id, q) -> bool`, `pull_quotes(id)`, `pull_all_quotes()`, `working_quote(id, side, level) -> const Order*` |
| Direct orders | `send(req) -> Result<ClientOrderId, RejectReason>`, `cancel(id)`, `replace(id, px, qty)`, `order(id) -> const Order*`, `open_qty(id, side)`, `oms()` |
| Timers | `every(period, tag) -> TimerId`, `once(delay, tag) -> TimerId`, `cancel_timer(id)` |
| Control | `quoting_enabled()`, `killed()`, `request_stop()` |
| Randomness | `rng()` (seeded from the config, so backtests and replays are reproducible) |

Prefer `set_quotes` over sending orders yourself. The quote manager diffs the desired quotes against
live orders, applies hysteresis, never touches orders that are pending an exchange response, and
uses replace when the venue supports it.

Direct orders return a `Result`; check it, since risk, the OMS and the kill switch can refuse them:

```cpp
auto id = ctx.send(NewOrderRequest::limit(inst.id, Side::Buy, px, qty).post_only());
if (!id) {
  FASTMM_LOG_WARN("order refused: {}", id.error());   // e.g. RejectReason::MaxPosition
  return;
}
hedge_id_ = *id;
```

`NewOrderRequest::limit` builds a GTC limit order; `.post_only()`, `.reduce_only()`, `.ioc()` and
`.tag(n)` refine it. Tags in the quote manager's range are refused with `RejectReason::InvalidTag`.
Logs are not journaled: strategy logic must not depend on them.

## Registering and testing

1. **Register it for simulation, backtests, replay and Python.** Strategies are registered
   explicitly, never through static initialisers: an object file inside a static library is only
   linked when something references it, so a self-registering strategy would silently disappear
   from some binaries. Add a factory and one `add` call to `src/backtest/registrations.cpp`:

   ```cpp
   #include "fastmm/strategies/my_quoter.hpp"

   std::unique_ptr<IEngineRunner> make_my_quoter(TransportKind k, RunnerDeps& d) {
     return sim::make_sim_or_replay_runner<MyQuoter>(k, d);
   }

   // inside register_builtin_strategies(), which registers the Sim and Replay factories:
   for (const TransportKind k : {TransportKind::Sim, TransportKind::Replay}) {
     static_cast<void>(r.add(MyQuoter::name(), &MyQuoter::schema(), k, make_my_quoter));
   }
   ```

   `fastmm-backtest`, `fastmm-replay`, the tests and the Python module all call
   `fastmm::bt::register_builtin_strategies()`, so the strategy is then available everywhere by
   name, and `fastmm.strategies()` in Python lists its parameter schema. Without registration,
   `fastmm::bt::run_backtest<MyQuoter>(config)` runs it directly.

2. **Test the hooks with the harness** in `tests/strategies/my_quoter_test.cpp`.
   `fastmm::sim::StrategyHarness<S>` (`include/fastmm/testing/strategy_harness.hpp`) runs the
   strategy in the real engine against a simulated venue on virtual time, so no fake context is
   needed:

   ```cpp
   #include "fastmm/testing/strategy_harness.hpp"

   fastmm::sim::StrategyHarness<MyQuoter> h({{"half_spread_bps", "10"}, {"quote_qty", "0.01"}});
   h.book("100.00", "100.02");          // snapshot: on_book runs, quotes are sent
   h.advance(milliseconds(1));          // orders reach the venue, acks come back
   REQUIRE(h.working_orders().size() == 2);
   h.fill(Side::Buy);                   // a taker at the venue fills our best bid: on_fill runs
   CHECK(h.engine().position(h.instrument()).qty.is_positive());
   h.pull_quotes();                     // on_quoting(false); h.resume_quotes() -> on_quoting(true)
   h.disconnect();                      // on_connection; h.reconnect() brings the channel back
   ```

   `trade(px, qty, aggressor)` delivers a public trade, and `push(msg.hdr)` any other message.
   Pure quoting logic can still be unit-tested directly (`BasicMM::compute_quotes` is an example).

3. **Backtest it** through `fastmm::bt::run_backtest<MyQuoter>(config)` on synthetic data with a
   fixed seed and assert invariants: inventory never exceeds the limit, there are no post-only
   rejects, both sides are quoted most of the time, and two runs with the same seed give the same
   `outbound_sha256`.

4. **Try it** without writing any C++ driver:

   ```bash
   ./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic \
       --strategy my_quoter --param half_spread_bps=0.02
   ```

   `examples/cpp/custom_strategy.cpp` shows the same flow as a self-contained program.
