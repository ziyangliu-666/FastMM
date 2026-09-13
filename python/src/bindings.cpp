// Placeholder pybind11 module; real bindings land in phase E.
#include "fastmm/version.hpp"

#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(_core, m) {
  m.doc() = "FastMM core bindings";
  m.attr("__version__") = fastmm::kVersionString;
  m.def("build_info", [] { return std::string(fastmm::build_info()); });
}
