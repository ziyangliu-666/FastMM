// Python hot hooks (ADR-0013, section 1): the ABI layout that fastmm/_hot/abi.py checks at import,
// and the backtest of a compiled hot strategy (HotStrategy, run with the GIL released).
#include "bind_common.hpp"
#include "hot_program.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/result.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_strategy.hpp"

#include <pybind11/stl.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace fastmm::py_bind {

namespace {

using HotSimEngine = Engine<HotStrategy, SimClock, sim::SimTransport, InlineFeed>;

// Hook calls of the hot backtest running in this process, published after every engine step, and
// whether one is running. Read by _hot_hold_gil (tests); two concurrent runs share them.
std::atomic<std::uint64_t> g_hot_calls{0};
std::atomic<int> g_hot_running{0};

// NOLINTBEGIN(bugprone-macro-parentheses)
#define FASTMM_HOT_OFFSET(d, T, f) (d)[#f] = offsetof(T, f);
// NOLINTEND(bugprone-macro-parentheses)

py::dict ctx_layout() {
  py::dict d;
  d["__size__"] = sizeof(fastmm_hot_ctx);
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, now_ns)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, instrument)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, quoting_enabled)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, connected)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, fill_side)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, fill_maker)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, tick)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, lot)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, min_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, position)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, tick_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, lot_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, min_qty_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, position_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, fill_price)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, fill_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, fill_price_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, fill_qty_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, action)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, flags)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, n_bids)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, n_asks)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, status)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, fail_code)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, bid_px)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, bid_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, ask_px)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, ask_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, bid_px_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, bid_qty_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, ask_px_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, ask_qty_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, bid_is_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_ctx, ask_is_raw)
  return d;
}

py::dict book_layout() {
  py::dict d;
  d["__size__"] = sizeof(fastmm_hot_book);
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, ts_ns)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, valid)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, n_bids)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, n_asks)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, mid)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_bid)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_ask)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_bid_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_ask_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, mid_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_bid_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_ask_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_bid_qty_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, best_ask_qty_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, bid_px)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, bid_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, ask_px)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, ask_qty)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, bid_px_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, bid_qty_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, ask_px_raw)
  FASTMM_HOT_OFFSET(d, fastmm_hot_book, ask_qty_raw)
  return d;
}

#undef FASTMM_HOT_OFFSET

}  // namespace

py::tuple run_hot_strategy(const bt::BacktestConfig& cfg,
                           sim::MdSource* source,
                           const py::dict& program) {
  const HotProgram p = py_hot::program_from(program, true);
  bt::BacktestSession session(cfg, source);
  std::unique_ptr<IEngineRunner> runner =
      session.backend().template make_runner<HotStrategy>(session.deps());
  auto* er = static_cast<EngineRunner<HotSimEngine, HotStrategy>*>(runner.get());
  HotStrategy& strategy = er->strategy();
  if (!strategy.attach(p, er->engine().instruments())) {
    throw py::value_error("fastmm: invalid hot program (record smaller than its parameter block)");
  }
  sim::EngineHooks hooks = session.backend().hooks;
  hooks.stopped = [](void* c) {
    auto* e = static_cast<HotSimEngine*>(c);
    g_hot_calls.store(e->strategy().calls(), std::memory_order_relaxed);
    return e->stopped();
  };
  std::shared_ptr<bt::BacktestResult> result;
  {
    const py::gil_scoped_release release;
    strategy.warm_up(er->engine().instruments());
    g_hot_calls.store(0, std::memory_order_relaxed);
    g_hot_running.store(1, std::memory_order_relaxed);
    struct Running {
      Running() = default;
      Running(const Running&) = delete;
      Running& operator=(const Running&) = delete;
      ~Running() { g_hot_running.store(0, std::memory_order_relaxed); }
    } const running;
    result = std::make_shared<bt::BacktestResult>(session.run(hooks, runner.get(), cfg.strategy));
  }
  py::object error = py::none();
  if (strategy.failed()) {
    const HotError& e = strategy.error();
    error = py::make_tuple(
        e.status,
        e.fail_code,
        e.hook >= 0 ? std::string(to_string(static_cast<HotHook>(e.hook))) : std::string(),
        e.timer,
        e.at.ns,
        er->engine().stats().events,
        std::string(to_string(er->engine().kill_reason())));
  }
  return py::make_tuple(result, error, strategy.calls());
}

void bind_hot(py::module_& m) {
  m.def(
      "_hot_abi",
      [] {
        py::dict d;
        d["version"] = FASTMM_HOT_ABI_VERSION;
        d["book_depth"] = FASTMM_HOT_BOOK_DEPTH;
        d["quote_levels"] = FASTMM_HOT_QUOTE_LEVELS;
        d["timer_limit"] = kMaxHotTimers;
        d["ctx"] = ctx_layout();
        d["book"] = book_layout();
        return d;
      },
      "Internal: layout of the hot hook C ABI (include/fastmm/strategies/hot_abi.h).");

  m.def(
      "_hot_hold_gil",
      [](std::uint64_t min_calls, double timeout_s) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
        {
          const py::gil_scoped_release release;
          while (g_hot_running.load(std::memory_order_relaxed) == 0 &&
                 std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
        // The GIL is held from here until the function returns.
        const std::uint64_t start = g_hot_calls.load(std::memory_order_relaxed);
        std::uint64_t now = start;
        while (std::chrono::steady_clock::now() < deadline) {
          now = g_hot_calls.load(std::memory_order_relaxed);
          if (now - start >= min_calls) return py::make_tuple(true, now - start);
          if (g_hot_running.load(std::memory_order_relaxed) == 0) break;
          std::this_thread::yield();
        }
        return py::make_tuple(false, now - start);
      },
      py::arg("min_calls"),
      py::arg("timeout_s"),
      "Internal (tests): wait for a hot backtest to start, then hold the GIL until its hooks "
      "have run min_calls more times. Returns (reached, calls).");
}

}  // namespace fastmm::py_bind
