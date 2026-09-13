#pragma once
// Shared declarations of the fastmm._core translation units (8.5).
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <string>

namespace fastmm::py_bind {

namespace py = pybind11;

void bind_config(py::module_& m);
void bind_backtest(py::module_& m);
void bind_book(py::module_& m);
void bind_strategies(py::module_& m);

// str / int / float / bool (and numpy scalars) -> the exact string form the C++ parameter
// parser accepts. Throws TypeError / ValueError.
std::string param_value_string(py::handle value);
// str or os.PathLike -> filesystem path string. Throws TypeError.
std::string fspath(py::handle path);

}  // namespace fastmm::py_bind
