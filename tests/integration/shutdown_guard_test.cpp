// Bounded shutdown (live/shutdown_guard.hpp): the second-signal rule, the watchdog's timing, and a
// child process whose shutdown never finishes being ended by the watchdog with kExitRuntime.
#include "fastmm/live/shutdown_guard.hpp"

#include "test_support.hpp"

#include "fastmm/live/session.hpp"

#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <thread>

using namespace fastmm;
using namespace fastmm::live;

namespace {
constexpr std::int64_t kMs = 1'000'000;
void sleep_ms(std::int64_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
void count_expiry(void* ctx) noexcept {
  static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
}
}  // namespace

TEST_CASE("live.shutdown_guard: a second stop signal forces the exit only after the grace") {
  static_assert(!second_signal_forces_exit(0, 100 * kSecondSignalAfterNs));  // the first signal
  CHECK_FALSE(second_signal_forces_exit(1'000, 1'000 + kSecondSignalAfterNs - 1));
  CHECK(second_signal_forces_exit(1'000, 1'000 + kSecondSignalAfterNs));
}

TEST_CASE("live.shutdown_guard: the watchdog fires its bound after the stop, not before") {
  std::atomic<int> fired{0};
  {
    ShutdownWatchdog w(200 * kMs, &count_expiry, &fired, 5 * kMs);
    sleep_ms(300);
    CHECK(fired.load() == 0);  // no stop requested: never
    w.stop_requested();
    const std::int64_t first = w.stop_requested_at();
    w.stop_requested();  // the first request counts
    CHECK(w.stop_requested_at() == first);
    sleep_ms(100);
    CHECK(fired.load() == 0);
    sleep_ms(400);
    CHECK(fired.load() == 1);
    CHECK(w.fired());
  }
  std::atomic<int> done_first{0};
  {
    ShutdownWatchdog w(200 * kMs, &count_expiry, &done_first, 5 * kMs);
    w.stop_requested();
    sleep_ms(50);
    w.done();  // the shutdown finished in time
    sleep_ms(300);
  }
  CHECK(done_first.load() == 0);
}

TEST_CASE("live.shutdown_guard: a shutdown that never finishes ends with kExitRuntime") {
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // A stop was requested and then nothing: the threads that would finish it never run.
    ShutdownWatchdog w(100 * kMs, nullptr, nullptr, 5 * kMs);
    w.stop_requested();
    for (;;) ::pause();
  }
  int status = 0;
  const std::int64_t start = monotonic_ns();
  pid_t r = 0;
  while ((r = ::waitpid(child, &status, WNOHANG)) == 0 && monotonic_ns() - start < 5'000 * kMs)
    sleep_ms(10);
  if (r == 0) {
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    FAIL("the watchdog did not end the child");
  }
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == kExitRuntime);
}
