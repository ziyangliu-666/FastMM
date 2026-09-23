#pragma once
// Fee schedules of the simulated venue, in centi-basis-points (integer) of the fill notional.
//
// Sign convention: a POSITIVE rate is a fee the account pays, a NEGATIVE rate is a rebate the
// account receives. The simulated venue books the signed amount as OrderFillMsg::fee, so the
// PnL ledger subtracts it: a rebate raises net PnL. `maker_bps = 10` is Binance spot VIP 0
// (0.1 %), `maker_bps = -0.5` is a maker rebate of half a basis point.
//
// FeeModel is a default schedule plus per-instrument overrides. Backtest configs map each
// venue's [venues.<name>.fees] onto the instruments of that venue, so a run with instruments on
// two venues charges each of them its own schedule; [[instruments]] maker_bps / taker_bps
// override a single instrument.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"

#include <array>
#include <cstdint>

namespace fastmm::sim {

// One maker/taker pair. 1 cbps == 0.01 bps == 1e-6 of the notional.
struct FeeSchedule {
  std::int32_t maker_cbps = 0;  // > 0 fee, < 0 rebate
  std::int32_t taker_cbps = 0;

  [[nodiscard]] static FeeSchedule from_bps(double maker_bps, double taker_bps) noexcept {
    FeeSchedule f;
    f.maker_cbps = to_cbps(maker_bps);
    f.taker_cbps = to_cbps(taker_bps);
    return f;
  }
  [[nodiscard]] static constexpr std::int32_t to_cbps(double bps) noexcept {
    return static_cast<std::int32_t>(bps * 100.0 + (bps < 0 ? -0.5 : 0.5));
  }
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
  [[nodiscard]] constexpr bool operator==(const FeeSchedule&) const noexcept = default;
};

class FeeModel {
 public:
  constexpr FeeModel() noexcept = default;

  // Every instrument pays the same schedule.
  [[nodiscard]] static FeeModel from_bps(double maker_bps, double taker_bps) noexcept {
    return uniform(FeeSchedule::from_bps(maker_bps, taker_bps));
  }
  [[nodiscard]] static FeeModel uniform(FeeSchedule s) noexcept {
    FeeModel f;
    f.default_ = s;
    return f;
  }

  // Schedule of every instrument without an override.
  void set_default(FeeSchedule s) noexcept { default_ = s; }
  void set_instrument(InstrumentId id, FeeSchedule s) noexcept {
    if (id.value >= kMaxInstruments) return;
    per_[id.value] = s;
    has_[id.value / 64] |= std::uint64_t{1} << (id.value % 64);
  }
  [[nodiscard]] constexpr bool has_override(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments &&
           (has_[id.value / 64] & (std::uint64_t{1} << (id.value % 64))) != 0;
  }
  [[nodiscard]] constexpr const FeeSchedule& default_schedule() const noexcept { return default_; }
  [[nodiscard]] constexpr const FeeSchedule& schedule(InstrumentId id) const noexcept {
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
  // The default schedule; for callers without an instrument (the sim exchange server).
  [[nodiscard]] constexpr Notional fee(Price px, Qty qty, Liquidity l) const noexcept {
    return default_.fee(px, qty, l);
  }
  [[nodiscard]] constexpr Notional fee(Notional n, Liquidity l) const noexcept {
    return default_.fee(n, l);
  }

 private:
  FeeSchedule default_{};
  std::array<FeeSchedule, kMaxInstruments> per_{};
  std::array<std::uint64_t, kMaxInstruments / 64> has_{};
};

static_assert(std::is_trivially_copyable_v<FeeModel>);

}  // namespace fastmm::sim
