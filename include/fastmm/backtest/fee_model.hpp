#pragma once
// Fees are charged by the simulated venue, so the model lives in fastmm::sim; the backtest
// exposes it under its own namespace for configuration and reporting.
#include "fastmm/sim/fee_model.hpp"

namespace fastmm::bt {
using sim::FeeModel;
}  // namespace fastmm::bt
