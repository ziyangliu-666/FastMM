// BacktestConfig bindings: TOML loading, hand-built single-instrument configs and the
// commonly tuned fields (seed, horizon, fill model, latency, fees, synthetic market).
#include "bind_common.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/fill_model.hpp"
#include "fastmm/config/config.hpp"

#include <pybind11/stl.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace fastmm::py_bind {

namespace {

using bt::BacktestConfig;

constexpr double kNsPerSecond = 1e9;

Duration seconds_from_float(double s, const char* what) {
  if (!std::isfinite(s) || s <= 0.0 || s > 1e9)
    throw py::value_error(std::string(what) + " must be a positive number of seconds");
  return nanoseconds(std::llround(s * kNsPerSecond));
}

std::int64_t micros_from_int(std::int64_t us, const char* what) {
  if (us < 0) throw py::value_error(std::string(what) + " must be >= 0");
  return us;
}

double to_seconds(Duration d) noexcept {
  return static_cast<double>(d.ns) / kNsPerSecond;
}

std::int64_t cbps_from_bps(double bps) {
  if (!std::isfinite(bps) || std::fabs(bps) > 10'000.0)
    throw py::value_error("fee must be a finite number of basis points in [-10000, 10000]");
  return std::llround(bps * 100.0);
}

Price price_from_py(py::handle v, const char* what) {
  if (py::isinstance<py::str>(v)) {
    const auto s = v.cast<std::string>();
    const auto p = Price::from_decimal(s);
    if (!p || !p->is_positive())
      throw py::value_error(std::string(what) + ": '" + s + "' is not a positive decimal");
    return *p;
  }
  const double d = py::float_(py::reinterpret_borrow<py::object>(v)).cast<double>();
  if (!std::isfinite(d) || d <= 0.0 || d > 9.2e10)
    throw py::value_error(std::string(what) + " must be a positive finite price");
  return Price::from_double(d);
}

py::dict params_dict(const ParamMap& params) {
  py::dict out;
  for (const auto& [k, v] : params) out[py::str(k)] = py::str(v);
  return out;
}

std::string format_price(Price p) {
  char buf[kMaxDecimalChars];
  return {buf, p.to_decimal(buf)};
}

}  // namespace

std::string fspath(py::handle path) {
  if (py::isinstance<py::str>(path)) return path.cast<std::string>();
  py::object s = py::module_::import("os").attr("fspath")(path);
  if (!py::isinstance<py::str>(s)) throw py::type_error("path must be str or os.PathLike[str]");
  return s.cast<std::string>();
}

std::string param_value_string(py::handle value) {
  py::module_ np = py::module_::import("numpy");
  if (PyBool_Check(value.ptr()) != 0 || py::isinstance(value, np.attr("bool_")))
    return py::cast<bool>(py::bool_(py::reinterpret_borrow<py::object>(value))) ? "true" : "false";
  if (py::isinstance<py::str>(value)) return value.cast<std::string>();
  if (PyFloat_Check(value.ptr()) == 0 && PyIndex_Check(value.ptr()) != 0)
    return py::str(py::int_(py::reinterpret_borrow<py::object>(value))).cast<std::string>();
  if (PyFloat_Check(value.ptr()) != 0 || PyNumber_Check(value.ptr()) != 0) {
    py::float_ f(py::reinterpret_borrow<py::object>(value));
    if (!std::isfinite(f.cast<double>())) throw py::value_error("parameter values must be finite");
    return py::repr(f).cast<std::string>();  // shortest round-trip form, e.g. "0.01"
  }
  throw py::type_error("parameter values must be str, int, float or bool, got " +
                       std::string(py::str(py::type::handle_of(value).attr("__name__"))));
}

void bind_config(py::module_& m) {
  py::register_exception<ConfigError>(m, "ConfigError", PyExc_ValueError);

  py::class_<BacktestConfig>(m,
                             "BacktestConfig",
                             "Everything one backtest needs: engine, instruments, strategy and "
                             "parameters, simulated venue (fill model, latency, fees) and the "
                             "synthetic market. Build one with from_toml() or "
                             "single_instrument().")
      .def_static(
          "from_toml",
          [](const py::object& path) {
            const std::string p = fspath(path);
            py::gil_scoped_release release;
            return BacktestConfig::from_config(Config::load(p));
          },
          py::arg("path"),
          "Load a FastMM TOML config (sections [engine] [[instruments]] [strategy] [risk] "
          "[venues.<x>.fees] [sim] [backtest]). Raises ConfigError.")
      .def_static(
          "from_toml_string",
          [](const std::string& text) { return BacktestConfig::from_config(Config::parse(text)); },
          py::arg("text"),
          "Parse a TOML document held in a string. Raises ConfigError.")
      .def_static(
          "single_instrument",
          [](const std::string& symbol, const std::string& tick, const std::string& lot) {
            const auto t = Price::from_decimal(tick);
            const auto l = Qty::from_decimal(lot);
            if (!t || !t->is_positive())
              throw py::value_error("tick: '" + tick + "' is not a positive decimal");
            if (!l || !l->is_positive())
              throw py::value_error("lot: '" + lot + "' is not a positive decimal");
            return BacktestConfig::single_instrument(symbol, *t, *l);
          },
          py::arg("symbol"),
          py::arg("tick"),
          py::arg("lot"),
          "One instrument on venue 0 with exact decimal tick/lot (e.g. '0.01', '0.00001'); "
          "synthetic market, matching fill model, no strategy selected.")
      .def("copy", [](const BacktestConfig& c) { return BacktestConfig(c); })
      .def("__copy__", [](const BacktestConfig& c) { return BacktestConfig(c); })
      .def(
          "__deepcopy__",
          [](const BacktestConfig& c, const py::object&) { return BacktestConfig(c); },
          py::arg("memo"))
      // ---- strategy ------------------------------------------------------------------------
      .def_readwrite("strategy", &BacktestConfig::strategy, "Registered strategy name.")
      .def_property(
          "params",
          [](const BacktestConfig& c) { return params_dict(c.params); },
          [](BacktestConfig& c, const py::dict& d) {
            ParamMap next;
            for (const auto& [k, v] : d)
              next[py::str(k).cast<std::string>()] = param_value_string(v);
            c.params = std::move(next);
          },
          "Strategy parameters as {name: str} (a copy; assign a dict or use set_param()).")
      .def(
          "set_param",
          [](BacktestConfig& c, const std::string& key, const py::handle& value) {
            c.set_param(key, param_value_string(value));
          },
          py::arg("key"),
          py::arg("value"),
          "Set one strategy parameter (str / int / float / bool).")
      .def(
          "clear_params",
          [](BacktestConfig& c) { c.params.clear(); },
          "Remove every strategy parameter (use before switching strategy).")
      // ---- run control -----------------------------------------------------------------------
      .def_property(
          "seed",
          [](const BacktestConfig& c) { return c.seed; },
          [](BacktestConfig& c, std::uint64_t s) { c.set_seed(s); },
          "Seed of the synthetic market and the latency model.")
      .def_property(
          "engine_seed",
          [](const BacktestConfig& c) { return c.engine.rng_seed; },
          [](BacktestConfig& c, std::uint64_t s) { c.engine.rng_seed = s; },
          "Engine RNG seed ([engine] rng_seed).")
      .def_property(
          "duration_s",
          [](const BacktestConfig& c) { return to_seconds(c.duration); },
          [](BacktestConfig& c, double s) { c.duration = seconds_from_float(s, "duration_s"); },
          "Synthetic horizon in seconds (data files run to their end).")
      .def_property(
          "start_ns",
          [](const BacktestConfig& c) { return c.start.ns; },
          [](BacktestConfig& c, std::int64_t ns) {
            if (ns <= 0) throw py::value_error("start_ns must be > 0");
            c.start = Timestamp{ns};
          },
          "Synthetic market start time (ns since the epoch).")
      .def_property(
          "equity_bar_s",
          [](const BacktestConfig& c) { return to_seconds(c.equity_bar); },
          [](BacktestConfig& c, double s) { c.equity_bar = seconds_from_float(s, "equity_bar_s"); },
          "Equity bar length in seconds.")
      .def_readwrite("initial_capital",
                     &BacktestConfig::initial_capital,
                     "Quote currency; only used for max_drawdown_pct.")
      .def_readwrite("measure_wall_clock",
                     &BacktestConfig::measure_wall_clock,
                     "Measure wall-clock tick-to-order per engine step.")
      .def_readwrite("source",
                     &BacktestConfig::source,
                     "'synthetic' | 'journal' | 'csv' | '' (used when run_backtest(data=None)).")
      .def_readwrite("path", &BacktestConfig::path, "Data file for source journal/csv.")
      .def_readwrite("output_dir",
                     &BacktestConfig::output_dir,
                     "Where the apps write results; run_backtest() never writes (see "
                     "BacktestResult.write_all).")
      .def_readwrite("journal_out",
                     &BacktestConfig::journal_out,
                     "Record the session to this .fmj ('' = no journal).")
      // ---- simulated venue -------------------------------------------------------------------
      .def_property(
          "fill_model",
          [](const BacktestConfig& c) { return std::string(to_string(c.transport.fill_model)); },
          [](BacktestConfig& c, const std::string& s) {
            const auto fm = bt::parse_fill_model(s);
            if (!fm) throw py::value_error("fill_model must be 'matching' or 'l2_queue'");
            c.transport.fill_model = *fm;
          },
          "'matching' (orders rest in the simulated book) or 'l2_queue' (queue position on "
          "L2 data).")
      .def_property(
          "queue_conservatism",
          [](const BacktestConfig& c) {
            return static_cast<double>(c.transport.queue_conservatism_bps) / 10'000.0;
          },
          [](BacktestConfig& c, double v) {
            if (!(v >= 0.0 && v <= 1.0))
              throw py::value_error("queue_conservatism must be in [0, 1]");
            c.transport.queue_conservatism_bps = std::llround(v * 10'000.0);
          },
          "L2 queue model: 0 = cancels ahead always help, 1 = never.")
      .def_property(
          "latency_fixed_us",
          [](const BacktestConfig& c) { return c.transport.order_out.fixed.micros(); },
          [](BacktestConfig& c, std::int64_t us) {
            const Duration d = microseconds(micros_from_int(us, "latency_fixed_us"));
            c.transport.order_out.fixed = d;
            c.transport.ack_in.fixed = d;
          },
          "Fixed order and ack latency, microseconds.")
      .def_property(
          "latency_jitter_us",
          [](const BacktestConfig& c) { return c.transport.order_out.jitter.micros(); },
          [](BacktestConfig& c, std::int64_t us) {
            const Duration d = microseconds(micros_from_int(us, "latency_jitter_us"));
            c.transport.order_out.jitter = d;
            c.transport.ack_in.jitter = d;
          },
          "Mean lognormal order and ack latency jitter, microseconds.")
      .def_property(
          "latency_md_us",
          [](const BacktestConfig& c) { return c.transport.md_in.fixed.micros(); },
          [](BacktestConfig& c, std::int64_t us) {
            c.transport.md_in.fixed = microseconds(micros_from_int(us, "latency_md_us"));
          },
          "Fixed market-data latency, microseconds.")
      .def_property(
          "latency_md_jitter_us",
          [](const BacktestConfig& c) { return c.transport.md_in.jitter.micros(); },
          [](BacktestConfig& c, std::int64_t us) {
            c.transport.md_in.jitter = microseconds(micros_from_int(us, "latency_md_jitter_us"));
          },
          "Mean market-data latency jitter, microseconds.")
      .def_property(
          "p_drop",
          [](const BacktestConfig& c) { return c.transport.order_out.p_drop; },
          [](BacktestConfig& c, double p) {
            if (!(p >= 0.0 && p < 1.0)) throw py::value_error("p_drop must be in [0, 1)");
            c.transport.order_out.p_drop = p;
          },
          "Probability an outbound order message is lost.")
      .def_property(
          "maker_fee_bps",
          [](const BacktestConfig& c) {
            return static_cast<double>(c.transport.fees.maker_cbps) / 100.0;
          },
          [](BacktestConfig& c, double bps) { c.transport.fees.maker_cbps = cbps_from_bps(bps); },
          "Maker fee in bps (negative = rebate), 0.01 bps resolution.")
      .def_property(
          "taker_fee_bps",
          [](const BacktestConfig& c) {
            return static_cast<double>(c.transport.fees.taker_cbps) / 100.0;
          },
          [](BacktestConfig& c, double bps) { c.transport.fees.taker_cbps = cbps_from_bps(bps); },
          "Taker fee in bps, 0.01 bps resolution.")
      .def_property(
          "supports_replace",
          [](const BacktestConfig& c) { return c.transport.supports_replace; },
          [](BacktestConfig& c, bool v) {
            c.transport.supports_replace = v;
            c.engine.quotes.supports_replace = v;
          },
          "Venue and engine use native replace instead of cancel-then-new.")
      // ---- synthetic market ------------------------------------------------------------------
      .def_property(
          "start_mid",
          [](const BacktestConfig& c) { return format_price(c.generator.start_mid); },
          [](BacktestConfig& c, const py::handle& v) {
            c.generator.start_mid = price_from_py(v, "start_mid");
          },
          "Synthetic market initial mid (exact decimal string; float accepted).")
      .def_property(
          "limit_rate_per_s",
          [](const BacktestConfig& c) { return c.generator.limit_rate_per_s; },
          [](BacktestConfig& c, double v) { c.generator.limit_rate_per_s = v; },
          "Synthetic limit order arrivals per second.")
      .def_property(
          "market_rate_per_s",
          [](const BacktestConfig& c) { return c.generator.market_rate_per_s; },
          [](BacktestConfig& c, double v) { c.generator.market_rate_per_s = v; },
          "Synthetic market order arrivals per second.")
      .def_property(
          "mid_step_rate_per_s",
          [](const BacktestConfig& c) { return c.generator.mid_step_rate_per_s; },
          [](BacktestConfig& c, double v) { c.generator.mid_step_rate_per_s = v; },
          "Synthetic latent mid +-1 tick steps per second.")
      .def_property(
          "cancel_rate_per_order_s",
          [](const BacktestConfig& c) { return c.generator.cancel_rate_per_order_s; },
          [](BacktestConfig& c, double v) { c.generator.cancel_rate_per_order_s = v; },
          "Synthetic per-order cancel hazard per second.")
      .def("__repr__", [](const BacktestConfig& c) {
        return "<BacktestConfig strategy='" + c.strategy + "' seed=" + std::to_string(c.seed) +
               " duration_s=" + std::to_string(to_seconds(c.duration)) + " fill_model='" +
               std::string(to_string(c.transport.fill_model)) +
               "' params=" + std::to_string(c.params.size()) + ">";
      });
}

}  // namespace fastmm::py_bind
