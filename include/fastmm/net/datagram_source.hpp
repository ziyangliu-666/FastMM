#pragma once
// Datagram receive interface for multicast market data (ADR-0015, section 1). A source joins one
// or more (interface, group, port, source?) subscriptions; the line index is the subscription's
// position. A unicast "group" is a local address the datagrams are sent to (no join): for networks
// without multicast. The net thread calls poll(handler), which delivers a batch and returns its
// size. Payload spans are valid until poll returns; no backend allocates after open.
//
// Backends: KernelDatagramSource (kernel_datagram_source.hpp), XdpDatagramSource
// (xdp_datagram_source.hpp), DpdkDatagramSource (dpdk_datagram_source.hpp). Descriptors and
// statistics are backend-specific.
#include "fastmm/core/time.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::net {

struct RxMeta {
  Cycles t0_cycles;         // rdtscp after the batch left the kernel or the RX ring
  std::int64_t t0_wall_ns;  // CLOCK_REALTIME read with t0_cycles; sw_ts_ns -> T0 in one clock
  std::int64_t sw_ts_ns;    // kernel receive time (CLOCK_REALTIME), 0 when unavailable
  std::int64_t hw_ts_ns;    // raw NIC (PHC) time, 0 when unavailable; not CLOCK_REALTIME
  std::uint32_t src_ip;     // network byte order
  std::uint32_t dst_ip;     // network byte order: the subscription's group
  std::uint16_t dst_port;   // host byte order
  std::uint8_t line;        // subscription index (A/B)
};

struct DatagramSourceStats {
  std::uint64_t datagrams = 0;  // delivered to the handler
  std::uint64_t bytes = 0;      // payload bytes delivered
  std::uint64_t truncated = 0;  // larger than the receive buffer; dropped
  std::uint64_t errors = 0;     // failed receive calls other than EAGAIN
  int last_error = 0;           // errno of the last failed receive call
};

// A handler that accepts every datagram: the shape poll() is checked against.
struct DatagramHandlerArchetype {
  void operator()(std::span<const std::byte>, const RxMeta&) const noexcept {}
};

template <class S>
concept DatagramSource = requires(S& s, DatagramHandlerArchetype h) {
  { s.poll(h) } noexcept -> std::same_as<std::size_t>;
};

}  // namespace fastmm::net
