#pragma once
// Instrument: the single definition of a tradable contract (5.3 / 5.15). 128 bytes: the
// first cache line holds everything the hot path reads (tick, lot, limits, multiplier),
// the second holds cold reference data (symbol, expiry, strike).
//
// All asset-class differences live here (contract_multiplier, option fields) so strategies
// never branch on asset class; they call inst.notional()/pnl_per_tick().
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/flat_map.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fastmm {

inline constexpr std::size_t kMaxInstruments = 256;
using Symbol = FixedString<20>;

struct alignas(kCacheLine) Instrument {
  enum Flags : std::uint8_t {
    kEnabled = 1U << 0,
    kInverse = 1U << 1,  // inverse contract (PnL in base currency)
    kReduceOnlySupported = 1U << 2,
  };

  // ---- hot line (64 bytes) -------------------------------------------------------------
  InstrumentId id{};                           // 4
  VenueId venue{};                             // 1
  AssetClass asset_class{};                    // 1
  std::uint8_t flags = 0;                      // 1
  std::uint8_t price_decimals = 8;             // 1  venue formatting hint
  Price tick{};                                // 8
  Qty lot{};                                   // 8
  Qty min_qty{};                               // 8
  Qty max_qty{};                               // 8
  Notional min_notional{};                     // 8
  Qty contract_multiplier = Qty::from_int(1);  // 8   units of underlying per contract
  Notional max_notional{};                     // 8 -> 64
  // ---- cold line (64 bytes) ------------------------------------------------------------
  std::int64_t expiry_ns = 0;                 // 8   (0 = perpetual / spot)
  Price strike{};                             // 8 -> 16
  Symbol symbol{};                            // 21
  FixedString<8> base{};                      // 9
  FixedString<8> quote{};                     // 9 -> 55
  OptionType option_type = OptionType::None;  // 1 -> 56
  std::uint8_t cold_pad_[8] = {};             // -> 64

  [[nodiscard]] constexpr bool enabled() const noexcept { return (flags & kEnabled) != 0; }
  [[nodiscard]] constexpr bool inverse() const noexcept { return (flags & kInverse) != 0; }

  // Passive rounding: bids down, asks up.
  [[nodiscard]] constexpr Price round_price(Price p, Side side) const noexcept {
    return round_to_tick(p, tick, side);
  }
  [[nodiscard]] constexpr Qty round_qty(Qty q) const noexcept { return round_to_lot(q, lot); }
  // n ticks as a price distance (tick * n).
  [[nodiscard]] constexpr Price ticks(std::int64_t n) const noexcept { return tick * n; }
  [[nodiscard]] constexpr bool valid_price(Price p) const noexcept {
    return p.is_positive() && on_tick(p, tick);
  }
  [[nodiscard]] constexpr bool valid_qty(Qty q) const noexcept {
    return q.is_positive() && on_lot(q, lot) && q >= min_qty && (max_qty.is_zero() || q <= max_qty);
  }
  // Notional in quote currency: price * qty * multiplier.
  [[nodiscard]] constexpr Notional notional(Price p, Qty q) const noexcept {
    const Notional n = mul(p, q);
    if (contract_multiplier == Qty::from_int(1)) return n;
    return Notional::from_raw(mul_raw(n, contract_multiplier));
  }
  // PnL change (quote ccy) for one tick move on one contract.
  [[nodiscard]] constexpr Notional pnl_per_tick() const noexcept {
    return Notional::from_raw(mul_raw(tick, contract_multiplier));
  }
  [[nodiscard]] constexpr bool is_derivative() const noexcept {
    return asset_class == AssetClass::Perpetual || asset_class == AssetClass::Future ||
           asset_class == AssetClass::Option;
  }
};
static_assert(sizeof(Instrument) == 128);
static_assert(alignof(Instrument) == 64);
static_assert(std::is_trivially_copyable_v<Instrument>);

enum class InstrumentError : std::uint8_t {
  TableFull,
  DuplicateSymbol,
  InvalidTick,
  InvalidLot,
  EmptySymbol,
};

// Dense-by-id table built at startup; symbol lookup is a sorted FlatMap keyed by
// (venue, symbol). Ids are the insertion index.
class InstrumentTable {
 public:
  struct Key {
    VenueId venue;
    Symbol symbol;
    constexpr auto operator<=>(const Key&) const noexcept = default;
  };

  Result<InstrumentId, InstrumentError> add(const Instrument& inst) noexcept {
    if (inst.symbol.empty()) return fail(InstrumentError::EmptySymbol);
    if (!inst.tick.is_positive()) return fail(InstrumentError::InvalidTick);
    if (!inst.lot.is_positive()) return fail(InstrumentError::InvalidLot);
    if (by_id_.full()) return fail(InstrumentError::TableFull);
    const InstrumentId id{static_cast<std::uint32_t>(by_id_.size())};
    auto [p, inserted] = by_symbol_.insert(Key{inst.venue, inst.symbol}, id);
    if (!inserted) return fail(InstrumentError::DuplicateSymbol);
    by_id_.push_back(inst);
    by_id_[id.value].id = id;
    return id;
  }

  [[nodiscard]] std::size_t size() const noexcept { return by_id_.size(); }
  [[nodiscard]] bool contains(InstrumentId id) const noexcept { return id.value < by_id_.size(); }
  [[nodiscard]] const Instrument& get(InstrumentId id) const noexcept {
    FASTMM_ASSERT(contains(id));
    return by_id_[id.value];
  }
  [[nodiscard]] Instrument& get(InstrumentId id) noexcept {
    FASTMM_ASSERT(contains(id));
    return by_id_[id.value];
  }
  [[nodiscard]] const Instrument& operator[](InstrumentId id) const noexcept { return get(id); }
  [[nodiscard]] const Instrument* find(VenueId venue, std::string_view symbol) const noexcept {
    const InstrumentId* id = by_symbol_.find(Key{venue, Symbol(symbol)});
    return id == nullptr ? nullptr : &by_id_[id->value];
  }
  [[nodiscard]] const Instrument* begin() const noexcept { return by_id_.begin(); }
  [[nodiscard]] const Instrument* end() const noexcept { return by_id_.end(); }
  [[nodiscard]] const Instrument* data() const noexcept { return by_id_.data(); }
  void clear() noexcept {
    by_id_.clear();
    by_symbol_.clear();
  }

 private:
  StaticVector<Instrument, kMaxInstruments> by_id_;
  FlatMap<Key, InstrumentId, kMaxInstruments> by_symbol_;
};

}  // namespace fastmm
