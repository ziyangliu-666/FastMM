#pragma once
// Network-thread order latency: what happens to an outbound order after the engine pushed it
// into the venue's outbound ring. Per order message the venue takes three rdtscp stamps
// (before encoding, after encoding, after the WebSocket/REST send call returned) and records
//
//   encode        JSON encoding and signing
//   send          the send call (TLS + WebSocket write, or queueing the REST request)
//   tick_to_trade after-send minus the header's t0_cycles, the network thread's receive stamp
//                 of the inbound event that triggered the order (only when t0_cycles != 0)
//
// Histograms are in TSC cycles and owned by the network thread; summarize() converts to ns
// with a calibration when the venue publishes its status. Recording never allocates.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>

namespace fastmm::venues {

struct WireLatencyStats {
  std::uint64_t count = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p99_ns = 0;
};

class WireLatencyRecorder {
 public:
  FASTMM_FORCE_INLINE void record(Cycles t0,
                                  Cycles before_encode,
                                  Cycles after_encode,
                                  Cycles after_send) noexcept {
    encode_.record(delta(after_encode, before_encode));
    send_.record(delta(after_send, after_encode));
    if (t0.v != 0) tick_to_trade_.record(delta(after_send, t0));
  }

  [[nodiscard]] const LogLinearHistogram& encode() const noexcept { return encode_; }
  [[nodiscard]] const LogLinearHistogram& send() const noexcept { return send_; }
  [[nodiscard]] const LogLinearHistogram& tick_to_trade() const noexcept { return tick_to_trade_; }

  // Percentiles in ns under `c`; without a TSC calibration only the counts are filled in.
  void summarize(const TscCalibration& c,
                 WireLatencyStats& tick_to_trade,
                 WireLatencyStats& encode,
                 WireLatencyStats& send) const noexcept {
    fill(tick_to_trade_, c, tick_to_trade);
    fill(encode_, c, encode);
    fill(send_, c, send);
  }

  void reset() noexcept {
    encode_.reset();
    send_.reset();
    tick_to_trade_.reset();
  }

 private:
  [[nodiscard]] static std::uint64_t delta(Cycles later, Cycles earlier) noexcept {
    return later.v > earlier.v ? later.v - earlier.v : 0;
  }
  [[nodiscard]] static std::uint64_t to_ns(std::uint64_t cycles, const TscCalibration& c) noexcept {
    if (!c.use_tsc) return 0;
    return static_cast<std::uint64_t>((static_cast<Uint128>(cycles) * c.ns_per_cycle_q32) >> 32);
  }
  static void fill(const LogLinearHistogram& h,
                   const TscCalibration& c,
                   WireLatencyStats& out) noexcept {
    out.count = h.count();
    out.p50_ns = to_ns(h.percentile(0.50), c);
    out.p99_ns = to_ns(h.percentile(0.99), c);
  }

  LogLinearHistogram encode_;
  LogLinearHistogram send_;
  LogLinearHistogram tick_to_trade_;
};

}  // namespace fastmm::venues
