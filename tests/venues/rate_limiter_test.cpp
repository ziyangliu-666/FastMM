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

TEST_CASE("venues.rate_limiter: a bulk request fits its share of the window only") {
  RateLimiter rl(0.9);
  REQUIRE(rl.add_weight_bucket(100, 60 * kSec));
  const std::int64_t now = kSec;
  CHECK(rl.can_send(45, now, false, RateLimiter::kBulkShare));
  CHECK_FALSE(rl.can_send(46, now, false, RateLimiter::kBulkShare));
  rl.on_sent(45, now);
  CHECK_FALSE(rl.can_send(1, now, false, RateLimiter::kBulkShare));  // the snapshots wait
  CHECK(rl.can_send(45, now));                                       // an order does not
  CHECK(rl.can_send(1, now + 60 * kSec, false, RateLimiter::kBulkShare));
}

TEST_CASE("venues.rate_limiter: pauses asked in a row wait longer each time") {
  RateLimiter rl;
  REQUIRE(rl.add_weight_bucket(100, kSec));
  rl.cooldown(kSec, 0);  // the first: Retry-After as asked
  CHECK(rl.cooldown_until() == kSec);
  CHECK(rl.streak() == 0);
  rl.cooldown(kSec, kSec);  // right after it ended: 2 s
  CHECK(rl.streak() == 1);
  CHECK(rl.cooldown_until() == 3 * kSec);
  rl.cooldown(kSec, 3 * kSec);  // 4 s
  CHECK(rl.streak() == 2);
  CHECK(rl.cooldown_until() == 7 * kSec);
  // During a pause (the requests already in flight come back 429): the same incident, the pause
  // only extends to its own wait.
  rl.cooldown(kSec, 4 * kSec);
  CHECK(rl.streak() == 2);
  CHECK(rl.cooldown_until() == 8 * kSec);
  // A longer Retry-After wins over the backoff.
  rl.cooldown(30 * kSec, 8 * kSec);
  CHECK(rl.streak() == 3);
  CHECK(rl.cooldown_until() == 38 * kSec);
  // Quiet for a minute after the pause: the streak is over.
  const std::int64_t later = 38 * kSec + RateLimiter::kStreakWindowNs + kSec;
  rl.cooldown(kSec, later);
  CHECK(rl.streak() == 0);
  CHECK(rl.cooldown_until() == later + kSec);
  CHECK(rl.cooldowns() == 6);
  // The backoff is capped.
  RateLimiter many;
  std::int64_t now = 0;
  for (int i = 0; i < 12; ++i) {
    many.cooldown(0, now);
    now = many.cooldown_until();
  }
  CHECK(many.cooldown_until() - (now - RateLimiter::kBackoffMaxNs) == RateLimiter::kBackoffMaxNs);
}

TEST_CASE("venues.rate_limiter: wait_for_weight sleeps until a bulk request fits") {
  RateLimiter rl(1.0);
  REQUIRE(rl.add_weight_bucket(100, kSec));
  rl.on_sent(50, 0);  // the share is spent
  std::int64_t slept = 0;
  wait_for_weight(rl, 10, 0, 5 * kSec, [&](std::int64_t ns) { slept += ns; });
  CHECK(slept == kSec);  // the window rolled
  slept = 0;
  rl.hard_stop();
  wait_for_weight(rl, 10, kSec, 2 * kSec, [&](std::int64_t ns) { slept += ns; });
  CHECK(slept == 2 * kSec);  // bounded
}

TEST_CASE("venues.rate_limiter: bybit remaining-style headers") {
  RateLimiter rl(1.0);
  REQUIRE(rl.add_weight_bucket(20, kSec));
  rl.on_remaining(20, 2, 0);
  CHECK(rl.can_send(2, 0));
  CHECK_FALSE(rl.can_send(3, 0));
}
