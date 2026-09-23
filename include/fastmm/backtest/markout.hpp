#pragma once
// Post-fill markouts: the metric that separates spread captured from adverse selection.
//
// For a fill of `qty` at `price` with signed quantity s (+qty bought, -qty sold) and the venue
// mid m(t) at the fill time t0 and at the horizon t0 + h:
//
//   spread capture  = s * (m(t0) - price)      what the passive quote earned against the mid
//   markout(h)      = s * (m(t0 + h) - price)  what the fill was still worth h later
//   adverse selection(h) = spread capture - markout(h)   how much of it the mid took back
//
// A passive quoter always has a positive spread capture: it quotes away from the mid by
// construction. Only the markout says whether the fills were worth taking. Amounts are in quote
// currency (raw 1e-8 fixed point) and, divided by the traded notional, in basis points.
//
// A fill whose horizon falls after the last event of the run has no mid to mark against and is
// EXCLUDED, not marked at the last known mid: `excluded_fills` counts them, and the buckets of
// that horizon only cover the fills that were measured. Capture is recomputed on the same
// subset so capture and markout at a horizon are directly comparable.
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::bt {

// Horizons measured by default, in simulated time.
inline constexpr std::int64_t kDefaultMarkoutHorizonsNs[] = {
    1'000'000'000LL, 10'000'000'000LL, 60'000'000'000LL};

struct MarkoutBucket {
  std::int64_t markout_raw = 0;   // sum of s * (mid(t0 + h) - price), quote currency
  std::int64_t capture_raw = 0;   // sum of s * (mid(t0) - price) over the same fills
  std::int64_t notional_raw = 0;  // sum of |qty| * price over the same fills
  std::uint64_t fills = 0;

  [[nodiscard]] double markout_quote() const noexcept;
  [[nodiscard]] double capture_quote() const noexcept;
  // capture - markout: the part of the captured spread the mid took back.
  [[nodiscard]] double adverse_selection_quote() const noexcept;
  // Notional-weighted basis points; 0 when no fill was measured.
  [[nodiscard]] double markout_bps() const noexcept;
  [[nodiscard]] double capture_bps() const noexcept;
  [[nodiscard]] double adverse_selection_bps() const noexcept;
  [[nodiscard]] double notional() const noexcept;

  void add(std::int64_t markout, std::int64_t capture, std::int64_t notional) noexcept {
    markout_raw += markout;
    capture_raw += capture;
    notional_raw += notional;
    ++fills;
  }
};

// Buckets of one horizon, all over the same measured fills.
struct MarkoutHorizon {
  std::int64_t horizon_ns = 0;
  MarkoutBucket total;
  MarkoutBucket buy;
  MarkoutBucket sell;
  MarkoutBucket maker;
  MarkoutBucket taker;
  // Indexed by instrument id, sized to the highest instrument that traded.
  std::vector<MarkoutBucket> instrument;
  // Fills the run could not mark, split by cause. Their sum is the number left out of the
  // buckets above; they are never marked at a substitute price.
  std::uint64_t excluded_fills = 0;     // the sum of the two below
  std::uint64_t excluded_past_end = 0;  // fill ts + horizon is after the last event of the run
  std::uint64_t excluded_no_mid = 0;    // the venue book had no two sides there (or at the fill)

  [[nodiscard]] std::string label() const;  // "1s", "250ms", "2m"
};

}  // namespace fastmm::bt
