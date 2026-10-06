#include "fastmm/core/host_tuning.hpp"

#include "test_support.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

std::string tmp_path(const char* name) {
  return std::string(FASTMM_TEST_TMP_DIR) + "/" + name;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("core.host_tuning: a CPU latency request writes the 32-bit target and holds the fd") {
  const std::string path = tmp_path("cpu_dma_latency");
  { std::ofstream out(path, std::ios::trunc); }
  CpuLatencyRequest req;
  CHECK_FALSE(req.active());
  REQUIRE(req.open(7, path.c_str()) == 0);
  CHECK(req.active());
  const std::string bytes = read_file(path);
  REQUIRE(bytes.size() == sizeof(std::int32_t));
  std::int32_t v = -1;
  std::memcpy(&v, bytes.data(), sizeof v);
  CHECK(v == 7);
  CpuLatencyRequest moved = std::move(req);
  CHECK(moved.active());
  CHECK_FALSE(req.active());  // NOLINT(bugprone-use-after-move)
  moved.close();
  CHECK_FALSE(moved.active());
  std::filesystem::remove(path);
}

TEST_CASE("core.host_tuning: a CPU latency request that cannot open the device reports errno") {
  CpuLatencyRequest req;
  CHECK(req.open(0, tmp_path("no-such-dir/cpu_dma_latency").c_str()) == ENOENT);
  CHECK_FALSE(req.active());
  CHECK(req.open(-1, tmp_path("unused").c_str()) == EINVAL);
  CHECK_FALSE(req.active());
}

TEST_CASE("core.host_tuning: parse_cpu_list") {
  CHECK(parse_cpu_list("0-3,6") == std::vector<int>{0, 1, 2, 3, 6});
  CHECK(parse_cpu_list(" 5\n") == std::vector<int>{5});
  CHECK(parse_cpu_list("2,1,1-2") == std::vector<int>{1, 2});
  CHECK(parse_cpu_list("").value().empty());
  CHECK_FALSE(parse_cpu_list("3-1"));
  CHECK_FALSE(parse_cpu_list("a"));
  CHECK_FALSE(parse_cpu_list("1,,2"));
  CHECK_FALSE(parse_cpu_list("-1"));
}

namespace {

void write_file(const std::filesystem::path& p, const std::string& text) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream(p) << text;
}

}  // namespace

TEST_CASE("core.host_tuning: nic_irqs finds queue interrupts by name, else by MSI vector") {
  namespace fs = std::filesystem;
  const fs::path root = fs::path(FASTMM_TEST_TMP_DIR) / "irq_tree";
  fs::remove_all(root);
  const fs::path sys = root / "sys";
  const fs::path proc = root / "proc";
  // eth1: queues named after the interface. eth10 must not match eth1. vnet0: a virtio device
  // whose vectors are on the PCI function above it. lo: no device.
  fs::create_directories(sys / "devices/pci0/eth1dev");
  fs::create_directories(sys / "devices/pci0/eth10dev");
  fs::create_directories(sys / "devices/pci1/virtio0");
  fs::create_directories(sys / "devices/pci1/msi_irqs/60");
  fs::create_directories(sys / "devices/pci1/msi_irqs/61");
  fs::create_directories(sys / "devices/pci1/msi_irqs/62");
  fs::create_directories(sys / "class/net/eth1");
  fs::create_directories(sys / "class/net/eth10");
  fs::create_directories(sys / "class/net/vnet0");
  fs::create_directories(sys / "class/net/lo");
  fs::create_directory_symlink(sys / "devices/pci0/eth1dev", sys / "class/net/eth1/device");
  fs::create_directory_symlink(sys / "devices/pci0/eth10dev", sys / "class/net/eth10/device");
  fs::create_directory_symlink(sys / "devices/pci1/virtio0", sys / "class/net/vnet0/device");
  write_file(proc / "interrupts",
             "           CPU0       CPU1\n"
             "  8:          0          0   IO-APIC   8-edge      rtc0\n"
             " 45:         10          0   PCI-MSI 1-edge      eth1-TxRx-0\n"
             " 46:          0         12   PCI-MSI 2-edge      eth1-TxRx-1\n"
             " 50:          0          0   PCI-MSI 3-edge      eth10-TxRx-0\n"
             " 60:          0          0   PCI-MSI 4-edge      virtio0-config\n"
             " 61:          0          0   PCI-MSI 5-edge      virtio0-input.0\n"
             " 62:          0          0   PCI-MSI 6-edge      virtio0-output.0\n"
             "NMI:          0          0   Non-maskable interrupts\n");
  write_file(proc / "irq/45/smp_affinity_list", "4\n");
  write_file(proc / "irq/46/smp_affinity_list", "0-7\n");
  write_file(proc / "irq/50/smp_affinity_list", "1\n");
  write_file(proc / "irq/61/smp_affinity_list", "6\n");

  const std::vector<NicIrqs> nics = nic_irqs(sys.string(), proc.string());
  REQUIRE(nics.size() == 3);
  CHECK(nics[0].iface == "eth1");
  REQUIRE(nics[0].irqs.size() == 2);
  CHECK(nics[0].irqs[0].irq == 45);
  CHECK(nics[0].irqs[0].name == "eth1-TxRx-0");
  CHECK(nics[0].irqs[0].cpus == std::vector<int>{4});
  CHECK(nics[0].irqs[1].cpus.size() == 8);
  CHECK(nics[1].iface == "eth10");
  REQUIRE(nics[1].irqs.size() == 1);
  CHECK(nics[1].irqs[0].irq == 50);
  CHECK(nics[2].iface == "vnet0");
  REQUIRE(nics[2].irqs.size() == 2);  // the config vector is left out
  CHECK(nics[2].irqs[0].irq == 61);
  CHECK(nics[2].irqs[1].irq == 62);
  CHECK(nics[2].irqs[1].cpus.empty());  // no smp_affinity_list

  const std::vector<int> net_cpus{6, 5};
  const std::vector<IrqReportLine> lines = irq_affinity_report(nics, 4, net_cpus);
  REQUIRE(lines.size() == 5);
  CHECK(lines[0].irq == 45);
  CHECK(lines[0].cpus == "4");
  CHECK(lines[0].engine);
  CHECK(lines[0].pinned == "engine");
  CHECK(lines[1].cpus == "0-7");
  CHECK(lines[1].pinned == "engine net 0 net 1");
  CHECK_FALSE(lines[2].engine);
  CHECK(lines[2].pinned.empty());
  CHECK(lines[3].pinned == "net 0");
  CHECK(lines[4].cpus == "?");
  CHECK_FALSE(lines[4].engine);

  std::vector<NicIrqs> none(1);
  none[0].iface = "eth9";
  const std::vector<IrqReportLine> empty = irq_affinity_report(none, -1, {});
  REQUIRE(empty.size() == 1);
  CHECK(empty[0].irq < 0);
  CHECK(nic_irqs((root / "missing").string(), (root / "missing").string()).empty());
  fs::remove_all(root);
}
