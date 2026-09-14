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
// The thread set is fixed for the whole session (the logger keeps a ring per thread). The strategy
// is built by the StrategyRegistry's TransportKind::Live factory for [strategy] name; register it
// (register_builtin_strategies, a strategy module) before calling run_live. run_live installs
// process-wide SIGINT/SIGTERM handlers.
#include "fastmm/config/config.hpp"

#include <cstdint>
#include <string>

namespace fastmm::live {

struct LiveOptions {
  std::string config_path;
  std::int64_t duration_ns = 0;  // 0 = until SIGINT/SIGTERM
  bool dry_run = false;
  std::string record_raw_dir;
  std::string journal_path;  // overrides [engine] journal_dir
  bool no_journal = false;
  std::string status_path;  // overrides the default /dev/shm/fastmm-<engine>.status
  bool no_status = false;
  std::string program = "fastmm-live";  // prefix of error messages (log lines keep fastmm-live:)
};

// Process exit codes (listed in fastmm-live --help and docs/how-to/operations/). A failed
// cancel-all wins over every other outcome: orders may still be resting.
inline constexpr int kExitOk = 0;       // --duration elapsed or SIGINT/SIGTERM, cancel_all ok
inline constexpr int kExitUsage = 2;    // bad command line, missing API keys
inline constexpr int kExitConfig = 3;   // bad config, strategy or parameters
inline constexpr int kExitVenue = 4;    // venue reference data failed
inline constexpr int kExitRuntime = 5;  // cancel_all failed, journal, ring overflow, uncaught error
inline constexpr int kExitKilled = 6;   // engine-tripped kill switch with [engine] on_kill = "exit"

// How often a session that stays up after a kill ([engine] on_kill = "stay") repeats its ERROR.
inline constexpr std::int64_t kKilledReminderNs = 10'000'000'000;

// Runs one session; returns the process exit code. `cfg` must already have its venue secrets
// resolved (or cleared for dry-run) and any command-line overrides applied: the journal embeds
// cfg.effective_toml() and hashes it.
int run_live(const Config& cfg, const LiveOptions& opts);

}  // namespace fastmm::live
