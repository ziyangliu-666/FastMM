#include "fastmm/core/rng.hpp"

#include "test_support.hpp"

#include <random>

using namespace fastmm;

TEST_CASE("core.rng: deterministic per seed, reasonable distribution") {
  Xoshiro256ss a(123);
  Xoshiro256ss b(123);
  for (int i = 0; i < 1000; ++i) CHECK(a.next() == b.next());
  Xoshiro256ss c(124);
  CHECK(a.next() != c.next());
  Xoshiro256ss z(0);  // zero seed must not yield the degenerate all-zero state
  CHECK(z.next() != 0);
  // uniform(n) in range, roughly uniform
  int counts[10] = {};
  for (int i = 0; i < 100'000; ++i) {
    const auto v = a.uniform(10);
    REQUIRE(v < 10);
    ++counts[v];
  }
  for (int cnt : counts) {
    CHECK(cnt > 9000);
    CHECK(cnt < 11000);
  }
  for (int i = 0; i < 1000; ++i) {
    const auto v = a.between(-5, 5);
    CHECK(v >= -5);
    CHECK(v <= 5);
  }
  for (int i = 0; i < 1000; ++i) {
    const double d = a.uniform01();
    CHECK(d >= 0.0);
    CHECK(d < 1.0);
  }
  std::uniform_int_distribution<int> dist(1, 6);
  const int die = dist(a);  // usable as a std generator
  CHECK(die >= 1);
  CHECK(die <= 6);
}
