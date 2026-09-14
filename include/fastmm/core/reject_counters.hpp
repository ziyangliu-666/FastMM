#pragma once
// Per-reason reject counters and the rate limiter for the engine's risk-reject warning.
//
// RejectCounts is a fixed array with one slot per RejectReason value: counting a reject is a single
// increment on the engine thread, no allocation. RejectLogLimiter decides which rejects are worth a
// log line: the first of each reason, then at most one per reason per interval, carrying the number
// suppressed in between. Both are engine-thread only; the limiter uses engine time, so a replay
// logs the same lines.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fastmm {

// One slot per RejectReason value; the largest is VenueUnknownOrder.
inline constexpr std::size_t kRejectReasonSlots =
    static_cast<std::size_t>(RejectReason::VenueUnknownOrder) + 1;

// A value outside the enum (a corrupt venue message) is counted as a generic venue reject.
[[nodiscard]] FASTMM_FORCE_INLINE constexpr std::size_t reject_slot(RejectReason r) noexcept {
  const auto v = static_cast<std::size_t>(r);
  return v < kRejectReasonSlots ? v : static_cast<std::size_t>(RejectReason::VenueReject);
}

struct RejectCounts {
  std::array<std::uint64_t, kRejectReasonSlots> by_reason{};

  FASTMM_FORCE_INLINE void add(RejectReason r) noexcept { ++by_reason[reject_slot(r)]; }
  [[nodiscard]] std::uint64_t operator[](RejectReason r) const noexcept {
    return by_reason[reject_slot(r)];
  }
  [[nodiscard]] std::uint64_t total() const noexcept {
    std::uint64_t n = 0;
    for (const std::uint64_t c : by_reason) n += c;
    return n;
  }
};

// The reasons with a non-zero count, most frequent first (equal counts in enum order).
[[nodiscard]] std::vector<std::pair<RejectReason, std::uint64_t>> nonzero_rejects(
    const RejectCounts& c);
// "MaxPosition 12, RateLimit 5"; empty when nothing was rejected.
[[nodiscard]] std::string format_reject_counts(const RejectCounts& c);

class RejectLogLimiter {
 public:
  explicit RejectLogLimiter(Duration interval = seconds(10)) noexcept : interval_(interval) {}
  void set_interval(Duration interval) noexcept { interval_ = interval; }
  [[nodiscard]] Duration interval() const noexcept { return interval_; }

  // True if a reject of reason `r` at `now` should be logged; `suppressed` then receives the number
  // of rejects of that reason not logged since its previous line. A zero interval logs every
  // reject; a clock that went backwards ends the quiet period.
  [[nodiscard]] bool admit(RejectReason r, Timestamp now, std::uint64_t& suppressed) noexcept {
    Slot& s = slots_[reject_slot(r)];
    if (s.logged) {
      const Duration since = now - s.last;
      if (since.ns >= 0 && since < interval_) {
        ++s.suppressed;
        return false;
      }
    }
    suppressed = s.suppressed;
    s.suppressed = 0;
    s.last = now;
    s.logged = true;
    return true;
  }

 private:
  struct Slot {
    Timestamp last{};
    std::uint64_t suppressed = 0;
    bool logged = false;
  };
  std::array<Slot, kRejectReasonSlots> slots_{};
  Duration interval_;
};

}  // namespace fastmm
