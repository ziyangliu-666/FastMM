#pragma once
// Accounting across settlement currencies ([accounting]). Positions, PnL and fees stay in each
// instrument's settlement currency; the totals the risk checks and reports read are converted to
// one reporting currency at the mid of an FX source instrument per other currency.
//
// FxPlan is built once, after the venues' reference data has loaded (an inverse contract settles
// in its base coin, and only the venue says which contracts are inverse): each instrument's
// currency index, and for each currency the instrument whose mid prices it. Index 0 is the
// reporting currency. A plan with fewer than two currencies converts nothing.
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

inline constexpr std::size_t kMaxCurrencies = 8;
using Currency = FixedString<8>;

// Currency names compare without regard to case ("usdt" from one venue, "USDT" from another).
[[nodiscard]] constexpr bool same_currency(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
    if (up(a[i]) != up(b[i])) return false;
  }
  return true;
}

// amount in the reporting currency = amount * num / den. Unknown (num == 0) converts to zero.
struct FxRate {
  std::int64_t num = 0;
  std::int64_t den = 1;

  [[nodiscard]] static constexpr FxRate identity() noexcept { return {1, 1}; }
  // The rate a source's mid gives: the mid itself, or its inverse when the source is quoted the
  // other way (the reporting currency is its base). Unknown for a non-positive mid.
  [[nodiscard]] static constexpr FxRate from_mid(Price mid, bool invert) noexcept {
    if (!mid.is_positive()) return {};
    return invert ? FxRate{kFixedScale, mid.raw} : FxRate{mid.raw, kFixedScale};
  }
  [[nodiscard]] constexpr bool known() const noexcept { return num > 0; }
  // One unit of the currency in the reporting currency.
  [[nodiscard]] constexpr Price price() const noexcept {
    return Price::from_raw(
        known() ? static_cast<std::int64_t>(static_cast<Int128>(kFixedScale) * num / den) : 0);
  }
};

[[nodiscard]] constexpr Notional convert(Notional n, FxRate r) noexcept {
  if (r.num == r.den) return n;
  return Notional::from_raw(static_cast<std::int64_t>(static_cast<Int128>(n.raw) * r.num / r.den));
}

// A currency's FX source: the instrument whose mid is the price of one unit in the reporting
// currency, or of one unit of the reporting currency in it (`invert`).
struct FxSource {
  InstrumentId instrument{};  // invalid: none (the reporting currency, or a currency nothing
                              // prices: its rate is never known)
  bool invert = false;
};

struct FxPlan {
  std::uint8_t count = 0;  // currencies; < 2: nothing is converted
  std::array<Currency, kMaxCurrencies> names{};
  std::array<FxSource, kMaxCurrencies> sources{};
  std::array<std::uint8_t, kMaxInstruments> ccy{};     // each instrument's currency index
  std::array<std::uint8_t, kMaxInstruments> prices{};  // 1 + the currency it prices; 0: none

  [[nodiscard]] bool active() const noexcept { return count > 1; }
  [[nodiscard]] std::string_view reporting() const noexcept { return names[0].view(); }
  // The currency `id` is an FX source of, or -1.
  [[nodiscard]] int priced_by(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? static_cast<int>(prices[id.value]) - 1 : -1;
  }
};

// [accounting]: the reporting currency and, per other currency, its source as "venue:symbol".
struct AccountingSpec {
  std::string reporting_currency;         // empty: no accounting (one currency, as before)
  std::map<std::string, std::string> fx;  // currency -> "venue:symbol"
  [[nodiscard]] bool configured() const noexcept { return !reporting_currency.empty(); }
};

// Builds the plan for `table`, whose venue ids index `venue_names`. Every settlement currency of
// an enabled instrument (of every instrument with `all_instruments`: the gateway's account holds
// all of them) must be the reporting currency or have a source; a source must be an instrument of
// the table that prices its currency in the reporting currency, either way round. An instrument
// outside that rule whose currency has no source gets a slot whose rate is never known. An
// unconfigured spec gives an inactive plan. The error names the currency and what to do.
[[nodiscard]] Result<FxPlan, std::string> build_fx_plan(const InstrumentTable& table,
                                                        const AccountingSpec& spec,
                                                        std::span<const std::string> venue_names,
                                                        bool all_instruments = false);

// The plan a session runs with (fastmm-live, fastmm-gateway, backtest, replay): build_fx_plan,
// except that a table the spec does not cover is an error only while `guarded` (a loss or exposure
// limit reads the totals). Otherwise `*warning` says why and the plan converts nothing, as without
// [accounting].
[[nodiscard]] Result<FxPlan, std::string> session_fx_plan(const InstrumentTable& table,
                                                          const AccountingSpec& spec,
                                                          std::span<const std::string> venue_names,
                                                          bool all_instruments,
                                                          bool guarded,
                                                          std::string* warning);

}  // namespace fastmm
