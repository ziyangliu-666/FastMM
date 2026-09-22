#include "fastmm/core/time.hpp"

#include "test_support.hpp"

#include "fastmm/core/thread_utils.hpp"

#include <sys/prctl.h>

#include <thread>

using namespace fastmm;

TEST_CASE("core.time: durations and timestamps") {
  CHECK(milliseconds(1).ns == 1'000'000);
  CHECK(seconds(2).millis() == 2000);
  Timestamp a{1000};
  Timestamp b = a + microseconds(1);
  CHECK((b - a).ns == 1000);
  CHECK(a < b);
  a += seconds(1);
  CHECK(a.ns == 1'000'001'000);
}

TEST_CASE("core.time: TscClock tracks CLOCK_REALTIME within tolerance") {
  TscClock clk;
  clk.calibrate(milliseconds(20));
  const auto& c = clk.calibration();
  INFO("use_tsc=" << c.use_tsc << " ghz=" << c.ghz);
  if (c.use_tsc) {
    CHECK(c.ghz > 0.5);
    CHECK(c.ghz < 10.0);
  }
  // Compare against wall time over a short interval.
  const Timestamp w0 = wall_now();
  const Timestamp t0 = clk.now();
  CHECK(std::llabs(w0.ns - t0.ns) < 1'000'000);  // < 1 ms right after calibration
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const Timestamp t1 = clk.now();
  const Timestamp w1 = wall_now();
  CHECK(t1 > t0);
  CHECK((t1 - t0).ns >= 25'000'000);
  // drift over 30 ms must stay small even with a coarse 20 ms calibration window
  CHECK(std::llabs((t1 - t0).ns - (w1 - w0).ns) < 5'000'000);
  // cycles → ns is monotone and consistent with to_timestamp
  const Cycles c0 = clk.cycles();
  const Cycles c1 = clk.cycles();
  CHECK(c1 >= c0);
  CHECK(clk.to_timestamp(c1) >= clk.to_timestamp(c0));
  CHECK(clk.cycles_to_ns(0) == 0);
}

TEST_CASE("core.time: TscClock without TSC falls back to clock_gettime") {
  TscCalibration c{};
  c.use_tsc = false;
  TscClock clk(c);
  const Timestamp w = wall_now();
  const Timestamp t = clk.now();
  CHECK(std::llabs(w.ns - t.ns) < 10'000'000);
  CHECK(clk.cycles_to_ns(42) == 42);
}

TEST_CASE("core.time: SimClock set/advance") {
  SimClock sc(Timestamp{100});
  CHECK(sc.now().ns == 100);
  sc.advance(milliseconds(1));
  CHECK(sc.now().ns == 1'000'100);
  sc.set(Timestamp{5'000'000});
  CHECK(sc.now().ns == 5'000'000);
  CHECK(sc.cycles().v == 5'000'000);
  CHECK(sc.to_timestamp(Cycles{7}).ns == 7);
  static_assert(ClockLike<SimClock>);
}

TEST_CASE("core.thread_utils: timer slack applies to the thread and the threads it starts") {
  // In a thread of its own so the test runner keeps its slack.
  std::thread([] {
    const long before = ::prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    CHECK(set_timer_slack(Duration{}));  // no-op
    CHECK(::prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) == before);
    REQUIRE(set_timer_slack(nanoseconds(1)));
    CHECK(::prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) == 1);
    long child = 0;
    std::thread([&child] { child = ::prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0); }).join();
    CHECK(child == 1);
  }).join();
}
