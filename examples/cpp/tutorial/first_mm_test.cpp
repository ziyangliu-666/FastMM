// Unit test of FirstMM. compute_quotes() is a plain function; the hooks run in the real engine
// through fastmm::sim::StrategyHarness, against a simulated venue on virtual time. Plain checks
// keep the example free of a test framework; doctest or GoogleTest work the same way.
#include "first_mm.hpp"

#include "fastmm/testing/strategy_harness.hpp"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok    " : "FAILED", what);
  if (!ok) ++g_failures;
}

}  // namespace

int main() {
  using namespace fastmm;
  using Harness = sim::StrategyHarness<tutorial::FirstMM>;

  // [start:pure]
  // compute_quotes on its own: microprice 100.01, 10 bps is 0.10001, rounded outwards to the tick.
  tutorial::FirstMMParams p;
  static_cast<void>(p.apply({{"edge_bps", "10"}, {"quote_qty", "0.001"}}));
  const Instrument inst = sim::harness_instruments().get(InstrumentId{0});  // tick 0.01, lot 0.001
  const DesiredQuotes q =
      tutorial::compute_quotes(p, inst, Level{100.00_px, 1_qty}, Level{100.02_px, 1_qty}, Qty{});
  check(q.bids.size() == 1 && q.bids[0].price == 99.90_px, "bid at 99.90");
  check(q.asks.size() == 1 && q.asks[0].price == 100.12_px, "ask at 100.12");
  // [end:pure]

  // [start:harness]
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
  // [end:harness]

  // [start:connection]
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
  // [end:connection]

  // [start:validate]
  tutorial::FirstMM s;
  check(s.configure({{"quote_qty", "0.01"}, {"max_position", "0.001"}}).has_value(),
        "quote_qty above max_position is rejected");
  check(s.configure({{"edge_bps", "0.00001"}}).has_value(),
        "more than 4 decimals of bps is rejected");
  // [end:validate]

  std::printf("first_mm_test: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
