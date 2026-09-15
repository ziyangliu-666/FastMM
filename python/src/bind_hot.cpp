// Python hot hooks and slow methods (ADR-0013, sections 1 and 2): the ABI layouts that
// fastmm/_hot/abi.py and fastmm/_slow/abi.py check at import, the backtest of a compiled hot
// strategy (HotStrategy, run with the GIL released, its slow methods at simulated times) and its
// replay.
#include "bind_common.hpp"
#include "hot_program.hpp"
#include "slow_channel_py.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/result.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/sim/param_schedule.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_strategy.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/slow_channel.hpp"

#include <pybind11/stl.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fastmm::py_bind {

namespace {

using HotSimEngine = Engine<HotStrategy, SimClock, sim::SimTransport, InlineFeed>;
using HotReplayEngine = Engine<HotStrategy, SimClock, sim::ReplayTransport, sim::JournalFeed>;

// Hook calls of the hot backtest running in this process, published after every engine step, and
// whether one is running. Read by _hot_hold_gil (tests); two concurrent runs share them.
std::atomic<std::uint64_t> g_hot_calls{0};
std::atomic<int> g_hot_running{0};
// The slow channel of the hot backtest running on this thread: EngineHooks::stopped gets only the
// engine as its context.
thread_local const SlowChannel* t_slow_channel = nullptr;

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

// fastmm._core's wrapper of a slow channel (slow_channel_py.hpp).
struct CoreSlowChannel {
  std::shared_ptr<SlowChannel> channel;
  std::vector<std::string> symbols;
  std::vector<std::uint64_t> recent_cursors;
};

std::vector<std::string> symbols_of(const InstrumentTable& instruments) {
  std::vector<std::string> out(instruments.size());
  for (const Instrument& inst : instruments) {
    if (inst.id.value < out.size()) out[inst.id.value] = std::string(inst.symbol.view());
  }
  return out;
}

// The slow tier of one backtest: the Python runner and the first error it raised.
struct SlowRun {
  py::object runner;
  py::object error;
  SlowChannel* channel = nullptr;
};

// SlowHooks::run: the runner's wake(now_ns) with the GIL held. An exception stops the slow tier
// (SlowFailure::Exception, which ends the run after this call) and is kept for the caller.
Timestamp run_slow(void* ctx, Timestamp now) {
  auto* run = static_cast<SlowRun*>(ctx);
  const py::gil_scoped_acquire gil;
  try {
    const auto next = run->runner.attr("wake")(now.ns).cast<std::int64_t>();
    return next < 0 ? Timestamp::max() : Timestamp{next};
  } catch (py::error_already_set& e) {
    run->error = e.value();
  } catch (const std::exception& e) {
    run->error = py::module_::import("builtins").attr("RuntimeError")(e.what());
  }
  run->channel->fail(SlowFailure::Exception);
  return Timestamp::max();
}

}  // namespace

py::tuple run_hot_strategy(const bt::BacktestConfig& cfg,
                           sim::MdSource* source,
                           const py::dict& program,
                           const std::string& strategy_meta,
                           const py::object& slow) {
  const HotProgram p = py_hot::program_from(program, true);
  const HotParamTable table(py_hot::param_fields_from(program), p.param_bytes);
  bt::BacktestSession session(cfg, source, &table.schema(), strategy_meta);
  std::unique_ptr<IEngineRunner> runner =
      session.backend().template make_runner<HotStrategy>(session.deps());
  auto* er = static_cast<EngineRunner<HotSimEngine, HotStrategy>*>(runner.get());
  HotStrategy& strategy = er->strategy();
  if (!strategy.attach(p, er->engine().instruments())) {
    throw py::value_error("fastmm: invalid hot program (record smaller than its parameter block)");
  }

  // The slow tier: a channel, parameter updates at simulated times and the runner's wake-ups.
  sim::ParamSchedule schedule;
  std::shared_ptr<SlowChannel> channel;
  SlowRun slow_run;
  slow_run.error = py::none();
  if (!slow.is_none()) {
    const auto s = slow.cast<py::dict>();
    SlowChannelConfig cc;
    cc.instruments = cfg.instruments.size();
    const auto capacity = s["fills_capacity"].cast<std::size_t>();
    cc.fills_capacity =
        capacity > 0 ? capacity
                     : slow_fills_capacity(cfg.engine.risk.orders_per_sec,
                                           Duration{s["longest_gap_ns"].cast<std::int64_t>()});
    cc.recent_rows = s["recent_rows"].cast<std::size_t>();
    cc.snapshot_interval = Duration{s["snapshot_interval_ns"].cast<std::int64_t>()};
    channel = std::make_shared<SlowChannel>(cc);
    if (!strategy.attach_slow(channel.get()))
      throw py::value_error("fastmm: the slow channel is smaller than the instrument table");
    schedule.set_delay(Duration{s["delay_ns"].cast<std::int64_t>()});
    channel->set_param_sink(schedule.threaded_sink(session.backend().clock));
    session.set_param_schedule(&schedule);
    slow_run.runner = s["runner"];
    slow_run.channel = channel.get();
    CoreSlowChannel wrapper{
        channel, symbols_of(cfg.instruments), std::vector<std::uint64_t>(cc.instruments, 0)};
    const auto first =
        slow_run.runner
            .attr("start")(py::cast(std::move(wrapper)), session.backend().clock.now().ns)
            .cast<std::int64_t>();
    sim::SlowHooks sh;
    sh.ctx = &slow_run;
    sh.run = &run_slow;
    sh.first = first < 0 ? Timestamp::max() : Timestamp{first};
    session.set_slow_hooks(sh);
  }

  sim::EngineHooks hooks = session.backend().hooks;
  hooks.stopped = [](void* c) {
    auto* e = static_cast<HotSimEngine*>(c);
    g_hot_calls.store(e->strategy().calls(), std::memory_order_relaxed);
    return e->stopped() ||
           (t_slow_channel != nullptr && t_slow_channel->failure() != SlowFailure::None);
  };
  std::shared_ptr<bt::BacktestResult> result;
  {
    const py::gil_scoped_release release;
    strategy.warm_up(er->engine().instruments());
    g_hot_calls.store(0, std::memory_order_relaxed);
    g_hot_running.store(1, std::memory_order_relaxed);
    t_slow_channel = channel.get();
    struct Running {
      Running() = default;
      Running(const Running&) = delete;
      Running& operator=(const Running&) = delete;
      Running(Running&&) = delete;
      Running& operator=(Running&&) = delete;
      ~Running() {
        g_hot_running.store(0, std::memory_order_relaxed);
        t_slow_channel = nullptr;
      }
    } const running;
    result = std::make_shared<bt::BacktestResult>(session.run(hooks, runner.get(), cfg.strategy));
  }
  if (channel) channel->close();
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
  const int failure = channel ? static_cast<int>(channel->failure()) : 0;
  return py::make_tuple(
      result, error, strategy.calls(), slow_run.error, failure, er->engine().stats().events);
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
      "_slow_abi",
      &slow_abi,
      "Internal: layout of the slow channel structs (include/fastmm/strategies/slow_channel.hpp).");

  m.def(
      "_hot_fixed_raw",
      [](double x) {
        std::int64_t raw = 0;
        return hot_fixed_raw(x, raw) ? raw : std::int64_t{0};
      },
      py::arg("x"),
      "Internal (tests): the `_raw` twin the engine computes for a published float.");

  py::class_<CoreSlowChannel> channel(
      m,
      "_SlowChannel",
      "Internal: the channel between the engine and a strategy's slow methods. The constructor "
      "(tests, live runner) makes one whose updates go to its own ring.");
  channel.def(py::init([](std::size_t instruments,
                          std::size_t fills_capacity,
                          std::size_t recent_rows,
                          std::int64_t snapshot_interval_ns,
                          std::vector<std::string> symbols) {
                SlowChannelConfig cc;
                cc.instruments = instruments;
                cc.fills_capacity = fills_capacity;
                cc.recent_rows = recent_rows;
                cc.snapshot_interval = Duration{snapshot_interval_ns};
                if (symbols.size() < instruments) symbols.resize(instruments);
                return CoreSlowChannel{std::make_shared<SlowChannel>(cc),
                                       std::move(symbols),
                                       std::vector<std::uint64_t>(instruments, 0)};
              }),
              py::arg("instruments") = 1,
              py::arg("fills_capacity") = 4096,
              py::arg("recent_rows") = 4096,
              py::arg("snapshot_interval_ns") = 10'000'000,
              py::arg("symbols") = std::vector<std::string>{});
  def_slow_channel(channel);

  m.def(
      "_replay_hot_strategy",
      [](const py::object& path,
         const bt::BacktestConfig& config,
         const std::string& name,
         const py::dict& program,
         bool verify,
         bool param_updates) {
        const std::string p = fspath(path);
        const HotProgram hot = py_hot::program_from(program, false);  // a replay drains the journal
        const HotParamTable table(py_hot::param_fields_from(program), hot.param_bytes);
        bt::ReplayOptions opt;
        opt.verify = verify;
        opt.param_updates = param_updates;
        bt::ReplayStrategy strategy;
        strategy.name = name;
        strategy.schema = &table.schema();
        strategy.make = [&hot](RunnerDeps& deps) -> std::unique_ptr<IEngineRunner> {
          auto* backend = static_cast<sim::ReplayBackend*>(deps.backend);
          std::unique_ptr<IEngineRunner> r = backend->template make_runner<HotStrategy>(deps);
          auto* er = static_cast<EngineRunner<HotReplayEngine, HotStrategy>*>(r.get());
          if (!er->strategy().attach(hot, er->engine().instruments()))
            throw std::invalid_argument("fastmm: invalid hot program");
          er->strategy().warm_up(er->engine().instruments());
          return r;
        };
        bt::ReplayResult res;
        {
          const py::gil_scoped_release release;
          res = bt::replay_journal(p, config, opt, strategy);
        }
        py::dict d;
        d["strategy"] = res.strategy;
        d["outbound_sha256"] = res.outbound_sha256;
        d["recorded_sha256"] = res.recorded_sha256;
        d["outbound_messages"] = res.outbound_messages;
        d["recorded_messages"] = res.recorded_messages;
        d["events"] = res.events;
        d["first_mismatch"] = res.first_mismatch;
        d["expected_message"] = res.expected_message;
        d["actual_message"] = res.actual_message;
        d["ok"] = res.ok();
        return d;
      },
      py::arg("path"),
      py::arg("config"),
      py::arg("name"),
      py::arg("program"),
      py::arg("verify"),
      py::arg("param_updates"),
      "Internal: replay of a compiled hot strategy from a journal; use fastmm.replay().");

  m.def(
      "_journal_config",
      [](const py::object& path) {
        const std::string p = fspath(path);
        return bt::journal_config(p);
      },
      py::arg("path"),
      "Internal: the configuration a journal embeds (RuntimeError when it has none).");

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
