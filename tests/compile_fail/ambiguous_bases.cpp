// Expected (documented limit): a hook inherited from two bases is ambiguous and reported as a wrong
// signature. The control brings one of them into scope with a using-declaration.
#include "fastmm/strategies/hooks.hpp"

namespace {

struct Left {
  void on_quoting(auto&, bool) noexcept {}
};
struct Right {
  void on_quoting(auto&, bool) noexcept {}
};
struct Quoter : Left, Right {
#ifdef FASTMM_CF_CONTROL
  using Left::on_quoting;
#endif
};

static_assert(fastmm::verify_strategy<Quoter>());

}  // namespace
