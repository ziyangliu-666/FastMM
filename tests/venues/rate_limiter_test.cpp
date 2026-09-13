#include "fastmm/venues/rate_limiter.hpp"

#include "test_support.hpp"

using namespace fastmm::venues;

namespace {
constexpr std::int64_t kSec = 1'000'000'000;
}

TEST_CASE("venues.rate_limiter: weight bucket refuses above threshold and rolls windows") {
  RateLimiter rl(0.9);
  REQUIRE(rl.add_weight_bucket(100, 60 * kSec));
  std::int64_t now = 10 * kSec;
  CHECK(rl.can_send(50, now));
  rl.on_sent(50, now);
  CHECK(rl.can_send(40, now));  // 90 <= 90
  CHECK_FALSE(rl.can_send(41, now));
  rl.on_sent(40, now);
  CHECK_FALSE(rl.can_send(1, now));
  now += 60 * kSec;  // window rolled
  CHECK(rl.can_send(90, now));
  CHECK(rl.weight_bucket(0)->used == 0);
}

TEST_CASE("venues.rate_limiter: venue headers overwrite the local estimate") {
  RateLimiter rl(1.0);
  REQUIRE(rl.add_weight_bucket(6000, 60 * kSec));
  REQUIRE(rl.add_order_bucket(50, 10 * kSec));
  const std::int64_t now = kSec;
  rl.on_sent(1, now, true);
  rl.on_headers(5990, 49, now);
  CHECK(rl.can_send(10, now, false));
  CHECK_FALSE(rl.can_send(11, now, false));
  CHECK(rl.can_send(1, now, true));
  rl.on_sent(1, now, true);
  CHECK_FALSE(rl.can_send(1, now, true));
  CHECK(rl.can_send(1, now, false));  // weight ok, only orders exhausted
  rl.on_headers(-1, -1, now);         // absent headers change nothing
  CHECK_FALSE(rl.can_send(1, now, true));
}

TEST_CASE("venues.rate_limiter: cooldown and hard stop") {
  RateLimiter rl;
  REQUIRE(rl.add_weight_bucket(100, kSec));
  std::int64_t now = 0;
  rl.cooldown(5 * kSec, now);
  CHECK(rl.in_cooldown(now));
  CHECK(rl.cooldown_until() == 5 * kSec);
  CHECK_FALSE(rl.can_send(1, now));
  now = 5 * kSec;
  CHECK(rl.can_send(1, now));
  CHECK(rl.cooldowns() == 1);
  rl.hard_stop();
  CHECK_FALSE(rl.can_send(1, now));
  rl.clear_hard_stop();
  CHECK(rl.can_send(1, now));
}

TEST_CASE("venues.rate_limiter: bybit remaining-style headers") {
  RateLimiter rl(1.0);
  REQUIRE(rl.add_weight_bucket(20, kSec));
  rl.on_remaining(20, 2, 0);
  CHECK(rl.can_send(2, 0));
  CHECK_FALSE(rl.can_send(3, 0));
}
