#include "test_support.hpp"

#include "fastmm/core/thread_utils.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

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

}  // namespace

TEST_CASE(
    "core.thread_utils: set_fifo_priority 0 leaves the thread alone, out of range is EINVAL") {
  std::thread t([] {});
  const int before = policy_of(t.native_handle());
  CHECK(set_fifo_priority(t.native_handle(), 0) == 0);
  CHECK(policy_of(t.native_handle()) == before);
  CHECK(set_fifo_priority(t.native_handle(), 100) == EINVAL);
  CHECK(set_fifo_priority(t.native_handle(), -5) == EINVAL);
  t.join();
}

TEST_CASE("core.thread_utils: set_fifo_priority without RLIMIT_RTPRIO or CAP_SYS_NICE is EPERM") {
  rlimit saved{};
  REQUIRE(getrlimit(RLIMIT_RTPRIO, &saved) == 0);
  rlimit none = saved;
  none.rlim_cur = 0;
  REQUIRE(setrlimit(RLIMIT_RTPRIO, &none) == 0);
  std::thread t([] {});
  const int err = set_fifo_priority(t.native_handle(), 10);
  // With CAP_SYS_NICE (root, a privileged container) the kernel ignores the limit.
  CHECK((err == EPERM || err == 0));
  if (err == 0) {
    CHECK(policy_of(t.native_handle()) == SCHED_FIFO);
    sched_param param{};
    pthread_setschedparam(t.native_handle(), SCHED_OTHER, &param);
  }
  t.join();
  REQUIRE(setrlimit(RLIMIT_RTPRIO, &saved) == 0);
}
