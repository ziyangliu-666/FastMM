#pragma once
// Seqlocked<T>: single-writer, many-reader publication of a trivially copyable snapshot.
// The writer never blocks; readers retry while a write is in progress (odd sequence) or
// the sequence changed underneath them. Used for top-of-book/position/latency observers.
#include "fastmm/core/config_macros.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace fastmm {

template <class T>
class Seqlocked {
  static_assert(std::is_trivially_copyable_v<T>, "seqlock payload must be memcpy-able");

 public:
  Seqlocked() noexcept : seq_(0), data_{} {}
  explicit Seqlocked(const T& init) noexcept : seq_(0), data_(init) {}

  // Writer side (one thread only).
  void store(const T& v) noexcept {
    const std::uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&data_, &v, sizeof(T));  // memcpy: readers may observe a torn copy; they retry
    std::atomic_thread_fence(std::memory_order_release);
    seq_.store(s + 2, std::memory_order_relaxed);
  }

  // Reader side: single attempt. Returns false if a write was in flight.
  [[nodiscard]] bool try_load(T& out) const noexcept {
    std::uint32_t version = 0;
    return try_load(out, version);
  }
  // Same, and on success reports the version() of the snapshot that was copied.
  [[nodiscard]] bool try_load(T& out, std::uint32_t& version) const noexcept {
    const std::uint32_t s1 = seq_.load(std::memory_order_acquire);
    if (s1 & 1U) return false;
    std::memcpy(&out, &data_, sizeof(T));
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t s2 = seq_.load(std::memory_order_relaxed);
    version = s1 >> 1;
    return s1 == s2;
  }

  // Reader side: spins until a consistent copy is obtained.
  [[nodiscard]] T load() const noexcept {
    T out;
    while (!try_load(out)) {
      __builtin_ia32_pause();
    }
    return out;
  }

  // Number of completed store() calls (a store in progress is not counted yet).
  [[nodiscard]] std::uint32_t version() const noexcept {
    return seq_.load(std::memory_order_acquire) >> 1;
  }

 private:
  alignas(kCacheLine) std::atomic<std::uint32_t> seq_;
  T data_;
};

}  // namespace fastmm
