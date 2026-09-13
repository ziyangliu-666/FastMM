# Adding a strategy

One header, one registry line, one test.

```cpp
// include/fastmm/strategies/my_quoter.hpp
#pragma once
#include "fastmm/strategies/strategy.hpp"

namespace fastmm::strategies {

struct MyQuoterParams {
  FASTMM_PARAM(double, half_spread_bps, 5.0, 0.0, 1000.0, "half spread in bps of mid");
  FASTMM_PARAM(double, quote_qty, 0.001, 0.0, 1e9, "quantity per level");
};

class MyQuoter : public StrategyBase<MyQuoterParams> {
 public:
  static constexpr std::string_view name() { return "my_quoter"; }
  using StrategyBase::StrategyBase;

  void on_book(StrategyContext& ctx, InstrumentId id) {
    const auto& book = ctx.book(id);
    if (!book.is_valid()) return ctx.pull_quotes(id);
    const Price mid = book.mid();
    const Price half = mid.bps(params().half_spread_bps);
    DesiredQuotes q;
    q.bid.push_back({ctx.instrument(id).round_price(mid - half, Side::Buy), Qty::from_double(params().quote_qty)});
    q.ask.push_back({ctx.instrument(id).round_price(mid + half, Side::Sell), Qty::from_double(params().quote_qty)});
    ctx.set_quotes(id, q);
  }
};

}  // namespace fastmm::strategies
```

Then `FASTMM_REGISTER_STRATEGY(fastmm::strategies::MyQuoter)` in
`apps/fastmm-live/registrations/strategies.cpp` (and the backtest app), and a test in
`tests/strategies/my_quoter_test.cpp` that runs it through `BacktestRunner` on `MarketGenerator`
data with a fixed seed and asserts invariants (inventory never exceeds the limit, both sides quoted
most of the time). The Python package sees the strategy and its parameter schema automatically.

Hooks are optional: implement only the ones you need (`on_book`, `on_trade`, `on_book_ticker`,
`on_fill`, `on_order_update`, `on_timer`, `on_start`, `on_stop`); the engine detects them at
compile time.
