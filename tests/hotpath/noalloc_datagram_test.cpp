// KernelDatagramSource::poll must not allocate: open() preallocates every buffer. Multicast on lo
// needs a network namespace (tests/net/netns_test_util.hpp); without one the case is skipped.
#if defined(FASTMM_HOTPATH_NET)
#include "../net/netns_test_util.hpp"
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/net/kernel_datagram_source.hpp"
#include "fastmm/net/udp_socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace fastmm::net;

TEST_CASE("hotpath.noalloc: KernelDatagramSource poll") {
  if (!test::in_multicast_netns()) return;
  KernelSourceConfig cfg;
  cfg.subscriptions = {
      {.interface = "lo", .group = "239.2.2.1", .port = 30100, .source = ""},
      {.interface = "lo", .group = "239.2.2.2", .port = 30100, .source = "127.0.0.1"}};
  cfg.batch = 8;
  cfg.max_datagram = 128;
  KernelDatagramSource src;
  REQUIRE(src.open(cfg).ok());

  UdpSocket tx = UdpSocket::open();
  REQUIRE(tx.bind(*SockAddr::from_ip("127.0.0.1", 0)) == 0);
  McastInterface lo;
  REQUIRE(McastInterface::resolve("lo", lo) == 0);
  REQUIRE(tx.set_multicast_if(lo) == 0);
  const SockAddr a = *SockAddr::from_ip("239.2.2.1", 30100);
  const SockAddr b = *SockAddr::from_ip("239.2.2.2", 30100);
  const std::array<std::byte, 100> msg{};
  const std::array<std::byte, 200> oversized{};

  std::size_t received = 0;
  std::uint64_t bytes = 0;
  auto handler = [&](std::span<const std::byte> p, const RxMeta& m) noexcept {
    ++received;
    bytes += p.size() + m.line;
  };
  std::uint64_t allocations = 0;
  {
    fastmm::test::NoAllocScope guard;
    for (int round = 0; round < 200; ++round) {
      for (int i = 0; i < 5; ++i) {
        static_cast<void>(tx.send_to(msg, a));
        static_cast<void>(tx.send_to(msg, b));
      }
      static_cast<void>(tx.send_to(oversized, a));
      for (int spins = 0; spins < 1000 && received < static_cast<std::size_t>(10 * (round + 1));
           ++spins) {
        src.poll(handler);
      }
      src.poll(handler);  // EAGAIN on both lines
    }
    allocations = guard.allocations_so_far();
  }
  CHECK(allocations == 0);
  CHECK(received == 2000);
  CHECK(src.stats().truncated == 200);
  CHECK(bytes == 2000 * 100 + 1000);
}
#endif
