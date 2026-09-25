#include "fastmm/core/thread_utils.hpp"

#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>

namespace fastmm {

bool pin_to_cpu(int cpu) noexcept {
  if (cpu < 0) return true;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

bool set_thread_name(const char* name) noexcept {
  return ::pthread_setname_np(::pthread_self(), name) == 0;
}

int current_cpu() noexcept {
  return ::sched_getcpu();
}

int cpu_count() noexcept {
  const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  return n < 1 ? 1 : static_cast<int>(n);
}

bool set_timer_slack(Duration slack) noexcept {
  if (slack.ns <= 0) return true;
  return ::prctl(PR_SET_TIMERSLACK, static_cast<unsigned long>(slack.ns), 0, 0, 0) == 0;
}

void Waker::wait(Duration timeout) noexcept {
  if (timeout.ns > 0) {
    timespec ts{};
    ts.tv_sec = timeout.ns / 1'000'000'000;
    ts.tv_nsec = timeout.ns % 1'000'000'000;
    // Returns at once (EAGAIN) when a producer already took the flag.
    ::syscall(
        SYS_futex, &flag_->word(), shared_ ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE, 1, &ts, nullptr, 0);
  }
  flag_->clear();
}

void Waker::wake_one() noexcept {
  ::syscall(
      SYS_futex, &flag_->word(), shared_ ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

int lock_all_memory() noexcept {
  return ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? 0 : errno;
}

void sleep_for(Duration d) noexcept {
  if (d.ns <= 0) return;
  timespec ts{};
  ts.tv_sec = d.ns / 1'000'000'000;
  ts.tv_nsec = d.ns % 1'000'000'000;
  ::nanosleep(&ts, nullptr);
}

}  // namespace fastmm
