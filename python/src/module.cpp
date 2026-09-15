// fastmm._core: pybind11 entry point. The bindings are split by area (bind_*.cpp).
#include "bind_common.hpp"

#include "fastmm/backtest/registrations.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/version.hpp"

#include <cstdio>
#include <string>

namespace {

namespace py = pybind11;

// The C++ logger has no sink until someone starts one; without this, records from strategies and
// the engine are dropped silently when running from Python.
std::FILE* g_log_file = nullptr;

fastmm::LogLevel parse_log_level(const std::string& s) {
  if (s == "trace") return fastmm::LogLevel::Trace;
  if (s == "debug") return fastmm::LogLevel::Debug;
  if (s == "info") return fastmm::LogLevel::Info;
  if (s == "warn" || s == "warning") return fastmm::LogLevel::Warn;
  if (s == "error") return fastmm::LogLevel::Error;
  if (s == "off") return fastmm::LogLevel::Off;
  throw py::value_error("unknown log level '" + s + "' (trace|debug|info|warn|error|off)");
}

void stop_logging() {
  fastmm::Logger& lg = fastmm::Logger::instance();
  if (lg.running()) {
    py::gil_scoped_release release;  // stop() drains the rings and joins the sink thread
    lg.stop();
  }
  if (g_log_file != nullptr) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
}

}  // namespace

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
  fb::bind_strategy_api(m);
  fb::bind_hot(m);

  m.def(
      "enable_logging",
      [](const std::string& level, py::object path) {
        const fastmm::LogLevel lvl = parse_log_level(level);
        stop_logging();
        std::FILE* out = stderr;
        if (!path.is_none()) {
          const std::string p = fb::fspath(path);
          out = std::fopen(p.c_str(), "a");
          if (out == nullptr) throw py::value_error("cannot open log file '" + p + "'");
          g_log_file = out;
        }
        fastmm::Logger& lg = fastmm::Logger::instance();
        lg.set_level(lvl);
        lg.start(out, fastmm::LogLevel::Warn);  // warnings and errors are also mirrored to stderr
      },
      py::arg("level") = "warn",
      py::arg("path") = py::none(),
      "Start writing C++ log records at `level` or above to `path` (appending) or to stderr. "
      "Calling it again restarts the logger with the new settings.");
  m.def("disable_logging",
        &stop_logging,
        "Flush pending log records, stop the logger thread and close the log file.");
  // Stop the sink thread before the interpreter finalizes.
  py::module_::import("atexit").attr("register")(py::cpp_function(&stop_logging));
}
