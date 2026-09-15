#pragma once
// Shared declarations of the fastmm._core translation units (8.5).
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <string>
#include <vector>

namespace fastmm::bt {
struct BacktestConfig;
}  // namespace fastmm::bt
namespace fastmm::sim {
class MdSource;
}  // namespace fastmm::sim

namespace fastmm::py_bind {

namespace py = pybind11;

void bind_config(py::module_& m);
void bind_backtest(py::module_& m);
void bind_book(py::module_& m);
void bind_strategies(py::module_& m);
void bind_strategy_api(py::module_& m);
void bind_hot(py::module_& m);

// Backtest of a fastmm.Strategy instance with the GIL held (bind_strategy_api.cpp). `hooks` names
// the hooks the class defines. Returns (BacktestResult, None) or (BacktestResult, (exception, hook,
// now_ns, engine_events)) when a hook raised; the run stopped early then.
py::tuple run_python_strategy(const bt::BacktestConfig& cfg,
                              sim::MdSource* source,
                              const py::object& instance,
                              const std::string& name,
                              const std::vector<std::string>& hooks);

// Backtest of a compiled hot strategy with the GIL released (bind_hot.cpp). `program` holds the
// hook addresses, timers, the initial record and the parameter block size. Returns (BacktestResult,
// error, hook calls); error is None or (status, fail_code, hook, timer, at_ns, engine_events,
// kill_reason) and the run stopped early then.
py::tuple run_hot_strategy(const bt::BacktestConfig& cfg,
                           sim::MdSource* source,
                           const py::dict& program);

// str / int / float / bool (and numpy scalars) -> the exact string form the C++ parameter
// parser accepts. Throws TypeError / ValueError.
std::string param_value_string(py::handle value);
// str or os.PathLike -> filesystem path string. Throws TypeError.
std::string fspath(py::handle path);

}  // namespace fastmm::py_bind
