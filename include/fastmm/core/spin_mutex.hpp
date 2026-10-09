#pragma once
// SpinMutex: a lock for critical sections of a few dozen nanoseconds shared by pinned threads (the
// IP weight of an account pool, venues/rate_limiter.hpp). std::mutex sends a waiter to the kernel
// (futex) as soon as the lock is taken, and the holder's unlock then pays a futex wake: a few
// microseconds on each side, on the path of an order. A waiter here spins with pause and yields
// the CPU only after kSpinsBeforeYield, which covers a holder preempted on a shared core.
// BasicLockable and Lockable: std::lock_guard and std::unique_lock take it.
#include <sched.h>
#include <x86intrin.h>

#include <atomic>
#include <cstdint>

namespace fastmm {

class SpinMutex {
 public:
  static constexpr std::uint32_t kSpinsBeforeYield = 1024;

  SpinMutex() noexcept = default;
  SpinMutex(const SpinMutex&) = delete;
  SpinMutex& operator=(const SpinMutex&) = delete;

  void lock() noexcept {
    // Test and test-and-set: waiters read the line shared and write it only when it looks free.
    while (locked_.exchange(true, std::memory_order_acquire)) {
      std::uint32_t spins = 0;
      while (locked_.load(std::memory_order_relaxed)) {
        if (++spins < kSpinsBeforeYield) {
          _mm_pause();
        } else {
          sched_yield();
          spins = 0;
        }
      }
    }
  }
  [[nodiscard]] bool try_lock() noexcept {
    return !locked_.load(std::memory_order_relaxed) &&
           !locked_.exchange(true, std::memory_order_acquire);
  }
  void unlock() noexcept { locked_.store(false, std::memory_order_release); }

 private:
  // A line of its own: the accounts' threads write it, and nothing next to it should move with it.
  alignas(64) std::atomic<bool> locked_{false};
};

static_assert(std::atomic<bool>::is_always_lock_free);

}  // namespace fastmm
