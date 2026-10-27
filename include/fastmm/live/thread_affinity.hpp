#pragma once
// Keeps a process's other threads off the CPUs its hot threads are pinned to. run_live and
// fastmm-gateway call confine_other_threads before the hot threads start; threads started later
// inherit the mask, and the resolver helpers take the main thread's (leave_pinned_cpus).
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace fastmm::live {

// The CPUs the other threads may use: `allowed` minus the engine's and the network threads' CPUs
// (a negative entry is not pinned). When those cover `allowed`, `allowed` minus the engine's CPU
// alone, the thread a preemption costs most. `allowed` itself when none of them is in it; empty
// when nothing is left.
[[nodiscard]] std::vector<int> spare_cpus(std::span<const int> allowed,
                                          int engine_cpu,
                                          std::span<const int> net_cpus);

// The CPUs a thread of this process may use: the union of their affinities.
[[nodiscard]] std::vector<int> process_cpus();

struct ConfineResult {
  std::size_t threads = 0;  // threads whose affinity was set
  std::string error;
};

// Sets the affinity of every thread in /proc/self/task to `cpus`. A thread that exits meanwhile
// is skipped.
[[nodiscard]] ConfineResult confine_threads(std::span<const int> cpus);

// confine_threads(spare_cpus(process_cpus(), ...)) and a log line; nothing when no CPU is pinned.
void confine_other_threads(int engine_cpu, std::span<const int> net_cpus);

}  // namespace fastmm::live
