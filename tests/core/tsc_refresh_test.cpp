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
  clk.attach_calibration_source(&pub);
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
  clk.attach_calibration_source(&pub);
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
