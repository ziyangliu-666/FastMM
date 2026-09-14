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
//                within ~5 s
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

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitConfig = 3;
inline constexpr int kExitVenue = 4;
inline constexpr int kExitRuntime = 5;

// Runs one session; returns the process exit code. `cfg` must already have its venue secrets
// resolved (or cleared for dry-run) and any command-line overrides applied: the journal embeds
// cfg.effective_toml() and hashes it.
int run_live(const Config& cfg, const LiveOptions& opts);

}  // namespace fastmm::live
