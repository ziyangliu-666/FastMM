#pragma once
// Strategy concept and the StrategyBase helper (8.6). A strategy is a plain class with
// static name()/schema(), a params struct declared with FASTMM_PARAMS, and any subset of
// the engine hooks (all optional; see strategies/hooks.hpp and docs/reference/strategy-api.md).
#include "fastmm/strategies/hooks.hpp"
#include "fastmm/strategies/params.hpp"

#include <concepts>
#include <optional>
#include <string>
#include <string_view>

namespace fastmm {

template <class S>
concept StrategyLike = requires {
  { S::name() } -> std::convertible_to<std::string_view>;
  { S::schema() } -> std::convertible_to<const ParamSchema&>;
} && std::is_default_constructible_v<S>;

template <class Params>
class StrategyBase {
 public:
  using params_type = Params;
  static const ParamSchema& schema() { return Params::schema(); }
  // Applies string parameters, then runs `validate() const` when the params struct declares one.
  // Returns the first error message; the parameters are unchanged then (startup only).
  std::optional<std::string> configure(const ParamMap& m) {
    Params next = params_;
    if (auto err = next.apply(m)) return err;
    if (auto err = detail::validate_params(next)) return err;
    params_ = next;
    return std::nullopt;
  }
  // Read-only: parameters change only through configure().
  [[nodiscard]] const Params& params() const noexcept { return params_; }
  [[nodiscard]] std::string describe_params() const { return params_.describe(); }

 private:
  Params params_{};
};

}  // namespace fastmm
