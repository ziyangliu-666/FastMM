#pragma once
// Strategies of the test strategy library (a STATIC archive the test binaries reach only through
// its registration functions, strategies.hpp).
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategy.hpp"

#include <string_view>

namespace test_mm {

using namespace fastmm::literals;

struct TestModuleParams {
  FASTMM_PARAMS(TestModuleParams)
  FASTMM_PARAM(int, edge_ticks, 1, 0, 100, "distance from the mid, ticks")
  FASTMM_PARAM(fastmm::Qty, quote_qty, 0.002_qty, 0_qty, 10_qty, "quantity per side")
};

// One level each side, edge_ticks from the mid.
class TestModuleMM : public fastmm::StrategyBase<TestModuleParams> {
 public:
  static constexpr std::string_view name() noexcept { return "test_module_mm"; }

  void on_book(auto& ctx, fastmm::InstrumentId id, const auto& book) noexcept {
    if (!book.is_valid()) return ctx.pull_quotes(id);
    const fastmm::Instrument& inst = ctx.instrument(id);
    const fastmm::Price mid = book.mid();
    const fastmm::Price edge = inst.ticks(params().edge_ticks);
    const fastmm::Qty qty = inst.round_qty(params().quote_qty);
    fastmm::DesiredQuotes q;
    q.bid(inst.round_price(mid - edge, fastmm::Side::Buy), qty);
    q.ask(inst.round_price(mid + edge, fastmm::Side::Sell), qty);
    static_cast<void>(ctx.set_quotes(id, q));
  }
};

// The built-in basic_mm's name and schema with different factories: registering it next to the
// built-in strategies is a conflict.
class ShadowBasicMM : public fastmm::BasicMM {};

}  // namespace test_mm
