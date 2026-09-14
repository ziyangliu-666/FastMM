# 4. Unit-test it

`examples/cpp/tutorial/first_mm_test.cpp` tests `first_mm` with plain checks; it needs no test framework.

## The quoting function

<!-- snippet: examples/cpp/tutorial/first_mm_test.cpp#pure -->
```cpp
// compute_quotes on its own: microprice 100.01, 10 bps is 0.10001, rounded outwards to the tick.
tutorial::FirstMMParams p;
static_cast<void>(p.apply({{"edge_bps", "10"}, {"quote_qty", "0.001"}}));
const Instrument inst = sim::harness_instruments().get(InstrumentId{0});  // tick 0.01, lot 0.001
const DesiredQuotes q =
    tutorial::compute_quotes(p, inst, Level{100.00_px, 1_qty}, Level{100.02_px, 1_qty}, Qty{});
check(q.bids.size() == 1 && q.bids[0].price == 99.90_px, "bid at 99.90");
check(q.asks.size() == 1 && q.asks[0].price == 100.12_px, "ask at 100.12");
```

## The hooks, with the harness

`fastmm::sim::StrategyHarness<S>` runs your strategy in the engine against a simulated venue on virtual time. Each call runs the engine until it is idle, so the strategy's reaction is visible when the call returns:

<!-- snippet: examples/cpp/tutorial/first_mm_test.cpp#harness -->
```cpp
// The hooks in the engine: one lot per side, a position limit of one lot, no status timer.
Harness h(
    {{"edge_bps", "10"}, {"quote_qty", "0.001"}, {"max_position", "0.001"}, {"report_ms", "0"}});
h.book("100.00", "100.02");  // a snapshot: on_book runs and the quotes are sent
h.advance(milliseconds(1));  // the orders reach the venue and their acks come back
std::vector<Order> orders = h.working_orders();
check(orders.size() == 2, "a bid and an ask rest after the first book");

check(h.fill(Side::Buy), "a taker fills our bid");  // on_fill runs
check(h.strategy().fills() == 1, "on_fill counted the fill");
h.book("100.01", "100.03");
h.advance(milliseconds(1));
orders = h.working_orders();
check(orders.size() == 1 && orders[0].side == Side::Sell,
      "at the position limit only the ask rests");
```

Orders are working only after `advance`. `fill(side)` trades a taker against your best order on that side.

Connection loss and paused quoting:

<!-- snippet: examples/cpp/tutorial/first_mm_test.cpp#connection -->
```cpp
// The market-data channel drops: the engine clears the book and pulls the quotes.
h.disconnect();
h.advance(milliseconds(1));
check(h.working_orders().empty(), "no orders while market data is down");
check(h.strategy().disconnects() == 1, "on_connection saw the disconnect");
h.reconnect();
h.book("100.01", "100.03");
h.advance(milliseconds(1));
check(!h.working_orders().empty(), "quotes are back with the next book");

// An operator pull pauses quoting; resuming requotes at once through on_quoting.
h.pull_quotes();
h.advance(milliseconds(1));
check(h.working_orders().empty(), "no orders while quoting is paused");
h.resume_quotes();
h.advance(milliseconds(1));
check(!h.working_orders().empty(), "quotes are back after resume_quotes, without a new book");
```

Parameters are checked when the strategy is configured:

<!-- snippet: examples/cpp/tutorial/first_mm_test.cpp#validate -->
```cpp
tutorial::FirstMM s;
check(s.configure({{"quote_qty", "0.01"}, {"max_position", "0.001"}}).has_value(),
      "quote_qty above max_position is rejected");
check(s.configure({{"edge_bps", "0.00001"}}).has_value(),
      "more than 4 decimals of bps is rejected");
```

## Run it

<!-- snippet: scripts/docs/tutorial.sh#unit-test -->
```bash
"$BIN"/first_mm_test
```

```text
ok    : bid at 99.90
ok    : ask at 100.12
ok    : a bid and an ask rest after the first book
...
first_mm_test: all checks passed
```

`ctest --test-dir build/release -L tutorial` runs the tutorial's tests, this one included. The [Strategy API](../../reference/strategy-api.md#test-harness) lists the other harness calls: `trade`, `push` for any message, `engine()`.

Next: [5. Backtest in C++](05-backtest-in-cpp.md)
