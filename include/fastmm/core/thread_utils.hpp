#pragma once
// Thread affinity/naming, the spin-wait policy of the engine and network loops, and the wake-up
// primitive their producers use to signal a thread that blocked while idle.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/time.hpp"

#include <x86intrin.h>

#include <atomic>
#include <cstdint>

namespace fastmm {

// cpu < 0 is a no-op (returns true). Implemented in src/core/thread_utils.cpp.
bool pin_to_cpu(int cpu) noexcept;
bool set_thread_name(const char* name) noexcept;  // <= 15 chars
[[nodiscard]] int current_cpu() noexcept;
[[nodiscard]] int cpu_count() noexcept;
void sleep_for(Duration d) noexcept;  // nanosleep
// PR_SET_TIMERSLACK for the calling thread; threads it creates afterwards inherit the value.
// The kernel default (50 us) lets a 50 us nanosleep return up to 50 us late. slack.ns <= 0 is a
// no-op. Returns false when prctl refused.
bool set_timer_slack(Duration slack) noexcept;
// mlockall(MCL_CURRENT | MCL_FUTURE): no page of the process is paged out, and later
// allocations are faulted in by the allocating call. Returns errno (0 on success); ENOMEM or
// EPERM usually mean RLIMIT_MEMLOCK (ulimit -l) is too small.
int lock_all_memory() noexcept;

enum class SpinMode : std::uint8_t { Busy = 0, Adaptive = 1 };

// Busy: always _mm_pause. Adaptive (WSL2 default): pause for `spins` idle iterations; after that
// idle() sleeps `sleep` per idle iteration and spin() returns false (the caller blocks on a Waker),
// until activity resets the counter.
class SpinPolicy {
 public:
  explicit SpinPolicy(SpinMode mode = SpinMode::Busy,
                      std::uint32_t spins = 20'000,
                      Duration sleep = microseconds(50)) noexcept
      : mode_(mode), spins_(spins), sleep_(sleep) {}

  // True: paused, keep polling. False: the spin budget is used up.
  [[nodiscard]] FASTMM_FORCE_INLINE bool spin() noexcept {
    if (mode_ == SpinMode::Busy || idle_ < spins_) {
      ++idle_;
      _mm_pause();
      return true;
    }
    return false;
  }
  FASTMM_FORCE_INLINE void idle() noexcept {
    if (!spin()) sleep_for(sleep_);
  }
  FASTMM_FORCE_INLINE void active() noexcept { idle_ = 0; }
  [[nodiscard]] SpinMode mode() const noexcept { return mode_; }

 private:
  SpinMode mode_;
  std::uint32_t spins_;
  std::uint32_t idle_ = 0;
  Duration sleep_;
};

// A flag a consumer sets before it blocks and a producer takes after it published work:
//
//   consumer: flag.set(); if (work pending) flag.clear(); else { block; flag.clear(); }
//   producer: publish; if (flag.take()) wake the consumer;
//
// set() and take() are sequentially consistent exchanges (locked instructions on x86): either the
// consumer's recheck sees the producer's publication, or the producer sees the flag and wakes it.
class SleepFlag {
 public:
  FASTMM_FORCE_INLINE void set() noexcept {
    static_cast<void>(word_.exchange(1, std::memory_order_seq_cst));
  }
  FASTMM_FORCE_INLINE void clear() noexcept { word_.store(0, std::memory_order_relaxed); }
  // True when the consumer had set the flag; clears it.
  [[nodiscard]] FASTMM_FORCE_INLINE bool take() noexcept {
    return word_.exchange(0, std::memory_order_seq_cst) != 0;
  }
  [[nodiscard]] bool is_set() const noexcept { return word_.load(std::memory_order_acquire) != 0; }
  [[nodiscard]] std::atomic<std::uint32_t>& word() noexcept { return word_; }

 private:
  std::atomic<std::uint32_t> word_{0};
};

// A SleepFlag in shared memory is used by two processes at once.
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

// SleepFlag with a futex: one consumer blocks in wait(), any number of producers call notify().
// notify() costs a locked exchange while the consumer is awake and a futex wake-up only while it
// is blocked.
//
// Across processes: share() moves the flag into a MAP_SHARED mapping both processes hold (the
// gateway's wake page, live/gateway.hpp) and switches to shared futex operations. Each process
// then has its own Waker over the same word; any of them may notify, one of them waits.
class Waker {
 public:
  Waker() noexcept = default;
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;

  // Before any thread uses this Waker. `flag` is a SleepFlag in shared memory that outlives it.
  void share(SleepFlag* flag) noexcept {
    flag_ = flag;
    shared_ = true;
  }
  [[nodiscard]] bool shared() const noexcept { return shared_; }

  FASTMM_FORCE_INLINE void prepare_wait() noexcept { flag_->set(); }
  FASTMM_FORCE_INLINE void cancel_wait() noexcept { flag_->clear(); }
  // After prepare_wait() and a recheck that found no work: blocks until notify() or `timeout`.
  void wait(Duration timeout) noexcept;
  FASTMM_FORCE_INLINE void notify() noexcept {
    if (flag_->take()) wake_one();
  }
  [[nodiscard]] bool waiting() const noexcept { return flag_->is_set(); }

 private:
  void wake_one() noexcept;
  SleepFlag own_;
  SleepFlag* flag_ = &own_;
  bool shared_ = false;
};

}  // namespace fastmm
