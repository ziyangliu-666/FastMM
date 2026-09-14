// Unit test of MicropriceMM's hooks: fastmm::sim::StrategyHarness runs the strategy in the real
// engine against a simulated venue on virtual time, so no fake context is needed. Plain checks
// keep the template free of a test framework; use doctest or GoogleTest the same way.
#include "mm/microprice_mm.hpp"

#include "fastmm/testing/strategy_harness.hpp"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  using fastmm::Order;
  using fastmm::Side;
  using Harness = fastmm::sim::StrategyHarness<mm::MicropriceMM>;

  // Harness instrument: tick 0.01, lot 0.001. One lot per side, position limit of one lot.
  Harness h({{"edge_ticks", "1"}, {"quote_qty", "0.001"}, {"max_position", "0.001"}});
  h.book("100.00", "100.02");  // equal sizes: microprice 100.01, quotes one tick away
  h.advance(fastmm::milliseconds(1));
  std::vector<Order> orders = h.working_orders();
  check(orders.size() == 2, "one bid and one ask rest after the first book");
  for (const Order& o : orders) {
    if (o.side == Side::Buy)
      check(o.price == Harness::price("100.00"), "bid at microprice - 1 tick");
    if (o.side == Side::Sell)
      check(o.price == Harness::price("100.02"), "ask at microprice + 1 tick");
  }

  // Filled on the bid: long one lot, at the limit, so the next book quotes only the ask.
  check(h.fill(Side::Buy), "a taker fills our bid");
  check(h.engine().position(Harness::instrument()).qty == Harness::quantity("0.001"),
        "position is one lot after the fill");
  h.book("100.01", "100.03");
  h.advance(fastmm::milliseconds(1));
  orders = h.working_orders();
  check(orders.size() == 1 && orders[0].side == Side::Sell, "at the limit only the ask rests");

  // An operator pull disables quoting; resuming requotes at once through on_quoting.
  h.pull_quotes();
  h.advance(fastmm::milliseconds(1));
  check(h.working_orders().empty(), "no orders while quoting is paused");
  h.resume_quotes();
  h.advance(fastmm::milliseconds(1));
  check(!h.working_orders().empty(), "quotes are back after resume_quotes");

  // Parameters are validated when the strategy is configured.
  mm::MicropriceMM s;
  check(s.configure({{"quote_qty", "0.01"}, {"max_position", "0.001"}}).has_value(),
        "quote_qty above max_position is rejected");

  if (g_failures == 0) std::puts("microprice_mm: all checks passed");
  return g_failures == 0 ? 0 : 1;
}
