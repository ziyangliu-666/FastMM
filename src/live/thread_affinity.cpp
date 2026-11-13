#include "fastmm/live/thread_affinity.hpp"

#include "fastmm/core/host_tuning.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/live/venue_slot.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>

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

CpuLayout cpu_layout(const Config& cfg, int engine_cpu, bool network_threads) {
  CpuLayout l;
  l.engine = engine_cpu;
  l.aux = cfg.engine.aux_cpus;
  if (!network_threads) return l;
  const auto add = [](std::vector<int>& v, int c) {
    if (c >= 0 && std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c);
  };
  const std::span<const int> net(cfg.engine.net_cpus);
  const SpinMode spin = cfg.spin_mode();
  const bool dedicated = cfg.engine.net_spin_dedicated;
  for (const std::vector<std::size_t>& g :
       net_groups(engine_cpu, net, cfg.venues.size(), spin, dedicated)) {
    const std::size_t i = g.front();
    const int cpu = i < net.size() ? net[i] : -1;
    if (cpu < 0) continue;
    add(l.hot, cpu);
    if (g.size() > 1 || net_never_blocks(spin, cpu, net_cpu_shared(engine_cpu, net, i), dedicated))
      add(l.polling, cpu);
  }
  for (const CopyCore& c : md_copy_cores(cfg, engine_cpu)) {
    add(l.hot, c.cpu);
    if (c.never_block) add(l.polling, c.cpu);
  }
  return l;
}

AuxPlacement aux_cpus_for(std::span<const int> allowed, const CpuLayout& layout) {
  AuxPlacement out;
  const auto in = [](std::span<const int> v, int c) {
    return std::find(v.begin(), v.end(), c) != v.end();
  };
  const auto minus = [&](auto&& drop) {
    std::vector<int> v;
    for (const int c : allowed) {
      if (!drop(c)) v.push_back(c);
    }
    return v;
  };
  if (!layout.aux.empty()) {
    out.cpus = minus([&](int c) { return !in(layout.aux, c); });
    if (out.cpus.empty()) {
      out.warning = fmt::format(
          "[engine] aux_cpus [{}]: none of them is a CPU this process may use ({}); the other "
          "threads stay where they are",
          fmt::join(layout.aux, ", "),
          cpu_ranges(std::vector<int>(allowed.begin(), allowed.end())));
      return out;
    }
    std::vector<int> spin;
    for (const int c : out.cpus) {
      if (c == layout.engine || in(layout.polling, c)) spin.push_back(c);
    }
    if (!spin.empty())
      out.warning = fmt::format(
          "[engine] aux_cpus takes in CPU {} where the engine or a network thread polls; the "
          "other threads preempt it there",
          fmt::join(spin, ", "));
    return out;
  }
  out.cpus = minus([&](int c) { return c == layout.engine || in(layout.hot, c); });
  if (!out.cpus.empty()) return out;
  out.cpus = minus([&](int c) { return c == layout.engine || in(layout.polling, c); });
  if (out.cpus.empty()) out.cpus = minus([&](int c) { return c == layout.engine; });
  if (out.cpus.empty()) return out;  // only the engine's core: nothing to choose
  out.warning = fmt::format(
      "no CPU is free of the engine and the network threads ([engine] cpu, net_cpus, "
      "md_copy_cpus): the other threads share CPUs {} with {}; set [engine] aux_cpus or give the "
      "host more CPUs",
      cpu_ranges(out.cpus),
      std::any_of(out.cpus.begin(), out.cpus.end(), [&](int c) { return in(layout.polling, c); })
          ? "network threads that poll without blocking"
          : "network threads that block when idle");
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

void confine_other_threads(const CpuLayout& layout) {
  const std::vector<int> allowed = process_cpus();
  const AuxPlacement p = aux_cpus_for(allowed, layout);
  if (!p.warning.empty()) FASTMM_LOG_WARN("thread affinity: {}", std::string_view(p.warning));
  if (p.cpus.empty() || p.cpus == allowed) return;
  const ConfineResult r = confine_threads(p.cpus);
  if (!r.error.empty()) {
    FASTMM_LOG_WARN("thread affinity: {}", std::string_view(r.error));
    return;
  }
  const std::string list = cpu_ranges(p.cpus);
  FASTMM_LOG_INFO("thread affinity: {} thread(s) of this process moved to CPUs {}",
                  r.threads,
                  std::string_view(list));
}

}  // namespace fastmm::live
