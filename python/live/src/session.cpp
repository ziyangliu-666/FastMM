// Live sessions of Python hot strategies (ADR-0013, sections 3 and 4), used by fastmm.run_live.
//
//   load_config(path, allow_inline_secrets)   [strategy] section, instruments, order rate, CPUs
//   SlowChannel(symbols, fills_capacity, ...) the session's channel: parameter ring, snapshot,
//                                             recent rows, fills and watchdog state
//   run(path, options, name, params, program, meta, channel, slow, slow_tid)
//                                             -> (exit code, hot error, calls)
//
// run() starts fastmm::live::run_live with a HotStrategy built from the compiled program and the
// channel's parameter ring as an engine input, with the GIL released. The channel is the ring's
// only producer: SlowChannel::publish serialises publishes from any thread. With `slow` the
// strategy also publishes snapshots, recent rows and fills into the channel, and the control
// thread's watchdog (live/slow_watchdog.hpp) stops the session with exit code 7 when the slow tier
// fails. Hooks are warmed up on a scratch strategy before any venue is contacted. One session runs
// per process; a child forked while a session runs is inert (_after_fork_in_child).
#include "session.hpp"

#include "hot_program.hpp"
#include "slow_channel_py.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/live/live_backend.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/live/slow_watchdog.hpp"
#include "fastmm/strategies/hot_params.hpp"
#include "fastmm/strategies/hot_strategy.hpp"
#include "fastmm/strategies/slow_channel.hpp"

#include <pybind11/stl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::py_live {

namespace {

namespace py = pybind11;

using HotLiveEngine = Engine<HotStrategy, TscClock, LiveTransport, RingFeed>;

std::atomic<bool> g_active{false};  // a session runs in this process
std::atomic<bool> g_forked{false};  // forked while a session ran: sessions are off

// fastmm_live's wrapper of a slow channel (slow_channel_py.hpp). run() holds its own reference to
// the channel, so the channel outlives whichever of the session and the Python objects ends last.
struct LiveSlowChannel {
  std::shared_ptr<SlowChannel> channel;
  std::vector<std::string> symbols;
  std::vector<std::uint64_t> recent_cursors;
};

struct ActiveSession {
  ActiveSession() {
    bool expected = false;
    if (!g_active.compare_exchange_strong(expected, true)) {
      throw std::runtime_error(
          "fastmm: a live session is already running in this process; run one session per "
          "process");
    }
  }
  ActiveSession(const ActiveSession&) = delete;
  ActiveSession& operator=(const ActiveSession&) = delete;
  ActiveSession(ActiveSession&&) = delete;
  ActiveSession& operator=(ActiveSession&&) = delete;
  ~ActiveSession() { g_active.store(false); }
};

Config load(const std::string& path, bool allow_inline_secrets) {
  Config::LoadOptions lo;
  lo.allow_inline_secrets = allow_inline_secrets;
  lo.substitute_env = false;  // resolved in run(), so a dry run needs no keys
  return Config::load(path, lo);
}

template <class T>
T option(const py::dict& d, const char* key, T fallback) {
  if (!d.contains(key) || d[key].is_none()) return fallback;
  return d[key].cast<T>();
}

py::dict load_config(const std::string& path, bool allow_inline_secrets) {
  Config cfg;
  InstrumentTable instruments;
  try {
    cfg = load(path, allow_inline_secrets);
    instruments = load_instruments(cfg);
  } catch (const std::exception& e) {
    throw py::value_error(e.what());
  }
  py::dict out;
  out["strategy"] = cfg.strategy.name;
  py::dict params;
  for (const auto& [k, v] : cfg.strategy.params) params[py::str(k)] = v;
  out["params"] = params;
  py::list symbols;
  for (const Instrument& inst : instruments) symbols.append(std::string(inst.symbol.view()));
  out["instruments"] = symbols;
  out["warnings"] = cfg.warnings;
  out["orders_per_sec"] = cfg.risk_limits().orders_per_sec;
  out["max_param_age_ms"] = cfg.strategy.max_param_age_ms;
  py::list reserved;
  if (cfg.engine.cpu >= 0) reserved.append(cfg.engine.cpu);
  for (const int c : cfg.engine.net_cpus) {
    if (c >= 0) reserved.append(c);
  }
  out["reserved_cpus"] = reserved;
  return out;
}

// Throws RuntimeError when this process cannot start a session now.
void check_can_run() {
  if (g_forked.load()) {
    throw std::runtime_error(
        "fastmm: this process was forked while a live session was running and cannot run one; "
        "start processes with the spawn or forkserver method");
  }
  if (g_active.load()) {
    throw std::runtime_error(
        "fastmm: a live session is already running in this process; run one session per process");
  }
}

py::tuple run(const std::string& path,
              const py::dict& options,
              const std::string& name,
              const py::dict& params,
              const py::dict& program,
              const std::string& meta,
              const LiveSlowChannel& channel,
              bool slow,
              std::int64_t slow_tid) {
  if (g_forked.load()) check_can_run();
  const ActiveSession active;
  if (channel.channel == nullptr) throw py::value_error("fastmm: run() needs a SlowChannel");
  if (channel.channel->closed())
    throw py::value_error("fastmm: the SlowChannel belongs to a session that has ended");
  const std::shared_ptr<SlowChannel> ch = channel.channel;  // the session's reference
  const HotProgram hot = py_hot::program_from(program, false);
  std::unique_ptr<HotParamTable> table;
  try {
    table = std::make_unique<HotParamTable>(py_hot::param_fields_from(program), hot.param_bytes);
  } catch (const std::invalid_argument& e) {
    throw py::value_error(e.what());
  }

  live::LiveOptions opts;
  opts.config_path = path;
  opts.program = "fastmm";
  opts.duration_ns = option<std::int64_t>(options, "duration_ns", 0);
  opts.dry_run = option<bool>(options, "dry_run", false);
  opts.record_raw_dir = option<std::string>(options, "record_raw", "");
  opts.journal_path = option<std::string>(options, "journal", "");
  opts.no_journal = option<bool>(options, "no_journal", false);
  opts.status_path = option<std::string>(options, "status", "");
  opts.no_status = option<bool>(options, "no_status", false);
  const auto log_path = option<std::string>(options, "log", "");
  const bool allow_inline = option<bool>(options, "allow_inline_secrets", false);
  const auto max_param_age_ms = option<std::int64_t>(options, "max_param_age_ms", -1);
  ParamMap effective;
  for (const auto& [k, v] : params)
    effective[py::str(k).cast<std::string>()] = py::str(v).cast<std::string>();

  HotError error{};
  bool failed = false;
  std::uint64_t calls = 0;
  int rc = live::kExitRuntime;
  {
    const py::gil_scoped_release release;
    Config cfg;
    InstrumentTable instruments;
    rc = 0;
    try {
      cfg = load(path, allow_inline);
      instruments = load_instruments(cfg);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "fastmm: %s\n", e.what());
      rc = live::kExitConfig;
    }
    cfg.strategy.name = name;
    cfg.strategy.params = effective;  // the journal embeds what the session ran with
    if (max_param_age_ms >= 0) cfg.strategy.max_param_age_ms = max_param_age_ms;
    if (rc == 0 && ch->instruments() < instruments.size()) {
      std::fprintf(stderr, "fastmm: the slow channel is smaller than the instrument table\n");
      rc = live::kExitConfig;
    }
    if (rc == 0 && !live::resolve_venue_env(cfg, opts.dry_run, "fastmm")) rc = live::kExitUsage;

    // Every hook runs once on scratch data before any venue is contacted.
    if (rc == 0) {
      HotStrategy scratch;
      if (!scratch.attach(hot, instruments)) {
        std::fprintf(stderr, "fastmm: %s: invalid hot program\n", name.c_str());
        rc = live::kExitConfig;
      } else {
        scratch.warm_up(instruments);
      }
    }

    std::FILE* log_file = nullptr;
    if (rc == 0 && !log_path.empty()) {
      log_file = std::fopen(log_path.c_str(), "a");
      if (log_file == nullptr) {
        std::fprintf(stderr, "fastmm: cannot open log file %s\n", log_path.c_str());
        rc = live::kExitUsage;
      }
    }
    if (rc == 0) {
      Logger::instance().set_level(cfg.log_level());
      Logger::instance().start(log_file != nullptr ? log_file : stderr, cfg.mirror_level());
      for (const std::string& w : cfg.warnings) FASTMM_LOG_WARN("config: {}", w);

      HotStrategy* strategy = nullptr;
      live::LiveStrategy ls;
      ls.name = name;
      ls.params = &table->schema();
      ls.meta = meta;
      ls.inputs.push_back(&ch->param_ring());
      ls.set_waker = [ch](std::function<void()> wake) { ch->set_notify(std::move(wake)); };
      ls.make = [&](RunnerDeps& deps) {
        std::unique_ptr<IEngineRunner> runner =
            live::make_live_runner<HotStrategy>(TransportKind::Live, deps);
        if (runner == nullptr) return runner;
        auto* er = static_cast<EngineRunner<HotLiveEngine, HotStrategy>*>(runner.get());
        if (!er->strategy().attach(hot, er->engine().instruments()))
          throw std::invalid_argument(name + ": invalid hot program");
        if (slow && !er->strategy().attach_slow(ch.get()))
          throw std::invalid_argument("the slow channel is smaller than the instrument table");
        strategy = &er->strategy();
        return runner;
      };
      ls.finished = [&](IEngineRunner&) {
        if (strategy == nullptr) return;
        calls = strategy->calls();
        failed = strategy->failed();
        error = strategy->error();
      };
      opts.strategy = &ls;
      opts.confine_other_threads = true;
      if (slow) opts.watchdog = live::slow_tier_watchdog(ch, slow_tid);
      try {
        rc = live::run_live(cfg, opts);
      } catch (const std::exception& e) {
        FASTMM_LOG_ERROR("fastmm-live: fatal: {}", std::string_view(e.what()));
        rc = live::kExitRuntime;
      }
      Logger::instance().stop();
    }
    if (log_file != nullptr) std::fclose(log_file);
    ch->close();
  }

  py::object err = py::none();
  if (failed) {
    const HotError& e = error;
    py::dict d;
    d["status"] = e.status;
    d["fail_code"] = e.fail_code;
    d["hook"] = e.hook >= 0 ? std::string(to_string(static_cast<HotHook>(e.hook))) : std::string();
    d["timer"] = e.timer;
    d["now_ns"] = e.at.ns;
    err = std::move(d);
  }
  return py::make_tuple(rc, err, calls);
}

}  // namespace

void bind_session(py::module_& m) {
  py::class_<LiveSlowChannel> channel(
      m,
      "SlowChannel",
      "Internal: the channel between a live session's engine and the strategy's slow methods and "
      "publishers. publish() pushes a validated update onto the parameter ring the engine polls; "
      "it returns False once the session has ended.");
  channel.def(py::init([](std::vector<std::string> symbols,
                          std::size_t fills_capacity,
                          std::size_t recent_rows,
                          std::int64_t snapshot_interval_ns) {
                if (symbols.empty())
                  throw py::value_error("fastmm: a slow channel needs at least one instrument");
                SlowChannelConfig cc;
                cc.instruments = symbols.size();
                cc.fills_capacity = fills_capacity;
                cc.recent_rows = recent_rows;
                cc.snapshot_interval = Duration{snapshot_interval_ns};
                const std::size_t n = symbols.size();
                return LiveSlowChannel{std::make_shared<SlowChannel>(cc),
                                       std::move(symbols),
                                       std::vector<std::uint64_t>(n, 0)};
              }),
              py::arg("symbols"),
              py::arg("fills_capacity"),
              py::arg("recent_rows") = 4096,
              py::arg("snapshot_interval_ns") = 10'000'000);
  fastmm::py_bind::def_slow_channel(channel);

  m.def(
      "slow_fills_capacity",
      [](std::uint32_t orders_per_sec, std::int64_t longest_gap_ns) {
        return slow_fills_capacity(orders_per_sec, Duration{longest_gap_ns});
      },
      py::arg("orders_per_sec"),
      py::arg("longest_gap_ns"),
      "Internal: the fills ring capacity of a session (strategies/slow_channel.hpp).");

  m.def("_check_can_run",
        &check_can_run,
        "Internal: raises RuntimeError when a session runs in this process or it was forked during "
        "one.");
  m.def("load_config",
        &load_config,
        py::arg("path"),
        py::arg("allow_inline_secrets") = false,
        "Internal: {'strategy', 'params', 'instruments', 'warnings', 'orders_per_sec', "
        "'max_param_age_ms', 'reserved_cpus'} of a live configuration. Raises ValueError when it "
        "does not load.");
  m.def(
      "run",
      &run,
      py::arg("path"),
      py::arg("options"),
      py::arg("name"),
      py::arg("params"),
      py::arg("program"),
      py::arg("meta"),
      py::arg("channel"),
      py::arg("slow"),
      py::arg("slow_tid"),
      "Internal: runs a live session of a compiled hot strategy with the GIL released; use "
      "fastmm.run_live. With `slow` the engine feeds `channel` and the watchdog watches it and "
      "the thread `slow_tid` (0: none). Returns (exit code, hot hook error or None, hook calls).");
  m.def(
      "_after_fork_in_child",
      [] {
        if (!g_active.load()) return;
        g_forked.store(true);
        live::restore_signal_handlers();
      },
      "Internal (os.register_at_fork): a child forked while a session runs cannot run a session "
      "and gets the previous SIGINT/SIGTERM handlers back.");
  m.attr("EXIT_SLOW_TIER") = live::kExitSlowTier;
}

}  // namespace fastmm::py_live
