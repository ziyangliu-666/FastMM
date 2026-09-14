#pragma once
// Mdp3InstrumentTable: CME SecurityID (tag 48) -> dense InstrumentId, filled from
// MDInstrumentDefinitionFuture54 (or by hand). Fixed capacity, no allocation after construction:
// the hash index is allocated once by OpenHashMap's constructor.
//
// Prices stay in CME Globex display units, exactly as MDP 3.0 sends them (MinPriceIncrement,
// MDEntryPx). DisplayFactor (tag 9787) is kept alongside so a consumer can convert to the
// conventional price (conventional = display * DisplayFactor, "MDP 3.0 - Market Data Security
// Definition - Futures", CME Client Systems Wiki).
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm::codecs::mdp3 {

inline constexpr std::size_t kMaxMdp3Instruments = 1024;
// MBP books are at most 10 levels deep ("maximum ten MBP aggregate levels", "MDP 3.0 -
// Incremental Refresh SBE Template Book Processing"); the per-instrument depth is tag 264
// MarketDepth of the GBX MDFeedType entry.
inline constexpr std::uint8_t kMaxMbpDepth = 10;

struct Mdp3Instrument {
  std::int32_t security_id = 0;     // tag 48
  InstrumentId id{};                // dense id used in engine events (defaults to the table index)
  Symbol symbol{};                  // tag 55
  FixedString<6> security_group{};  // tag 1151
  FixedString<6> asset{};           // tag 6937
  Price tick{};                     // tag 969 MinPriceIncrement (display units); 0 if absent
  std::int64_t display_factor_e9 = 0;    // tag 9787 DisplayFactor mantissa, exponent -9
  Qty unit_of_measure_qty{};             // tag 1147 contract size; 0 if null or not exact at 1e-8
  std::int32_t contract_multiplier = 0;  // tag 231; 0 if null
  std::int16_t appl_id = 0;              // tag 1180 channel id
  std::uint8_t market_depth = kMaxMbpDepth;  // tag 264 for MDFeedType GBX
  std::uint8_t implied_depth = 0;            // tag 264 for MDFeedType GBI (implied book)
  std::uint8_t trading_status = 0;  // last SecurityTradingStatus (tag 326 / 1682); 0 = unknown
  bool deleted = false;             // SecurityUpdateAction 'D'
};

class Mdp3InstrumentTable {
 public:
  Mdp3InstrumentTable() noexcept = default;
  Mdp3InstrumentTable(const Mdp3InstrumentTable&) = delete;
  Mdp3InstrumentTable& operator=(const Mdp3InstrumentTable&) = delete;

  // Existing entry for security_id, or a new one with id == its table index. nullptr when full.
  // `inserted` reports which.
  [[nodiscard]] Mdp3Instrument* upsert(std::int32_t security_id, bool& inserted) noexcept {
    inserted = false;
    if (const std::uint32_t* idx = index_.find(security_id)) return &by_index_[*idx];
    if (by_index_.full()) return nullptr;
    const auto idx = static_cast<std::uint32_t>(by_index_.size());
    if (index_.insert(security_id, idx).first == nullptr) return nullptr;
    Mdp3Instrument inst{};
    inst.security_id = security_id;
    inst.id = InstrumentId{idx};
    static_cast<void>(by_index_.push_back(inst));
    inserted = true;
    return &by_index_[idx];
  }

  // Table index for a SecurityID, or -1.
  [[nodiscard]] std::int32_t index_of(std::int32_t security_id) const noexcept {
    const std::uint32_t* idx = index_.find(security_id);
    return idx == nullptr ? -1 : static_cast<std::int32_t>(*idx);
  }
  [[nodiscard]] const Mdp3Instrument* find(std::int32_t security_id) const noexcept {
    const std::uint32_t* idx = index_.find(security_id);
    return idx == nullptr ? nullptr : &by_index_[*idx];
  }
  [[nodiscard]] std::size_t size() const noexcept { return by_index_.size(); }
  [[nodiscard]] const Mdp3Instrument& at(std::size_t index) const noexcept {
    return by_index_[index];
  }
  [[nodiscard]] Mdp3Instrument& at(std::size_t index) noexcept { return by_index_[index]; }

  // Engine Instrument for a definition: symbol, tick (display units, lot 1 contract) and
  // contract multiplier (UnitOfMeasureQty when present). Not a complete risk description.
  [[nodiscard]] static Instrument to_instrument(const Mdp3Instrument& d, VenueId venue) noexcept {
    Instrument i{};
    i.id = d.id;
    i.venue = venue;
    i.asset_class = AssetClass::Future;
    i.flags = Instrument::kEnabled;
    i.tick = d.tick;
    i.lot = Qty::from_int(1);
    i.min_qty = Qty::from_int(1);
    if (d.unit_of_measure_qty.is_positive()) i.contract_multiplier = d.unit_of_measure_qty;
    i.symbol = d.symbol;
    return i;
  }

 private:
  StaticVector<Mdp3Instrument, kMaxMdp3Instruments> by_index_;
  OpenHashMap<std::int32_t, std::uint32_t, 2048> index_;
};

}  // namespace fastmm::codecs::mdp3
