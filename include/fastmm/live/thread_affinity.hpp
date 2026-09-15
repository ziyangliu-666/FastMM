#pragma once
// confine_threads(): keeps a process's other threads off the CPUs a live session pins (ADR-0013,
// section 3). fastmm_live._live calls it through LiveOptions::confine_other_threads before the
// engine and venue threads start, so Python, numpy and BLAS threads do not share the engine's
// core; threads started later inherit the mask.
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace fastmm::live {

struct ConfineResult {
  std::size_t threads = 0;  // threads whose affinity was set
  std::vector<int> cpus;    // the CPUs they may use; empty when nothing changed
  std::string error;        // why nothing changed although CPUs were reserved
};

// Sets the affinity of every thread in /proc/self/task to the calling thread's CPUs minus
// `reserved` (negative entries are ignored). Without a reserved CPU nothing changes. When every CPU
// is reserved nothing changes and `error` says so. A thread that exits meanwhile is skipped.
[[nodiscard]] ConfineResult confine_threads(std::span<const int> reserved);

}  // namespace fastmm::live
