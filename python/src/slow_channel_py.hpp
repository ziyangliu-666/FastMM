#pragma once
// Python methods over a fastmm::SlowChannel (ADR-0013, section 1), shared by fastmm._core and the
// live module. Each module binds its own wrapper type, so no C++ type is bound by both:
//
//   struct Channel {
//     std::shared_ptr<fastmm::SlowChannel> channel;
//     std::vector<std::string> symbols;            // instrument symbols by id
//     std::vector<std::uint64_t> recent_cursors;   // one per instrument, starting at 0
//   };
//   py::class_<Channel> c(m, "SlowChannel");
//   fastmm::py_bind::def_slow_channel(c);
//
// Buffers cross as bytes laid out as the structs in strategies/slow_channel.hpp;
// fastmm/_slow/abi.py holds the numpy dtypes and checks them against slow_abi() at import.
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/slow_channel.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::py_bind {

namespace slow_detail {

namespace py = pybind11;

inline py::bytes as_bytes(const void* data, std::size_t n) {
  return {static_cast<const char*>(data), n};
}

inline InstrumentId instrument_arg(std::int64_t inst, std::size_t instruments) {
  if (inst < 0 || static_cast<std::uint64_t>(inst) >= instruments) {
    throw py::value_error("instrument " + std::to_string(inst) + " is not in the instrument table");
  }
  InstrumentId id{};
  id.value = static_cast<decltype(id.value)>(inst);
  return id;
}

// NOLINTBEGIN(bugprone-macro-parentheses)
#define FASTMM_SLOW_OFFSET(d, T, f) (d)[#f] = offsetof(T, f);
// NOLINTEND(bugprone-macro-parentheses)

}  // namespace slow_detail

// Sizes and field offsets of the shared structs.
inline pybind11::dict slow_abi() {
  namespace py = pybind11;
  py::dict header;
  header["__size__"] = sizeof(SlowSnapshotHeader);
  FASTMM_SLOW_OFFSET(header, SlowSnapshotHeader, ts_ns)
  FASTMM_SLOW_OFFSET(header, SlowSnapshotHeader, version)
  FASTMM_SLOW_OFFSET(header, SlowSnapshotHeader, instruments)
  FASTMM_SLOW_OFFSET(header, SlowSnapshotHeader, quoting_enabled)
  FASTMM_SLOW_OFFSET(header, SlowSnapshotHeader, killed)
  py::dict state;
  state["__size__"] = sizeof(SlowInstrumentState);
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, book_ts_ns)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, best_bid_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, best_bid_qty_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, best_ask_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, best_ask_qty_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, mid_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, position_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, avg_price_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, realized_pnl_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, unrealized_pnl_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, fees_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, bid_open_qty_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, ask_open_qty_raw)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, param_ts_ns)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, param_seq)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, fills)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, book_valid)
  FASTMM_SLOW_OFFSET(state, SlowInstrumentState, quoting)
  py::dict row;
  row["__size__"] = sizeof(SlowRecentRow);
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, ts_ns)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, bid_raw)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, bid_qty_raw)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, ask_raw)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, ask_qty_raw)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, trade_price_raw)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, trade_qty_raw)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, kind)
  FASTMM_SLOW_OFFSET(row, SlowRecentRow, side)
  py::dict fill;
  fill["__size__"] = sizeof(SlowFill);
  FASTMM_SLOW_OFFSET(fill, SlowFill, seq)
  FASTMM_SLOW_OFFSET(fill, SlowFill, ts_ns)
  FASTMM_SLOW_OFFSET(fill, SlowFill, price_raw)
  FASTMM_SLOW_OFFSET(fill, SlowFill, qty_raw)
  FASTMM_SLOW_OFFSET(fill, SlowFill, fee_raw)
  FASTMM_SLOW_OFFSET(fill, SlowFill, position_raw)
  FASTMM_SLOW_OFFSET(fill, SlowFill, instrument)
  FASTMM_SLOW_OFFSET(fill, SlowFill, side)
  FASTMM_SLOW_OFFSET(fill, SlowFill, maker)
  py::dict d;
  d["header"] = header;
  d["state"] = state;
  d["row"] = row;
  d["fill"] = fill;
  d["max_fields"] = ParamUpdateMsg::kMaxFields;
  d["failure_exception"] = static_cast<int>(SlowFailure::Exception);
  d["failure_timeout"] = static_cast<int>(SlowFailure::Timeout);
  d["failure_fills_overflow"] = static_cast<int>(SlowFailure::FillsOverflow);
  d["failure_thread_exited"] = static_cast<int>(SlowFailure::ThreadExited);
  return d;
}

#undef FASTMM_SLOW_OFFSET

// Binds the channel methods on `c` (see the top of this file for the wrapper's members).
template <class W>
void def_slow_channel(pybind11::class_<W>& c) {
  namespace py = pybind11;
  using slow_detail::as_bytes;
  c.def_property_readonly(
       "instruments", [](const W& w) { return w.channel->instruments(); }, "Instruments.")
      .def_property_readonly(
          "symbols", [](const W& w) { return w.symbols; }, "Instrument symbols by id.")
      .def_property_readonly(
          "fills_capacity",
          [](const W& w) { return w.channel->config().fills_capacity; },
          "Fills the ring holds.")
      .def_property_readonly(
          "recent_rows",
          [](const W& w) { return w.channel->config().recent_rows; },
          "Rows per instrument in the recent window.")
      .def(
          "snapshot",
          [](const W& w) {
            const std::size_t n = w.channel->instruments();
            std::vector<SlowInstrumentState> states(n);
            SlowSnapshotHeader h{};
            w.channel->snapshot(h, states);
            return py::make_tuple(h.ts_ns,
                                  h.version,
                                  h.quoting_enabled != 0,
                                  h.killed != 0,
                                  as_bytes(states.data(), n * sizeof(SlowInstrumentState)));
          },
          "(ts_ns, version, quoting_enabled, killed, bytes of SlowInstrumentState per instrument).")
      .def(
          "recent",
          [](W& w, std::int64_t inst) {
            const InstrumentId id = slow_detail::instrument_arg(inst, w.channel->instruments());
            std::vector<SlowRecentRow> rows(w.channel->config().recent_rows);
            const SlowChannel::RecentRead r =
                w.channel->recent(id, rows, w.recent_cursors.at(id.value));
            return py::make_tuple(as_bytes(rows.data(), r.rows * sizeof(SlowRecentRow)), r.dropped);
          },
          py::arg("inst"),
          "(bytes of SlowRecentRow oldest first, rows dropped since the previous call).")
      .def(
          "drain_fills",
          [](W& w) {
            std::vector<SlowFill> fills(w.channel->fills_pending());
            const std::size_t n = w.channel->drain_fills(fills);
            return as_bytes(fills.data(), n * sizeof(SlowFill));
          },
          "Bytes of the SlowFill records taken out of the ring.")
      .def(
          "publish",
          [](W& w,
             std::int64_t inst,
             const std::vector<std::uint16_t>& fields,
             const std::vector<std::int64_t>& values) {
            const InstrumentId id =
                inst < 0 ? ParamUpdateMsg::kAllInstruments
                         : slow_detail::instrument_arg(inst, w.channel->instruments());
            return w.channel->publish(id, fields, values);
          },
          py::arg("inst"),
          py::arg("fields"),
          py::arg("values"),
          "Send a validated update (inst -1: every instrument). False when refused or closed.")
      .def(
          "close", [](W& w) { w.channel->close(); }, "Later publishes return False.")
      .def_property_readonly("closed", [](const W& w) { return w.channel->closed(); })
      .def_property_readonly("published", [](const W& w) { return w.channel->published(); })
      .def_property_readonly("refused", [](const W& w) { return w.channel->refused(); })
      .def(
          "heartbeat",
          [](W& w, std::int64_t steady_ns) { w.channel->heartbeat(steady_ns); },
          py::arg("steady_ns"))
      .def(
          "begin_call",
          [](W& w, std::int64_t steady_ns, std::int64_t timeout_ns) {
            w.channel->begin_call(steady_ns, Duration{timeout_ns});
          },
          py::arg("steady_ns"),
          py::arg("timeout_ns"))
      .def(
          "end_call",
          [](W& w, std::int64_t steady_ns) { w.channel->end_call(steady_ns); },
          py::arg("steady_ns"))
      .def(
          "fail",
          [](W& w, int code) {
            if (code <= 0 || code > static_cast<int>(SlowFailure::ThreadExited))
              throw py::value_error("bad slow failure code " + std::to_string(code));
            return w.channel->fail(static_cast<SlowFailure>(code));
          },
          py::arg("code"),
          "Record a failure unless one is recorded; True when this one is kept.")
      .def(
          "failure",
          [](const W& w) { return static_cast<int>(w.channel->failure()); },
          "The recorded failure code (0: none).")
      .def(
          "check",
          [](W& w, std::int64_t steady_ns) {
            return static_cast<int>(w.channel->check(steady_ns));
          },
          py::arg("steady_ns"),
          "Watchdog check: records a timeout when a call ran past its deadline; the failure code.")
      .def_property_readonly("last_heartbeat",
                             [](const W& w) { return w.channel->last_heartbeat(); });
}

}  // namespace fastmm::py_bind
