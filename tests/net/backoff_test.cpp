#include "fastmm/net/backoff.hpp"

#include "test_support.hpp"

using namespace fastmm::net;

TEST_CASE("backoff: deterministic doubling with a cap when jitter is 0") {
  ExponentialBackoff b(BackoffConfig{.base_ms = 250, .max_ms = 30'000, .jitter = 0.0});
  CHECK(b.next_ms() == 250);
  CHECK(b.next_ms() == 500);
  CHECK(b.next_ms() == 1000);
  CHECK(b.next_ms() == 2000);
  CHECK(b.next_ms() == 4000);
  CHECK(b.next_ms() == 8000);
  CHECK(b.next_ms() == 16000);
  CHECK(b.next_ms() == 30000);  // capped
  CHECK(b.next_ms() == 30000);
  CHECK(b.attempt() == 9);
  b.reset();
  CHECK(b.attempt() == 0);
  CHECK(b.next_ms() == 250);
  // Shift overflow guard: many attempts never wrap.
  for (int i = 1; i < 100; ++i) {
    const auto v = b.next_ms();
    if (i >= 7) CHECK(v == 30000);
  }
}

TEST_CASE("backoff: full jitter stays within [0, cap] and is seeded") {
  ExponentialBackoff a(BackoffConfig{.base_ms = 100, .max_ms = 1000, .jitter = 1.0}, 42);
  ExponentialBackoff b(BackoffConfig{.base_ms = 100, .max_ms = 1000, .jitter = 1.0}, 42);
  ExponentialBackoff c(BackoffConfig{.base_ms = 100, .max_ms = 1000, .jitter = 1.0}, 43);
  bool differs = false;
  for (std::uint32_t i = 0; i < 20; ++i) {
    const std::uint32_t cap = a.capped_delay(i);
    const auto va = a.next_ms();
    const auto vb = b.next_ms();
    const auto vc = c.next_ms();
    CHECK(va == vb);  // same seed -> same sequence
    CHECK(va <= cap);
    if (va != vc) differs = true;
  }
  CHECK(differs);
  CHECK(a.capped_delay(0) == 100);
  CHECK(a.capped_delay(3) == 800);
  CHECK(a.capped_delay(4) == 1000);
}

TEST_CASE("backoff: partial jitter keeps a floor") {
  ExponentialBackoff b(BackoffConfig{.base_ms = 1000, .max_ms = 1000, .jitter = 0.5}, 7);
  for (int i = 0; i < 50; ++i) {
    const auto v = b.next_ms();
    CHECK(v >= 500);
    CHECK(v <= 1000);
  }
}

TEST_CASE("backoff: config is sanitised") {
  ExponentialBackoff b(BackoffConfig{.base_ms = 500, .max_ms = 10, .jitter = 7.0});
  CHECK(b.config().max_ms == 500);
  CHECK(b.config().jitter == 1.0);
}
