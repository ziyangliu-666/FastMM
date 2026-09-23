// features() / evaluate_signal() bindings (include/fastmm/research).
//
// Every column of a FeatureTable is a read-only numpy view whose base capsule owns a
// shared_ptr<FeatureTable>, so the arrays outlive the Python table object, exactly as the
// backtest result columns do. A caller's signal array is borrowed the same way in the other
// direction: it must be 1-D, C-contiguous, aligned float64, and nothing is cast or copied.
#include "bind_common.hpp"

#include "fastmm/research/extractor.hpp"
#include "fastmm/research/signal_eval.hpp"

#include <pybind11/stl.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::py_bind {

namespace rs = fastmm::research;

namespace {

using TablePtr = std::shared_ptr<rs::FeatureTable>;

template <class T>
py::array column_view(const TablePtr& owner, const std::vector<T>& v) {
  auto holder = std::make_unique<TablePtr>(owner);
  py::capsule base(holder.get(), [](void* p) { delete static_cast<TablePtr*>(p); });
  static_cast<void>(holder.release());
  py::array_t<T> a(static_cast<py::ssize_t>(v.size()), v.data(), base);
  a.attr("setflags")(py::arg("write") = false);
  return a;
}

py::dict columns_dict(const TablePtr& t) {
  const rs::FeatureRows& r = t->rows;
  py::dict d;
  d["ts"] = column_view(t, r.ts);
  d["inst"] = column_view(t, r.instrument);
  d["mid"] = column_view(t, r.mid);
  d["microprice"] = column_view(t, r.microprice);
  d["best_bid"] = column_view(t, r.best_bid);
  d["best_ask"] = column_view(t, r.best_ask);
  d["bid_qty"] = column_view(t, r.bid_qty);
  d["ask_qty"] = column_view(t, r.ask_qty);
  d["imbalance"] = column_view(t, r.imbalance);
  d["spread"] = column_view(t, r.spread);
  return d;
}

py::dict forward_mid_dict(const TablePtr& t) {
  py::dict d;
  for (std::size_t j = 0; j < t->coverage.size() && j < t->rows.forward_mid.size(); ++j)
    d[py::str(t->coverage[j].label())] = column_view(t, t->rows.forward_mid[j]);
  return d;
}

py::list coverage_list(const rs::FeatureTable& t) {
  py::list out;
  for (const rs::HorizonCoverage& c : t.coverage) {
    py::dict d;
    d["horizon_ns"] = c.horizon_ns;
    d["label"] = c.label();
    d["resolved"] = c.resolved;
    d["excluded_past_end"] = c.excluded_past_end;
    d["excluded_no_mid"] = c.excluded_no_mid;
    out.append(d);
  }
  return out;
}

std::vector<Duration> horizons_from(const py::object& h) {
  std::vector<Duration> out;
  if (h.is_none()) return out;
  for (const py::handle item : h) {
    const double s = item.cast<double>();
    if (!std::isfinite(s) || s <= 0.0)
      throw py::value_error("horizons: every horizon must be a positive number of seconds");
    out.push_back(Duration{static_cast<std::int64_t>(s * 1e9)});
  }
  return out;
}

Duration seconds_arg(double s, const char* name) {
  if (!std::isfinite(s) || s < 0.0)
    throw py::value_error(std::string(name) + " must be a non-negative number of seconds");
  return Duration{static_cast<std::int64_t>(s * 1e9)};
}

py::dict bucket_dict(const rs::SignalBucket& b) {
  py::dict d;
  d["lo"] = b.lo;
  d["hi"] = b.hi;
  d["n"] = b.n;
  d["mean_signal"] = b.mean_signal;
  d["forward_bps"] = b.forward_bps;
  d["buy_bps"] = b.buy_bps;
  d["sell_bps"] = b.sell_bps;
  d["half_spread_bps"] = b.half_spread_bps;
  return d;
}

py::dict eval_dict(const rs::SignalEval& e) {
  py::dict out;
  out["signal"] = e.signal;
  out["rows"] = e.rows;
  out["blocks"] = e.blocks;
  py::list hs;
  for (const rs::HorizonEval& h : e.horizons) {
    py::dict d;
    d["horizon_ns"] = h.horizon_ns;
    d["label"] = h.label();
    d["n"] = h.n;
    d["excluded_past_end"] = h.excluded_past_end;
    d["excluded_no_mid"] = h.excluded_no_mid;
    d["ic"] = h.ic;
    d["block_ic"] = h.block_ic;
    d["block_ic_mean"] = h.block_ic_mean;
    d["block_ic_stdev"] = h.block_ic_stdev;
    d["block_ic_min"] = h.block_ic_min;
    d["block_ic_max"] = h.block_ic_max;
    d["block_sign_agreement"] = h.block_sign_agreement;
    py::list bs;
    for (const rs::SignalBucket& b : h.buckets) bs.append(bucket_dict(b));
    d["buckets"] = bs;
    hs.append(d);
  }
  out["horizons"] = hs;
  out["table"] = e.table();
  return out;
}

rs::Feature named_feature(const std::string& name) {
  if (name == "imbalance") return rs::Feature::Imbalance;
  if (name == "microprice_edge_bps") return rs::Feature::MicropriceEdge;
  if (name == "spread_bps") return rs::Feature::Spread;
  throw py::value_error("values: unknown feature '" + name +
                        "' (imbalance, microprice_edge_bps, spread_bps)");
}

}  // namespace

void bind_research(py::module_& m) {
  py::class_<rs::FeatureTable, TablePtr>(
      m,
      "FeatureTable",
      "Book features and forward mid moves, one row per sampled market-data event. Columns are "
      "read-only numpy views onto the C++ table and stay valid after it goes out of scope. "
      "Prices, quantities and spreads are raw 1e-8 fixed-point int64; imbalance is in the same "
      "scale, so 100000000 is 1.0.")
      .def_property_readonly("columns", &columns_dict, "Feature columns (zero-copy numpy views).")
      .def_property_readonly("forward_mid",
                             &forward_mid_dict,
                             "Forward mid per horizon, keyed by label ('100ms', '1s'). Zero-copy "
                             "numpy views; 0 marks a row with no value at that horizon.")
      .def_property_readonly(
          "horizons",
          [](const rs::FeatureTable& t) { return t.rows.horizon_ns; },
          "Horizons in nanoseconds, ascending.")
      .def_property_readonly("coverage",
                             &coverage_list,
                             "Per horizon: how many rows got a forward mid, how many were past "
                             "the end of the data and how many landed on a one-sided book.")
      .def_property_readonly("events", [](const rs::FeatureTable& t) { return t.events; })
      .def_property_readonly("book_updates",
                             [](const rs::FeatureTable& t) { return t.book_updates; })
      .def_property_readonly("skipped_one_sided",
                             [](const rs::FeatureTable& t) { return t.skipped_one_sided; })
      .def_property_readonly("skipped_subsample",
                             [](const rs::FeatureTable& t) { return t.skipped_subsample; })
      .def_property_readonly("start_ts", [](const rs::FeatureTable& t) { return t.start_ts; })
      .def_property_readonly("end_ts", [](const rs::FeatureTable& t) { return t.end_ts; })
      .def("__len__", [](const rs::FeatureTable& t) { return t.rows.size(); })
      .def("summary", &rs::FeatureTable::summary_table, "Row counts and per-horizon coverage.")
      .def("csv", &rs::FeatureTable::csv, "Every column as CSV; an unset forward mid is empty.");

  m.def(
      "features",
      [](const py::object& data,
         const py::object& horizons,
         std::size_t imbalance_levels,
         double subsample,
         bool sample_book_updates,
         bool sample_trades,
         std::uint32_t instruments) {
        rs::FeatureConfig cfg;
        cfg.horizons = horizons_from(horizons);
        cfg.imbalance_levels = imbalance_levels;
        cfg.subsample = seconds_arg(subsample, "subsample");
        cfg.sample_book_updates = sample_book_updates;
        cfg.sample_trades = sample_trades;
        cfg.instruments = instruments;
        std::vector<py::object> keep;
        std::unique_ptr<sim::MdSource> source = open_md_source(data, nullptr, keep);
        if (source == nullptr) {
          throw py::value_error(
              "data: 'synthetic' is a generator coupled to the simulated venue, not a stream of "
              "events; record one with convert_data('synthetic', out, config) and read the "
              "journal");
        }
        auto table = std::make_shared<rs::FeatureTable>();
        {
          py::gil_scoped_release release;  // touches only C++ state and the kept-alive buffers
          *table = rs::extract_features(*source, cfg);
        }
        return table;
      },
      py::arg("data"),
      py::arg("horizons") = py::none(),
      py::arg("imbalance_levels") = 1,
      py::arg("subsample") = 0.0,
      py::arg("sample_book_updates") = true,
      py::arg("sample_trades") = false,
      py::arg("instruments") = 1,
      "Book features and forward mid moves from a market-data source.\n\n"
      "data: a '<source>:<args>' spec ('binance:BTCUSDT,2024-03-27'), a .fmj/.csv path, or a "
      "dict of numpy columns, the same argument run_backtest() takes.\n"
      "horizons: forward horizons in seconds; None uses 0.1, 1, 10 and 60.\n"
      "imbalance_levels: book levels summed into the imbalance column; 1 is the touch.\n"
      "subsample: minimum spacing between rows in seconds; 0 samples every book update.\n"
      "sample_book_updates / sample_trades: what emits a row. With book updates off and trades "
      "on, the table has one row per trade, at the timestamps a backtest's fills land on.\n"
      "instruments: books kept; events for a higher instrument id are ignored.");

  m.def(
      "evaluate_signal",
      [](const TablePtr& table,
         const py::object& values,
         const py::object& name,
         std::uint32_t buckets,
         std::uint32_t blocks) {
        rs::EvalConfig cfg;
        cfg.buckets = buckets;
        cfg.blocks = blocks;
        rs::SignalEval out;
        if (py::isinstance<py::str>(values)) {
          const rs::Feature f = named_feature(values.cast<std::string>());
          {
            py::gil_scoped_release release;
            out = rs::evaluate_feature(*table, f, cfg);
          }
          return eval_dict(out);
        }
        if (!py::isinstance<py::array>(values))
          throw py::type_error("values must be a float64 numpy array or a feature name");
        auto a = py::reinterpret_borrow<py::array>(values);
        if (a.ndim() != 1) throw py::value_error("values must be 1-D");
        if ((a.flags() & py::array::c_style) == 0)
          throw py::value_error("values must be C-contiguous; copy it explicitly");
        if ((a.flags() & py::detail::npy_api::NPY_ARRAY_ALIGNED_) == 0)
          throw py::value_error("values is not aligned; copy it explicitly");
        if (!a.dtype().equal(py::dtype::of<double>())) {
          throw py::type_error("values must have dtype float64, got " +
                               py::str(a.dtype()).cast<std::string>() +
                               " (no implicit casts; use .astype(float) explicitly)");
        }
        const std::string label = name.is_none() ? std::string("signal") : name.cast<std::string>();
        const std::span<const double> span{static_cast<const double*>(a.data()),
                                           static_cast<std::size_t>(a.size())};
        {
          py::gil_scoped_release release;
          out = rs::evaluate_signal(*table, span, label, cfg);
        }
        return eval_dict(out);
      },
      py::arg("table"),
      py::arg("values"),
      py::arg("name") = py::none(),
      py::arg("buckets") = 10,
      py::arg("blocks") = 10,
      "Information coefficient, bucket table and conditional touch markout of a signal, per "
      "horizon of `table`.\n\n"
      "values: a float64 array with one entry per row, or the name of a built-in feature "
      "('imbalance', 'microprice_edge_bps', 'spread_bps').\n"
      "name: label for the report; defaults to the feature name or 'signal'.\n"
      "buckets: equal-count buckets of the signal; 10 is a decile table.\n"
      "blocks: contiguous equal-sized blocks the IC is recomputed over.\n\n"
      "Each horizon reports 'ic' (Spearman), the per-block ICs, and buckets holding "
      "'forward_bps' (the mean forward mid move), 'half_spread_bps', and 'buy_bps' / "
      "'sell_bps': what a quote resting at the touch would have made or lost by the horizon, "
      "gross of fees, in basis points of the mid.");
}

}  // namespace fastmm::py_bind
