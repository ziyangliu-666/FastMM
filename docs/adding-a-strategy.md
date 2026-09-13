# Adding a strategy

A strategy is one header: a parameter struct, a class with a static `name()`, and whichever engine
hooks it needs. Hooks are member templates, so the engine calls them with its concrete context and
book types and everything inlines. There are no virtual calls.

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
  template <class Ctx>
  void on_start(Ctx&) noexcept {
    half_cbps_ = static_cast<std::int64_t>(params_.half_spread_bps * 100.0 + 0.5);
    qty_ = Qty::from_double(params_.quote_qty);
  }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);
    const Instrument& inst = ctx.instrument(id);
    const Price mid = book.mid();
    const Price half =
        Price::from_raw(static_cast<std::int64_t>(Int128{mid.raw} * half_cbps_ / 1'000'000));
    DesiredQuotes q;
    static_cast<void>(q.bids.push_back(Level{inst.round_price(mid - half, Side::Buy), inst.round_qty(qty_)}));
    static_cast<void>(q.asks.push_back(Level{inst.round_price(mid + half, Side::Sell), inst.round_qty(qty_)}));
    ctx.set_quotes(id, q);
  }

 private:
  std::int64_t half_cbps_ = 0;  // centi-bps keeps two decimals of the config value
  Qty qty_{};
};

static_assert(StrategyLike<MyQuoter>);

}  // namespace fastmm
```

## Hooks

All hooks are optional; the engine detects them at compile time with `requires`.

| Hook | Called when |
|---|---|
| `on_start(ctx)` / `on_stop(ctx)` | engine starts / stops |
| `on_book(ctx, id, book)` | after a book snapshot or delta is applied |
| `on_book_ticker(ctx, id, msg)` | top-of-book update without full depth |
| `on_trade(ctx, id, msg)` | public trade |
| `on_fill(ctx, update, fill)` | one of our orders filled; position is already updated |
| `on_order_update(ctx, update)` | any OMS state transition |
| `on_timer(ctx, timer_id, user_data)` | a timer added with `ctx.add_timer(period, repeat, user_data)` fired |

Check `include/fastmm/strategies/basic_mm.hpp` for the exact signatures in use.

## What the context gives you

`ctx.book(id)`, `ctx.position(id)`, `ctx.instrument(id)`, `ctx.now()`, `ctx.set_quotes(id, q)`,
`ctx.pull_quotes(id)`, `ctx.send(order)`, `ctx.cancel(id)`, `ctx.replace(id, px, qty)`,
`ctx.add_timer(...)`, `ctx.rng()` (seeded, so backtests are reproducible).

Prefer `set_quotes` over sending orders yourself. The quote manager diffs the desired quotes against
live orders, applies hysteresis, never touches orders that are pending an exchange response, and
uses replace when the venue supports it.

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
   name, and `fastmm.strategies()` in Python lists its parameter schema.

2. **Test it** in `tests/strategies/my_quoter_test.cpp`. Unit-test the pure quoting logic directly
   (`BasicMM::compute_quotes` and `BasicMM::clamp_to_touch` are examples), then run the strategy
   through `fastmm::bt::run_backtest<MyQuoter>(config)` on synthetic data with a fixed seed and
   assert invariants: inventory never exceeds the limit, there are no post-only rejects, both
   sides are quoted most of the time, and two runs with the same seed give the same
   `outbound_sha256`.

3. **Try it** without writing any C++ driver:

   ```bash
   ./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic \
       --strategy my_quoter --param half_spread_bps=0.02
   ```

   `examples/cpp/custom_strategy.cpp` shows the same flow as a self-contained program.
