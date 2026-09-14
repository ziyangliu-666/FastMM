// Expected: hooks return void; a timer hook returning bool is reported as a wrong signature.
#include "fastmm/strategies/hooks.hpp"

#include <cstdint>

namespace {

struct Quoter {
#ifdef FASTMM_CF_CONTROL
  void on_timer(auto&, fastmm::TimerId, std::uint64_t) noexcept {}
#else
  bool on_timer(auto&, fastmm::TimerId, std::uint64_t) noexcept { return true; }
#endif
};

static_assert(fastmm::verify_strategy<Quoter>());

}  // namespace
