// run_backtest / sweep / BacktestResult / inspect_journal bindings (8.5).
//
// Data input: None (the config's source), "synthetic", a .fmj / .csv path, or a dict of numpy
// columns wrapped by ArraySource without copying. Dtypes must match exactly and arrays must be
// 1-D, C-contiguous, aligned and native byte order; nothing is cast or copied silently.
// Result columns are exposed as read-only numpy views whose base capsule owns a
// shared_ptr<BacktestResult>, so they outlive the Python result object.
#include "bind_common.hpp"

#include "fastmm/backtest/array_source.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/result.hpp"
#include "fastmm/backtest/sweep.hpp"

#include <pybind11/stl.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::py_bind {

namespace {

using bt::BacktestConfig;
using bt::BacktestResult;
using ResultPtr = std::shared_ptr<BacktestResult>;

constexpr double kScale = 1e8;

// ---- result views --------------------------------------------------------------------------

template <class T>
py::array column_view(const ResultPtr& owner, const std::vector<T>& v) {
  auto holder = std::make_unique<ResultPtr>(owner);
  py::capsule base(holder.get(), [](void* p) { delete static_cast<ResultPtr*>(p); });
  static_cast<void>(holder.release());
  py::array_t<T> a(static_cast<py::ssize_t>(v.size()), v.data(), base);
  a.attr("setflags")(py::arg("write") = false);
  return a;
}

py::dict fills_dict(const ResultPtr& r) {
  const bt::FillRows& f = r->fills;
  py::dict d;
  d["ts"] = column_view(r, f.ts);
  d["instrument"] = column_view(r, f.instrument);
  d["side"] = column_view(r, f.side);
  d["price"] = column_view(r, f.price);
  d["qty"] = column_view(r, f.qty);
  d["fee"] = column_view(r, f.fee);
  d["cl_ord_id"] = column_view(r, f.cl_ord_id);
  d["liquidity"] = column_view(r, f.liquidity);
  d["mid"] = column_view(r, f.mid);
  return d;
}

py::dict equity_dict(const ResultPtr& r) {
  const bt::EquityRows& e = r->equity;
  py::dict d;
  d["ts"] = column_view(r, e.ts);
  d["realized"] = column_view(r, e.realized);
  d["unrealized"] = column_view(r, e.unrealized);
  d["fees"] = column_view(r, e.fees);
  d["position"] = column_view(r, e.position);
  d["mid"] = column_view(r, e.mid);
  d["quoted"] = column_view(r, e.quoted);
  return d;
}

py::dict orders_dict(const ResultPtr& r) {
  const bt::OrderRows& o = r->orders;
  py::dict d;
  d["ts"] = column_view(r, o.ts);
  d["venue_ts"] = column_view(r, o.venue_ts);
  d["trigger_ts"] = column_view(r, o.trigger_ts);
  d["cl_ord_id"] = column_view(r, o.cl_ord_id);
  d["instrument"] = column_view(r, o.instrument);
  d["side"] = column_view(r, o.side);
  d["price"] = column_view(r, o.price);
  d["qty"] = column_view(r, o.qty);
  d["kind"] = column_view(r, o.kind);
  d["type"] = column_view(r, o.type);
  return d;
}

py::dict metrics_dict(const bt::Metrics& x) {
  py::dict d;
  d["net_pnl"] = x.net_pnl;
  d["realized_pnl"] = x.realized_pnl;
  d["unrealized_pnl"] = x.unrealized_pnl;
  d["fees"] = x.fees;
  d["final_position"] = x.final_position;
  d["sharpe_bar"] = x.sharpe_bar;
  d["sharpe_annualized"] = x.sharpe_annualized;
  d["max_drawdown"] = x.max_drawdown;
  d["max_drawdown_pct"] = x.max_drawdown_pct;
  d["fills"] = x.fills;
  d["maker_fills"] = x.maker_fills;
  d["taker_fills"] = x.taker_fills;
  d["orders"] = x.orders;
  d["cancels"] = x.cancels;
  d["replaces"] = x.replaces;
  d["rejects"] = x.rejects;
  d["fill_ratio"] = x.fill_ratio;
  d["spread_captured_bps"] = x.spread_captured_bps;
  d["volume_base"] = x.volume_base;
  d["volume_quote"] = x.volume_quote;
  d["inventory_mean"] = x.inventory_mean;
  d["inventory_abs_mean"] = x.inventory_abs_mean;
  d["inventory_max"] = x.inventory_max;
  d["quote_uptime"] = x.quote_uptime;
  d["bars"] = x.bars;
  d["duration_s"] = x.duration_s;
  d["virtual_tick_to_order_p50_ns"] = x.virtual_tick_to_order_p50_ns;
  d["virtual_tick_to_order_p99_ns"] = x.virtual_tick_to_order_p99_ns;
  d["wall_tick_to_order_p50_ns"] = x.wall_tick_to_order_p50_ns;
  d["wall_tick_to_order_p99_ns"] = x.wall_tick_to_order_p99_ns;
  return d;
}

double raw_to_float(std::int64_t raw) noexcept {
  return static_cast<double>(raw) / kScale;
}

py::dict engine_dict(const RunnerStats& s) {
  py::dict d;
  d["events"] = s.events;
  d["book_updates"] = s.book_updates;
  d["orders_sent"] = s.orders_sent;
  d["cancels_sent"] = s.cancels_sent;
  d["replaces_sent"] = s.replaces_sent;
  d["fills"] = s.fills;
  d["risk_rejects"] = s.risk_rejects;
  d["journal_overflows"] = s.journal_overflows;
  d["transport_full"] = s.transport_full;
  d["timers_fired"] = s.timers_fired;
  d["realized_pnl"] = raw_to_float(s.realized_pnl_raw);
  d["unrealized_pnl"] = raw_to_float(s.unrealized_pnl_raw);
  d["fees"] = raw_to_float(s.fees_raw);
  d["tick_to_trade_p50_ns"] = s.tick_to_trade_p50_ns;
  d["tick_to_trade_p99_ns"] = s.tick_to_trade_p99_ns;
  return d;
}

py::dict transport_dict(const sim::SimTransportStats& s) {
  py::dict d;
  d["orders_sent"] = s.orders_sent;
  d["cancels_sent"] = s.cancels_sent;
  d["replaces_sent"] = s.replaces_sent;
  d["dropped"] = s.dropped;
  d["scheduler_full"] = s.scheduler_full;
  d["wire_full"] = s.wire_full;
  d["acks"] = s.acks;
  d["rejects"] = s.rejects;
  d["rejects_post_only"] = s.rejects_post_only;
  d["rejects_level_full"] = s.rejects_level_full;
  d["rejects_invalid"] = s.rejects_invalid;
  d["rejects_duplicate"] = s.rejects_duplicate;
  d["rejects_other"] = s.rejects_other;
  d["fills"] = s.fills;
  d["cancel_acks"] = s.cancel_acks;
  d["cancel_rejects"] = s.cancel_rejects;
  d["expired"] = s.expired;
  d["md_forwarded"] = s.md_forwarded;
  d["md_delivered"] = s.md_delivered;
  d["order_events_delivered"] = s.order_events_delivered;
  d["fees_charged"] = raw_to_float(s.fees_charged.raw);
  return d;
}

py::dict string_map(const ParamMap& m) {
  py::dict d;
  for (const auto& [k, v] : m) d[py::str(k)] = py::str(v);
  return d;
}

// ---- data input ----------------------------------------------------------------------------

struct DataSpec {
  enum class Kind : std::uint8_t { Config, Path, Arrays };
  Kind kind = Kind::Config;
  std::string path;              // Kind::Path: "synthetic", *.fmj or *.csv
  std::vector<py::object> keep;  // Kind::Arrays: owners of the column buffers
  bt::ArrayColumns cols;
};

std::string dtype_name(const py::array& a) {
  return py::str(a.dtype()).cast<std::string>();
}

bool is_dtype(const py::array& a, const py::dtype& want) {
  return a.dtype().equal(want);
}

// Validates everything but the dtype: ndim, layout, alignment, byte order.
py::array checked_array(const py::dict& d, const char* key) {
  py::object v = d[key];
  if (!py::isinstance<py::array>(v)) {
    throw py::type_error("data['" + std::string(key) + "'] must be a numpy.ndarray, got " +
                         py::str(py::type::handle_of(v).attr("__name__")).cast<std::string>());
  }
  auto a = py::reinterpret_borrow<py::array>(v);
  if (a.ndim() != 1) {
    throw py::value_error("data['" + std::string(key) + "'] must be 1-D, got ndim " +
                          std::to_string(a.ndim()));
  }
  const int flags = a.flags();
  if ((flags & py::array::c_style) == 0) {
    throw py::value_error("data['" + std::string(key) +
                          "'] must be C-contiguous (no strided views); pass "
                          "numpy.ascontiguousarray(...) to copy explicitly");
  }
  if ((flags & py::detail::npy_api::NPY_ARRAY_ALIGNED_) == 0) {
    throw py::value_error("data['" + std::string(key) + "'] is not aligned; copy it explicitly");
  }
  return a;
}

template <class T>
std::span<const T> as_span(const py::array& a) {
  return {static_cast<const T*>(a.data()), static_cast<std::size_t>(a.size())};
}

template <class T>
std::span<const T> exact_column(const py::dict& d, const char* key, DataSpec& spec) {
  py::array a = checked_array(d, key);
  if (!is_dtype(a, py::dtype::of<T>())) {
    throw py::type_error("data['" + std::string(key) + "'] must have dtype " +
                         py::str(py::dtype::of<T>()).cast<std::string>() + ", got " +
                         dtype_name(a) + " (no implicit casts; use .astype() explicitly)");
  }
  spec.keep.push_back(a);
  return as_span<T>(a);
}

// price / qty: int64 raw 1e-8 or float64.
void fixed_column(const py::dict& d,
                  const char* key,
                  DataSpec& spec,
                  std::span<const std::int64_t>& raw,
                  std::span<const double>& f64) {
  py::array a = checked_array(d, key);
  if (is_dtype(a, py::dtype::of<std::int64_t>())) {
    raw = as_span<std::int64_t>(a);
  } else if (is_dtype(a, py::dtype::of<double>())) {
    f64 = as_span<double>(a);
  } else {
    throw py::type_error("data['" + std::string(key) +
                         "'] must have dtype int64 (raw 1e-8 fixed point) or float64, got " +
                         dtype_name(a) + " (no implicit casts; use .astype() explicitly)");
  }
  spec.keep.push_back(a);
}

DataSpec parse_data(const py::object& data) {
  DataSpec spec;
  if (data.is_none()) return spec;
  if (py::isinstance<py::dict>(data)) {
    auto d = py::reinterpret_borrow<py::dict>(data);
    static constexpr const char* kKeys[] = {"ts", "type", "inst", "side", "price", "qty", "seq"};
    for (const auto& item : d) {
      const std::string k = py::str(item.first).cast<std::string>();
      bool known = false;
      for (const char* want : kKeys) known = known || k == want;
      if (!known) {
        throw py::value_error("data: unknown column '" + k +
                              "' (expected ts, type, inst, side, price, qty and optional seq)");
      }
    }
    for (const char* req : {"ts", "type", "inst", "side", "price", "qty"}) {
      if (!d.contains(req))
        throw py::value_error("data: missing required column '" + std::string(req) + "'");
    }
    spec.kind = DataSpec::Kind::Arrays;
    spec.cols.ts = exact_column<std::int64_t>(d, "ts", spec);
    spec.cols.type = exact_column<std::uint8_t>(d, "type", spec);
    spec.cols.inst = exact_column<std::uint32_t>(d, "inst", spec);
    spec.cols.side = exact_column<std::int8_t>(d, "side", spec);
    fixed_column(d, "price", spec, spec.cols.price_i64, spec.cols.price_f64);
    fixed_column(d, "qty", spec, spec.cols.qty_i64, spec.cols.qty_f64);
    if (d.contains("seq")) spec.cols.seq = exact_column<std::uint64_t>(d, "seq", spec);
    if (const std::string err = spec.cols.validate(); !err.empty())
      throw py::value_error("data: " + err);
    return spec;
  }
  if (py::isinstance<py::str>(data) || py::hasattr(data, "__fspath__")) {
    spec.kind = DataSpec::Kind::Path;
    spec.path = fspath(data);
    return spec;
  }
  throw py::type_error(
      "data must be None, 'synthetic', a .fmj/.csv path or a dict of numpy arrays");
}

// Runs without the GIL: touches only C++ state and the (kept-alive) column buffers.
std::unique_ptr<bt::MdSource> open_spec(const DataSpec& spec, const BacktestConfig& cfg) {
  switch (spec.kind) {
    case DataSpec::Kind::Config:
      return bt::open_source(cfg);
    case DataSpec::Kind::Path:
      return bt::open_data(spec.path);
    case DataSpec::Kind::Arrays:
      return std::make_unique<bt::ArraySource>(spec.cols);
  }
  return nullptr;
}

std::string resolve_strategy(const BacktestConfig& cfg, const std::optional<std::string>& s) {
  std::string name = s ? *s : cfg.strategy;
  if (name.empty()) {
    throw py::value_error(
        "no strategy: set config.strategy or pass strategy=... (see fastmm.strategies())");
  }
  return name;
}

}  // namespace

void bind_backtest(py::module_& m) {
  py::class_<BacktestResult, std::shared_ptr<BacktestResult>>(
      m,
      "BacktestResult",
      "Outcome of one backtest. fills / equity / orders are dicts of read-only numpy views "
      "over the C++ vectors (prices, quantities, fees and PnL are raw int64 with a 1e-8 "
      "scale); to_pandas() converts them.")
      .def_readonly("strategy", &BacktestResult::strategy)
      .def_readonly("seed", &BacktestResult::seed)
      .def_property_readonly(
          "params",
          [](const BacktestResult& r) { return string_map(r.params); },
          "Strategy parameters of the run as {name: str}.")
      .def_readonly("outbound_sha256",
                    &BacktestResult::outbound_sha256,
                    "SHA-256 of every message the engine sent (determinism fingerprint).")
      .def_readonly("outbound_messages", &BacktestResult::outbound_messages)
      .def_readonly(
          "md_events", &BacktestResult::md_events, "Market-data messages delivered to the engine.")
      .def_readonly("engine_steps", &BacktestResult::engine_steps)
      .def_readonly("start_ts", &BacktestResult::start_ts, "First event time, ns.")
      .def_readonly("end_ts", &BacktestResult::end_ts, "Last event time, ns.")
      .def_readonly("wall_seconds", &BacktestResult::wall_seconds)
      .def_property_readonly("fills", &fills_dict, "Fill columns (zero-copy numpy views).")
      .def_property_readonly("equity", &equity_dict, "Equity bar columns (zero-copy numpy views).")
      .def_property_readonly("orders", &orders_dict, "Order columns (zero-copy numpy views).")
      .def(
          "stats",
          [](const BacktestResult& r) { return metrics_dict(r.metrics); },
          "Every summary metric: PnL / volume / inventory in quote or base units (float), "
          "counts and latency percentiles (int, ns).")
      .def(
          "engine_stats",
          [](const BacktestResult& r) { return engine_dict(r.engine); },
          "The engine's own counters (PnL converted to float).")
      .def(
          "transport_stats",
          [](const BacktestResult& r) { return transport_dict(r.transport); },
          "Simulated venue / transport counters.")
      .def("summary_table", &BacktestResult::summary_table, "Human-readable metrics table.")
      .def("summary_json", &BacktestResult::summary_json, "Metrics as a JSON document.")
      .def(
          "write_all",
          [](const BacktestResult& r, const py::object& dir) {
            const std::string d = fspath(dir);
            bool ok = false;
            {
              py::gil_scoped_release release;
              ok = r.write_all(d);
            }
            if (!ok) throw py::value_error("write_all: cannot write results to '" + d + "'");
          },
          py::arg("dir"),
          "Write equity.csv, fills.csv, orders.csv and summary.json into dir (created).")
      .def(
          "to_pandas",
          [](const py::object& self) {
            return py::module_::import("fastmm.results").attr("to_pandas")(self);
          },
          "fills / equity / orders as pandas DataFrames with float prices and datetime64 "
          "timestamps (see fastmm.results.to_pandas).")
      .def("__repr__", [](const BacktestResult& r) {
        return "<BacktestResult strategy='" + r.strategy +
               "' fills=" + std::to_string(r.fills.size()) +
               " orders=" + std::to_string(r.orders.size()) +
               " net_pnl=" + std::to_string(r.metrics.net_pnl) +
               " sha256=" + r.outbound_sha256.substr(0, 12) + ">";
      });

  m.def(
      "run_backtest",
      [](const BacktestConfig& config,
         const py::object& data,
         const std::optional<std::string>& strategy) {
        DataSpec spec = parse_data(data);
        const BacktestConfig cfg = config;  // Python may mutate `config` while we run
        const std::string name = resolve_strategy(cfg, strategy);
        ResultPtr out;
        {
          py::gil_scoped_release release;
          std::unique_ptr<bt::MdSource> source = open_spec(spec, cfg);
          out = std::make_shared<BacktestResult>(bt::run_backtest(cfg, name, source.get()));
        }
        return out;
      },
      py::arg("config"),
      py::arg("data") = py::none(),
      py::arg("strategy") = py::none(),
      "Run one backtest with the GIL released.\n\n"
      "data: None (config.source / config.path), 'synthetic', a .fmj or .csv path, or a dict "
      "of numpy arrays {ts: int64, type: uint8, inst: uint32, side: int8, price: int64 (raw "
      "1e-8) | float64, qty: int64 | float64, seq: uint64 (optional)} used without copying.\n"
      "strategy: registry name; defaults to config.strategy.");

  m.def(
      "_run_strategy",
      [](const BacktestConfig& config,
         const py::object& data,
         const py::object& instance,
         const std::string& name,
         const std::vector<std::string>& hooks,
         const py::dict& params) {
        DataSpec spec = parse_data(data);
        BacktestConfig cfg = config;
        cfg.strategy = name;  // journal header (truncated to its field) and result name
        ParamMap effective;
        for (const auto& [k, v] : params)
          effective[py::str(k).cast<std::string>()] = py::str(v).cast<std::string>();
        cfg.params = std::move(effective);
        std::unique_ptr<bt::MdSource> source;
        {
          py::gil_scoped_release release;
          source = open_spec(spec, cfg);
        }
        return run_python_strategy(cfg, source.get(), instance, name, hooks);
      },
      py::arg("config"),
      py::arg("data"),
      py::arg("instance"),
      py::arg("name"),
      py::arg("hooks"),
      py::arg("params"),
      "Internal: backtest of a fastmm.Strategy instance with the GIL held; use "
      "fastmm.run_backtest(config, data, strategy=MyStrategy).");

  m.def(
      "_run_hot_strategy",
      [](const BacktestConfig& config,
         const py::object& data,
         const std::string& name,
         const py::dict& params,
         const py::dict& program) {
        DataSpec spec = parse_data(data);
        BacktestConfig cfg = config;
        cfg.strategy = name;
        ParamMap effective;
        for (const auto& [k, v] : params)
          effective[py::str(k).cast<std::string>()] = py::str(v).cast<std::string>();
        cfg.params = std::move(effective);
        std::unique_ptr<bt::MdSource> source;
        {
          py::gil_scoped_release release;
          source = open_spec(spec, cfg);
        }
        return run_hot_strategy(cfg, source.get(), program);
      },
      py::arg("config"),
      py::arg("data"),
      py::arg("name"),
      py::arg("params"),
      py::arg("program"),
      "Internal: backtest of a compiled hot strategy with the GIL released; use "
      "fastmm.run_backtest(config, data, strategy=MyStrategy).");

  m.def(
      "sweep",
      [](const BacktestConfig& config,
         const py::dict& grid,
         const py::object& data,
         const std::optional<std::string>& strategy,
         int threads) {
        bt::ParamGrid g;
        // Original Python values per (key, string form), so the returned dicts keep the types
        // the caller used (floats stay floats).
        std::vector<std::map<std::string, py::object>> originals;
        for (const auto& [k, values] : grid) {
          const std::string key = py::str(k).cast<std::string>();
          if (py::isinstance<py::str>(values) || !py::isinstance<py::iterable>(values)) {
            throw py::type_error("grid['" + key + "'] must be a list of values");
          }
          std::vector<std::string> strs;
          std::map<std::string, py::object> orig;
          for (const auto& v : values) {
            std::string s = param_value_string(v);
            orig.emplace(s, py::reinterpret_borrow<py::object>(v));
            strs.push_back(std::move(s));
          }
          if (strs.empty()) throw py::value_error("grid['" + key + "'] is empty");
          g.emplace_back(key, std::move(strs));
          originals.push_back(std::move(orig));
        }
        DataSpec spec = parse_data(data);
        const BacktestConfig cfg = config;
        const std::string name = resolve_strategy(cfg, strategy);

        std::vector<bt::SweepPoint> points;
        {
          py::gil_scoped_release release;
          // Open once up front: unreadable files raise here, before any worker starts (a
          // throwing factory would otherwise take down a worker thread).
          const bool has_source = open_spec(spec, cfg) != nullptr;
          std::mutex mu;
          std::exception_ptr factory_error;
          bt::SourceFactory factory;
          if (has_source) {
            factory = [&]() -> std::unique_ptr<bt::MdSource> {
              try {
                return open_spec(spec, cfg);
              } catch (...) {
                const std::lock_guard<std::mutex> lock(mu);
                if (!factory_error) factory_error = std::current_exception();
                return nullptr;
              }
            };
          }
          points = bt::sweep_by_name(cfg, name, g, factory, threads);
          if (factory_error) std::rethrow_exception(factory_error);
        }

        py::list out;
        for (bt::SweepPoint& p : points) {
          py::dict params;
          for (std::size_t i = 0; i < g.size(); ++i) {
            const auto it = p.params.find(g[i].first);
            if (it == p.params.end()) continue;
            params[py::str(g[i].first)] = originals[i].at(it->second);
          }
          out.append(py::make_tuple(params, std::make_shared<BacktestResult>(std::move(p.result))));
        }
        return out;
      },
      py::arg("config"),
      py::arg("grid"),
      py::arg("data") = py::none(),
      py::arg("strategy") = py::none(),
      py::arg("threads") = 0,
      "Cartesian parameter sweep on a thread pool (GIL released). grid: {param: [values]}. "
      "Returns [(params, BacktestResult)] in grid order, first parameter varying slowest. "
      "Every worker opens its own cursor over `data` (same forms as run_backtest). "
      "threads <= 0 uses every hardware thread.");

  m.def(
      "inspect_journal",
      [](const py::object& path) {
        const std::string p = fspath(path);
        bt::JournalInfo info;
        {
          py::gil_scoped_release release;
          info = bt::inspect_journal(p);
        }
        py::dict d;
        d["strategy"] = info.strategy;
        d["rng_seed"] = info.rng_seed;
        d["config_hash"] = info.config_hash;
        d["start_ts"] = info.start_ts;
        d["messages"] = info.messages;
        d["outbound_messages"] = info.outbound_messages;
        d["market_data_messages"] = info.market_data_messages;
        py::dict meta;  // key=value lines
        std::string_view text = info.strategy_meta;
        while (!text.empty()) {
          const std::size_t nl = text.find('\n');
          const std::string_view line = text.substr(0, nl);
          text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
          const std::size_t eq = line.find('=');
          if (eq != std::string_view::npos)
            meta[py::str(std::string(line.substr(0, eq)))] = std::string(line.substr(eq + 1));
        }
        d["strategy_meta"] = meta;
        return d;
      },
      py::arg("path"),
      "Header and message counts of an .fmj journal.");
}

}  // namespace fastmm::py_bind
