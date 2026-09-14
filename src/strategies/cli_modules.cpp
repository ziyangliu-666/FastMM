#include "fastmm/cli/modules.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cstdio>
#include <exception>

namespace fastmm::cli {

std::string program_name(int argc, char** argv, std::string_view fallback) {
  if (argc < 1 || argv == nullptr || argv[0] == nullptr || argv[0][0] == '\0')
    return std::string(fallback);
  const std::string_view path(argv[0]);
  const std::size_t slash = path.find_last_of('/');
  const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
  return base.empty() ? std::string(fallback) : std::string(base);
}

bool register_strategy_modules(std::string_view program, std::span<const StrategyModule> modules) {
  StrategyRegistry& r = StrategyRegistry::instance();
  try {
    register_builtin_strategies(r);
    for (const StrategyModule m : modules) {
      if (m != nullptr) m(r);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%.*s: %s\n", static_cast<int>(program.size()), program.data(), e.what());
    return false;
  }
  return true;
}

}  // namespace fastmm::cli
