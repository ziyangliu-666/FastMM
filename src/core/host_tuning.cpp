#include "fastmm/core/host_tuning.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace fastmm {

int CpuLatencyRequest::open(std::int32_t max_us, const char* path) noexcept {
  close();
  if (max_us < 0) return EINVAL;
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return errno;
  const ssize_t n = ::write(fd, &max_us, sizeof max_us);
  if (n != static_cast<ssize_t>(sizeof max_us)) {
    const int err = n < 0 ? errno : EIO;
    ::close(fd);
    return err;
  }
  fd_ = fd;
  return 0;
}

void CpuLatencyRequest::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

namespace {

namespace fs = std::filesystem;

bool parse_int(std::string_view s, int& out) {
  if (s.empty()) return false;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && end == s.data() + s.size() && out >= 0;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
  return s;
}

// IRQ number -> action name, from /proc/interrupts ("  45:  0  0  IR-PCI-MSI 1-edge  eth1-TxRx-0").
std::map<int, std::string> interrupt_names(const std::string& proc_root) {
  std::map<int, std::string> out;
  std::ifstream in(proc_root + "/interrupts");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string first;
    std::string word;
    std::string last;
    if (!(words >> first) || first.size() < 2 || first.back() != ':') continue;
    int irq = -1;
    if (!parse_int(std::string_view(first).substr(0, first.size() - 1), irq)) continue;
    while (words >> word) last = word;
    out[irq] = last;
  }
  return out;
}

// `name` names `iface`: the interface's name, not followed by a letter or digit (eth1 is not
// eth10).
bool names_iface(std::string_view name, std::string_view iface) {
  for (std::size_t at = name.find(iface); at != std::string_view::npos;
       at = name.find(iface, at + 1)) {
    const std::size_t end = at + iface.size();
    if (end == name.size() || std::isalnum(static_cast<unsigned char>(name[end])) == 0) return true;
  }
  return false;
}

bool control_vector(std::string_view name) {
  constexpr std::string_view kWords[] = {"config", "async", "mgmnt", "ctrl"};
  return std::ranges::any_of(
      kWords, [name](std::string_view w) { return name.find(w) != std::string_view::npos; });
}

std::vector<int> msi_irqs(const fs::path& device) {
  std::vector<int> out;
  std::error_code ec;
  for (const fs::path& dir : {device / "msi_irqs", device.parent_path() / "msi_irqs"}) {
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      int irq = -1;
      if (parse_int(entry.path().filename().string(), irq)) out.push_back(irq);
    }
    break;
  }
  return out;
}

std::string cpu_ranges(const std::vector<int>& cpus) {
  std::string out;
  for (std::size_t i = 0; i < cpus.size();) {
    std::size_t j = i;
    while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
    if (!out.empty()) out += ',';
    out += std::to_string(cpus[i]);
    if (j > i) out += '-' + std::to_string(cpus[j]);
    i = j + 1;
  }
  return out;
}

}  // namespace

std::optional<std::vector<int>> parse_cpu_list(std::string_view list) {
  std::vector<int> out;
  list = trim(list);
  while (!list.empty()) {
    const std::size_t comma = list.find(',');
    const std::string_view part = trim(list.substr(0, comma));
    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    const std::size_t dash = part.find('-');
    int lo = -1;
    int hi = -1;
    if (!parse_int(part.substr(0, dash), lo)) return std::nullopt;
    hi = lo;
    if (dash != std::string_view::npos && !parse_int(part.substr(dash + 1), hi))
      return std::nullopt;
    if (hi < lo) return std::nullopt;
    for (int c = lo; c <= hi; ++c) out.push_back(c);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<NicIrqs> nic_irqs(const std::string& sys_root, const std::string& proc_root) {
  std::vector<NicIrqs> out;
  const std::map<int, std::string> names = interrupt_names(proc_root);
  std::error_code ec;
  std::vector<std::string> ifaces;
  for (const auto& entry : fs::directory_iterator(sys_root + "/class/net", ec)) {
    if (fs::exists(entry.path() / "device", ec)) ifaces.push_back(entry.path().filename().string());
  }
  std::sort(ifaces.begin(), ifaces.end());
  for (const std::string& iface : ifaces) {
    NicIrqs nic;
    nic.iface = iface;
    std::vector<int> irqs;
    for (const auto& [irq, name] : names) {
      if (names_iface(name, iface)) irqs.push_back(irq);
    }
    if (irqs.empty()) {
      const fs::path device =
          fs::canonical(fs::path(sys_root) / "class/net" / iface / "device", ec);
      if (!ec) {
        for (const int irq : msi_irqs(device)) {
          const auto it = names.find(irq);
          if (it == names.end() || !control_vector(it->second)) irqs.push_back(irq);
        }
      }
      ec.clear();
    }
    std::sort(irqs.begin(), irqs.end());
    for (const int irq : irqs) {
      NicIrq q;
      q.irq = irq;
      if (const auto it = names.find(irq); it != names.end()) q.name = it->second;
      std::ifstream in(fs::path(proc_root) / "irq" / std::to_string(irq) / "smp_affinity_list");
      std::string list;
      if (std::getline(in, list)) {
        if (auto cpus = parse_cpu_list(list)) q.cpus = std::move(*cpus);
      }
      nic.irqs.push_back(std::move(q));
    }
    out.push_back(std::move(nic));
  }
  return out;
}

std::vector<IrqReportLine> irq_affinity_report(std::span<const NicIrqs> nics,
                                               int engine_cpu,
                                               std::span<const int> net_cpus) {
  std::vector<IrqReportLine> out;
  for (const NicIrqs& nic : nics) {
    if (nic.irqs.empty()) {
      IrqReportLine none;
      none.iface = nic.iface;
      out.push_back(std::move(none));
    }
    for (const NicIrq& q : nic.irqs) {
      IrqReportLine line;
      line.iface = nic.iface;
      line.irq = q.irq;
      line.name = q.name;
      line.cpus = q.cpus.empty() ? std::string("?") : cpu_ranges(q.cpus);
      const auto has = [&q](int cpu) {
        return cpu >= 0 && std::binary_search(q.cpus.begin(), q.cpus.end(), cpu);
      };
      line.engine = has(engine_cpu);
      if (line.engine) line.pinned = "engine";
      for (std::size_t n = 0; n < net_cpus.size(); ++n) {
        if (!has(net_cpus[n])) continue;
        if (!line.pinned.empty()) line.pinned += ' ';
        line.pinned += "net " + std::to_string(n);
      }
      out.push_back(std::move(line));
    }
  }
  return out;
}

std::map<int, std::uint64_t> cpu_cores(const std::string& sys_root) {
  std::map<int, std::uint64_t> out;
  const fs::path cpu_root = fs::path(sys_root) / "devices/system/cpu";
  std::ifstream online_in(cpu_root / "online");
  std::string list;
  if (!std::getline(online_in, list)) return out;
  const auto online = parse_cpu_list(list);
  if (!online) return out;
  const auto read_id = [](const fs::path& path, int& v) {
    std::ifstream in(path);
    std::string line;
    return std::getline(in, line) && parse_int(trim(line), v);
  };
  for (const int cpu : *online) {
    const fs::path topo = cpu_root / ("cpu" + std::to_string(cpu)) / "topology";
    int package = 0;
    int core = 0;
    if (!read_id(topo / "physical_package_id", package) || !read_id(topo / "core_id", core))
      continue;
    out[cpu] = (static_cast<std::uint64_t>(package) << 32) | static_cast<std::uint32_t>(core);
  }
  return out;
}

std::vector<TopologyLine> topology_report(const TopologyInput& in) {
  std::vector<TopologyLine> out;
  const auto warn = [&out](std::string text) { out.push_back({true, std::move(text)}); };
  const auto core_of = [&in](int cpu) -> std::optional<std::uint64_t> {
    const auto it = in.cores.find(cpu);
    if (it == in.cores.end()) return std::nullopt;
    return it->second;
  };
  std::vector<std::uint64_t> all_cores;
  for (const auto& [cpu, core] : in.cores) all_cores.push_back(core);
  std::sort(all_cores.begin(), all_cores.end());
  all_cores.erase(std::unique(all_cores.begin(), all_cores.end()), all_cores.end());

  std::string summary = std::to_string(in.hot.size()) + " hot thread(s):";
  for (const HotThread& t : in.hot)
    summary += " " + t.name + (t.cpu < 0 ? " unpinned" : " CPU " + std::to_string(t.cpu)) + ",";
  summary.back() = ';';
  summary += " " + std::to_string(all_cores.size()) + " physical core(s), " +
             std::to_string(in.cores.size()) + " logical CPU(s) online";
  if (!in.process_cpus.empty()) summary += "; other threads on CPUs " + cpu_ranges(in.process_cpus);
  out.push_back({false, summary});

  // A pinned hot thread shares its core with another: same CPU, or hyperthreads of one core.
  std::vector<bool> shares(in.hot.size(), false);
  bool any_pinned = false;
  for (std::size_t a = 0; a < in.hot.size(); ++a) {
    const HotThread& ta = in.hot[a];
    if (ta.cpu < 0) continue;
    any_pinned = true;
    if (!in.cores.empty() && !core_of(ta.cpu)) {
      warn(ta.name + ": CPU " + std::to_string(ta.cpu) + " is not online");
      continue;
    }
    for (std::size_t b = a + 1; b < in.hot.size(); ++b) {
      const HotThread& tb = in.hot[b];
      if (tb.cpu < 0) continue;
      if (ta.cpu == tb.cpu) {
        shares[a] = shares[b] = true;
        warn(ta.name + " and " + tb.name + " share CPU " + std::to_string(ta.cpu));
      } else if (const auto ca = core_of(ta.cpu); ca && ca == core_of(tb.cpu)) {
        shares[a] = shares[b] = true;
        warn(ta.name + " (CPU " + std::to_string(ta.cpu) + ") and " + tb.name + " (CPU " +
             std::to_string(tb.cpu) + ") are hyperthreads of one core");
      }
    }
  }
  if (!all_cores.empty() && in.hot.size() > all_cores.size()) {
    warn(std::to_string(in.hot.size()) + " hot threads and " + std::to_string(all_cores.size()) +
         " physical cores: some of them take turns on one");
  }
  if (in.busy && std::find(shares.begin(), shares.end(), true) != shares.end())
    warn("spin_mode = \"busy\": threads that share a core take its time from each other");

  // Other threads (journal, store, log, control) may run on a hot thread's core.
  if (any_pinned && !in.process_cpus.empty() && !in.cores.empty()) {
    std::vector<std::uint64_t> hot_cores;
    for (const HotThread& t : in.hot) {
      const auto c = t.cpu < 0 ? std::nullopt : core_of(t.cpu);
      if (!c) continue;
      hot_cores.push_back(*c);
    }
    std::vector<int> overlap;
    std::vector<int> rest;
    for (const auto& [cpu, core] : in.cores) {
      const bool hot = std::find(hot_cores.begin(), hot_cores.end(), core) != hot_cores.end();
      if (hot && std::binary_search(in.process_cpus.begin(), in.process_cpus.end(), cpu))
        overlap.push_back(cpu);
      if (!hot) rest.push_back(cpu);
    }
    if (!overlap.empty()) {
      std::string text = "the journal, store, log and control threads may run on CPU(s) " +
                         cpu_ranges(overlap) + ", on the hot threads' cores";
      if (!rest.empty()) text += "; start the process under taskset -c " + cpu_ranges(rest);
      warn(std::move(text));
    }
  }

  // Adaptive: a thread alone on its core sleeps when idle and pays a wake-up on the next event.
  if (!in.busy) {
    std::string sleepers;
    for (std::size_t i = 0; i < in.hot.size(); ++i) {
      const HotThread& t = in.hot[i];
      if (t.cpu < 0 || shares[i]) continue;
      if (i > 0 && in.net_spin_dedicated) continue;  // a network thread that never blocks
      sleepers += (sleepers.empty() ? "" : ", ") + t.name;
    }
    if (!sleepers.empty()) {
      warn(
          "spin_mode = \"adaptive\": idle threads on cores of their own sleep and pay a wake-up "
          "on the next event (" +
          sleepers + "); set spin_mode = \"busy\"");
    }
  }
  for (const HotThread& t : in.hot) {
    if (t.cpu < 0) out.push_back({false, t.name + " is not pinned ([engine] cpu, net_cpus)"});
  }
  return out;
}

}  // namespace fastmm
