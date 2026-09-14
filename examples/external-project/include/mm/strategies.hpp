#pragma once
// The project's strategy module: every app (live, backtest, replay) passes this function to the
// FastMM command line, and nothing registers itself.
namespace fastmm {
class StrategyRegistry;
}

namespace mm {

// Registers MicropriceMM (microprice_mm) for backtests, replay and live trading.
void register_strategies(fastmm::StrategyRegistry& r);

}  // namespace mm
