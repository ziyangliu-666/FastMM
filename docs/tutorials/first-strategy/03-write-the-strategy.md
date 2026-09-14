# 3. Write the strategy

The strategy is one header, `examples/cpp/tutorial/first_mm.hpp`.

## Parameters

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#params -->
```cpp
struct FirstMMParams {
  FASTMM_PARAMS(FirstMMParams)
  FASTMM_PARAM_BPS(
      edge_bps, 5_bps, 0_bps, 500_bps, "distance of each quote from the microprice, bps")
  FASTMM_PARAM(Qty, quote_qty, 0.001_qty, 0_qty, 1000_qty, "quantity per side, base units")
  FASTMM_PARAM(
      Qty, max_position, 0.004_qty, 0_qty, 1000_qty, "position limit, base units (0 = none)")
  FASTMM_PARAM_MS(report_ms,
                  milliseconds(10000),
                  milliseconds(0),
                  milliseconds(3600000),
                  "interval of the status log line, ms (0 = off)")

  // Checked after all parameters are applied: a quote larger than the limit could never be sent.
  [[nodiscard]] std::optional<std::string> validate() const {
    if (max_position.is_positive() && quote_qty > max_position)
      return "quote_qty must not exceed max_position";
    return std::nullopt;
  }
};
```

Each `FASTMM_PARAM*` line declares a field with its default, range and description. The macro sets how the value is written in a configuration:

- `FASTMM_PARAM_BPS` declares a `Ratio` configured in basis points (`edge_bps = 5.0`).
- `FASTMM_PARAM(Qty, ...)` declares a `Qty` (`quote_qty = 0.001`).
- `FASTMM_PARAM_MS` declares a `Duration` configured in whole milliseconds.

`validate()` rejects inconsistent combinations. The same declarations drive `--list-strategies`, `--param key=value` and configuration errors.

## The quoting function

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#compute_quotes -->
```cpp
// The quotes for one book: integer arithmetic on plain values, no engine state.
[[nodiscard]] inline DesiredQuotes compute_quotes(const FirstMMParams& p,
                                                  const Instrument& inst,
                                                  Level best_bid,
                                                  Level best_ask,
                                                  Qty position) noexcept {
  const Price micro = microprice(best_bid, best_ask);
  const Price edge = micro * p.edge_bps;  // Price * Ratio: exact, truncated toward zero
  const Qty qty = inst.round_qty(p.quote_qty);
  DesiredQuotes q;
  if (inventory_allows(Side::Buy, position, qty, p.max_position))
    q.bid(inst.round_price(micro - edge, Side::Buy), qty);  // rounds down to the tick
  if (inventory_allows(Side::Sell, position, qty, p.max_position))
    q.ask(inst.round_price(micro + edge, Side::Sell), qty);    // rounds up to the tick
  keep_passive(q, best_bid.price, best_ask.price, inst.tick);  // post-only quotes must not cross
  return q;
}
```

- `microprice` weights the best bid and ask by the size on the opposite side, so it leans towards the side that is about to be taken.
- Tick rounding never makes a quote more aggressive; `round_qty` rounds down to the lot.
- `inventory_allows` drops the side that would take the position past `max_position`.
- `keep_passive` moves a crossing quote back one tick inside the touch; the venue rejects a crossing post-only order.

## The hooks

The class derives from `StrategyBase<FirstMMParams>`, which stores the parameters (`params()`), and names itself with `name()`. `on_start` runs once before the first event and starts the status-line timer:

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_start -->
```cpp
void on_start(auto& ctx) noexcept {
  if (params().report_ms > Duration{}) report_timer_ = ctx.every(params().report_ms);
  FASTMM_LOG_INFO("first_mm: started, quoting {}",
                  ctx.quoting_enabled() ? "enabled" : "disabled (dry run)");
}
```

`on_book` runs after each book update of a configured instrument. `ctx` is the strategy's view of the engine: books, positions, quotes, orders, timers:

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_book -->
```cpp
void on_book(auto& ctx, InstrumentId id, const auto& book) noexcept {
  if (!book.is_valid()) return ctx.pull_quotes(id);  // empty or crossed
  const DesiredQuotes q = compute_quotes(
      params(), ctx.instrument(id), book.best_bid(), book.best_ask(), ctx.position(id).qty);
  ctx.set_quotes(id, q);  // the engine sends only the difference to the resting orders
}
```

`on_fill` runs for each of your executions, after the position and fees are updated:

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_fill -->
```cpp
void on_fill(auto& /*ctx*/, const Fill& fill) noexcept {
  ++fills_;
  if (fill.late) ++late_fills_;  // the order had already been cancelled
}
```

`on_connection` runs when a venue channel changes state. Off `Live`, the engine has already pulled that venue's quotes and, for market data, cleared its books:

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_connection -->
```cpp
void on_connection(auto& /*ctx*/, const ConnectionStateMsg& m) noexcept {
  if (m.state == ConnState::Live) {
    FASTMM_LOG_INFO("first_mm: venue {} channel {} is live", m.hdr.venue.value, m.channel);
    return;
  }
  ++disconnects_;  // the engine has already pulled this venue's quotes
  FASTMM_LOG_WARN("first_mm: venue {} channel {} is {}; quotes pulled",
                  m.hdr.venue.value,
                  m.channel,
                  to_string(m.state));
}
```

`on_quoting` runs when quoting is paused or resumed: an operator pull, the kill switch, or a reconciliation after a reconnect. On resume the strategy requotes without waiting for the next book update:

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_quoting -->
```cpp
void on_quoting(auto& ctx, bool enabled) noexcept {
  if (!enabled) return;  // paused: the engine has pulled the quotes
  for (const Instrument& inst : ctx.instruments()) on_book(ctx, inst.id, ctx.book(inst.id));
}
```

`on_timer` prints the status line. Logs are not journaled, so they must never change what the strategy does:

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#on_timer -->
```cpp
void on_timer(auto& ctx, TimerId id, std::uint64_t /*tag*/) noexcept {
  if (id != report_timer_) return;
  FASTMM_LOG_INFO("first_mm: fills={} late_fills={} disconnects={} net_pnl={}",
                  fills_,
                  late_fills_,
                  disconnects_,
                  ctx.portfolio().net);
}
```

## Check the hooks early

<!-- snippet: examples/cpp/tutorial/first_mm.hpp#verify -->
```cpp
static_assert(verify_strategy<FirstMM>());  // a hook with a wrong signature fails here
```

If you misspell a hook's parameters, for example `on_fill(auto& ctx, const OrderFillMsg& m)`, the build stops with `fastmm: on_fill has the wrong signature or is not public; expected void on_fill(auto& ctx, const Fill& fill)`. A likely misspelling of a name, such as `on_fills`, is a warning. The [Strategy API](../../reference/strategy-api.md) lists the hooks.

Next: [4. Unit-test it](04-unit-test.md)
