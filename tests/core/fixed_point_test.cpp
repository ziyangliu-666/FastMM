// Ratio, exact literals and exponent-aware parsing (ADR-0012 section 3).
#include "fastmm/core/fixed_point.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::literals;

namespace {
// Compile-time exactness: these are static_asserts, not runtime checks.
static_assert((0.00000001_qty).raw == 1);
static_assert((1e-8_qty).raw == 1);
static_assert((100.25_px).raw == 10'025'000'000);
static_assert((1'000.5_px).raw == 100'050'000'000);
static_assert((0.01_qty).raw == 1'000'000);
static_assert((5_bps).raw == 50'000);
static_assert((0.25_bps).raw == 2'500);
static_assert((0.0001_bps).raw == 1);
static_assert((10000_bps).raw == kFixedScale);
static_assert(100_px == Price::from_int(100));  // integer overloads still work
static_assert(2_qty == Qty::from_int(2));
static_assert((100_px * 5_bps).raw == 5'000'000);  // 100 * 0.0005 = 0.05
static_assert((5_bps * 100_px).raw == 5'000'000);
}  // namespace

TEST_CASE("core.fixed: literals are exact") {
  CHECK((0.00000001_qty).raw == 1);
  CHECK(92233720368.54775807_px == Price::max());
  CHECK(-0.5_px == Price::from_raw(-50'000'000));
  CHECK(1.5e3_px == Price::from_int(1500));
  CHECK((2.5_bps).to_bps() == doctest::Approx(2.5));
}

TEST_CASE("core.fixed: parse accepts exponent notation exactly") {
  const std::vector<std::pair<std::string, std::int64_t>> good = {
      {"2e-05", 2'000},
      {"0.1", 10'000'000},
      {"1e-8", 1},
      {"1E-8", 1},
      {"1.5E3", 150'000'000'000},
      {"1e+2", 10'000'000'000},
      {"-2.5e-3", -250'000},
      {"+3", 300'000'000},
      {"0.000000010e0", 1},
      {"100000000e-16", 1},
      {"0.00000000000000000000e30", 0},
      {"5.", 500'000'000},
      {".5", 50'000'000},
      {"92233720368.54775807", 9'223'372'036'854'775'807},
      {"9.223372036854775807e10", 9'223'372'036'854'775'807},
      {"1.500000000000", 150'000'000},
  };
  for (const auto& [s, raw] : good) {
    CAPTURE(s);
    const auto q = Qty::parse(s);
    REQUIRE(q.has_value());
    CHECK(q->raw == raw);
  }
  for (const char* bad : {"",
                          "-",
                          ".",
                          "e5",
                          "1e",
                          "1e+",
                          "1.5e-8",
                          "0.000000001",
                          "1e11",
                          "92233720368.54775808",
                          " 1",
                          "1 ",
                          "1,5",
                          "1.2.3",
                          "0x10",
                          "inf",
                          "nan",
                          "1e99999999999"}) {
    CAPTURE(bad);
    CHECK_FALSE(Qty::parse(bad).has_value());
  }
  // from_decimal stays strict for venue strings.
  CHECK_FALSE(Qty::from_decimal("2e-05").has_value());
}

TEST_CASE("core.fixed: Ratio operators truncate toward zero once") {
  CHECK(Ratio::from_bps(2.5).raw == 25'000);
  CHECK(Ratio::from_bps(-0.00005).raw == -1);  // rounded to 0.0001 bp
  CHECK(Ratio::from_raw(12'345).to_bps() == doctest::Approx(1.2345));
  CHECK(ratio(1_px, 4_px).raw == 25'000'000);
  CHECK(ratio(Price::from_int(-1), 3_px).raw == -33'333'333);
  CHECK(ratio(1_px, Price{}).is_zero());
  CHECK((Price::from_raw(15) * Ratio::from_raw(50'000'000)).raw == 7);
  CHECK((Price::from_raw(-15) * Ratio::from_raw(50'000'000)).raw == -7);  // not -8
  CHECK((Qty::from_raw(15) * Ratio::from_raw(-50'000'000)).raw == -7);
  CHECK((5_bps * 5_bps).raw == 25);  // 0.0005 * 0.0005 = 2.5e-7
  CHECK((Notional::from_int(1'000'000) * 1_bps) == Notional::from_int(100));
  // A large value does not overflow the intermediate product.
  CHECK((Price::max() * Ratio::from_raw(kFixedScale)) == Price::max());
}

// BasicMM carried bps as centi-bps: half = mid.raw * cbps / 1e6, skew = mid.raw * cbps * -inv / 1e6
// and centre = mid + skew. With Ratio (1 cbps == 100 raw) it is mid * r and mid - mid * (r * inv).
TEST_CASE("core.fixed: mid * Ratio equals the centi-bps Int128 formula (property)") {
  std::mt19937_64 rng(20260914);
  std::uniform_int_distribution<std::int64_t> mid_small(-1'000'000, 1'000'000);
  std::uniform_int_distribution<std::int64_t> mid_large(-100'000'000'000'000, 100'000'000'000'000);
  std::uniform_int_distribution<std::int64_t> cbps_dist(0, 1'000'000);  // up to 10'000 bps
  std::uniform_int_distribution<std::int64_t> inv_dist(-10'000, 10'000);
  for (int n = 0; n < 200'000; ++n) {
    const std::int64_t mid_raw = (n % 4 == 0) ? mid_small(rng) : mid_large(rng);
    const std::int64_t cbps = (n % 7 == 0) ? cbps_dist(rng) % 100 : cbps_dist(rng);
    const std::int64_t inv = (n % 5 == 0) ? inv_dist(rng) % 3 : inv_dist(rng);
    const Price mid = Price::from_raw(mid_raw);
    const Ratio r = Ratio::from_raw(cbps * 100);

    const auto old_half =
        static_cast<std::int64_t>(static_cast<Int128>(mid_raw) * cbps / 1'000'000);
    const auto old_skew =
        static_cast<std::int64_t>(static_cast<Int128>(mid_raw) * cbps * -inv / 1'000'000);
    const Price old_centre = mid + Price::from_raw(old_skew);

    const Price half = mid * r;
    const Price centre = mid - mid * (r * inv);
    if (half.raw != old_half || centre != old_centre) {
      CAPTURE(mid_raw);
      CAPTURE(cbps);
      CAPTURE(inv);
      CHECK(half.raw == old_half);
      CHECK(centre == old_centre);
      break;
    }
  }
}
