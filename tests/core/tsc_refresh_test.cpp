// Periodic TSC recalibration: TscClock::refresh() from a Seqlocked<TscCalibration>, continuity
// across a re-anchor, the step path above the threshold, and a calibrator thread publishing
// while a reader refreshes and reads (run under TSan).
#include "test_support.hpp"

#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/time.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>

using namespace fastmm;

namespace {

constexpr double kQ32 = 4294967296.0;

// A synthetic TSC calibration: `ns_per_cycle` ns per cycle anchored at (tsc0, ns0).
TscCalibration synthetic(std::uint64_t tsc0, std::int64_t ns0, double ns_per_cycle) {
  TscCalibration c{};
  c.tsc0 = tsc0;
  c.ns0 = ns0;
  c.ns_per_cycle_q32 = static_cast<std::uint64_t>(ns_per_cycle * kQ32);
  c.ghz = 1.0 / ns_per_cycle;
  c.use_tsc = true;
  return c;
}

// Agrees with `base` at TSC reading `at` plus `offset_ns`, then runs at `ns_per_cycle`.
TscCalibration shifted(const TscCalibration& base,
                       std::uint64_t at,
                       std::int64_t offset_ns,
                       double ns_per_cycle) {
  return synthetic(at, tsc_to_ns(base, at) + offset_ns, ns_per_cycle);
}

}  // namespace

TEST_CASE("core.time: refresh copies a published calibration only when its version changes") {
  const TscCalibration a = synthetic(1000, 1'700'000'000'000'000'000, 0.4);
  Seqlocked<TscCalibration> pub(a);
  TscClock clk;
  CHECK(clk.refresh() == TscRefresh::None);  // no source attached
  // Slewing off: this test is about versioning, and checks the adopted rate exactly.
  clk.attach_calibration_source(&pub, TscClock::kDefaultStepThreshold, Duration{0});
  CHECK(clk.calibration_source() == &pub);
  CHECK(clk.refresh() == TscRefresh::Adopted);  // no TSC mapping yet: taken as is
  CHECK(clk.calibration().tsc0 == a.tsc0);
  CHECK(clk.calibration().ns0 == a.ns0);
  CHECK(clk.calibration().ns_per_cycle_q32 == a.ns_per_cycle_q32);
  for (int i = 0; i < 10; ++i) CHECK(clk.refresh() == TscRefresh::None);
  CHECK(clk.reanchors() == 0);

  const TscCalibration b = shifted(a, rdtsc().v, 10'000, 0.4 * (1 + 20e-6));
  pub.store(b);
  CHECK(clk.refresh() == TscRefresh::Reanchored);
  CHECK(clk.reanchors() == 1);
  CHECK(clk.calibration().ns_per_cycle_q32 == b.ns_per_cycle_q32);
  CHECK(clk.last_offset_ns() > 9'000);  // b is 10 us ahead of a, plus a few ns of rate change
  CHECK(clk.last_offset_ns() < 11'000);
  CHECK(clk.refresh() == TscRefresh::None);
  CHECK(clk.reanchors() == 1);
  CHECK(clk.steps() == 0);

  // A calibration without a TSC mapping never replaces a working one.
  pub.store(TscCalibration{});
  CHECK(clk.refresh() == TscRefresh::Rejected);
  CHECK(clk.calibration().use_tsc);
  CHECK(clk.refresh() == TscRefresh::None);
}

TEST_CASE("core.time: re-anchoring keeps time continuous and switches to the new rate") {
  const TscCalibration old = synthetic(0, 1'700'000'000'000'000'000, 0.4);
  const std::uint64_t at = 1'000'000'000'000;  // TSC reading of the refresh point
  for (const std::int64_t offset : {std::int64_t{600'000}, std::int64_t{-600'000}}) {
    CAPTURE(offset);
    TscClock clk(old);
    const Timestamp before = clk.to_timestamp(Cycles{at - 1});
    const Timestamp at_old = clk.to_timestamp(Cycles{at});
    const double new_rate = 0.4 * (1 + 100e-6);
    const TscCalibration fresh = shifted(old, at, offset, new_rate);
    CHECK(clk.reanchor(fresh, Cycles{at}) == TscRefresh::Reanchored);
    CHECK(clk.last_offset_ns() == offset);
    CHECK(clk.to_timestamp(Cycles{at}) == at_old);  // same time at the refresh point
    CHECK(clk.to_timestamp(Cycles{at}) >= before);
    Timestamp prev = at_old;
    for (std::uint64_t k = 1; k <= 10'000; ++k) {
      const Timestamp t = clk.to_timestamp(Cycles{at + k * 7919});
      REQUIRE(t >= prev);
      prev = t;
    }
    // New rate from the refresh point on: 1e12 cycles later is 1e12 * new_rate ns later.
    const std::int64_t later = (clk.to_timestamp(Cycles{at + 1'000'000'000'000}) - at_old).ns;
    CHECK(std::llabs(later - static_cast<std::int64_t>(1e12 * new_rate)) < 1'000);
    // Cycles stamped before the anchor (another thread's receive stamp) map to earlier times.
    CHECK(clk.to_timestamp(Cycles{at - 1'000}) < at_old);
    CHECK(clk.to_timestamp(Cycles{at - 1'000}).ns >= at_old.ns - 1'000);
    CHECK(clk.steps() == 0);
  }
}

TEST_CASE("core.time: successive now() calls never go backwards across refreshes") {
  TscClock clk;
  clk.calibrate(milliseconds(10));
  if (!clk.calibration().use_tsc) {
    MESSAGE("no invariant TSC on this machine; skipping");
    return;
  }
  Seqlocked<TscCalibration> pub(clk.calibration());
  clk.attach_calibration_source(&pub);
  const TscCalibration base = clk.calibration();
  const double rate = static_cast<double>(base.ns_per_cycle_q32) / kQ32;
  Timestamp prev = clk.now();
  int refreshed = 0;
  for (int i = 1; i <= 20'000; ++i) {
    if (i % 200 == 0) {
      // +-500 us off the base mapping and +-50 ppm off its rate: below the 1 ms threshold.
      const std::int64_t offset = (i % 400 == 0) ? 500'000 : -500'000;
      const double r = rate * (1 + ((i % 600 == 0) ? 50e-6 : -50e-6));
      pub.store(shifted(base, rdtsc().v, offset, r));
    }
    if (clk.refresh() != TscRefresh::None) ++refreshed;
    const Timestamp t = clk.now();
    REQUIRE(t >= prev);
    prev = t;
  }
  CHECK(refreshed == 100 + 1);  // the version current at attach time, then every publish
  CHECK(clk.steps() == 0);
}

TEST_CASE("core.time: a refresh steps when the old mapping is off by more than the threshold") {
  const TscCalibration old = synthetic(0, 1'700'000'000'000'000'000, 0.4);
  const std::uint64_t at = 5'000'000'000'000;
  {
    TscClock clk(old);
    const TscCalibration fresh = shifted(old, at, -5'000'000, 0.4);  // 5 ms behind
    CHECK(clk.reanchor(fresh, Cycles{at}) == TscRefresh::Stepped);
    CHECK(clk.steps() == 1);
    CHECK(clk.reanchors() == 0);
    CHECK(clk.last_offset_ns() == -5'000'000);
    CHECK(clk.calibration().tsc0 == fresh.tsc0);  // took the measured anchor
    CHECK(clk.calibration().ns0 == fresh.ns0);
    CHECK(clk.to_timestamp(Cycles{at}).ns == tsc_to_ns(old, at) - 5'000'000);
  }
  {
    // The threshold is configurable; an offset exactly at the threshold still re-anchors.
    Seqlocked<TscCalibration> pub(shifted(old, at, 2'000'000, 0.4));
    TscClock clk(old);
    clk.attach_calibration_source(&pub, milliseconds(2));
    CHECK(clk.step_threshold() == milliseconds(2));
    CHECK(clk.reanchor(pub.load(), Cycles{at}) == TscRefresh::Reanchored);
    TscClock strict(old);
    CHECK(strict.reanchor(pub.load(), Cycles{at}) == TscRefresh::Stepped);
  }
}

TEST_CASE("core.time: calibrator thread publishing while a reader refreshes and reads") {
  TscCalibration base = calibrate_tsc(milliseconds(5));
  if (!base.use_tsc) base = synthetic(rdtsc().v, wall_now().ns, 0.4);
  base.ghz = 0.0;  // the version current at attach time follows the same rate/ghz rule (k = 0)
  const std::uint64_t base_rate = base.ns_per_cycle_q32;
  // Publication k has rate base_rate + k % 7 and ghz == k % 7: a torn copy shows up as a rate
  // that does not match its ghz.
  auto rate_for = [base_rate](int k) { return base_rate + static_cast<std::uint64_t>(k % 7); };
  constexpr int kPublications = 4000;
  Seqlocked<TscCalibration> pub(base);
  std::atomic<bool> done{false};
  std::thread calibrator([&] {
    for (int k = 1; k <= kPublications; ++k) {
      TscCalibration c = base;
      c.tsc0 = rdtsc().v;
      c.ns0 = tsc_to_ns(base, c.tsc0);
      c.ns_per_cycle_q32 = rate_for(k);
      c.ghz = static_cast<double>(k % 7);
      pub.store(c);
      if (k % 256 == 0) std::this_thread::yield();
    }
    done.store(true);
  });
  std::uint64_t refreshes = 0;
  std::uint64_t torn = 0;
  std::uint64_t backwards = 0;
  TscClock clk(base);
  // Slewing off: torn copies are detected by the published rate, which slewing would adjust.
  clk.attach_calibration_source(&pub, TscClock::kDefaultStepThreshold, Duration{0});
  Timestamp prev = clk.now();
  bool last_pass = false;
  while (true) {
    // One more pass after the calibrator finished, so the final publication is picked up.
    if (done.load(std::memory_order_relaxed)) last_pass = true;
    const TscRefresh r = clk.refresh();
    if (r != TscRefresh::None) {
      ++refreshes;
      const TscCalibration& c = clk.calibration();
      if (c.ns_per_cycle_q32 != rate_for(static_cast<int>(c.ghz))) ++torn;
    }
    const Timestamp t = clk.now();
    if (r != TscRefresh::Stepped && t < prev) ++backwards;
    prev = t;
    if (last_pass) break;
  }
  calibrator.join();
  CHECK(refreshes > 0);
  CHECK(torn == 0);
  CHECK(backwards == 0);
  CHECK(clk.steps() == 0);
  CHECK(clk.calibration().ns_per_cycle_q32 == rate_for(kPublications));
}

TEST_CASE("core.time: slewing absorbs the measured offset over the horizon without a jump") {
  const TscCalibration old = synthetic(0, 1'700'000'000'000'000'000, 0.4);
  const std::uint64_t at = 1'000'000'000'000;
  const Duration horizon = seconds(10);
  const std::uint64_t horizon_cycles = static_cast<std::uint64_t>(10e9 / 0.4);
  for (const std::int64_t offset : {std::int64_t{400'000}, std::int64_t{-400'000}}) {
    CAPTURE(offset);
    Seqlocked<TscCalibration> pub(old);
    TscClock clk(old);
    clk.attach_calibration_source(&pub, TscClock::kDefaultStepThreshold, horizon);
    const TscCalibration fresh = shifted(old, at, offset, 0.4);  // same rate, offset only
    const Timestamp at_old = clk.to_timestamp(Cycles{at});
    CHECK(clk.reanchor(fresh, Cycles{at}) == TscRefresh::Reanchored);
    CHECK(clk.to_timestamp(Cycles{at}) == at_old);  // no jump at the refresh point
    // 400 us over 10 s is 40'000 ppb; allow 1 ppb of integer rounding.
    CHECK(std::llabs(clk.slew_ppb() - offset / 10) <= 1);
    // One horizon later the clock agrees with the fresh measurement.
    const std::int64_t err =
        clk.to_timestamp(Cycles{at + horizon_cycles}).ns - tsc_to_ns(fresh, at + horizon_cycles);
    CHECK(std::llabs(err) < 100);
  }
}

TEST_CASE("core.time: the slew rate correction is bounded") {
  const TscCalibration old = synthetic(0, 1'700'000'000'000'000'000, 0.4);
  const std::uint64_t at = 1'000'000'000'000;
  Seqlocked<TscCalibration> pub(old);
  TscClock clk(old);
  clk.attach_calibration_source(&pub, TscClock::kDefaultStepThreshold, seconds(1));
  // 900 us over 1 s would be 900 ppm; the servo caps it at kMaxSlewPpm.
  CHECK(clk.reanchor(shifted(old, at, 900'000, 0.4), Cycles{at}) == TscRefresh::Reanchored);
  CHECK(std::llabs(clk.slew_ppb() - TscClock::kMaxSlewPpm * 1'000) <= 1);  // integer rounding
  const std::uint64_t one_second = static_cast<std::uint64_t>(1e9 / 0.4);
  const std::int64_t gained =
      clk.to_timestamp(Cycles{at + one_second}).ns - tsc_to_ns(old, at + one_second);
  CHECK(std::llabs(gained - 500'000) < 100);  // only 500 us absorbed in the first second
}

TEST_CASE("core.time: repeated recalibrations converge on a drifting clock without steps") {
  // The "true" wall mapping runs 80 ppm faster than the clock's initial calibration and starts
  // 100 us ahead; the calibrator measures it exactly every 10 s.
  const TscCalibration initial = synthetic(0, 1'700'000'000'000'000'000, 0.4);
  const TscCalibration truth = shifted(initial, 0, 100'000, 0.4 * (1 + 80e-6));
  Seqlocked<TscCalibration> pub(initial);
  TscClock clk(initial);
  clk.attach_calibration_source(&pub, TscClock::kDefaultStepThreshold, seconds(10));
  const std::uint64_t period = static_cast<std::uint64_t>(10e9 / 0.4);
  std::int64_t first_offset = 0;
  for (std::uint64_t k = 1; k <= 12; ++k) {
    const std::uint64_t at = k * period;
    const TscCalibration measured = shifted(truth, at, 0, 0.4 * (1 + 80e-6));
    REQUIRE(clk.reanchor(measured, Cycles{at}) == TscRefresh::Reanchored);
    if (k == 1) first_offset = clk.last_offset_ns();
  }
  CHECK(std::llabs(first_offset) > 500'000);        // 100 us start + 800 us of drift in 10 s
  CHECK(std::llabs(clk.last_offset_ns()) < 1'000);  // converged
  CHECK(clk.steps() == 0);
}

// ---- long-baseline calibrator -------------------------------------------------------------------

namespace fake_clock {
// One hidden "true" time that advances only when the TSC is read (or advance() is called), so
// every reading is deterministic. Realtime can be stepped; raw readings can be given jitter.
std::uint64_t g_tsc = 0;
std::int64_t g_true_ns = 0;
std::int64_t g_wall_offset_ns = 0;
double g_ns_per_cycle = 0.25;
std::int64_t g_raw_jitter_ns = 0;  // added to every 5th raw reading
std::uint64_t g_raw_reads = 0;
std::int64_t g_wall_step_at_ns = -1;  // true time at which realtime steps by g_wall_step_ns
std::int64_t g_wall_step_ns = 0;

std::uint64_t read_tsc() noexcept {
  g_tsc += 400;
  g_true_ns += static_cast<std::int64_t>(400 * g_ns_per_cycle);
  return g_tsc;
}
std::int64_t read_realtime() noexcept {
  if (g_wall_step_at_ns >= 0 && g_true_ns >= g_wall_step_at_ns) {
    g_wall_offset_ns += g_wall_step_ns;
    g_wall_step_at_ns = -1;
  }
  return g_true_ns + g_wall_offset_ns;
}
std::int64_t read_raw() noexcept {
  return g_true_ns + ((++g_raw_reads % 5 == 0) ? g_raw_jitter_ns : 0);
}
void advance(std::int64_t ns) noexcept {
  g_tsc += static_cast<std::uint64_t>(static_cast<double>(ns) / g_ns_per_cycle);
  g_true_ns += ns;
}
ClockReadings reset(double ns_per_cycle) noexcept {
  g_tsc = 1'000'000;
  g_true_ns = 0;
  g_wall_offset_ns = 1'700'000'000'000'000'000;
  g_ns_per_cycle = ns_per_cycle;
  g_raw_jitter_ns = 0;
  g_raw_reads = 0;
  g_wall_step_at_ns = -1;
  g_wall_step_ns = 0;
  return ClockReadings{&read_tsc, &read_realtime, &read_raw, false};
}
}  // namespace fake_clock

namespace {
constexpr std::int64_t kPpmOfQuarter = 1074;  // 1 ppm of 0.25 ns/cycle in 32.32 fixed point
constexpr std::uint64_t kQuarterQ32 = 1'073'741'824;
std::int64_t q32_error(const TscCalibration& c) {
  return static_cast<std::int64_t>(c.ns_per_cycle_q32) - static_cast<std::int64_t>(kQuarterQ32);
}
}  // namespace

TEST_CASE("core.time: the calibrator measures the rate over the whole baseline") {
  TscCalibrator cal(fake_clock::reset(0.25));
  REQUIRE(cal.start(milliseconds(50)).use_tsc);
  fake_clock::advance(10'000'000'000);
  const TscRecalibration r = cal.update();
  REQUIRE(r.ok);
  CHECK(std::llabs(q32_error(r.calibration)) <= kPpmOfQuarter);
  CHECK(std::llabs(r.drift_ns) < 1'000);
  CHECK(r.host_step_ns == 0);
  CHECK(r.elapsed_s > 9.9);
}

TEST_CASE("core.time: a host wall-clock step is reported and does not distort the rate") {
  TscCalibrator cal(fake_clock::reset(0.25));
  REQUIRE(cal.start(milliseconds(50)).use_tsc);
  fake_clock::advance(10'000'000'000);
  fake_clock::g_wall_offset_ns += 1'550'000'000;  // what WSL2 does when it resyncs with Windows
  const TscRecalibration r = cal.update();
  REQUIRE(r.ok);
  CHECK(r.host_step_ns == 1'550'000'000);
  CHECK(std::llabs(r.drift_ns - 1'550'000'000) < 1'000);
  CHECK(std::llabs(q32_error(r.calibration)) <= kPpmOfQuarter);  // rate from CLOCK_MONOTONIC_RAW
  CHECK(r.calibration.ns0 > fake_clock::g_wall_offset_ns);       // anchored on CLOCK_REALTIME
}

TEST_CASE("core.time: a host wall-clock step inside the startup window does not distort the rate") {
  // calibrate_tsc() (TscClock::calibrate(), the integration tests' live engine) used to take the
  // rate from CLOCK_REALTIME over its 50 ms window. On WSL2 the wall clock steps by 0.5-1.5 s every
  // 10-40 s; a step inside the window made the clock run tens of times fast, and every order then
  // failed the stale-market-data check until the process ended.
  const ClockReadings readings = fake_clock::reset(0.25);
  fake_clock::g_wall_step_at_ns = 20'000'000;
  fake_clock::g_wall_step_ns = 1'450'000'000;
  const TscCalibration c = calibrate_tsc(milliseconds(50), readings);
  REQUIRE(c.use_tsc);
  CHECK(fake_clock::g_wall_step_at_ns < 0);  // the step happened inside the window
  CHECK(std::llabs(q32_error(c)) <= kPpmOfQuarter);
  CHECK(c.ns0 > fake_clock::g_wall_offset_ns);  // anchored on CLOCK_REALTIME after the step
}

TEST_CASE("core.time: anchor jitter costs a few ppm over a long baseline") {
  // 60 us on some raw readings: over the 50 ms startup window that is up to 1200 ppm of rate
  // error, over a 10 s baseline at most 6 ppm.
  TscCalibrator cal(fake_clock::reset(0.25));
  fake_clock::g_raw_jitter_ns = 60'000;
  const TscCalibration initial = cal.start(milliseconds(50));
  REQUIRE(initial.use_tsc);
  MESSAGE("startup rate error: " << static_cast<double>(q32_error(initial)) / kPpmOfQuarter
                                 << " ppm");
  fake_clock::advance(10'000'000'000);
  const TscRecalibration r = cal.update();
  REQUIRE(r.ok);
  CHECK(std::llabs(q32_error(r.calibration)) <= 20 * kPpmOfQuarter);
}

TEST_CASE("core.time: the calibrator refuses a baseline that is too short") {
  TscCalibrator cal(fake_clock::reset(0.25));
  REQUIRE(cal.start(milliseconds(50)).use_tsc);
  fake_clock::advance(100'000'000);
  CHECK_FALSE(cal.update().ok);
  fake_clock::advance(1'000'000'000);
  CHECK(cal.update().ok);
}
