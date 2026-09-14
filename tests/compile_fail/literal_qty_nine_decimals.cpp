// Expected: a _qty literal with 9 decimals does not compile (Qty holds 8). The control uses 8.
#include "fastmm/core/fixed_point.hpp"

namespace {

using namespace fastmm::literals;

#ifdef FASTMM_CF_CONTROL
[[maybe_unused]] constexpr fastmm::Qty kQty = 0.00000001_qty;
#else
[[maybe_unused]] constexpr fastmm::Qty kQty = 0.000000001_qty;
#endif

}  // namespace
