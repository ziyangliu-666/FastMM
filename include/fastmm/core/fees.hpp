#pragma once
// Fee rates per instrument, in centi-basis-points (integer) of the fill notional.
//
// Sign convention: a POSITIVE rate is a fee the account pays, a NEGATIVE rate is a rebate the
// account receives. `maker_bps = 10` is Binance spot VIP 0 (0.1 %), `maker_bps = -0.5` is a maker
// rebate of half a basis point.
//
// FeeTable is a default pair plus per-instrument overrides. The configuration maps each venue's
// [venues.<name>.fees] onto the instruments of that venue and [[instruments]] maker_bps /
// taker_bps override one instrument (fee_table, config/config.hpp). The simulated venue charges
// fills with it (sim::FeeModel is this type) and the engine hands the same table to strategies
// (StrategyContext::fees).
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace fastmm {

// One maker/taker pair. 1 cbps == 0.01 bps == 1e-6 of the notional.
struct FeeRates {
  std::int32_t maker_cbps = 0;  // > 0 fee, < 0 rebate
  std::int32_t taker_cbps = 0;

  [[nodiscard]] static FeeRates from_bps(double maker_bps, double taker_bps) noexcept {
    FeeRates f;
    f.maker_cbps = to_cbps(maker_bps);
    f.taker_cbps = to_cbps(taker_bps);
    return f;
  }
  [[nodiscard]] static constexpr std::int32_t to_cbps(double bps) noexcept {
    return static_cast<std::int32_t>(bps * 100.0 + (bps < 0 ? -0.5 : 0.5));
  }
  [[nodiscard]] constexpr double maker_bps() const noexcept { return maker_cbps / 100.0; }
  [[nodiscard]] constexpr double taker_bps() const noexcept { return taker_cbps / 100.0; }
  [[nodiscard]] constexpr std::int64_t cbps(Liquidity l) const noexcept {
    return l == Liquidity::Taker ? taker_cbps : maker_cbps;
  }
  [[nodiscard]] constexpr Notional fee(Notional notional, Liquidity l) const noexcept {
    return Notional::from_raw(
        static_cast<std::int64_t>(static_cast<Int128>(notional.raw) * cbps(l) / 1'000'000));
  }
  [[nodiscard]] constexpr Notional fee(Price px, Qty qty, Liquidity l) const noexcept {
    return fee(mul(px, qty), l);
  }
  [[nodiscard]] constexpr bool operator==(const FeeRates&) const noexcept = default;
};

class FeeTable {
 public:
  constexpr FeeTable() noexcept = default;

  // Every instrument pays the same rates.
  [[nodiscard]] static FeeTable from_bps(double maker_bps, double taker_bps) noexcept {
    return uniform(FeeRates::from_bps(maker_bps, taker_bps));
  }
  [[nodiscard]] static FeeTable uniform(FeeRates s) noexcept {
    FeeTable f;
    f.default_ = s;
    return f;
  }

  // Rates of every instrument without an override.
  void set_default(FeeRates s) noexcept { default_ = s; }
  void set_instrument(InstrumentId id, FeeRates s) noexcept {
    if (id.value >= kMaxInstruments) return;
    per_[id.value] = s;
    has_[id.value / 64] |= std::uint64_t{1} << (id.value % 64);
  }
  [[nodiscard]] constexpr bool has_override(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments &&
           (has_[id.value / 64] & (std::uint64_t{1} << (id.value % 64))) != 0;
  }
  [[nodiscard]] constexpr const FeeRates& default_schedule() const noexcept { return default_; }
  [[nodiscard]] constexpr const FeeRates& schedule(InstrumentId id) const noexcept {
    return has_override(id) ? per_[id.value] : default_;
  }
  // True when every override equals the default, so one line describes the whole run.
  [[nodiscard]] constexpr bool is_uniform() const noexcept {
    for (std::size_t i = 0; i < kMaxInstruments; ++i) {
      if (has_override(InstrumentId{static_cast<std::uint32_t>(i)}) && !(per_[i] == default_))
        return false;
    }
    return true;
  }

  [[nodiscard]] constexpr std::int64_t maker_cbps() const noexcept { return default_.maker_cbps; }
  [[nodiscard]] constexpr std::int64_t taker_cbps() const noexcept { return default_.taker_cbps; }

  [[nodiscard]] constexpr Notional fee(InstrumentId id,
                                       Price px,
                                       Qty qty,
                                       Liquidity l) const noexcept {
    return schedule(id).fee(px, qty, l);
  }
  // The default rates; for callers without an instrument (the sim exchange server).
  [[nodiscard]] constexpr Notional fee(Price px, Qty qty, Liquidity l) const noexcept {
    return default_.fee(px, qty, l);
  }
  [[nodiscard]] constexpr Notional fee(Notional n, Liquidity l) const noexcept {
    return default_.fee(n, l);
  }

 private:
  FeeRates default_{};
  std::array<FeeRates, kMaxInstruments> per_{};
  std::array<std::uint64_t, kMaxInstruments / 64> has_{};
};

static_assert(std::is_trivially_copyable_v<FeeTable>);

}  // namespace fastmm
