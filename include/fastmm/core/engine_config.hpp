#pragma once
// EngineConfig: the per-session settings an Engine is constructed with. Separate from engine.hpp so
// the strategy registry and registration files do not parse the Engine template.
#include "fastmm/core/balance_book.hpp"
#include "fastmm/core/fees.hpp"
#include "fastmm/core/fx.hpp"
#include "fastmm/core/perp_book.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/underlying.hpp"

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
  // [engine] queue_conservatism (a backtest: [backtest] queue_conservatism), in bps of 1: how much
  // of a level's shrink ahead of our order the queue estimate (StrategyContext::queue_ahead)
  // credits to cancels ahead of it. 10000: none.
  std::int64_t queue_conservatism_bps = 10'000;
  // Net PnL carried over from earlier sessions ([risk] max_loss is a budget for the deployment,
  // not per process); fastmm-live reads it from the durable kill state (core/session_state.hpp).
  Notional pnl_carry{};
  bool quoting_enabled = true;
  // Bit v: venue v's positions are not current until its first reconciliation ends. A live
  // session sets it for the venues that replay executions at connect: until then the position is
  // the store's plus whatever the replay has booked so far, and an order sized, hedged or
  // risk-checked on it could double a hedge or pass a limit. No order is sent and quoting stays
  // off until every such venue has reconciled (journal header await_reconcile, so a replay does
  // the same).
  std::uint32_t await_reconcile = 0;
  int cpu = -1;
  SpinMode spin_mode = SpinMode::Busy;
  RiskLimits risk;
  QuoteParams quotes;
  // [accounting]: the settlement currencies and their FX sources (inactive: nothing converted).
  FxPlan fx;
  // Maker/taker rates per instrument (fee_table, config/config.hpp), what StrategyContext::fees
  // reports. The simulated venue charges fills with the same table; a live venue reports its own.
  FeeTable fees;
  // [risk.underlying]: the underlyings with a net position limit and their instruments (inactive:
  // none).
  UnderlyingPlan underlying;
  // [risk] check_balance and the initial margin rates ([[instruments]] initial_margin) the balance
  // check and the estimate use (core/balance_book.hpp).
  BalanceConfig balance;
  // [accounting] mark, stale_mark_ms, stale_funding_ms: what a derivative's position is valued at
  // and when the venue's mark, index and funding are stale (core/perp_book.hpp).
  PerpConfig perp;
};

}  // namespace fastmm
