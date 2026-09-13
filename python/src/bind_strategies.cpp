// strategies(): parameter schemas of every registered strategy (8.6).
#include "bind_common.hpp"

#include "fastmm/backtest/registrations.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cmath>
#include <string>

namespace fastmm::py_bind {

namespace {

py::object typed_value(ParamType t, double v) {
  switch (t) {
    case ParamType::Bool:
      return py::bool_(v != 0.0);
    case ParamType::Int:
      return py::int_(std::llround(v));
    case ParamType::Double:
      return py::float_(v);
  }
  return py::float_(v);
}

}  // namespace

void bind_strategies(py::module_& m) {
  m.def(
      "strategies",
      [] {
        static_cast<void>(bt::register_builtin_strategies());
        py::dict out;
        for (const StrategyEntry& e : list_strategies()) {
          py::list params;
          if (e.schema != nullptr) {
            for (const ParamDesc& d : *e.schema) {
              py::dict p;
              p["name"] = d.name;
              p["type"] = std::string(to_string(d.type));
              p["default"] = typed_value(d.type, d.def);
              p["min"] = typed_value(d.type, d.min);
              p["max"] = typed_value(d.type, d.max);
              p["doc"] = d.doc;
              params.append(std::move(p));
            }
          }
          out[py::str(std::string(e.name))] = std::move(params);
        }
        return out;
      },
      "Registered strategies and their parameter schemas: "
      "{name: [{'name', 'type', 'default', 'min', 'max', 'doc'}, ...]}.");
}

}  // namespace fastmm::py_bind
