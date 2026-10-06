#pragma once
// Host settings a live process holds or inspects at start: the CPU wake-up latency request
// (/dev/cpu_dma_latency) and the CPUs the network interfaces' interrupts may run on.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

inline constexpr const char* kCpuDmaLatencyPath = "/dev/cpu_dma_latency";

// A PM QoS CPU latency request: while the file descriptor is open, the kernel keeps every CPU out
// of idle states whose exit latency exceeds the requested value; 0 keeps them polling in C0.
// Closing the descriptor (close() or the destructor) withdraws the request.
class CpuLatencyRequest {
 public:
  CpuLatencyRequest() noexcept = default;
  ~CpuLatencyRequest() { close(); }
  CpuLatencyRequest(const CpuLatencyRequest&) = delete;
  CpuLatencyRequest& operator=(const CpuLatencyRequest&) = delete;
  CpuLatencyRequest(CpuLatencyRequest&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  CpuLatencyRequest& operator=(CpuLatencyRequest&& o) noexcept {
    if (this != &o) {
      close();
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }

  // Opens `path` and writes `max_us` as a 32-bit integer; replaces a request this object held.
  // Returns 0, or the errno of the open or write (ENOENT: no such device; EACCES: the device is
  // root's, 0600 by default). max_us < 0 is EINVAL.
  int open(std::int32_t max_us, const char* path = kCpuDmaLatencyPath) noexcept;
  void close() noexcept;
  [[nodiscard]] bool active() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

// "0-3,6" -> {0, 1, 2, 3, 6}; nullopt when malformed. An empty or blank list is empty.
[[nodiscard]] std::optional<std::vector<int>> parse_cpu_list(std::string_view list);

struct NicIrq {
  int irq = -1;
  std::string name;       // the action name, last column of /proc/interrupts
  std::vector<int> cpus;  // /proc/irq/<irq>/smp_affinity_list; empty when unreadable
};

struct NicIrqs {
  std::string iface;
  std::vector<NicIrq> irqs;  // ascending
};

// Every interface with a device behind it (<sys_root>/class/net/<iface>/device) and its queue
// interrupts: the /proc/interrupts lines whose name contains the interface's name (eth1-TxRx-0),
// or else the device's MSI vectors (<device>/msi_irqs or its parent's: virtio, mlx5) minus the
// ones named config, async, mgmnt or ctrl. Read only; whatever cannot be read is left out.
[[nodiscard]] std::vector<NicIrqs> nic_irqs(const std::string& sys_root = "/sys",
                                            const std::string& proc_root = "/proc");

// One queue interrupt against the pinned CPUs; irq < 0: the interface has none.
struct IrqReportLine {
  std::string iface;
  int irq = -1;
  std::string name;
  std::string cpus;     // "0-3,6"; "?" when unreadable
  std::string pinned;   // "engine", "net 0", "net 1", ... space separated; empty for none
  bool engine = false;  // may run on the engine's CPU
};

// One line per queue interrupt of each interface, naming the pinned threads whose CPU
// (`engine_cpu`, `net_cpus` by index; negative = unpinned) it may run on.
[[nodiscard]] std::vector<IrqReportLine> irq_affinity_report(std::span<const NicIrqs> nics,
                                                             int engine_cpu,
                                                             std::span<const int> net_cpus);

}  // namespace fastmm
