#pragma once
// Every factory definition (Sim, Replay, Live). Include it in the file that calls
// register_strategy<S>(r): that file then instantiates the three engines of each strategy it
// registers. See strategies/module.hpp.
#include "fastmm/strategies/factory_live.hpp"
#include "fastmm/strategies/factory_replay.hpp"
#include "fastmm/strategies/factory_sim.hpp"
#include "fastmm/strategies/module.hpp"
