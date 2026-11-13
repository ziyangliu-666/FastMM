#pragma once
// Keeps a process's other threads off the CPUs its hot threads are pinned to. run_live and
// fastmm-gateway call confine_other_threads before the hot threads start; threads started later
// inherit the mask, and the resolver helpers take the main thread's (leave_pinned_cpus).
#include "fastmm/config/config.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace fastmm::live {

// Where a session's hot threads sit: the engine's core (-1: not pinned, or not in this process),
// the cores of the pinned network and market-data copy threads, those of them whose thread never
// blocks (busy polling: net_never_blocks, md_copy_cores) and [engine] aux_cpus.
struct CpuLayout {
  int engine = -1;
  std::vector<int> hot;
  std::vector<int> polling;
  std::vector<int> aux;
};
// The layout of `cfg` with the engine on `engine_cpu`; `network_threads` false when the engine
// thread runs the network loop (threading = "single": net_cpus and md_copy_cpus are not used).
[[nodiscard]] CpuLayout cpu_layout(const Config& cfg, int engine_cpu, bool network_threads);

// The cores the other threads (journal, store, audit, treasury, resolvers, main) go to, out of
// `allowed`, and a warning to log when the choice is a compromise:
//   aux set      aux ∩ allowed (a warning when it takes in the engine's or a polling core; none
//                left: empty, with a warning);
//   otherwise    allowed minus the engine's and the hot cores; when those cover `allowed`, minus
//                the engine's and the polling cores, or, when that leaves nothing too, minus the
//                engine's alone, with a warning either way.
// `allowed` itself when nothing is pinned in it.
struct AuxPlacement {
  std::vector<int> cpus;
  std::string warning;
};
[[nodiscard]] AuxPlacement aux_cpus_for(std::span<const int> allowed, const CpuLayout& layout);

// The CPUs a thread of this process may use: the union of their affinities.
[[nodiscard]] std::vector<int> process_cpus();

struct ConfineResult {
  std::size_t threads = 0;  // threads whose affinity was set
  std::string error;
};

// Sets the affinity of every thread in /proc/self/task to `cpus`. A thread that exits meanwhile
// is skipped.
[[nodiscard]] ConfineResult confine_threads(std::span<const int> cpus);

// confine_threads(aux_cpus_for(process_cpus(), layout)), its warning and a log line; nothing when
// no CPU is pinned.
void confine_other_threads(const CpuLayout& layout);

}  // namespace fastmm::live
