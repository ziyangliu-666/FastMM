#pragma once
// LiveBackend: wires venues, rings, the engine and the journal into the plan 5.1 thread model
// and runs a session.
//
//   fm-net-<i>   one per venue: net::Reactor loop (connections, parsing into the venue's md and
//                order rings, draining the venue's outbound ring when the engine wakes it)
//   fm-engine    Engine<S, TscClock, LiveTransport, RingFeed>::run() (pinned / spin mode from
//                [engine] cpu / spin_mode)
//   fm-journal   JournalFileWriter drain thread (when journaling)
//   main thread  control: posts Venue::on_timer every second, prints stats, handles
//                SIGINT/SIGTERM and --duration: kill switch -> cancel_all on every venue over
//                an independent REST connection -> stop, all within ~5 s
//
// The thread set is fixed for the whole session (the logger keeps a ring per thread).
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
};

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitConfig = 3;
inline constexpr int kExitVenue = 4;
inline constexpr int kExitRuntime = 5;

// Runs one session; returns the process exit code. `cfg` must already have its venue
// secrets resolved (or cleared for dry-run).
int run_live(const Config& cfg, const LiveOptions& opts);

}  // namespace fastmm::live
