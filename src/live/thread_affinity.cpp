#include "fastmm/live/thread_affinity.hpp"

#include <sched.h>
#include <sys/types.h>

#include <cerrno>
#include <charconv>
#include <filesystem>
#include <string>
#include <system_error>

namespace fastmm::live {

ConfineResult confine_threads(std::span<const int> reserved) {
  ConfineResult out;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof mask, &mask) != 0) {
    out.error = "thread affinity unchanged: sched_getaffinity failed: " +
                std::error_code(errno, std::generic_category()).message();
    return out;
  }
  bool any = false;
  for (const int c : reserved) {
    if (c < 0 || c >= CPU_SETSIZE) continue;
    CPU_CLR(static_cast<std::size_t>(c), &mask);
    any = true;
  }
  if (!any) return out;
  for (int c = 0; c < CPU_SETSIZE; ++c) {
    if (CPU_ISSET(static_cast<std::size_t>(c), &mask)) out.cpus.push_back(c);
  }
  if (out.cpus.empty()) {
    out.error =
        "thread affinity unchanged: every CPU this process may use is pinned in [engine] "
        "cpu or net_cpus";
    return out;
  }
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    const std::string name = entry.path().filename().string();
    pid_t tid = 0;
    const auto [end, parse_ec] = std::from_chars(name.data(), name.data() + name.size(), tid);
    if (parse_ec != std::errc{} || end != name.data() + name.size()) continue;
    if (sched_setaffinity(tid, sizeof mask, &mask) == 0) ++out.threads;
  }
  if (ec) {
    out.error = "thread affinity: cannot list /proc/self/task: " + ec.message();
    out.cpus.clear();
  }
  return out;
}

}  // namespace fastmm::live
