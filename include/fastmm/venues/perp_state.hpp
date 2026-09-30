#pragma once
// What a connector needs to report a derivative's mark, index and funding (PerpStateMsg,
// core/messages.hpp): a builder for the message, filled field by field as a venue's payload names
// them. Market data: the message goes to the market-data sink like a book update (a decoder writes
// it into its scratch buffer, MdKind::PerpState). One message per venue payload; a venue that
// publishes mark, index and funding on separate channels sends one per channel, and a venue whose
// updates carry only the fields that changed (Bybit's tickers deltas) keeps the last value of each
// and sends them all, so an unchanged field does not age in the engine's table.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cmath>
#include <cstdint>

namespace fastmm::venues {

class PerpStateBuilder {
 public:
  // Starts a message for `inst` at the venue's time `venue_ms` (Unix ms; 0: unknown).
  PerpStateBuilder(PerpStateMsg& m,
                   InstrumentId inst,
                   VenueId venue,
                   std::int64_t venue_ms,
                   Timestamp recv_ts,
                   Cycles t0) noexcept
      : m_(m) {
    init_header(m_, EventType::PerpState, inst, venue);
    m_.mark_price = Price{};
    m_.index_price = Price{};
    m_.funding_rate = 0.0;
    m_.funding_interval = Duration{};
    m_.next_funding = Timestamp{};
    m_.open_interest = Qty{};
    m_.fields = 0;
    if (venue_ms > 0) m_.hdr.exch_ts = Timestamp{venue_ms * 1'000'000};
    m_.hdr.recv_ts = recv_ts;
    m_.hdr.t0_cycles = t0;
  }
  PerpStateBuilder& mark(Price p) noexcept {
    if (p.is_positive()) {
      m_.mark_price = p;
      m_.fields |= PerpStateMsg::kMark;
    }
    return *this;
  }
  PerpStateBuilder& index(Price p) noexcept {
    if (p.is_positive()) {
      m_.index_price = p;
      m_.fields |= PerpStateMsg::kIndex;
    }
    return *this;
  }
  // `rate` per `interval`, applied at `next_ms` (Unix ms; 0: continuous or unknown). A rate that is
  // not finite is not set.
  PerpStateBuilder& funding(double rate, Duration interval, std::int64_t next_ms) noexcept {
    if (!std::isfinite(rate)) return *this;
    m_.funding_rate = rate;
    m_.funding_interval = interval;
    m_.next_funding = next_ms > 0 ? Timestamp{next_ms * 1'000'000} : Timestamp{};
    m_.fields |= PerpStateMsg::kFunding;
    return *this;
  }
  PerpStateBuilder& open_interest(Qty contracts) noexcept {
    m_.open_interest = contracts;
    m_.fields |= PerpStateMsg::kOpenInterest;
    return *this;
  }
  // The message carries at least one field.
  [[nodiscard]] bool any() const noexcept { return m_.fields != 0; }
  [[nodiscard]] PerpStateMsg& msg() noexcept { return m_; }

 private:
  PerpStateMsg& m_;
};

}  // namespace fastmm::venues
