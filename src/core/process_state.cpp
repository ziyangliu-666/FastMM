#include "fastmm/core/process_state.hpp"

#include "fastmm/core/time.hpp"

#include <fmt/format.h>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fastmm {

namespace {

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// "Key:\tvalue" lines of a /proc status file.
std::map<std::string, std::string> status_fields(const std::string& path) {
  std::map<std::string, std::string> out;
  std::istringstream in(slurp(path));
  for (std::string line; std::getline(in, line);) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::size_t v = colon + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
    out[line.substr(0, colon)] = line.substr(v);
  }
  return out;
}

std::uint64_t number(const std::map<std::string, std::string>& f, const char* key) {
  const auto it = f.find(key);
  return it == f.end() ? 0 : std::strtoull(it->second.c_str(), nullptr, 10);
}

// The fields of a stat line after the command (which may hold spaces and parentheses):
// fields[0] is the state (field 3 of proc(5)).
std::vector<std::string> stat_fields(const std::string& stat) {
  std::vector<std::string> out;
  const std::size_t close = stat.rfind(')');
  if (close == std::string::npos) return out;
  std::istringstream in(stat.substr(close + 1));
  for (std::string f; in >> f;) out.push_back(f);
  return out;
}

// proc(5) field n (1-based) from stat_fields().
const std::string& field(const std::vector<std::string>& f, std::size_t n) {
  static const std::string kEmpty;
  return n >= 3 && n - 3 < f.size() ? f[n - 3] : kEmpty;
}

std::string json_text(std::string_view v) {
  std::string out = "\"";
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}

std::string label(std::string_view v) {
  std::string out;
  for (const char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    if (c != '\n') out += c;
  }
  return out;
}

}  // namespace

ProcessState read_process_state(std::int32_t pid, std::int64_t started_ns) {
  ProcessState s;
  s.pid = pid;
  s.sampled_ns = wall_now().ns;
  const std::string dir = "/proc/" + std::to_string(pid);
  const std::vector<std::string> stat = stat_fields(slurp(dir + "/stat"));
  if (pid <= 0 || stat.empty()) {
    s.error = "no process " + std::to_string(pid) + " here";
    return s;
  }
  const long hz = ::sysconf(_SC_CLK_TCK);
  const double tick = hz > 0 ? 1.0 / static_cast<double>(hz) : 0.01;
  if (started_ns != 0) {
    // Field 22: start time in ticks since boot; /proc/stat btime: boot time in seconds.
    std::uint64_t btime = 0;
    std::istringstream in(slurp("/proc/stat"));
    for (std::string line; std::getline(in, line);) {
      if (line.rfind("btime ", 0) == 0) btime = std::strtoull(line.c_str() + 6, nullptr, 10);
    }
    const double start_s =
        static_cast<double>(btime) +
        static_cast<double>(std::strtoull(field(stat, 22).c_str(), nullptr, 10)) * tick;
    if (btime != 0 && std::abs(start_s - static_cast<double>(started_ns) / 1e9) > 10.0) {
      s.error = "process " + std::to_string(pid) +
                " started at another time than the session: not the session's process";
      return s;
    }
  }
  const auto status = status_fields(dir + "/status");
  s.resident_bytes = number(status, "VmRSS") * 1024;
  s.resident_peak_bytes = number(status, "VmHWM") * 1024;
  const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
  s.online_cpus = cpus > 0 ? static_cast<std::uint32_t>(cpus) : 0;
  s.load1 = std::strtod(slurp("/proc/loadavg").c_str(), nullptr);
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir + "/task", ec)) {
    const std::string tdir = e.path().string();
    const std::vector<std::string> ts = stat_fields(slurp(tdir + "/stat"));
    if (ts.empty()) continue;  // ended in between
    ThreadState t;
    t.tid = std::atoi(e.path().filename().c_str());
    t.name = slurp(tdir + "/comm");
    while (!t.name.empty() && t.name.back() == '\n') t.name.pop_back();
    t.state = field(ts, 3).empty() ? '?' : field(ts, 3)[0];
    t.minor_faults = std::strtoull(field(ts, 10).c_str(), nullptr, 10);
    t.major_faults = std::strtoull(field(ts, 12).c_str(), nullptr, 10);
    t.cpu_seconds = static_cast<double>(std::strtoull(field(ts, 14).c_str(), nullptr, 10) +
                                        std::strtoull(field(ts, 15).c_str(), nullptr, 10)) *
                    tick;
    t.cpu = std::atoi(field(ts, 39).c_str());
    const auto tstatus = status_fields(tdir + "/status");
    t.voluntary_switches = number(tstatus, "voluntary_ctxt_switches");
    t.involuntary_switches = number(tstatus, "nonvoluntary_ctxt_switches");
    if (const auto it = tstatus.find("Cpus_allowed_list"); it != tstatus.end())
      t.allowed_cpus = it->second;
    s.threads.push_back(std::move(t));
  }
  std::sort(s.threads.begin(), s.threads.end(), [](const ThreadState& a, const ThreadState& b) {
    return a.tid < b.tid;
  });
  s.ok = !s.threads.empty();
  if (!s.ok) s.error = "no thread of process " + std::to_string(pid) + " could be read";
  return s;
}

std::string format_process_state(const ProcessState& now, const ProcessState* prev) {
  if (!now.ok) return "\nprocess   " + now.error + "\n";
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "\nprocess   pid {}  rss {:.1f} MiB (peak {:.1f})  host load {:.2f} on {} cpus\n",
                 now.pid,
                 static_cast<double>(now.resident_bytes) / 1048576.0,
                 static_cast<double>(now.resident_peak_bytes) / 1048576.0,
                 now.load1,
                 now.online_cpus);
  const bool rate =
      prev != nullptr && prev->ok && prev->pid == now.pid && now.sampled_ns > prev->sampled_ns;
  const double dt = rate ? static_cast<double>(now.sampled_ns - prev->sampled_ns) / 1e9 : 0.0;
  fmt::format_to(it,
                 "{:<16} {:>8} {:>2} {:>4} {:>10} {:>7} {:>10} {:>10} {:>8}\n",
                 "thread",
                 "tid",
                 "st",
                 "cpu",
                 "allowed",
                 rate ? "cpu%" : "cpu_s",
                 rate ? "invol/s" : "invol",
                 rate ? "vol/s" : "vol",
                 "majflt");
  for (const ThreadState& t : now.threads) {
    const ThreadState* p = nullptr;
    if (rate) {
      for (const ThreadState& q : prev->threads) {
        if (q.tid == t.tid) p = &q;
      }
    }
    const auto per_s = [&](std::uint64_t a, std::uint64_t b) {
      return fmt::format("{:.0f}", static_cast<double>(a - std::min(a, b)) / dt);
    };
    fmt::format_to(it,
                   "{:<16} {:>8} {:>2} {:>4} {:>10} {:>7} {:>10} {:>10} {:>8}\n",
                   t.name,
                   t.tid,
                   t.state,
                   t.cpu,
                   t.allowed_cpus,
                   p != nullptr
                       ? fmt::format("{:.0f}", 100.0 * (t.cpu_seconds - p->cpu_seconds) / dt)
                       : fmt::format("{:.1f}", t.cpu_seconds),
                   p != nullptr ? per_s(t.involuntary_switches, p->involuntary_switches)
                                : std::to_string(t.involuntary_switches),
                   p != nullptr ? per_s(t.voluntary_switches, p->voluntary_switches)
                                : std::to_string(t.voluntary_switches),
                   t.major_faults);
  }
  return out;
}

std::string process_state_json(const ProcessState& s) {
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it, "\"process\": {{\"ok\": {}", s.ok ? "true" : "false");
  if (!s.ok) {
    fmt::format_to(it, ", \"error\": {}}}", json_text(s.error));
    return out;
  }
  fmt::format_to(it,
                 ", \"pid\": {}, \"sampled_ns\": {}, \"resident_bytes\": {}, "
                 "\"resident_peak_bytes\": {}, \"online_cpus\": {}, \"load1\": {}, \"threads\": [",
                 s.pid,
                 s.sampled_ns,
                 s.resident_bytes,
                 s.resident_peak_bytes,
                 s.online_cpus,
                 s.load1);
  for (std::size_t i = 0; i < s.threads.size(); ++i) {
    const ThreadState& t = s.threads[i];
    fmt::format_to(it,
                   "{}{{\"tid\": {}, \"name\": {}, \"state\": \"{}\", \"cpu\": {}, "
                   "\"allowed_cpus\": {}, \"cpu_seconds\": {:.2f}, \"voluntary_switches\": {}, "
                   "\"involuntary_switches\": {}, \"minor_faults\": {}, \"major_faults\": {}}}",
                   i == 0 ? "" : ", ",
                   t.tid,
                   json_text(t.name),
                   t.state,
                   t.cpu,
                   json_text(t.allowed_cpus),
                   t.cpu_seconds,
                   t.voluntary_switches,
                   t.involuntary_switches,
                   t.minor_faults,
                   t.major_faults);
  }
  out += "]}";
  return out;
}

std::string process_state_prometheus(const ProcessState& s) {
  if (!s.ok) return {};
  std::string out;
  auto it = std::back_inserter(out);
  fmt::format_to(it,
                 "# HELP fastmm_process_resident_bytes resident memory of the session's process\n"
                 "# TYPE fastmm_process_resident_bytes gauge\n"
                 "fastmm_process_resident_bytes {}\n"
                 "# HELP fastmm_host_load1 the host's 1-minute load average\n"
                 "# TYPE fastmm_host_load1 gauge\n"
                 "fastmm_host_load1 {}\n",
                 s.resident_bytes,
                 s.load1);
  struct Family {
    const char* name;
    const char* type;
    const char* help;
  };
  const Family families[] = {
      {"fastmm_thread_cpu_seconds_total", "counter", "CPU time of the thread, user + system"},
      {"fastmm_thread_involuntary_switches_total",
       "counter",
       "times another task took the thread's core"},
      {"fastmm_thread_voluntary_switches_total", "counter", "times the thread gave up its core"},
      {"fastmm_thread_major_faults_total",
       "counter",
       "page faults the thread waited on a disk for"},
      {"fastmm_thread_cpu", "gauge", "the CPU the thread last ran on"},
  };
  for (std::size_t f = 0; f < std::size(families); ++f) {
    fmt::format_to(it,
                   "# HELP {} {}\n# TYPE {} {}\n",
                   families[f].name,
                   families[f].help,
                   families[f].name,
                   families[f].type);
    for (const ThreadState& t : s.threads) {
      const double v = f == 0   ? t.cpu_seconds
                       : f == 1 ? static_cast<double>(t.involuntary_switches)
                       : f == 2 ? static_cast<double>(t.voluntary_switches)
                       : f == 3 ? static_cast<double>(t.major_faults)
                                : static_cast<double>(t.cpu);
      fmt::format_to(it,
                     "{}{{thread=\"{}\",tid=\"{}\"}} {:.10g}\n",
                     families[f].name,
                     label(t.name),
                     t.tid,
                     v);
    }
  }
  return out;
}

}  // namespace fastmm
