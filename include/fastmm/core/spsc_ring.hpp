#pragma once
// SpscRing<T, N>: wait-free single-producer / single-consumer ring of fixed-size records.
//
// Layout: producer index and consumer index live on their own 128-byte lines (Zen4 prefetches
// adjacent lines) and each side keeps a cached copy of the other's index so the common case
// touches only its own line. Storage is inline: allocate the ring itself with make_unique
// or in an Arena at startup.
#include "fastmm/core/config_macros.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace fastmm {

template <class T, std::size_t N>
class SpscRing {
  static_assert(std::has_single_bit(N), "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  static constexpr std::size_t kCapacity = N;

  SpscRing() noexcept = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // ---- producer ------------------------------------------------------------------------
  [[nodiscard]] FASTMM_FORCE_INLINE bool try_push(const T& v) noexcept {
    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    if (FASTMM_UNLIKELY(t - head_cache_ >= N)) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ >= N) return false;
    }
    buf_[t & (N - 1)] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  // Two-phase variant for large records: fill in place then publish.
  [[nodiscard]] FASTMM_FORCE_INLINE T* try_reserve() noexcept {
    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    if (FASTMM_UNLIKELY(t - head_cache_ >= N)) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ >= N) return nullptr;
    }
    return &buf_[t & (N - 1)];
  }
  FASTMM_FORCE_INLINE void commit() noexcept {
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  // ---- consumer ------------------------------------------------------------------------
  [[nodiscard]] FASTMM_FORCE_INLINE bool try_pop(T& out) noexcept {
    const T* p = front();
    if (p == nullptr) return false;
    out = *p;
    pop();
    return true;
  }
  // nullptr when empty; the pointer stays valid until pop().
  [[nodiscard]] FASTMM_FORCE_INLINE const T* front() noexcept {
    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return nullptr;
    }
    return &buf_[h & (N - 1)];
  }
  FASTMM_FORCE_INLINE void pop() noexcept {
    head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  // Approximate (either side may be mid-update); fine for stats.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  alignas(kDestructiveInterference) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t head_cache_ = 0;  // producer-private
  alignas(kDestructiveInterference) std::atomic<std::uint64_t> head_{0};
  std::uint64_t tail_cache_ = 0;  // consumer-private
  alignas(kDestructiveInterference) T buf_[N];
};

}  // namespace fastmm
