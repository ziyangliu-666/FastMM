#include "fastmm/core/spin_mutex.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using namespace fastmm;

TEST_CASE("core.spin_mutex: try_lock fails while held and succeeds after unlock") {
  SpinMutex m;
  REQUIRE(m.try_lock());
  CHECK_FALSE(m.try_lock());
  m.unlock();
  CHECK(m.try_lock());
  m.unlock();
  {
    const std::lock_guard<SpinMutex> lock(m);
    CHECK_FALSE(m.try_lock());
  }
  CHECK(m.try_lock());
  m.unlock();
}

TEST_CASE("core.spin_mutex: threads more than the cores never lose an increment") {
  // More threads than a CI runner's cores: a holder is preempted with waiters spinning, which
  // the yield after kSpinsBeforeYield has to get through.
  SpinMutex m;
  std::uint64_t a = 0;
  std::uint64_t b = 0;  // == a, unless a section was entered twice at once
  constexpr int kThreads = 8;
  constexpr int kIters = 20'000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        const std::lock_guard<SpinMutex> lock(m);
        ++a;
        b = a;
      }
    });
  }
  for (std::thread& t : threads) t.join();
  CHECK(a == static_cast<std::uint64_t>(kThreads) * kIters);
  CHECK(b == a);
}
