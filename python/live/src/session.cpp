// Live sessions of Python hot strategies (ADR-0013, section 3), used by fastmm.run_live.
//
//   load_config(path, allow_inline_secrets)   [strategy] section, instruments, warnings
//   ParamChannel(fields, param_bytes, record, instruments, ring_bytes)
//                                             the parameter ring and its ParamPublisher
//   run(path, options, name, params, program, meta, channel) -> (exit code, hot error, calls)
//
// run() starts fastmm::live::run_live with a HotStrategy built from the compiled program and the
// channel's ring as an engine input, with the GIL released. Hooks are warmed up on a scratch
// strategy before any venue is contacted. One session runs per process; a child forked while a
// session runs is inert (_after_fork_in_child).
#include "session.hpp"

#include "hot_program.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/live/live_backend.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/strategies/hot_params.hpp"
#include "fastmm/strategies/hot_strategy.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::py_live {

namespace {

namespace py = pybind11;

using HotLiveEngine = Engine<HotStrategy, TscClock, LiveTransport, RingFeed>;

std::atomic<bool> g_active{false};  // a session runs in this process
std::atomic<bool> g_forked{false};  // forked while a session ran: sessions and publishes are off

// Set by _fail_slow_tier; the session's watchdog reads it (exit code 7).
std::mutex g_slow_mutex;
std::string g_slow_cause;

class ParamChannel {
 public:
  ParamChannel(std::vector<HotParamField> fields,
               std::size_t param_bytes,
               std::span<const std::uint8_t> block,
               std::size_t instruments,
               std::size_t ring_bytes)
      : table_(std::move(fields), param_bytes),
        ring_(std::bit_ceil(std::max<std::size_t>(ring_bytes, 1U << 16))),
        publisher_(table_.publisher(ParamSink::to_ring(ring_), block, instruments)) {}

  bool publish(const std::vector<ParamPublisher::ParamValue>& values, InstrumentId inst) {
    if (g_forked.load(std::memory_order_relaxed)) return false;
    return publisher_->publish(values, inst);
  }
  void close() { publisher_->close(); }
  [[nodiscard]] const ParamPublisher& publisher() const noexcept { return *publisher_; }
  [[nodiscard]] const HotParamTable& table() const noexcept { return table_; }
  [[nodiscard]] MsgRing& ring() noexcept { return ring_; }

 private:
  HotParamTable table_;
  MsgRing ring_;
  std::unique_ptr<ParamPublisher> publisher_;
};

std::string value_string(const py::handle& v, const std::string& name) {
  if (py::isinstance<py::bool_>(v)) return v.cast<bool>() ? "true" : "false";
  if (py::isinstance<py::int_>(v)) return py::str(v).cast<std::string>();
  if (py::isinstance<py::float_>(v)) return py::repr(v).cast<std::string>();
  if (py::isinstance<py::str>(v)) return v.cast<std::string>();
  throw py::type_error("fastmm: parameter '" + name + "': values must be str, int, float or bool");
}

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
  return out;
}

py::tuple run(const std::string& path,
              const py::dict& options,
              const std::string& name,
              const py::dict& params,
              const py::dict& program,
              const std::string& meta,
              const std::shared_ptr<ParamChannel>& channel) {
  if (g_forked.load()) {
    throw std::runtime_error(
        "fastmm: this process was forked while a live session was running and cannot run one; "
        "start processes with the spawn or forkserver method");
  }
  const ActiveSession active;
  {
    const std::lock_guard<std::mutex> lock(g_slow_mutex);
    g_slow_cause.clear();
  }
  if (channel == nullptr) throw py::value_error("fastmm: run() needs a ParamChannel");
  HotProgram hot = py_hot::program_from(program, false);
  if (hot.param_bytes != channel->table().param_bytes())
    throw py::value_error(
        "fastmm: the ParamChannel and the program disagree on the parameter block");
  hot.params = channel->table().slots();

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
  ParamMap effective;
  for (const auto& [k, v] : params)
    effective[py::str(k).cast<std::string>()] = py::str(v).cast<std::string>();

  std::optional<HotError> error;
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
    if (rc == 0 && !live::resolve_venue_env(cfg, opts.dry_run, "fastmm")) rc = live::kExitUsage;

    // Every hook runs once on scratch data before any venue is contacted.
    if (rc == 0) {
      HotStrategy scratch;
      if (!scratch.attach(hot, instruments)) {
        std::fprintf(stderr, "fastmm: %s: invalid hot program\n", name.c_str());
        rc = live::kExitConfig;
      } else {
        scratch.warm_up(instruments);
        rc = 0;
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
      ls.params = &channel->table().schema();
      ls.meta = meta;
      ls.inputs.push_back(&channel->ring());
      ls.make = [&](RunnerDeps& deps) {
        std::unique_ptr<IEngineRunner> runner =
            live::make_live_runner<HotStrategy>(TransportKind::Live, deps);
        if (runner == nullptr) return runner;
        auto* er = static_cast<EngineRunner<HotLiveEngine, HotStrategy>*>(runner.get());
        if (!er->strategy().attach(hot, er->engine().instruments()))
          throw std::invalid_argument(name + ": invalid hot program");
        strategy = &er->strategy();
        return runner;
      };
      ls.finished = [&](IEngineRunner&) {
        if (strategy == nullptr) return;
        calls = strategy->calls();
        if (strategy->failed()) error = strategy->error();
      };
      opts.strategy = &ls;
      opts.confine_other_threads = true;
      opts.watchdog = [] {
        const std::lock_guard<std::mutex> lock(g_slow_mutex);
        return g_slow_cause;
      };
      try {
        rc = live::run_live(cfg, opts);
      } catch (const std::exception& e) {
        FASTMM_LOG_ERROR("fastmm-live: fatal: {}", std::string_view(e.what()));
        rc = live::kExitRuntime;
      }
      Logger::instance().stop();
    }
    if (log_file != nullptr) std::fclose(log_file);
    channel->close();
  }

  py::object err = py::none();
  if (error.has_value()) {
    const HotError& e = error.value();
    py::dict d;
    d["status"] = e.status;
    d["fail_code"] = e.fail_code;
    d["hook"] =
        e.hook >= 0 ? std::string(to_string(static_cast<HotHook>(e.hook))) : std::string();
    d["timer"] = e.timer;
    d["now_ns"] = e.at.ns;
    err = std::move(d);
  }
  return py::make_tuple(rc, err, calls);
}

}  // namespace

void bind_session(py::module_& m) {
  py::class_<ParamChannel, std::shared_ptr<ParamChannel>>(
      m,
      "ParamChannel",
      "The parameter ring of a live session and its publisher: publish() validates an update and "
      "pushes it for the engine, which applies it at one event and journals it.")
      .def(py::init([](const py::list& fields,
                       std::size_t param_bytes,
                       const py::bytes& record,
                       std::size_t instruments,
                       std::size_t ring_bytes) {
             py::dict program;
             program["param_fields"] = fields;
             const auto bytes = record.cast<std::string>();
             try {
               return std::make_shared<ParamChannel>(
                   py_hot::param_fields_from(program),
                   param_bytes,
                   std::span<const std::uint8_t>(
                       reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()),
                   instruments,
                   ring_bytes);
             } catch (const std::invalid_argument& e) {
               throw py::value_error(e.what());
             }
           }),
           py::arg("fields"),
           py::arg("param_bytes"),
           py::arg("record"),
           py::arg("instruments"),
           py::arg("ring_bytes") = 1U << 16)
      .def(
          "publish",
          [](ParamChannel& c, const py::dict& values, std::optional<std::int64_t> instrument) {
            std::vector<ParamPublisher::ParamValue> v;
            v.reserve(values.size());
            for (const auto& [k, value] : values) {
              const auto key = py::str(k).cast<std::string>();
              v.emplace_back(key, value_string(value, key));
            }
            InstrumentId inst = ParamPublisher::kAllInstruments;
            if (instrument) {
              if (*instrument < 0 || *instrument > 0xFFFF'FFFELL)
                throw py::value_error("fastmm: instrument " + std::to_string(*instrument) +
                                      " is not in the instrument table");
              inst = InstrumentId{static_cast<std::uint32_t>(*instrument)};
            }
            std::string why;
            bool ok = false;
            {
              const py::gil_scoped_release release;
              try {
                ok = c.publish(v, inst);
              } catch (const std::invalid_argument& e) {
                why = e.what();
              }
            }
            if (!why.empty()) throw py::value_error(why);
            return ok;
          },
          py::arg("values"),
          py::arg("instrument") = py::none(),
          "Validates {name: value} (str, int, float or bool) and pushes one update for every "
          "instrument, or for one instrument id. Raises ValueError when it is invalid; returns "
          "False when the ring is full or the session has stopped.")
      .def("close", &ParamChannel::close, "Later publishes return False.")
      .def_property_readonly("closed", [](const ParamChannel& c) { return c.publisher().closed(); })
      .def_property_readonly("published",
                             [](const ParamChannel& c) { return c.publisher().published(); })
      .def_property_readonly("refused",
                             [](const ParamChannel& c) { return c.publisher().refused(); })
      .def_property_readonly("names",
                             [](const ParamChannel& c) {
                               std::vector<std::string> out;
                               for (const HotParamField& f : c.table().fields())
                                 out.push_back(f.name);
                               return out;
                             })
      .def(
          "describe",
          [](const ParamChannel& c, std::optional<std::uint32_t> instrument) {
            return c.publisher().describe(instrument ? InstrumentId{*instrument}
                                                     : ParamPublisher::kAllInstruments);
          },
          py::arg("instrument") = py::none(),
          "The publisher's copy of the parameters as name=value pairs.");

  m.def("load_config",
        &load_config,
        py::arg("path"),
        py::arg("allow_inline_secrets") = false,
        "Internal: {'strategy', 'params', 'instruments', 'warnings'} of a live configuration. "
        "Raises ValueError when it does not load.");
  m.def("run",
        &run,
        py::arg("path"),
        py::arg("options"),
        py::arg("name"),
        py::arg("params"),
        py::arg("program"),
        py::arg("meta"),
        py::arg("channel"),
        "Internal: runs a live session of a compiled hot strategy with the GIL released; use "
        "fastmm.run_live. Returns (exit code, hot hook error or None, hook calls).");
  m.def(
      "_fail_slow_tier",
      [](const std::string& cause) {
        const std::lock_guard<std::mutex> lock(g_slow_mutex);
        g_slow_cause = cause.empty() ? std::string("slow tier failed") : cause;
      },
      py::arg("cause"),
      "Internal: stop the running session like SIGTERM with exit code 7, logging `cause`.");
  m.def(
      "_after_fork_in_child",
      [] {
        if (!g_active.load()) return;
        g_forked.store(true);
        live::restore_signal_handlers();
      },
      "Internal (os.register_at_fork): a child forked while a session runs cannot run a session "
      "or publish, and gets the previous SIGINT/SIGTERM handlers back.");
  m.attr("EXIT_SLOW_TIER") = live::kExitSlowTier;
}

}  // namespace fastmm::py_live
