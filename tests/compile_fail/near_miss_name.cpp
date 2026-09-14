// Expected: a member called on_fills draws the "did you mean on_fill" deprecation warning (an error
// in the near_miss_name_werror case, built with -Werror=deprecated-declarations). The control opts
// out with fastmm_allow_near_miss_names.
#include "fastmm/strategies/hooks.hpp"

namespace {

struct Quoter {
#ifdef FASTMM_CF_CONTROL
  static constexpr bool fastmm_allow_near_miss_names = true;
#endif
  void on_fills(auto&, const fastmm::Fill&) noexcept {}
};

static_assert(fastmm::verify_strategy<Quoter>());

}  // namespace
