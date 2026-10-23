#pragma once
// Bounds on how long fastmm-live takes to stop once it was asked to (SIGINT/SIGTERM, --duration,
// fastmm-ctl stop, a kill switch). A stop normally trips the kill switch, cancels all over REST and
// joins the engine and network threads in well under a second; a process that does not get there
// (its threads starved by another busy-spinning engine on the same CPUs, a venue call that never
// returns) must still end:
//   * a second SIGINT/SIGTERM at least kSecondSignalAfterNs after the first exits at once;
//   * ShutdownWatchdog exits kShutdownBoundNs after the stop was requested.
// Both write one line to stderr and _exit(kExitRuntime) without cancel_all: what still rests at the
// venue is cancelled by its dead man's switch where it has one, and by the next start's sweep.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace fastmm::live {

inline constexpr std::int64_t kSecondSignalAfterNs = 5'000'000'000;
inline constexpr std::int64_t kShutdownBoundNs = 60'000'000'000;

// CLOCK_MONOTONIC in ns; async-signal-safe.
[[nodiscard]] std::int64_t monotonic_ns() noexcept;

// Whether a stop signal at `now_ns` forces the exit, the first having come at `first_ns` (0: none).
[[nodiscard]] constexpr bool second_signal_forces_exit(std::int64_t first_ns,
                                                       std::int64_t now_ns) noexcept {
  return first_ns != 0 && now_ns - first_ns >= kSecondSignalAfterNs;
}

// The control loops (fastmm-live's main thread, fastmm-gateway's) wait in ::poll(2) on this eventfd
// besides their sockets, and the signal handlers write it: a SIGINT, SIGTERM or SIGHUP is acted on
// at once, not at the end of the loop's period. Made on the first call, which comes before a
// handler is installed; -1 when eventfd() failed (the loops then wait out their period).
[[nodiscard]] int signal_wake_fd() noexcept;
// Wakes a loop waiting on signal_wake_fd(); async-signal-safe.
void signal_wake() noexcept;
// Takes the wake-up, once the loop is awake.
void signal_wake_drain() noexcept;

// Writes `why` and a newline to stderr and _exit()s with kExitRuntime; async-signal-safe.
[[noreturn]] void force_exit(const char* why) noexcept;

class ShutdownWatchdog {
 public:
  using ExpiryFn = void (*)(void* ctx) noexcept;

  // Default expiry: force_exit(). `poll_ns` is how often the thread looks.
  explicit ShutdownWatchdog(std::int64_t bound_ns = kShutdownBoundNs,
                            ExpiryFn on_expiry = nullptr,
                            void* ctx = nullptr,
                            std::int64_t poll_ns = 100'000'000);
  ~ShutdownWatchdog();  // done(), then joins
  ShutdownWatchdog(const ShutdownWatchdog&) = delete;
  ShutdownWatchdog& operator=(const ShutdownWatchdog&) = delete;

  // The first call starts the clock (async-signal-safe: one atomic compare-exchange).
  void stop_requested(std::int64_t now_ns = monotonic_ns()) noexcept;
  [[nodiscard]] std::int64_t stop_requested_at() const noexcept {
    return stop_ns_.load(std::memory_order_acquire);
  }
  // The shutdown finished: the watchdog will not fire. Wakes the thread, so that the destructor
  // does not wait out a poll period to join it.
  void done() noexcept;
  [[nodiscard]] bool fired() const noexcept { return fired_.load(std::memory_order_acquire); }

 private:
  void run() noexcept;

  std::int64_t bound_ns_;
  ExpiryFn on_expiry_;
  void* ctx_;
  std::int64_t poll_ns_;
  std::atomic<std::int64_t> stop_ns_{0};
  std::atomic<bool> done_{false};
  std::mutex mutex_;  // the thread's wait; stop_requested() (a signal handler) never takes it
  std::condition_variable wake_;
  std::atomic<bool> fired_{false};
  std::thread thread_;
};

}  // namespace fastmm::live
