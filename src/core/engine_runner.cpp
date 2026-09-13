#include "fastmm/core/engine_runner.hpp"

#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

namespace fastmm {

namespace {
std::string dec(std::int64_t raw) {
  char buf[kMaxDecimalChars];
  return std::string(buf, Notional::from_raw(raw).to_decimal(buf));
}
}  // namespace

std::string format_runner_stats(const RunnerStats& s) {
  return fmt::format(
      "events={} book_updates={} orders={} cancels={} replaces={} fills={} risk_rejects={} "
      "journal_overflows={} transport_full={} timers={} realized={} unrealized={} fees={} "
      "tick_to_trade_p50={}ns p99={}ns\n",
      s.events,
      s.book_updates,
      s.orders_sent,
      s.cancels_sent,
      s.replaces_sent,
      s.fills,
      s.risk_rejects,
      s.journal_overflows,
      s.transport_full,
      s.timers_fired,
      dec(s.realized_pnl_raw),
      dec(s.unrealized_pnl_raw),
      dec(s.fees_raw),
      s.tick_to_trade_p50_ns,
      s.tick_to_trade_p99_ns);
}

}  // namespace fastmm
