#pragma once
// run_live(): wires venues, rings, the engine and the journal into the plan 5.1 thread model and
// runs a session (fastmm::live, src/live/session.cpp). fastmm::cli::live (cli/live.hpp) is the
// command line around it.
//
//   fm-net-<i>   one per venue: net::Reactor loop (connections, parsing into the venue's md and
//                order rings, draining the venue's outbound ring when the engine wakes it)
//   fm-engine    Engine<S, TscClock, LiveTransport, RingFeed>::run() (pinned / spin mode from
//                [engine] cpu / spin_mode)
//   fm-journal   JournalFileWriter drain thread (when journaling)
//   log sink     Logger::start(): formats and writes log records from every thread's ring
//   main thread  control: posts Venue::on_timer every second, prints stats, recalibrates the
//                TSC every [engine] tsc_recalibrate_s and publishes it (the engine and the
//                venues pick it up), handles SIGINT/SIGTERM and --duration: kill switch ->
//                cancel_all on every venue over an independent REST connection -> stop, all
//                within ~5 s. It also watches the engine's kill switch: a kill the engine trips
//                itself (risk limit, full ring, every venue killed) runs the same shutdown and
//                exits with kExitKilled when [engine] on_kill = "exit" (the default); with "stay"
//                the session keeps running and logs an ERROR line every 10 s. Venue kills are
//                logged once per venue.
//
// [engine] threading = "single" (one venue): no fm-net-0; fm-engine runs the venue's reactor
// iteration between engine steps (Engine::run_inline), takes each market-data event as the venue
// commits it (EventSink drain hook -> Engine::drain) and hands its orders to Venue::send_now.
//
// The thread set is fixed for the whole session (the logger keeps a ring per thread). The strategy
// is built by the StrategyRegistry's TransportKind::Live factory for [strategy] name; register it
// (register_builtin_strategies, a strategy module) before calling run_live, or pass a
// LiveStrategy (fastmm_live._live runs Python hot strategies that way). run_live installs
// process-wide SIGINT/SIGTERM handlers and restores the previous ones when it returns.
#include "fastmm/config/config.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fastmm {
class IEngineRunner;
class MsgRing;
class ParamSchema;
struct RunnerDeps;
}  // namespace fastmm

namespace fastmm::live {

// A strategy the caller builds instead of the registry's [strategy] name.
struct LiveStrategy {
  std::string name;                     // journal header, status file and log lines
  const ParamSchema* params = nullptr;  // the journal's parameter table
  std::string meta;                     // the journal's strategy metadata (`key=value` lines)
  // Rings the engine polls besides the venues' (parameter updates); the caller keeps them alive.
  std::vector<MsgRing*> inputs;
  // Called with a function that wakes the engine, before the session's threads start, and with an
  // empty one before the session returns. With spin_mode = "adaptive" an idle engine blocks; a
  // producer on `inputs` calls the function after each push, or its message waits up to 1 ms.
  std::function<void(std::function<void()> wake)> set_waker;
  // Builds the runner on `deps` (deps.backend is a LiveBackend) on the calling thread, after the
  // venues' reference data has loaded and before any session thread starts. An exception or
  // nullptr gives kExitConfig.
  std::function<std::unique_ptr<IEngineRunner>(RunnerDeps& deps)> make;
  // Called after the engine thread has stopped, before the runner is destroyed.
  std::function<void(IEngineRunner& runner)> finished;
  // The control socket's `param`: validates the (name, value) pairs against this strategy's schema
  // and publishes them onto one of `inputs` (the caller owns the publisher, so its copy of the
  // parameters stays the only one). Returns the error message, empty on success. Unset: the
  // session answers `param` with an error.
  std::function<std::string(const std::vector<std::pair<std::string, std::string>>&, InstrumentId)>
      set_params;
};

struct LiveOptions {
  std::string config_path;
  std::int64_t duration_ns = 0;  // 0 = until SIGINT/SIGTERM
  bool dry_run = false;
  std::string record_raw_dir;
  std::string journal_path;  // overrides [engine] journal_dir
  bool no_journal = false;
  std::string status_path;  // overrides the default /dev/shm/fastmm-<engine>.status
  bool no_status = false;
  // Control socket (live/control_socket.hpp); empty takes <journal_dir>/<engine name>.ctl.
  std::string control_path;
  bool no_control = false;
  // Removes the durable kill state before starting: clears a latched max-loss trip and arms the
  // whole [risk] max_loss budget again ([engine] kill_file).
  bool clear_kill = false;
  std::string program = "fastmm-live";     // prefix of error messages (log lines keep fastmm-live:)
  const LiveStrategy* strategy = nullptr;  // nullptr: [strategy] name from the registry
  // Called by the control thread every 50 ms; must not block. A non-empty result stops the session
  // like SIGTERM (kill switch, cancel_all) with kExitSlowTier and is logged as the cause.
  std::function<std::string()> watchdog;
  // Before the session starts its threads, set every thread of the process to the CPUs not listed
  // in [engine] cpu and net_cpus (confine_threads, live/thread_affinity.hpp).
  bool confine_other_threads = false;
};

// Process exit codes (listed in fastmm-live --help and docs/how-to/operations/). A failed
// cancel-all wins over every other outcome: orders may still be resting.
inline constexpr int kExitOk = 0;       // --duration elapsed or SIGINT/SIGTERM, cancel_all ok
inline constexpr int kExitUsage = 2;    // bad command line, missing API keys
inline constexpr int kExitConfig = 3;   // bad config, strategy or parameters
inline constexpr int kExitVenue = 4;    // venue reference data failed
inline constexpr int kExitRuntime = 5;  // cancel_all failed, journal, ring overflow, uncaught error
inline constexpr int kExitKilled = 6;   // engine-tripped kill switch with [engine] on_kill = "exit"
inline constexpr int kExitSlowTier = 7;  // LiveOptions::watchdog (a Python slow tier failed)

// How often a session that stays up after a kill ([engine] on_kill = "stay") repeats its ERROR.
inline constexpr std::int64_t kKilledReminderNs = 10'000'000'000;

// Runs one session; returns the process exit code. `cfg` must already have its venue secrets
// resolved (or cleared for dry-run) and any command-line overrides applied: the journal embeds
// cfg.effective_toml() and hashes it.
int run_live(const Config& cfg, const LiveOptions& opts);

// Resolves ${VAR} in every venue string. With `dry_run`, an unset variable in api_key or api_secret
// clears the key; anything else unset, or a venue without keys outside a dry run, prints an error
// prefixed with `prog` to stderr and returns false (kExitUsage).
bool resolve_venue_env(Config& cfg, bool dry_run, const char* prog);

// Restores the SIGINT/SIGTERM handlers run_live replaced; a no-op when none are replaced. For a
// child forked during a session, whose control thread does not exist.
void restore_signal_handlers() noexcept;

}  // namespace fastmm::live
