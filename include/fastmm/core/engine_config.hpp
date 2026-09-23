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
  // [strategy] max_param_age_ms: quoting is disabled before the first ParamUpdate and while none
  // was applied for this long (zero: off).
  Duration max_param_age{};
  // [engine] ack_timeout_ms: an order still waiting for its ack this long is force-cancelled, so a
  // lost request cannot hold a pool slot, a max_open_orders slot and max_position exposure for
  // good (zero: off).
  Duration ack_timeout{};
  // Operator flatten (ControlCommand::Flatten, the control socket's `flatten`): how often the
  // engine looks at the remaining position and sends the next reduce-only slice, how long it
  // keeps trying (zero: until it is flat or the operator stops it) and the slippage allowance a
  // `flatten` without --max-slippage-bps uses.
  Duration flatten_interval = milliseconds(500);
  Duration flatten_timeout = seconds(60);
  std::int64_t flatten_slippage_bps = 25;
  // Net PnL carried over from earlier sessions ([risk] max_loss is a budget for the deployment,
  // not per process); fastmm-live reads it from the durable kill state (core/session_state.hpp).
  Notional pnl_carry{};
  bool quoting_enabled = true;
  int cpu = -1;
  SpinMode spin_mode = SpinMode::Busy;
  RiskLimits risk;
  QuoteParams quotes;
};

}  // namespace fastmm
