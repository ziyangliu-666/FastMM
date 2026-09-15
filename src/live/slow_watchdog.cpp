#include "fastmm/live/slow_watchdog.hpp"

#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <utility>

namespace fastmm::live {

std::string slow_failure_cause(SlowFailure f) {
  switch (f) {
    case SlowFailure::None:
      return {};
    case SlowFailure::Exception:
      return "slow tier failed (Exception): a slow method raised";
    case SlowFailure::Timeout:
      return "slow tier failed (Timeout): a slow method ran past its timeout";
    case SlowFailure::FillsOverflow:
      return "slow tier failed (FillsOverflow): the fills ring was full";
    case SlowFailure::ThreadExited:
      return "slow tier failed (ThreadExited): the slow thread ended";
  }
  return "slow tier failed";
}

bool thread_alive(std::int64_t tid) noexcept {
  if (tid <= 0) return true;
  const long rc = ::syscall(SYS_tgkill, static_cast<long>(::getpid()), static_cast<long>(tid), 0L);
  return rc == 0 || errno != ESRCH;
}

std::function<std::string()> slow_tier_watchdog(std::shared_ptr<SlowChannel> channel,
                                                std::int64_t slow_tid) {
  return [ch = std::move(channel), slow_tid]() -> std::string {
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    SlowFailure f = ch->check(now);
    if (f == SlowFailure::None && !thread_alive(slow_tid)) {
      static_cast<void>(ch->fail(SlowFailure::ThreadExited));
      f = ch->failure();
    }
    return slow_failure_cause(f);
  };
}

}  // namespace fastmm::live
