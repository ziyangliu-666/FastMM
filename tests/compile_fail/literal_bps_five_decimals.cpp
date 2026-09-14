// Expected: a _bps literal with 5 decimals does not compile (Ratio holds 0.0001 bp). The control
// uses 4.
#include "fastmm/core/fixed_point.hpp"

namespace {

using namespace fastmm::literals;

#ifdef FASTMM_CF_CONTROL
[[maybe_unused]] constexpr fastmm::Ratio kSpread = 0.0001_bps;
#else
[[maybe_unused]] constexpr fastmm::Ratio kSpread = 0.00001_bps;
#endif

}  // namespace
