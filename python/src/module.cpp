// fastmm._core: pybind11 entry point. The bindings are split by area (bind_*.cpp).
#include "bind_common.hpp"

#include "fastmm/backtest/registrations.hpp"
#include "fastmm/version.hpp"

#include <string>

PYBIND11_MODULE(_core, m) {
  namespace fb = fastmm::py_bind;
  m.doc() = "FastMM core bindings: backtests, parameter sweeps, order book research helpers";
  m.attr("__version__") = fastmm::kVersionString;
  m.def(
      "build_info",
      [] { return std::string(fastmm::build_info()); },
      "Compiler, flags and build type of the native module.");
  // The registry is not thread-safe for writers: register before any worker can run.
  static_cast<void>(fastmm::bt::register_builtin_strategies());
  fb::bind_config(m);
  fb::bind_backtest(m);
  fb::bind_book(m);
  fb::bind_strategies(m);
}
