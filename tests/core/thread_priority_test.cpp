#include "test_support.hpp"

#include "fastmm/core/thread_utils.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include <atomic>
#include <cerrno>
#include <thread>

using namespace fastmm;

namespace {

int policy_of(pthread_t t) {
  int policy = -1;
  sched_param param{};
  pthread_getschedparam(t, &policy, &param);
  return policy;
}

// A thread that stays alive until stop(): pthread_setschedparam on one that has already run off
// its end answers ESRCH.
struct Parked {
  std::atomic<bool> done{false};
  std::thread t{[this] {
    while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
  }};
  void stop() {
    done.store(true, std::memory_order_release);
    t.join();
  }
};

}  // namespace

TEST_CASE(
    "core.thread_utils: set_fifo_priority 0 leaves the thread alone, out of range is EINVAL") {
  Parked p;
  const int before = policy_of(p.t.native_handle());
  CHECK(set_fifo_priority(p.t.native_handle(), 0) == 0);
  CHECK(policy_of(p.t.native_handle()) == before);
  CHECK(set_fifo_priority(p.t.native_handle(), 100) == EINVAL);
  CHECK(set_fifo_priority(p.t.native_handle(), -5) == EINVAL);
  p.stop();
}

TEST_CASE("core.thread_utils: set_fifo_priority without RLIMIT_RTPRIO or CAP_SYS_NICE is EPERM") {
  rlimit saved{};
  REQUIRE(getrlimit(RLIMIT_RTPRIO, &saved) == 0);
  rlimit none = saved;
  none.rlim_cur = 0;
  REQUIRE(setrlimit(RLIMIT_RTPRIO, &none) == 0);
  Parked p;
  const int err = set_fifo_priority(p.t.native_handle(), 10);
  // With CAP_SYS_NICE (root, a privileged container) the kernel ignores the limit.
  CHECK((err == EPERM || err == 0));
  if (err == 0) {
    CHECK(policy_of(p.t.native_handle()) == SCHED_FIFO);
    sched_param param{};
    pthread_setschedparam(p.t.native_handle(), SCHED_OTHER, &param);
  }
  p.stop();
  REQUIRE(setrlimit(RLIMIT_RTPRIO, &saved) == 0);
}

TEST_CASE("core.thread_utils: leave_pinned_cpus takes the main thread's CPUs and SCHED_OTHER") {
  cpu_set_t main_set;
  CPU_ZERO(&main_set);
  REQUIRE(sched_getaffinity(0, sizeof main_set, &main_set) == 0);
  if (CPU_COUNT(&main_set) < 2) return;  // nothing to tell apart on one CPU
  int first = 0;
  while (!CPU_ISSET(static_cast<unsigned>(first), &main_set)) ++first;
  cpu_set_t after;
  CPU_ZERO(&after);
  bool pinned = false;
  bool left = false;
  std::thread([&] {
    pinned = pin_to_cpu(first);  // the network thread a helper is started from
    left = leave_pinned_cpus();
    sched_getaffinity(0, sizeof after, &after);
  }).join();
  REQUIRE(pinned);
  CHECK(left);
  CHECK(CPU_EQUAL(&after, &main_set));
  CHECK(CPU_COUNT(&after) > 1);
}
