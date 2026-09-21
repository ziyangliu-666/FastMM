#pragma once
// Datagram receive interface for multicast market data (ADR-0015, section 1). The net thread
// calls poll(handler), which delivers a batch and returns its size. Payload spans are valid until
// poll returns; no backend allocates after open.
#include "fastmm/core/time.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::net {

struct RxMeta {
  Cycles t0_cycles;         // rdtscp when the batch was taken from the kernel or RX ring
  std::int64_t t0_wall_ns;  // CLOCK_REALTIME read next to t0_cycles
  std::int64_t sw_ts_ns;    // kernel RX timestamp (CLOCK_REALTIME), 0 when unavailable
  std::int64_t hw_ts_ns;    // NIC timestamp (PHC time), 0 when unavailable
  std::uint32_t src_ip;     // network byte order
  std::uint32_t dst_ip;     // network byte order
  std::uint16_t dst_port;   // host byte order
  std::uint8_t line;        // index of the subscription (A/B)
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
