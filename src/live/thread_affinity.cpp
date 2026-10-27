#include "fastmm/live/thread_affinity.hpp"

#include "fastmm/core/host_tuning.hpp"
#include "fastmm/core/log.hpp"

#include <sched.h>
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace fastmm::live {
namespace {

// The thread ids in /proc/self/task; `ec` is set when the directory cannot be listed.
std::vector<pid_t> task_ids(std::error_code& ec) {
  std::vector<pid_t> out;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    const std::string name = entry.path().filename().string();
    pid_t tid = 0;
    const auto [end, parse_ec] = std::from_chars(name.data(), name.data() + name.size(), tid);
    if (parse_ec == std::errc{} && end == name.data() + name.size()) out.push_back(tid);
  }
  return out;
}

}  // namespace

std::vector<int> spare_cpus(std::span<const int> allowed,
                            int engine_cpu,
                            std::span<const int> net_cpus) {
  const auto hot = [&](int c) {
    return c == engine_cpu || std::find(net_cpus.begin(), net_cpus.end(), c) != net_cpus.end();
  };
  std::vector<int> out;
  for (const int c : allowed) {
    if (!hot(c)) out.push_back(c);
  }
  if (!out.empty() || engine_cpu < 0) return out;
  for (const int c : allowed) {
    if (c != engine_cpu) out.push_back(c);
  }
  return out;
}

std::vector<int> process_cpus() {
  cpu_set_t all;
  CPU_ZERO(&all);
  std::error_code ec;
  for (const pid_t tid : task_ids(ec)) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(tid, sizeof mask, &mask) == 0) CPU_OR(&all, &all, &mask);
  }
  if (CPU_COUNT(&all) == 0 && sched_getaffinity(0, sizeof all, &all) != 0) return {};
  std::vector<int> out;
  for (int c = 0; c < CPU_SETSIZE; ++c) {
    if (CPU_ISSET(static_cast<std::size_t>(c), &all)) out.push_back(c);
  }
  return out;
}

ConfineResult confine_threads(std::span<const int> cpus) {
  ConfineResult out;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  for (const int c : cpus) {
    if (c >= 0 && c < CPU_SETSIZE) CPU_SET(static_cast<std::size_t>(c), &mask);
  }
  if (CPU_COUNT(&mask) == 0) {
    out.error = "no CPU to move the threads to";
    return out;
  }
  std::error_code ec;
  for (const pid_t tid : task_ids(ec)) {
    if (sched_setaffinity(tid, sizeof mask, &mask) == 0) ++out.threads;
  }
  if (ec) out.error = "cannot list /proc/self/task: " + ec.message();
  return out;
}

void confine_other_threads(int engine_cpu, std::span<const int> net_cpus) {
  const std::vector<int> allowed = process_cpus();
  const std::vector<int> spare = spare_cpus(allowed, engine_cpu, net_cpus);
  if (spare == allowed) return;  // nothing pinned among them
  if (spare.empty()) {
    FASTMM_LOG_WARN(
        "thread affinity unchanged: every CPU this process may use is pinned in [engine] cpu or "
        "net_cpus");
    return;
  }
  const ConfineResult r = confine_threads(spare);
  if (!r.error.empty()) {
    FASTMM_LOG_WARN("thread affinity: {}", std::string_view(r.error));
    return;
  }
  const std::string list = cpu_ranges(spare);
  FASTMM_LOG_INFO("thread affinity: {} thread(s) of this process moved to CPUs {}",
                  r.threads,
                  std::string_view(list));
}

}  // namespace fastmm::live
