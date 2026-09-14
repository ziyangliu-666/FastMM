#pragma once
// The tutorial's strategy module: the live, backtest and replay apps pass this function to the
// FastMM command lines.
namespace fastmm {
class StrategyRegistry;
}

namespace tutorial {

// Registers FirstMM (first_mm) for backtests, replay and live trading.
void register_strategies(fastmm::StrategyRegistry& r);

}  // namespace tutorial
