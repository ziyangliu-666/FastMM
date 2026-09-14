// Expected: verify_strategy rejects on_fill with its parameters swapped.
#include "fastmm/strategies/hooks.hpp"

namespace {

struct Quoter {
#ifdef FASTMM_CF_CONTROL
  void on_fill(auto&, const fastmm::Fill&) noexcept {}
#else
  void on_fill(const fastmm::Fill&, auto&) noexcept {}
#endif
};

static_assert(fastmm::verify_strategy<Quoter>());

}  // namespace
