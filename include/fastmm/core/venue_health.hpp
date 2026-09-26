#pragma once
// VenueHealth: per venue, how late its market data arrives and how long our orders take to be
// acknowledged, from the events the engine consumes (engine thread only).
//
//   feed lag     recv_ts - exch_ts of every market-data message that carries a venue time (book
//                deltas, trades, tickers; snapshots excluded). The host and the venue clocks differ
//                by a constant offset, so the lag is compared with its baseline: the minimum over
//                the last kWindow (kBuckets buckets of kBucket, rolled on the engine clock).
//   ack RTT      engine time from sending a new order to consuming its first ack, last and smoothed
//                (srtt += (rtt - srtt) / 8, as TCP's SRTT).
//
// Every input is in the journal (the message headers and the engine clock), so a replay computes
// the same values. The feed-lag gate ([risk] max_feed_lag_ms) holds a venue for kGateHold after the
// last message whose excess lag was over the limit.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fastmm {

// What StrategyContext::venue_health returns. Durations are zero until the first sample.
struct VenueHealthView {
  Duration feed_lag{};         // recv_ts - exch_ts of the latest market-data message
  Duration feed_lag_base{};    // its minimum over the last kWindow
  Duration feed_lag_excess{};  // feed_lag - feed_lag_base
  Duration ack_rtt{};          // latest send -> first ack
  Duration ack_rtt_smoothed{};
  Timestamp md_updated{};   // engine time of the latest feed-lag sample
  Timestamp ack_updated{};  // ... and of the latest ack sample
  std::uint64_t md_samples = 0;
  std::uint64_t ack_samples = 0;
  std::uint64_t gate_engagements = 0;  // times the feed-lag gate engaged
  bool gated = false;                  // the feed-lag gate holds this venue now
};

class VenueHealth {
 public:
  static constexpr std::size_t kVenues = 8;  // kMaxVenues
  static constexpr std::size_t kBuckets = 8;
  static constexpr Duration kBucket = seconds(1);
  static constexpr Duration kWindow = Duration{kBucket.ns * static_cast<std::int64_t>(kBuckets)};
  static constexpr Duration kGateHold = milliseconds(100);

  // A market-data message of venue `v` with `lag` = recv_ts - exch_ts, consumed at `now`. With
  // `limit` > 0, returns true when the message engages the gate (it was not holding the venue).
  FASTMM_FORCE_INLINE bool on_md(VenueId v,
                                 std::int64_t lag,
                                 Timestamp now,
                                 Duration limit) noexcept {
    if (FASTMM_UNLIKELY(v.value >= kVenues)) return false;
    Slot& s = slots_[v.value];
    if (FASTMM_UNLIKELY(now >= s.bucket_end)) roll(s, now);
    if (lag < s.cur_min) s.cur_min = lag;
    s.lag = lag;
    s.md_ts = now;
    ++s.md_samples;
    if (FASTMM_LIKELY(limit.ns <= 0)) return false;
    return over_limit(s, lag, now, limit);
  }
  void on_ack(VenueId v, Duration rtt, Timestamp now) noexcept {
    if (v.value >= kVenues || rtt.ns < 0) return;
    Slot& s = slots_[v.value];
    s.rtt = rtt.ns;
    s.srtt = s.ack_samples == 0 ? rtt.ns : s.srtt + (rtt.ns - s.srtt) / 8;
    s.ack_ts = now;
    ++s.ack_samples;
  }
  [[nodiscard]] FASTMM_FORCE_INLINE bool gated(VenueId v, Timestamp now) const noexcept {
    return v.value < kVenues && now < slots_[v.value].gate_until;
  }
  [[nodiscard]] VenueHealthView view(VenueId v, Timestamp now) const noexcept {
    VenueHealthView out;
    if (v.value >= kVenues) return out;
    const Slot& s = slots_[v.value];
    if (s.md_samples != 0) {
      const std::int64_t base = s.cur_min < s.done_min ? s.cur_min : s.done_min;
      out.feed_lag = Duration{s.lag};
      out.feed_lag_base = Duration{base};
      out.feed_lag_excess = Duration{s.lag - base};
    }
    out.ack_rtt = Duration{s.rtt};
    out.ack_rtt_smoothed = Duration{s.srtt};
    out.md_updated = s.md_ts;
    out.ack_updated = s.ack_ts;
    out.md_samples = s.md_samples;
    out.ack_samples = s.ack_samples;
    out.gate_engagements = s.gate_engagements;
    out.gated = now < s.gate_until;
    return out;
  }

 private:
  static constexpr std::int64_t kNone = std::numeric_limits<std::int64_t>::max();
  struct Slot {
    Timestamp bucket_end{};         // the current bucket ends here (engine time)
    std::int64_t cur_min = kNone;   // of the current bucket
    std::int64_t done_min = kNone;  // of the completed buckets of the window
    std::int64_t lag = 0;
    Timestamp md_ts{};
    Timestamp gate_until{};
    std::uint64_t md_samples = 0;
    std::uint64_t gate_engagements = 0;
    std::array<std::int64_t, kBuckets - 1> done{};  // minima of the completed buckets
    std::uint32_t next = 0;                         // slot in `done` the next roll writes
    std::int64_t rtt = 0;
    std::int64_t srtt = 0;
    Timestamp ack_ts{};
    std::uint64_t ack_samples = 0;
  };

  static bool over_limit(Slot& s, std::int64_t lag, Timestamp now, Duration limit) noexcept {
    const std::int64_t base = s.cur_min < s.done_min ? s.cur_min : s.done_min;
    if (lag - base <= limit.ns) return false;
    const bool engaged = now >= s.gate_until;
    s.gate_until = now + kGateHold;
    if (engaged) ++s.gate_engagements;
    return engaged;
  }
  // Closes the buckets that ended before `now`; a gap of a whole window or more clears them all.
  FASTMM_NOINLINE static void roll(Slot& s, Timestamp now) noexcept {
    if (!s.bucket_end.valid() || now - s.bucket_end >= kWindow) {
      s.done.fill(kNone);
      s.next = 0;
      s.cur_min = kNone;
      s.bucket_end = now + kBucket;
    } else {
      while (now >= s.bucket_end) {
        s.done[s.next] = s.cur_min;
        s.next = (s.next + 1) % static_cast<std::uint32_t>(s.done.size());
        s.cur_min = kNone;
        s.bucket_end = s.bucket_end + kBucket;
      }
    }
    s.done_min = kNone;
    for (const std::int64_t m : s.done)
      if (m < s.done_min) s.done_min = m;
  }

  std::array<Slot, kVenues> slots_{};
};

}  // namespace fastmm
