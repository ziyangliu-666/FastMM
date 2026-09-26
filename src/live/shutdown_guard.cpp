#include "fastmm/live/shutdown_guard.hpp"

#include "fastmm/live/session.hpp"

#include <time.h>
#include <unistd.h>

#include <cstring>

namespace fastmm::live {

std::int64_t monotonic_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

void force_exit(const char* why) noexcept {
  static constexpr char kPrefix[] = "fastmm-live: ";
  // Best effort: nothing to do about a failed write on the way out.
  if (::write(STDERR_FILENO, kPrefix, sizeof kPrefix - 1) >= 0 &&
      ::write(STDERR_FILENO, why, std::strlen(why)) >= 0)
    static_cast<void>(!::write(STDERR_FILENO, "\n", 1));
  ::_exit(kExitRuntime);
}

ShutdownWatchdog::ShutdownWatchdog(std::int64_t bound_ns,
                                   ExpiryFn on_expiry,
                                   void* ctx,
                                   std::int64_t poll_ns)
    : bound_ns_(bound_ns), on_expiry_(on_expiry), ctx_(ctx), poll_ns_(poll_ns) {
  thread_ = std::thread([this] { run(); });
}

ShutdownWatchdog::~ShutdownWatchdog() {
  done();
  if (thread_.joinable()) thread_.join();
}

void ShutdownWatchdog::stop_requested(std::int64_t now_ns) noexcept {
  std::int64_t expected = 0;
  stop_ns_.compare_exchange_strong(expected, now_ns == 0 ? 1 : now_ns, std::memory_order_acq_rel);
}

void ShutdownWatchdog::run() noexcept {
  const timespec pause{poll_ns_ / 1'000'000'000, poll_ns_ % 1'000'000'000};
  while (!done_.load(std::memory_order_acquire)) {
    ::nanosleep(&pause, nullptr);
    const std::int64_t since = stop_ns_.load(std::memory_order_acquire);
    if (since == 0 || done_.load(std::memory_order_acquire)) continue;
    if (monotonic_ns() - since < bound_ns_) continue;
    fired_.store(true, std::memory_order_release);
    if (on_expiry_ != nullptr) {
      on_expiry_(ctx_);
      return;
    }
    force_exit(
        "shutdown did not finish within its bound (threads starved or a venue call stuck): "
        "exiting without waiting for cancel_all");
  }
}

}  // namespace fastmm::live
