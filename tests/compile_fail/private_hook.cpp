// Expected: verify_strategy rejects a hook the engine cannot call because it is private.
#include "fastmm/strategies/hooks.hpp"

namespace {

class Quoter {
#ifdef FASTMM_CF_CONTROL
 public:
#endif
  template <class Ctx, class Book>
  void on_book(Ctx&, fastmm::InstrumentId, const Book&) noexcept {}
};

static_assert(fastmm::verify_strategy<Quoter>());

}  // namespace
