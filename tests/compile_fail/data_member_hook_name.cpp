// Expected: a data member that uses a hook's name is caught by the name probe.
#include "fastmm/strategies/hooks.hpp"

namespace {

struct Quoter {
#ifdef FASTMM_CF_CONTROL
  bool connected = false;
#else
  bool on_connection = false;
#endif
};

static_assert(fastmm::verify_strategy<Quoter>());

}  // namespace
