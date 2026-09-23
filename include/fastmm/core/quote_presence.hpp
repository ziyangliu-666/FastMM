#pragma once
// Time-weighted quoting presence, per instrument and for the session.
//
// Market-maker programmes pay their rebate for being on both sides of the book, and they measure it
// in time, not in orders sent: Binance, Bybit and Deribit all evaluate a time-weighted two-sided
// presence against a spread band. No venue publishes what it measured for you, so the number has to
// come from our own record and be reconciled against their statement.
//
// The accumulator advances on order-state transitions only (an order becomes live at its ack and
// stops being live at its terminal state), so a quoting session pays a few integer operations per
// order event and nothing per market-data event. `live` counts orders resting at the venue, which
// is what the programmes count: an order pending cancellation is still on the book.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>

namespace fastmm {

struct QuotePresenceStats {
  std::int64_t elapsed_ns = 0;    // since the first transition of this instrument
  std::int64_t two_sided_ns = 0;  // a live order on both sides
  std::int64_t one_sided_ns = 0;
  std::uint32_t live_bid = 0;
  std::uint32_t live_ask = 0;

  // Fraction of the elapsed time with a live order on both sides, 0 before anything rested.
  [[nodiscard]] double uptime() const noexcept {
    return elapsed_ns > 0 ? static_cast<double>(two_sided_ns) / static_cast<double>(elapsed_ns) : 0.0;
  }
};

class QuotePresence {
 public:
  // An order of `side` started (delta +1) or stopped (delta -1) resting at the venue.
  void on_live_change(InstrumentId inst, Side side, int delta, Timestamp now) noexcept {
    if (inst.value >= kMaxInstruments) return;
    Entry& e = entries_[inst.value];
    advance(e, now);
    std::uint32_t& live = side == Side::Buy ? e.s.live_bid : e.s.live_ask;
    if (delta > 0) {
      ++live;
    } else if (live > 0) {
      --live;
    }
  }

  // Brings the accumulators up to `now` without changing the state; call before reading.
  void settle(Timestamp now) noexcept {
    for (auto& e : entries_) {
      if (e.started) advance(e, now);
    }
  }

  [[nodiscard]] const QuotePresenceStats& stats(InstrumentId inst) const noexcept {
    return entries_[inst.value < kMaxInstruments ? inst.value : 0].s;
  }

  // Session totals: the sum over instruments that have quoted at least once.
  [[nodiscard]] QuotePresenceStats total() const noexcept {
    QuotePresenceStats t;
    for (const auto& e : entries_) {
      if (!e.started) continue;
      t.elapsed_ns += e.s.elapsed_ns;
      t.two_sided_ns += e.s.two_sided_ns;
      t.one_sided_ns += e.s.one_sided_ns;
      t.live_bid += e.s.live_bid;
      t.live_ask += e.s.live_ask;
    }
    return t;
  }

 private:
  struct Entry {
    QuotePresenceStats s;
    std::int64_t last_ns = 0;
    bool started = false;
  };

  static void advance(Entry& e, Timestamp now) noexcept {
    if (!e.started) {
      e.started = true;
      e.last_ns = now.ns;
      return;
    }
    const std::int64_t dt = now.ns - e.last_ns;
    if (dt <= 0) return;  // a clock that did not move adds nothing
    e.last_ns = now.ns;
    e.s.elapsed_ns += dt;
    if (e.s.live_bid > 0 && e.s.live_ask > 0) {
      e.s.two_sided_ns += dt;
    } else if (e.s.live_bid > 0 || e.s.live_ask > 0) {
      e.s.one_sided_ns += dt;
    }
  }

  Entry entries_[kMaxInstruments];
};

}  // namespace fastmm
