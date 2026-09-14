// register_strategy<S> in a file without the factory definitions (only strategies/module.hpp) and
// no explicit instantiation anywhere: the program compiles and fails to link, naming the missing
// factory. The control includes factories.hpp and links.
#include "fastmm/strategies/module.hpp"
#include "fastmm/strategy.hpp"
#ifdef FASTMM_CF_CONTROL
#include "fastmm/strategies/factories.hpp"
#endif

#include <string_view>

struct LinkParams {
  FASTMM_PARAMS(LinkParams)
  FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels per side")
};

class LinkMM : public fastmm::StrategyBase<LinkParams> {
 public:
  static constexpr std::string_view name() noexcept { return "link_mm"; }
};

int main() {
  fastmm::StrategyRegistry registry;
  fastmm::register_strategy<LinkMM>(registry);
  return registry.find("link_mm") != nullptr ? 0 : 1;
}
