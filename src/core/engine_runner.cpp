#include "fastmm/core/engine_runner.hpp"

#include "fastmm/core/fixed_point.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <iterator>

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

std::vector<std::pair<RejectReason, std::uint64_t>> nonzero_rejects(const RejectCounts& c) {
  std::vector<std::pair<RejectReason, std::uint64_t>> out;
  for (std::size_t i = 0; i < c.by_reason.size(); ++i) {
    if (c.by_reason[i] != 0) out.emplace_back(static_cast<RejectReason>(i), c.by_reason[i]);
  }
  std::stable_sort(
      out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  return out;
}

std::string format_reject_counts(const RejectCounts& c) {
  std::string out;
  for (const auto& [reason, count] : nonzero_rejects(c)) {
    fmt::format_to(
        std::back_inserter(out), "{}{} {}", out.empty() ? "" : ", ", to_string(reason), count);
  }
  return out;
}

}  // namespace fastmm
