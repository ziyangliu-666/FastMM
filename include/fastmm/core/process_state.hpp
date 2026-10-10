#pragma once
// The resource state of a running session's process, read from /proc by a monitor (fastmm-top):
// per thread the CPU it last ran on, its CPU time, its context switches (involuntary ones mean
// another task took the core: a latency spike that is not FastMM's) and page faults; for the
// process its resident memory. The session itself is not involved: nothing is published, and the
// numbers are the kernel's own. Linux only; elsewhere `ok` is false.
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm {

struct ThreadState {
  std::int32_t tid = 0;
  std::string name;          // comm: fm-engine, fm-store, the venue threads, ...
  char state = '?';          // R running, S sleeping, D waiting on I/O, ...
  std::int32_t cpu = -1;     // the CPU it last ran on
  std::string allowed_cpus;  // Cpus_allowed_list: its affinity
  double cpu_seconds = 0.0;  // user + system
  std::uint64_t voluntary_switches = 0;
  std::uint64_t involuntary_switches = 0;
  std::uint64_t minor_faults = 0;
  std::uint64_t major_faults = 0;
};

struct ProcessState {
  bool ok = false;
  std::string error;  // why not ok
  std::int32_t pid = 0;
  std::int64_t sampled_ns = 0;  // wall clock of the read
  std::uint64_t resident_bytes = 0;
  std::uint64_t resident_peak_bytes = 0;
  std::uint32_t online_cpus = 0;
  double load1 = 0.0;  // the host's 1-minute load average
  std::vector<ThreadState> threads;
};

// Reads /proc/<pid>. `started_ns` (wall clock, 0: not checked) is the session's start as its
// status file says: a process of that pid that started more than 10 s away from it is another
// process (the session ended and the pid was reused, or the file comes from another host or pid
// namespace) and is refused.
[[nodiscard]] ProcessState read_process_state(std::int32_t pid, std::int64_t started_ns = 0);

// A table of the threads, with CPU use and context switches per second since `prev` when it is
// the same process (nullptr: totals only).
[[nodiscard]] std::string format_process_state(const ProcessState& now, const ProcessState* prev);
// `"process": {...}` for a JSON object; cumulative counters, for the reader to difference.
[[nodiscard]] std::string process_state_json(const ProcessState& s);
// fastmm_thread_* and fastmm_process_* samples in the Prometheus text format.
[[nodiscard]] std::string process_state_prometheus(const ProcessState& s);

}  // namespace fastmm
