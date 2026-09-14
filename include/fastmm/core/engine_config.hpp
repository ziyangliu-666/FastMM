#pragma once
// EngineConfig: the per-session settings an Engine is constructed with. Separate from engine.hpp so
// the strategy registry and registration files do not parse the Engine template.
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>

namespace fastmm {

struct EngineConfig {
  std::uint64_t session_id = 0;
  std::uint64_t rng_seed = 1;
  std::uint16_t session_epoch = 1;
  std::uint32_t max_events_per_step = 64;
  Duration crossed_grace = milliseconds(100);
  Duration latency_publish_interval = seconds(1);
  // Risk rejects are logged at WARN: the first of each reason, then at most one line per reason per
  // interval with the number suppressed in between (0 logs every reject).
  Duration reject_log_interval = seconds(10);
  bool quoting_enabled = true;
  int cpu = -1;
  SpinMode spin_mode = SpinMode::Busy;
  RiskLimits risk;
  QuoteParams quotes;
};

}  // namespace fastmm
