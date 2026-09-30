#pragma once
// PerpBook: the venue's mark, index and funding of each derivative (PerpStateMsg), and which price
// its position is valued at. The engine keeps one for its instruments; fastmm-gateway keeps one per
// venue in the account's book (core/account_book.hpp).
//
// Each field of a report is kept with the local time it arrived (the engine clock, which the
// journal records, so a replay ages the table as the session did). A field older than its limit is
// stale: the views say so, and a stale mark stops valuing the position. Mark and index share
// [accounting] stale_mark_ms, the funding fields stale_funding_ms.
//
// Valuation ([accounting] mark = "venue", the default): a position whose instrument has a fresh
// venue mark is valued at it, for the unrealized PnL, [risk] max_loss and the exposure limits; the
// book's mid no longer moves it. When the mark goes stale the next book update values it at the
// mid again (logged, counted) until a fresh mark arrives. An instrument that never reports a mark
// (spot) is valued at the mid as before. mark = "mid" keeps the mid for every instrument.
//
// Nothing here allocates after construction.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace fastmm {

// [accounting] mark, stale_mark_ms and stale_funding_ms.
struct PerpConfig {
  bool venue_mark = true;  // value a position at its venue's mark while it is fresh
  Duration stale_mark = seconds(15);
  Duration stale_funding = seconds(180);
};

// The last value of each field of one instrument, and when it arrived.
struct PerpRow {
  Price mark;
  Price index;
  double funding_rate = 0.0;  // per funding_interval, decimal
  Duration funding_interval{};
  Timestamp next_funding{};  // venue time; zero: continuous or not reported
  Qty open_interest;
  Timestamp venue_ts{};  // hdr.exch_ts of the last report
  Timestamp mark_at{};   // local time each field last arrived; zero: never
  Timestamp index_at{};
  Timestamp funding_at{};
  Timestamp open_interest_at{};
  std::uint64_t reports = 0;
  bool marking = false;  // the position is valued at `mark`
};

// A venue price of one instrument now (StrategyContext::mark, ::index).
struct RefPrice {
  Price price;        // zero: never reported
  Timestamp at{};     // local time it arrived
  bool stale = true;  // older than the limit, or never reported
  [[nodiscard]] bool usable() const noexcept { return price.is_positive() && !stale; }
};

// The venue's funding of one perpetual now (StrategyContext::funding).
struct FundingView {
  double rate = 0.0;  // per interval, decimal; positive: longs pay shorts
  Duration interval{};
  Timestamp next{};  // venue time of the next payment; zero: continuous or not reported
  Timestamp at{};    // local time it arrived; zero: never reported
  bool stale = true;
  [[nodiscard]] bool usable() const noexcept { return at.valid() && !stale && interval.ns > 0; }
  // The rate over `d` of holding the position: rate * d / interval (0 without an interval).
  [[nodiscard]] double over(Duration d) const noexcept {
    return interval.ns > 0
               ? rate * static_cast<double>(d.ns) / static_cast<double>(interval.ns)
               : 0.0;
  }
};

struct PerpStats {
  std::uint64_t reports = 0;
  std::uint64_t unknown_instrument = 0;  // reports for an instrument outside the table
  std::uint64_t mark_fallbacks = 0;      // a venue mark went stale: valued at the mid again
};

class PerpBook {
 public:
  PerpBook() : rows_(std::make_unique<std::array<PerpRow, kMaxInstruments>>()) {}

  void configure(const PerpConfig& c) noexcept { cfg_ = c; }
  [[nodiscard]] const PerpConfig& config() const noexcept { return cfg_; }

  // Keeps the fields of `m`, arrived at `now`. True when the instrument's position is to be
  // valued at m.mark_price from now on.
  bool on_report(const PerpStateMsg& m, Timestamp now) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (FASTMM_UNLIKELY(id.value >= kMaxInstruments)) {
      ++stats_.unknown_instrument;
      return false;
    }
    PerpRow& r = (*rows_)[id.value];
    if (r.reports++ == 0) ++instruments_;
    ++stats_.reports;
    if (m.hdr.exch_ts.valid()) r.venue_ts = m.hdr.exch_ts;
    const bool has_mark = (m.fields & PerpStateMsg::kMark) != 0 && m.mark_price.is_positive();
    if (has_mark) {
      r.mark = m.mark_price;
      r.mark_at = now;
    }
    if ((m.fields & PerpStateMsg::kIndex) != 0 && m.index_price.is_positive()) {
      r.index = m.index_price;
      r.index_at = now;
    }
    if ((m.fields & PerpStateMsg::kFunding) != 0) {
      r.funding_rate = m.funding_rate;
      r.funding_interval = m.funding_interval;
      r.next_funding = m.next_funding;
      r.funding_at = now;
    }
    if ((m.fields & PerpStateMsg::kOpenInterest) != 0) {
      r.open_interest = m.open_interest;
      r.open_interest_at = now;
    }
    if (!cfg_.venue_mark || !has_mark) return false;
    r.marking = true;
    marking_ = true;
    return true;
  }

  // Some instrument is valued at its venue's mark (a cheap gate for the book path).
  [[nodiscard]] bool marking() const noexcept { return marking_; }
  // True when the position of `id` is valued at the book's mid now: no venue mark values it, or
  // the one that did is stale (it stops, logged).
  [[nodiscard]] FASTMM_FORCE_INLINE bool mid_marks(InstrumentId id, Timestamp now) noexcept {
    PerpRow& r = (*rows_)[id.value];
    if (FASTMM_LIKELY(!r.marking)) return true;
    if (now - r.mark_at <= cfg_.stale_mark) return false;
    fall_back(id, r, now);
    return true;
  }
  // The venue mark the position of `id` is valued at now; zero when it is valued at the mid.
  [[nodiscard]] Price valuation(InstrumentId id) const noexcept {
    if (id.value >= kMaxInstruments) return {};
    const PerpRow& r = (*rows_)[id.value];
    return r.marking ? r.mark : Price{};
  }

  [[nodiscard]] RefPrice mark(InstrumentId id, Timestamp now) const noexcept {
    if (id.value >= kMaxInstruments) return {};
    const PerpRow& r = (*rows_)[id.value];
    return ref(r.mark, r.mark_at, now, cfg_.stale_mark);
  }
  [[nodiscard]] RefPrice index(InstrumentId id, Timestamp now) const noexcept {
    if (id.value >= kMaxInstruments) return {};
    const PerpRow& r = (*rows_)[id.value];
    return ref(r.index, r.index_at, now, cfg_.stale_mark);
  }
  [[nodiscard]] FundingView funding(InstrumentId id, Timestamp now) const noexcept {
    FundingView f;
    if (id.value >= kMaxInstruments) return f;
    const PerpRow& r = (*rows_)[id.value];
    if (!r.funding_at.valid()) return f;
    f.rate = r.funding_rate;
    f.interval = r.funding_interval;
    f.next = r.next_funding;
    f.at = r.funding_at;
    f.stale = now - r.funding_at > cfg_.stale_funding;
    return f;
  }
  // The row as last reported (no staleness applied); a default row for an id out of range.
  [[nodiscard]] const PerpRow& row(InstrumentId id) const noexcept {
    static const PerpRow kNone{};
    return id.value < kMaxInstruments ? (*rows_)[id.value] : kNone;
  }
  // Instruments that have reported.
  [[nodiscard]] std::size_t instruments() const noexcept { return instruments_; }
  [[nodiscard]] const PerpStats& stats() const noexcept { return stats_; }

 private:
  [[nodiscard]] static RefPrice ref(Price p, Timestamp at, Timestamp now, Duration limit) noexcept {
    RefPrice v;
    if (!at.valid()) return v;
    v.price = p;
    v.at = at;
    v.stale = now - at > limit;
    return v;
  }
  FASTMM_NOINLINE void fall_back(InstrumentId id, PerpRow& r, Timestamp now) noexcept {
    r.marking = false;
    ++stats_.mark_fallbacks;
    FASTMM_LOG_WARN(
        "instrument {}: the venue's mark is {} ms old (stale_mark_ms {}): its position is valued "
        "at the book's mid until a fresh mark arrives",
        id.value,
        (now - r.mark_at).millis(),
        cfg_.stale_mark.millis());
  }

  PerpConfig cfg_{};
  std::unique_ptr<std::array<PerpRow, kMaxInstruments>> rows_;
  std::size_t instruments_ = 0;
  PerpStats stats_{};
  bool marking_ = false;
};

}  // namespace fastmm
