#pragma once
// An instrument's trading rules while the session runs. Venue::load_reference_data() reads them
// once, before the engine starts; a venue can change them later (a token split moves the lot step,
// a halt stops trading), and an engine that kept the start-up values would have every order
// refused until a restart. A connector that can read them again keeps a RulesWatch:
//
//   * load_reference_data() seeds it with what the venue said at start-up;
//   * on_timer() reads them again every [venues.<name>] rules_refresh_s (due()), and soon after the
//     venue refused an order or a cancel for an instrument's rules or status (want());
//   * each read goes through update(), which emits an InstrumentRulesMsg into the order sink for an
//     instrument whose rules or status differ from the last it reported. The engine applies it on
//     its own thread and the journal keeps it, so a replay changes the rules at the same point;
//   * trading() tells any thread whether the venue trades the instrument as of the last read: the
//     connector's blocking cancel-all skips one that does not, since the venue refuses it there.
//
// Reactor thread for everything but trading() and seeded().
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace fastmm::venues {

class EventSink;

// The rules of one instrument as a venue states them.
struct VenueRules {
  Price tick{};
  Qty lot{};
  Qty min_qty{};
  Qty max_qty{};  // zero: no limit
  Notional min_notional{};
  Notional max_notional{};  // zero: no limit
  TradingStatus status = TradingStatus::Trading;
  constexpr bool operator==(const VenueRules&) const noexcept = default;
};

// The instrument's rules as the table holds them.
[[nodiscard]] VenueRules rules_of(const Instrument& inst) noexcept;
// InstrumentRulesMsg::kTick, kLot, ... for every field of `now` that differs from `before`.
[[nodiscard]] std::uint8_t rules_changed(const VenueRules& before, const VenueRules& now) noexcept;
// The message the engine takes `r` from: hdr.instrument `id`, hdr.venue `venue`.
void fill_rules_msg(InstrumentRulesMsg& m,
                    VenueId venue,
                    InstrumentId id,
                    const VenueRules& r,
                    std::uint8_t changed,
                    std::uint8_t cause) noexcept;

class RulesWatch {
 public:
  static constexpr std::int64_t kDefaultPeriodNs = 60'000'000'000;
  // The shortest time between two reads a refusal asks for: a burst of refusals reads once.
  static constexpr std::int64_t kRefusalGapNs = 5'000'000'000;

  // `period_ns` 0: no periodic read; refusals still ask for one.
  void configure(std::int64_t period_ns) noexcept { period_ns_ = period_ns; }
  [[nodiscard]] std::int64_t period_ns() const noexcept { return period_ns_; }
  // The venue's rules at start-up (load_reference_data): what later reads are compared with.
  void seed(InstrumentId id, const VenueRules& r) noexcept;
  // The instrument was seeded: the connector reads its rules.
  [[nodiscard]] bool seeded(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments && seeded_[id.value];
  }
  // The venue refused something for an instrument's rules or status: read them soon.
  void want() noexcept { wanted_ = true; }
  // A read is due at `now_ns`: the period has passed since the last one, or a refusal asked and
  // kRefusalGapNs has; never while one is in flight, nor before anything was seeded.
  [[nodiscard]] bool due(std::int64_t now_ns) const noexcept;
  // A read went out (the cause the messages it leads to carry: InstrumentRulesMsg::kRefresh or
  // kRefusal), and came back, answered or not.
  void started(std::int64_t now_ns) noexcept;
  void finished() noexcept { in_flight_ = false; }
  [[nodiscard]] bool in_flight() const noexcept { return in_flight_; }
  // A read's rules for `id`. When they differ from the last ones, an InstrumentRulesMsg goes into
  // `sink` (none when `sink` is null: a pool member reads only for trading()) and they become the
  // last ones. Returns the fields that changed (zero: none, or `id` was never seeded). A rule the
  // venue gave no usable value for (a tick or lot that is not positive) keeps the last one.
  std::uint8_t update(EventSink* sink, VenueId venue, InstrumentId id, VenueRules r) noexcept;
  // The venue trades the instrument as of the last read (true for one never seeded). Any thread.
  [[nodiscard]] bool trading(InstrumentId id) const noexcept {
    return id.value >= kMaxInstruments || halted_[id.value].load(std::memory_order_relaxed) == 0;
  }
  [[nodiscard]] const VenueRules& last(InstrumentId id) const noexcept { return last_[id.value]; }
  // Reads made, and the changes they found (InstrumentRulesMsg emitted or, for a pool member,
  // that would have been).
  [[nodiscard]] std::uint64_t reads() const noexcept { return reads_; }
  [[nodiscard]] std::uint64_t changes() const noexcept { return changes_; }

 private:
  std::array<VenueRules, kMaxInstruments> last_{};
  std::array<bool, kMaxInstruments> seeded_{};
  std::array<std::atomic<std::uint8_t>, kMaxInstruments> halted_{};  // 1: not trading
  std::int64_t period_ns_ = kDefaultPeriodNs;
  std::int64_t last_read_ns_ = 0;
  std::uint64_t reads_ = 0;
  std::uint64_t changes_ = 0;
  std::uint8_t cause_ = InstrumentRulesMsg::kRefresh;
  bool any_seeded_ = false;
  bool wanted_ = false;
  bool in_flight_ = false;
};

// The venue's refusal names an instrument's rules or status (InvalidTick, InvalidLot,
// BelowMinNotional, InstrumentDisabled): the rules may have changed under the session.
[[nodiscard]] constexpr bool refusal_reads_rules(RejectReason r) noexcept {
  return r == RejectReason::InvalidTick || r == RejectReason::InvalidLot ||
         r == RejectReason::BelowMinNotional || r == RejectReason::InstrumentDisabled;
}

}  // namespace fastmm::venues
