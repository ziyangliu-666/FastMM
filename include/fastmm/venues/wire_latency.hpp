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
//
// Coalesced sends (drain_outbound_coalesced): between begin_batch() and end_batch() record()
// keeps the encode stamps of up to kMaxBatch orders and ignores its after-send stamp, since the
// send call only queued the bytes; end_batch() records every kept order with the stamp taken
// after the one write of the batch returned.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm::venues {

struct WireLatencyStats {
  std::uint64_t count = 0;
  std::uint64_t p50_ns = 0;
  std::uint64_t p99_ns = 0;
  std::uint64_t p999_ns = 0;
  std::uint64_t max_ns = 0;
};

class WireLatencyRecorder {
 public:
  static constexpr std::size_t kMaxBatch = 64;

  FASTMM_FORCE_INLINE void record(Cycles t0,
                                  Cycles before_encode,
                                  Cycles after_encode,
                                  Cycles after_send) noexcept {
    if (batching_ && batch_len_ < kMaxBatch) {
      batch_[batch_len_++] = Staged{t0, before_encode, after_encode};
      return;
    }
    record_now(t0, before_encode, after_encode, after_send);
  }

  void begin_batch() noexcept {
    batching_ = true;
    batch_len_ = 0;
  }
  [[nodiscard]] bool batch_full() const noexcept { return batch_len_ == kMaxBatch; }
  // `sent`: the batch's write succeeded (or was queued); false drops the kept stamps.
  void end_batch(Cycles after_send, bool sent) noexcept {
    batching_ = false;
    if (sent) {
      for (std::size_t i = 0; i < batch_len_; ++i) {
        const Staged& s = batch_[i];
        record_now(s.t0, s.before_encode, s.after_encode, after_send);
      }
    }
    batch_len_ = 0;
  }

  [[nodiscard]] const LogLinearHistogram& encode() const noexcept { return encode_; }
  [[nodiscard]] const LogLinearHistogram& send() const noexcept { return send_; }
  [[nodiscard]] const LogLinearHistogram& tick_to_trade() const noexcept { return tick_to_trade_; }

  // Percentiles in ns under `c`'s TSC rate (also set when only constant_tsc is present);
  // without a rate only the counts are filled in.
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
  struct Staged {
    Cycles t0;
    Cycles before_encode;
    Cycles after_encode;
  };

  FASTMM_FORCE_INLINE void record_now(Cycles t0,
                                      Cycles before_encode,
                                      Cycles after_encode,
                                      Cycles after_send) noexcept {
    encode_.record(delta(after_encode, before_encode));
    send_.record(delta(after_send, after_encode));
    if (t0.v != 0) tick_to_trade_.record(delta(after_send, t0));
  }
  [[nodiscard]] static std::uint64_t delta(Cycles later, Cycles earlier) noexcept {
    return later.v > earlier.v ? later.v - earlier.v : 0;
  }
  [[nodiscard]] static std::uint64_t to_ns(std::uint64_t cycles, const TscCalibration& c) noexcept {
    return tsc_interval_ns(c, cycles);  // 0 without a TSC rate
  }
  static void fill(const LogLinearHistogram& h,
                   const TscCalibration& c,
                   WireLatencyStats& out) noexcept {
    out.count = h.count();
    out.p50_ns = to_ns(h.percentile(0.50), c);
    out.p99_ns = to_ns(h.percentile(0.99), c);
    out.p999_ns = to_ns(h.percentile(0.999), c);
    out.max_ns = to_ns(h.max(), c);
  }

  LogLinearHistogram encode_;
  LogLinearHistogram send_;
  LogLinearHistogram tick_to_trade_;
  Staged batch_[kMaxBatch] = {};
  std::size_t batch_len_ = 0;
  bool batching_ = false;
};

// Drains the engine's outbound ring in batches of at most kMaxBatch messages: cork() holds the
// connection's writes back, on_message(header) encodes and "sends" each message into the
// connection's buffer, uncork() writes the batch with one system call and returns false when the
// connection failed. Every order of a batch gets the stamp taken after uncork() returned.
template <class Cork, class OnMessage, class Uncork>
void drain_outbound_coalesced(MsgRing& ring,
                              WireLatencyRecorder& wire,
                              Cork&& cork,
                              OnMessage&& on_message,
                              Uncork&& uncork) {
  while (ring.try_peek() != nullptr) {
    cork();
    wire.begin_batch();
    for (std::size_t n = 0; n < WireLatencyRecorder::kMaxBatch; ++n) {
      const std::byte* p = ring.try_peek();
      if (p == nullptr) break;
      on_message(*reinterpret_cast<const EventHeader*>(p));
      ring.release();
    }
    const bool sent = uncork();
    wire.end_batch(rdtscp(), sent);
  }
}

}  // namespace fastmm::venues
