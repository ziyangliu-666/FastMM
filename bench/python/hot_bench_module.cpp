// _hot_bench: cost per on_book call of HotStrategy against C++ BasicMM on the same books, positions
// and parameters, without the engine. Built in-tree next to the bench targets (not in the wheel);
// bench/python/bench_hot_strategy.py drives it.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_strategy.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;
using namespace fastmm;

namespace {

volatile std::int64_t g_sink = 0;

// The context methods BasicMM and HotStrategy call; set_quotes is an out-of-line sink.
struct BenchCtx {
  const InstrumentTable* table = nullptr;
  const L2Book<256>* book0 = nullptr;
  Position pos{};
  Timestamp t{seconds(1'700'000'000).ns};

  [[nodiscard]] Timestamp now() const noexcept { return t; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const noexcept {
    return table->get(id);
  }
  [[nodiscard]] const InstrumentTable& instruments() const noexcept { return *table; }
  [[nodiscard]] bool contains(InstrumentId id) const noexcept { return table->contains(id); }
  [[nodiscard]] const Position& position(InstrumentId) const noexcept { return pos; }
  [[nodiscard]] bool quoting_enabled() const noexcept { return true; }
  [[nodiscard]] bool venue_killed(VenueId) const noexcept { return false; }
  [[nodiscard]] const L2Book<256>& book(InstrumentId) const noexcept { return *book0; }
  [[gnu::noinline]] bool set_quotes(InstrumentId, const DesiredQuotes& q) noexcept {
    g_sink =
        (q.bids.empty() ? 0 : q.bids[0].price.raw) + (q.asks.empty() ? 0 : q.asks[0].price.raw);
    return true;
  }
  [[gnu::noinline]] void pull_quotes(InstrumentId) noexcept { g_sink = 0; }
  [[gnu::noinline]] void pull_all_quotes() noexcept { g_sink = 0; }
  void trip_kill(KillReason) noexcept {}
  void request_stop() noexcept {}
  [[nodiscard]] TimerId every(Duration, std::uint64_t) noexcept { return TimerId{}; }
  // Used only with a slow channel attached, which the benchmark never does.
  [[nodiscard]] TimerId once(Duration, std::uint64_t) noexcept { return TimerId{}; }
  [[nodiscard]] bool killed() const noexcept { return false; }
  [[nodiscard]] Qty open_qty(InstrumentId, Side) const noexcept { return Qty{}; }
};

// BTCUSDT (tick 0.01, lot 0.00001), four touches with 20 levels a side, four positions.
struct Market {
  InstrumentTable table;
  std::array<L2Book<256>, 4> books{};
  std::array<Qty, 4> positions{};

  Market() {
    Instrument inst{};
    inst.symbol = "BTCUSDT";
    inst.flags = Instrument::kEnabled;
    inst.tick = *Price::from_decimal("0.01");
    inst.lot = *Qty::from_decimal("0.00001");
    inst.min_qty = inst.lot;
    if (!table.add(inst)) throw std::runtime_error("instrument");
    const std::array<std::array<const char*, 2>, 4> touch{{{"60000.00", "60000.01"},
                                                           {"60000.12", "60000.13"},
                                                           {"59999.99", "60000.00"},
                                                           {"60001.49", "60001.51"}}};
    const Qty q = *Qty::from_decimal("0.01");
    for (std::size_t k = 0; k < books.size(); ++k) {
      const Price bb = *Price::from_decimal(touch[k][0]);
      const Price ba = *Price::from_decimal(touch[k][1]);
      std::vector<Level> bids;
      std::vector<Level> asks;
      for (int i = 0; i < 20; ++i) {
        bids.push_back(Level{bb - inst.tick * i, q});
        asks.push_back(Level{ba + inst.tick * i, q});
      }
      books[k].apply_snapshot(bids, asks, 1, Timestamp{seconds(1'700'000'000).ns});
    }
    positions = {Qty{},
                 *Qty::from_decimal("0.004"),
                 *Qty::from_decimal("-0.006"),
                 *Qty::from_decimal("0.01")};
  }
};

// ns per call of `step(k)`: one entry per run of `iters` calls, after a warm-up.
template <class Step>
std::vector<double> time_calls(Step&& step, std::uint64_t iters, int runs) {
  for (std::uint64_t k = 0; k < 100'000; ++k) step(k);
  std::vector<double> out;
  for (int r = 0; r < runs; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t k = 0; k < iters; ++k) step(k);
    const auto t1 = std::chrono::steady_clock::now();
    out.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                  static_cast<double>(iters));
  }
  return out;
}

std::vector<double> cpp_on_book(const py::dict& params, std::uint64_t iters, int runs) {
  Market m;
  BasicMM s;
  ParamMap pm;
  for (const auto& [k, v] : params) pm[py::str(k)] = py::str(v);
  if (auto err = s.configure(pm)) throw std::invalid_argument(*err);
  BenchCtx ctx;
  ctx.table = &m.table;
  const py::gil_scoped_release release;
  return time_calls(
      [&](std::uint64_t k) {
        const std::size_t i = k & 3U;
        ctx.book0 = &m.books[i];
        ctx.pos.qty = m.positions[i];
        s.on_book(ctx, InstrumentId{0}, m.books[i]);
      },
      iters,
      runs);
}

HotProgram program_from(const py::dict& program) {
  HotProgram p;
  const auto hooks = program["hooks"].cast<py::dict>();
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  p.hooks[0] = reinterpret_cast<fastmm_hot_fn>(hooks["on_book"].cast<std::uintptr_t>());
  const auto record = program["record"].cast<std::string>();
  p.record.assign(record.begin(), record.end());
  p.param_bytes = program["param_bytes"].cast<std::size_t>();
  p.book_depth = program["book_depth"].cast<bool>();
  return p;
}

std::vector<double> hot_on_book(const py::dict& program, std::uint64_t iters, int runs) {
  Market m;
  HotStrategy s;
  if (!s.attach(program_from(program), m.table)) throw std::invalid_argument("program");
  BenchCtx ctx;
  ctx.table = &m.table;
  const py::gil_scoped_release release;
  return time_calls(
      [&](std::uint64_t k) {
        const std::size_t i = k & 3U;
        ctx.book0 = &m.books[i];
        ctx.pos.qty = m.positions[i];
        s.on_book(ctx, InstrumentId{0}, m.books[i]);
      },
      iters,
      runs);
}

}  // namespace

PYBIND11_MODULE(_hot_bench, m) {
  m.doc() = "Hook cost of HotStrategy and C++ BasicMM without the engine (bench only)";
  m.def("cpp_on_book", &cpp_on_book, py::arg("params"), py::arg("iters"), py::arg("runs"));
  m.def("hot_on_book", &hot_on_book, py::arg("program"), py::arg("iters"), py::arg("runs"));
}
