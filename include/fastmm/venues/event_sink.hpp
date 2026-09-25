#pragma once
// EventSink (5.15): the net -> engine seam. A thin wrapper over the per-venue inbound
// MsgRing that the parsers write normalised events into. Two sinks per venue: market data
// (lossy: a full ring drops the delta and the book resyncs) and order events (never dropped:
// bounded spin then overflow callback, which the venue treats as fatal).
//
// Run-to-completion (fastmm-live [engine] threading = "single"): the engine runs on the venue's
// thread and a drain hook hands it the ring's events in place. With `on_commit` the hook runs
// after every commit, so the engine takes each event as soon as it is decoded; a full ring runs it
// before the policy's spin either way.
//
// fastmm-gateway points the sinks at ShmRings the attached strategy process reads
// (attach(ShmRing*)) and back at a local ring it discards while no strategy is attached. Switch
// only on the thread that pushes (a posted reactor task), never between a reserve and its commit.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/shm_ring.hpp"

#include <x86intrin.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fastmm::venues {

enum class SinkPolicy : std::uint8_t {
  Drop = 0,  // market data: return false, count, notify
  Spin = 1,  // order events: spin up to spin_limit before giving up
};

class EventSink {
 public:
  using OverflowFn = void (*)(void* ctx, const EventSink& sink) noexcept;
  using DrainFn = void (*)(void* ctx) noexcept;

  EventSink() noexcept = default;
  EventSink(MsgRing* ring, SinkPolicy policy, std::uint32_t spin_limit = 1'000'000) noexcept
      : ring_(ring), policy_(policy), spin_limit_(spin_limit) {}

  void attach(MsgRing* ring, SinkPolicy policy, std::uint32_t spin_limit = 1'000'000) noexcept {
    ring_ = ring;
    shm_ = nullptr;
    policy_ = policy;
    spin_limit_ = spin_limit;
  }
  // A ring another process reads (fastmm-gateway).
  void attach(ShmRing* ring, SinkPolicy policy, std::uint32_t spin_limit = 1'000'000) noexcept {
    ring_ = nullptr;
    shm_ = ring;
    policy_ = policy;
    spin_limit_ = spin_limit;
  }
  void set_overflow_callback(OverflowFn fn, void* ctx) noexcept {
    overflow_fn_ = fn;
    overflow_ctx_ = ctx;
  }
  void set_drain_hook(DrainFn fn, void* ctx, bool on_commit) noexcept {
    drain_fn_ = fn;
    drain_ctx_ = ctx;
    drain_on_commit_ = fn != nullptr && on_commit;
  }
  [[nodiscard]] bool attached() const noexcept { return ring_ != nullptr || shm_ != nullptr; }
  [[nodiscard]] MsgRing* ring() const noexcept { return ring_; }
  [[nodiscard]] ShmRing* shm_ring() const noexcept { return shm_; }

  // In-place construction: reserve, fill, commit. nullptr when the ring is full (after the
  // policy's spin) or detached; the caller must not commit then.
  template <MessageLike M>
  [[nodiscard]] M* reserve(std::uint32_t len = static_cast<std::uint32_t>(sizeof(M))) noexcept {
    return reinterpret_cast<M*>(reserve_bytes(len));
  }
  [[nodiscard]] std::byte* reserve_bytes(std::uint32_t len) noexcept {
    if (!attached()) return nullptr;
    std::byte* p = try_reserve(len);
    if (FASTMM_LIKELY(p != nullptr)) return p;
    if (drain_fn_ != nullptr) {
      // The consumer is this thread: spinning frees nothing.
      drain_fn_(drain_ctx_);
      p = try_reserve(len);
      if (p != nullptr) return p;
    } else if (policy_ == SinkPolicy::Spin) {
      for (std::uint32_t i = 0; i < spin_limit_; ++i) {
        _mm_pause();
        p = try_reserve(len);
        if (p != nullptr) return p;
      }
    }
    on_overflow();
    return nullptr;
  }
  void commit() noexcept {
    if (FASTMM_LIKELY(ring_ != nullptr)) {
      ring_->commit();
    } else {
      shm_->commit();
    }
    ++pushed_;
    if (drain_on_commit_) drain_fn_(drain_ctx_);
  }

  // Copy-in: `m.len` bytes starting at the header.
  [[nodiscard]] bool push(const EventHeader& m) noexcept {
    std::byte* p = reserve_bytes(m.len);
    if (p == nullptr) return false;
    std::memcpy(p, &m, m.len);
    commit();
    return true;
  }

  [[nodiscard]] std::uint64_t pushed() const noexcept { return pushed_; }
  [[nodiscard]] std::uint64_t overflows() const noexcept { return overflows_; }
  [[nodiscard]] SinkPolicy policy() const noexcept { return policy_; }

 private:
  [[nodiscard]] FASTMM_FORCE_INLINE std::byte* try_reserve(std::uint32_t len) noexcept {
    return FASTMM_LIKELY(ring_ != nullptr) ? ring_->try_reserve(len) : shm_->try_reserve(len);
  }
  void on_overflow() noexcept {
    ++overflows_;
    if (overflow_fn_ != nullptr) overflow_fn_(overflow_ctx_, *this);
  }

  MsgRing* ring_ = nullptr;
  ShmRing* shm_ = nullptr;
  SinkPolicy policy_ = SinkPolicy::Drop;
  std::uint32_t spin_limit_ = 0;
  std::uint64_t pushed_ = 0;
  std::uint64_t overflows_ = 0;
  OverflowFn overflow_fn_ = nullptr;
  void* overflow_ctx_ = nullptr;
  DrainFn drain_fn_ = nullptr;
  void* drain_ctx_ = nullptr;
  bool drain_on_commit_ = false;
};

}  // namespace fastmm::venues
