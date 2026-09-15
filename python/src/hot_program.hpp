#pragma once
// The dict fastmm._hot.compiler.CompiledHot.program() returns, as a HotProgram. fastmm._core
// (backtests) and fastmm_live._live (live sessions) each compile their own copy.
//
//   hooks         {hook name: cfunc address}
//   timers        [(cfunc address, period ns)]
//   record        bytes: one `self` record, the parameter block first
//   param_bytes   size of the parameter block
//   book_depth    optional bool
//   param_fields  optional [{name, type ("int", "double", "bool"), offset, raw_offset, min, max,
//                 default, doc}] (fastmm._hot.meta.param_fields)
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_params.hpp"
#include "fastmm/strategies/hot_strategy.hpp"
#include "fastmm/strategies/params.hpp"

#include <pybind11/pybind11.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fastmm::py_hot {

namespace py = pybind11;

inline fastmm_hot_fn fn_from(py::handle address) {
  const auto a = address.cast<std::uintptr_t>();
  if (a == 0) throw py::value_error("fastmm: a hot hook address is 0");
  return reinterpret_cast<fastmm_hot_fn>(a);  // NOLINT(performance-no-int-to-ptr)
}

inline std::vector<HotParamField> param_fields_from(const py::dict& program) {
  std::vector<HotParamField> out;
  if (!program.contains("param_fields")) return out;
  for (const auto& item : program["param_fields"].cast<py::list>()) {
    const auto f = item.cast<py::dict>();
    HotParamField field;
    field.name = f["name"].cast<std::string>();
    const auto type = f["type"].cast<std::string>();
    if (type == "int") {
      field.type = ParamType::Int;
    } else if (type == "double") {
      field.type = ParamType::Double;
    } else if (type == "bool") {
      field.type = ParamType::Bool;
    } else {
      throw py::value_error("fastmm: parameter '" + field.name + "' has type '" + type +
                            "'; expected int, double or bool");
    }
    field.offset = f["offset"].cast<std::uint32_t>();
    field.raw_offset = f["raw_offset"].cast<std::int32_t>();
    if (field.type == ParamType::Int) {
      if (!f["min"].is_none()) field.int_min = f["min"].cast<std::int64_t>();
      if (!f["max"].is_none()) field.int_max = f["max"].cast<std::int64_t>();
    } else if (field.type == ParamType::Double) {
      if (!f["min"].is_none()) field.min = f["min"].cast<double>();
      if (!f["max"].is_none()) field.max = f["max"].cast<double>();
    }
    field.def = f["default"].cast<double>();
    field.doc = f["doc"].cast<std::string>();
    out.push_back(std::move(field));
  }
  return out;
}

inline HotProgram program_from(const py::dict& program, bool stop_on_error) {
  HotProgram p;
  for (const auto& [name, address] : program["hooks"].cast<py::dict>()) {
    const auto n = name.cast<std::string>();
    bool found = false;
    for (std::size_t i = 0; i < p.hooks.size(); ++i) {
      if (to_string(static_cast<HotHook>(i)) == n) {
        p.hooks[i] = fn_from(address);
        found = true;
      }
    }
    if (!found) throw py::value_error("fastmm: '" + n + "' is not a hot hook");
  }
  for (const auto& t : program["timers"].cast<py::list>()) {
    if (p.n_timers == kMaxHotTimers) {
      throw py::value_error("fastmm: more than " + std::to_string(kMaxHotTimers) +
                            " hot timer hooks");
    }
    const auto pair = t.cast<py::tuple>();
    const auto period = pair[1].cast<std::int64_t>();
    if (period <= 0) throw py::value_error("fastmm: a hot timer period must be positive");
    p.timers[p.n_timers].fn = fn_from(pair[0]);
    p.timers[p.n_timers].period = Duration{period};
    ++p.n_timers;
  }
  const auto record = program["record"].cast<std::string>();
  p.record.assign(record.begin(), record.end());
  p.param_bytes = program["param_bytes"].cast<std::size_t>();
  if (program.contains("book_depth")) p.book_depth = program["book_depth"].cast<bool>();
  for (const HotParamField& f : param_fields_from(program))
    p.params.push_back({f.offset, f.raw_offset, f.type});
  p.stop_on_error = stop_on_error;
  return p;
}

}  // namespace fastmm::py_hot
