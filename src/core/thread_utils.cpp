#include "fastmm/core/thread_utils.hpp"

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

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

void sleep_for(Duration d) noexcept {
  if (d.ns <= 0) return;
  timespec ts{};
  ts.tv_sec = d.ns / 1'000'000'000;
  ts.tv_nsec = d.ns % 1'000'000'000;
  ::nanosleep(&ts, nullptr);
}

}  // namespace fastmm
