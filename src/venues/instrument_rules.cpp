// RulesWatch: an instrument's trading rules read again while the session runs.
#include "fastmm/venues/instrument_rules.hpp"

#include "fastmm/venues/event_sink.hpp"

namespace fastmm::venues {

VenueRules rules_of(const Instrument& inst) noexcept {
  return VenueRules{inst.tick,
                    inst.lot,
                    inst.min_qty,
                    inst.max_qty,
                    inst.min_notional,
                    inst.max_notional,
                    inst.status};
}

std::uint8_t rules_changed(const VenueRules& before, const VenueRules& now) noexcept {
  std::uint8_t c = 0;
  if (before.tick != now.tick) c |= InstrumentRulesMsg::kTick;
  if (before.lot != now.lot) c |= InstrumentRulesMsg::kLot;
  if (before.min_qty != now.min_qty) c |= InstrumentRulesMsg::kMinQty;
  if (before.max_qty != now.max_qty) c |= InstrumentRulesMsg::kMaxQty;
  if (before.min_notional != now.min_notional) c |= InstrumentRulesMsg::kMinNotional;
  if (before.max_notional != now.max_notional) c |= InstrumentRulesMsg::kMaxNotional;
  if (before.status != now.status) c |= InstrumentRulesMsg::kStatus;
  return c;
}

void fill_rules_msg(InstrumentRulesMsg& m,
                    VenueId venue,
                    InstrumentId id,
                    const VenueRules& r,
                    std::uint8_t changed,
                    std::uint8_t cause) noexcept {
  m = InstrumentRulesMsg{};
  init_header(m, EventType::InstrumentRules, id, venue);
  m.tick = r.tick;
  m.lot = r.lot;
  m.min_qty = r.min_qty;
  m.max_qty = r.max_qty;
  m.min_notional = r.min_notional;
  m.max_notional = r.max_notional;
  m.status = r.status;
  m.changed = changed;
  m.cause = cause;
}

void RulesWatch::seed(InstrumentId id, const VenueRules& r) noexcept {
  if (id.value >= kMaxInstruments) return;
  last_[id.value] = r;
  seeded_[id.value] = true;
  halted_[id.value].store(r.status == TradingStatus::Trading ? 0 : 1, std::memory_order_relaxed);
  any_seeded_ = true;
}

bool RulesWatch::due(std::int64_t now_ns) const noexcept {
  if (!any_seeded_ || in_flight_) return false;
  if (wanted_ && now_ns - last_read_ns_ >= kRefusalGapNs) return true;
  return period_ns_ > 0 && now_ns - last_read_ns_ >= period_ns_;
}

void RulesWatch::started(std::int64_t now_ns) noexcept {
  cause_ = wanted_ ? InstrumentRulesMsg::kRefusal : InstrumentRulesMsg::kRefresh;
  wanted_ = false;
  in_flight_ = true;
  last_read_ns_ = now_ns;
  ++reads_;
}

std::uint8_t RulesWatch::update(EventSink* sink,
                                VenueId venue,
                                InstrumentId id,
                                VenueRules r) noexcept {
  if (!seeded(id)) return 0;
  VenueRules& last = last_[id.value];
  if (!r.tick.is_positive()) r.tick = last.tick;
  if (!r.lot.is_positive()) r.lot = last.lot;
  const std::uint8_t changed = rules_changed(last, r);
  if (changed == 0) return 0;
  last = r;
  halted_[id.value].store(r.status == TradingStatus::Trading ? 0 : 1, std::memory_order_relaxed);
  ++changes_;
  if (sink != nullptr) {
    InstrumentRulesMsg m;
    fill_rules_msg(m, venue, id, r, changed, cause_);
    static_cast<void>(sink->push(m.hdr));  // the order sink spins rather than drop
  }
  return changed;
}

}  // namespace fastmm::venues
