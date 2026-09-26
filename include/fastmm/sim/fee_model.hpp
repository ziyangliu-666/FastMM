#pragma once
// Fee schedules of the simulated venue: the engine's fee table (core/fees.hpp), so a backtest
// charges fills with the rates StrategyContext::fees reports.
#include "fastmm/core/fees.hpp"

namespace fastmm::sim {

using FeeSchedule = FeeRates;
using FeeModel = FeeTable;

}  // namespace fastmm::sim
