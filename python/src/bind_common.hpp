#pragma once
// Shared declarations of the fastmm._core translation units (8.5).
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <memory>
#include <string>
#include <vector>

namespace fastmm {
class InstrumentTable;
}  // namespace fastmm
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
void bind_research(py::module_& m);

// The `data=` argument of the entry points that take one without a BacktestConfig behind it: a
// "<source>:<args>" spec, a .fmj / .csv path, or a dict of numpy columns. The buffers a dict
// borrows are pushed onto `keep`, which must outlive the source. Throws TypeError / ValueError,
// and ValueError for None (there is no configuration to fall back on). Defined in
// bind_backtest.cpp, which owns the column parsing.
std::unique_ptr<sim::MdSource> open_md_source(const py::object& data,
                                              const InstrumentTable* instruments,
                                              std::vector<py::object>& keep);

// Backtest of a fastmm.Strategy instance with the GIL held (bind_strategy_api.cpp). `hooks` names
// the hooks the class defines. Returns (BacktestResult, None) or (BacktestResult, (exception, hook,
// now_ns, engine_events)) when a hook raised; the run stopped early then.
py::tuple run_python_strategy(const bt::BacktestConfig& cfg,
                              sim::MdSource* source,
                              const py::object& instance,
                              const std::string& name,
                              const std::vector<std::string>& hooks);

// Backtest of a compiled hot strategy with the GIL released (bind_hot.cpp). `program` holds the
// hook addresses, timers, the initial record, the parameter block size and the parameter fields;
// `metadata` goes into the journal. `slow` is None or a dict with the slow runner and the channel
// settings. Returns (BacktestResult, error, hook calls, slow error, slow failure code, engine
// events); error is None or (status, fail_code, hook, timer, at_ns, engine_events, kill_reason),
// slow error None or the exception a slow method raised, and the run stopped early after either.
py::tuple run_hot_strategy(const bt::BacktestConfig& cfg,
                           sim::MdSource* source,
                           const py::dict& program,
                           const std::string& metadata,
                           const py::object& slow);

// str / int / float / bool (and numpy scalars) -> the exact string form the C++ parameter
// parser accepts. Throws TypeError / ValueError.
std::string param_value_string(py::handle value);
// str or os.PathLike -> filesystem path string. Throws TypeError.
std::string fspath(py::handle path);

}  // namespace fastmm::py_bind
