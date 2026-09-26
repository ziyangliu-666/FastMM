#pragma once
// Net position per underlying ([risk.underlying.<BASE>], [gateway.underlying.<BASE>]): a limit on
// the signed position in one base asset, in base units, over every instrument and venue that trades
// it. Long 0.3 BTC on one venue and short 0.3 BTC on another net to zero.
//
// An instrument counts towards the underlying its `base` names (case-insensitive). Its quantity is
// converted to base units: qty * contract_multiplier for a linear contract (or spot), and
// qty * multiplier / mark for an inverse one, whose contracts are sized in the quote currency
// (Instrument::notional, in the base coin). Options do not count: their exposure in the underlying
// is a delta, which needs a model the engine does not have.
//
// UnderlyingPlan is built once, after the venues' reference data has loaded (only the venue says
// which contracts are inverse and what their multiplier is): each configured underlying's index,
// its limit, and the instruments that count towards it.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/strong_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace fastmm {

inline constexpr std::size_t kMaxUnderlyings = 8;
using UnderlyingName = FixedString<8>;

// [risk.underlying.<BASE>] (or [gateway.underlying.<BASE>]): max_net per base asset, a decimal
// string as written ("0" keeps the underlying tracked with no limit, to be set at run time).
struct UnderlyingSpec {
  std::map<std::string, std::string> max_net;
  [[nodiscard]] bool configured() const noexcept { return !max_net.empty(); }
};

// Does `inst` count towards the underlying its base names? Options do not.
[[nodiscard]] constexpr bool counts_toward_underlying(const Instrument& inst) noexcept {
  return inst.asset_class != AssetClass::Option && !inst.coin_quoted();
}

// `qty_raw` contracts of `inst` in base units (raw Qty): at `mark` for an inverse contract, which
// fails (returns false) without a positive mark; a linear one needs none.
[[nodiscard]] constexpr bool to_base_units(const Instrument& inst,
                                           std::int64_t qty_raw,
                                           Price mark,
                                           std::int64_t& out) noexcept {
  if (qty_raw == 0) {
    out = 0;
    return true;
  }
  if (inst.inverse()) {
    if (!mark.is_positive()) return false;
    out = static_cast<std::int64_t>(static_cast<Int128>(qty_raw) * inst.contract_multiplier.raw /
                                    mark.raw);
    return true;
  }
  out = inst.contract_multiplier == Qty::from_int(1)
            ? qty_raw
            : mul_raw(Qty::from_raw(qty_raw), inst.contract_multiplier);
  return true;
}

// The limit on one order: `net` is the underlying's position now, `worst` the position with every
// open order on the order's side and the order itself filled (base units, raw). Like max_position,
// an order is refused only when it takes |worst| past the limit and further from zero than the
// position is now, so an order that brings the underlying back towards zero always passes.
[[nodiscard]] constexpr bool underlying_exceeds(std::int64_t max_net,
                                                std::int64_t net,
                                                std::int64_t worst) noexcept {
  const std::int64_t abs_worst = worst < 0 ? -worst : worst;
  const std::int64_t abs_net = net < 0 ? -net : net;
  return max_net > 0 && abs_worst > max_net && abs_worst > abs_net;
}

struct UnderlyingPlan {
  std::uint8_t count = 0;  // configured underlyings
  std::array<UnderlyingName, kMaxUnderlyings> names{};
  std::array<Qty, kMaxUnderlyings> max_net{};      // base units; 0: tracked, no limit
  std::array<std::uint8_t, kMaxInstruments> of{};  // 1 + the underlying of each instrument; 0: none
  // The instruments of underlying u are members[begin[u] .. begin[u + 1]).
  std::array<InstrumentId, kMaxInstruments> members{};
  std::array<std::uint16_t, kMaxUnderlyings + 1> begin{};

  [[nodiscard]] bool active() const noexcept { return count > 0; }
  // The underlying `id` counts towards, or -1.
  [[nodiscard]] int underlying_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? static_cast<int>(of[id.value]) - 1 : -1;
  }
  [[nodiscard]] std::span<const InstrumentId> instruments(std::size_t u) const noexcept {
    return u < count ? std::span<const InstrumentId>(members.data() + begin[u],
                                                     members.data() + begin[u + 1])
                     : std::span<const InstrumentId>{};
  }
  // The index of the underlying `name` (case-insensitive), or -1.
  [[nodiscard]] int find(std::string_view name) const noexcept;
};

// Builds the plan for `table`. Every configured underlying needs an instrument of the table whose
// base it is and that counts (not an option); `section` names the configuration section in the
// error ("risk" or "gateway"). An unconfigured spec gives an inactive plan.
[[nodiscard]] Result<UnderlyingPlan, std::string> build_underlying_plan(
    const InstrumentTable& table, const UnderlyingSpec& spec, std::string_view section = "risk");

}  // namespace fastmm
