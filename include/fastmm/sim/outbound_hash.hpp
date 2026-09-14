#pragma once
// OutboundHasher: SHA-256 over the engine's outbound message stream (5.11). Messages are
// normalized before hashing so the hash of a live/sim run equals the hash of its replay and
// of the journal's own Out* records:
//   * seq and the journal flags (kOutbound / kReplayed / kDropped / kEngineTime, reserved0) are
//     assigned by the journal;
//   * t0_cycles / t1_delta / t2_delta are latency diagnostics, not order content. They are
//     also not replay-stable: an order sent from a timer callback inherits the T0 of the
//     previous feed event in the original run, but the T0 of the (zero-stamped) journaled
//     TimerMsg in the replay.
#include "fastmm/core/messages.hpp"
#include "fastmm/sim/sha256.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace fastmm::sim {

class OutboundHasher {
 public:
  static constexpr std::uint32_t kMaxOutBytes = 192;

  static void normalize(EventHeader& h) noexcept {
    h.seq = 0;
    h.flags = static_cast<std::uint8_t>(
        h.flags & ~(EventHeader::kOutbound | EventHeader::kReplayed | EventHeader::kDropped));
    h.t0_cycles = Cycles{};
    h.t1_delta = 0;
    h.t2_delta = 0;
  }
  // Copies `m` (m.len <= kMaxOutBytes) into `out` normalized; returns the length.
  static std::uint32_t normalized_copy(const EventHeader& m, std::byte* out) noexcept {
    const std::uint32_t len = m.len <= kMaxOutBytes ? m.len : kMaxOutBytes;
    std::memcpy(out, &m, len);
    normalize(*reinterpret_cast<EventHeader*>(out));
    return len;
  }

  void add(const EventHeader& m) noexcept {
    alignas(64) std::byte buf[kMaxOutBytes];
    const std::uint32_t len = normalized_copy(m, buf);
    sha_.update(buf, len);
    ++count_;
  }
  [[nodiscard]] std::string hex() const { return sha_.hex(); }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  void reset() noexcept {
    sha_.reset();
    count_ = 0;
  }

 private:
  Sha256 sha_;
  std::uint64_t count_ = 0;
};

}  // namespace fastmm::sim
