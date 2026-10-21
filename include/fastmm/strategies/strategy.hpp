#pragma once
// Strategy concept and the StrategyBase helper (8.6). A strategy is a plain class with
// static name()/schema(), a params struct declared with FASTMM_PARAMS, and any subset of
// the engine hooks (all optional; see strategies/hooks.hpp and docs/reference/strategy-api.md).
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/strategies/hooks.hpp"
#include "fastmm/strategies/params.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace fastmm {

template <class S>
concept StrategyLike = requires {
  { S::name() } -> std::convertible_to<std::string_view>;
  { S::schema() } -> std::convertible_to<const ParamSchema&>;
} && std::is_default_constructible_v<S>;

// How a StrategyBase keeps its parameters. Shared: one set for every instrument, and an update
// naming an instrument is refused by its publisher. PerInstrument: a set per instrument as well,
// which params(id) reads; an update without an instrument changes every set, one naming an
// instrument changes that instrument's (fastmm-ctl param --instrument, an [ab] test that splits
// the instruments).
enum class ParamScope : std::uint8_t { Shared, PerInstrument };

template <class Params, ParamScope Scope = ParamScope::Shared>
class StrategyBase {
 public:
  using params_type = Params;
  static constexpr ParamScope param_scope = Scope;
  static const ParamSchema& schema() { return Params::schema(); }
  // Applies string parameters, then runs `validate() const` when the params struct declares one.
  // Returns the first error message; the parameters are unchanged then (startup only).
  std::optional<std::string> configure(const ParamMap& m) {
    Params next = params_;
    if (auto err = next.apply(m)) return err;
    if (auto err = detail::validate_params(next)) return err;
    params_ = next;
    if constexpr (Scope == ParamScope::PerInstrument) per_instrument_.fill(params_);
    return std::nullopt;
  }
  // Read-only: parameters change through configure() and apply_param_update(). The set every
  // instrument starts from: the configured values and the updates that named no instrument.
  [[nodiscard]] const Params& params() const noexcept { return params_; }
  // Instrument `id`'s parameters: params() with the updates that named it on top. A Shared
  // strategy has one set, so this is params().
  [[nodiscard]] const Params& params(InstrumentId id) const noexcept {
    if constexpr (Scope == ParamScope::PerInstrument) {
      return id.value < kMaxInstruments ? per_instrument_[id.value] : params_;
    } else {
      static_cast<void>(id);
      return params_;
    }
  }
  // Engine thread: assigns the (field, raw value) pairs of a ParamUpdateMsg, which the publisher
  // validated (strategies/param_publisher.hpp): to every set, or with PerInstrument and an
  // instrument in the message, to that instrument's. A field outside the schema is skipped.
  void apply_param_update(const ParamUpdateMsg& m) noexcept {
    if constexpr (Scope == ParamScope::PerInstrument) {
      if (!m.all_instruments()) {
        if (m.hdr.instrument.value < kMaxInstruments)
          assign(m, per_instrument_[m.hdr.instrument.value]);
        return;
      }
      for (Params& p : per_instrument_) assign(m, p);
    }
    assign(m, params_);
  }
  [[nodiscard]] std::string describe_params() const { return params_.describe(); }

 private:
  static void assign(const ParamUpdateMsg& m, Params& p) noexcept {
    const ParamSchema& s = Params::schema();
    const std::size_t n =
        m.count < ParamUpdateMsg::kMaxFields ? m.count : ParamUpdateMsg::kMaxFields;
    for (std::size_t i = 0; i < n; ++i) {
      if (m.field[i] < s.size()) s.begin()[m.field[i]].set_raw(&p, m.value[i]);
    }
  }

  struct NoSets {};
  Params params_{};
  // PerInstrument: one set per instrument id.
  [[no_unique_address]] std::conditional_t<Scope == ParamScope::PerInstrument,
                                           std::array<Params, kMaxInstruments>,
                                           NoSets> per_instrument_{};
};

}  // namespace fastmm
