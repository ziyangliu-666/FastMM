#include "fastmm/venues/rate_limiter.hpp"

#include "test_support.hpp"

#include "fastmm/venues/connector_common.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

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

TEST_CASE("venues.rate_limiter: on a shared IP an older weight header does not lower it") {
  RateLimiter rl(1.0);
  rl.share_ip(shared_rate("test-host-older-header"));
  REQUIRE(rl.add_weight_bucket(6000, 60 * kSec));
  const std::int64_t now = kSec;
  rl.on_sent(3000, now);        // a burst in flight
  rl.on_headers(200, -1, now);  // the answer to its first request
  CHECK_FALSE(rl.can_send(3001, now));
  CHECK(rl.can_send(3000, now));
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

TEST_CASE("venues.rate_limiter: accounts sharing an IP spend one weight window and one cooldown") {
  const auto ip = shared_rate("test-host-shared-weight");
  RateLimiter a(1.0);
  RateLimiter b(1.0);
  a.share_ip(ip);
  b.share_ip(ip);
  REQUIRE(a.add_weight_bucket(100, 60'000'000'000));
  REQUIRE(b.add_weight_bucket(100, 60'000'000'000));  // the same bucket: not a second one
  REQUIRE(a.add_order_bucket(10, 10'000'000'000));
  REQUIRE(b.add_order_bucket(10, 10'000'000'000));
  const std::int64_t t = 1'000'000'000;
  a.on_sent(60, t);
  CHECK(b.can_send(40, t));
  CHECK_FALSE(b.can_send(41, t));  // a's 60 count against b
  // Order counts stay each account's own.
  for (int i = 0; i < 10; ++i) a.on_sent(0, t, true);
  CHECK_FALSE(a.can_send(0, t, true));
  CHECK(b.can_send(0, t, true));
  // A 429 seen by one pauses both; a 418 stops both.
  b.cooldown(5'000'000'000, t);
  CHECK(a.in_cooldown(t + 1));
  CHECK_FALSE(a.can_send(1, t + 1));
  CHECK(a.can_send(1, t + 6'000'000'000));
  a.hard_stop();
  CHECK(b.hard_stopped());
  b.clear_hard_stop();
  CHECK_FALSE(a.hard_stopped());
  // The header is the IP's count: either account's reply sets it for both.
  a.on_headers(90, -1, t + 7'000'000'000);
  CHECK_FALSE(b.can_send(11, t + 7'000'000'000));
  REQUIRE(b.weight_bucket(0).has_value());
  CHECK(b.weight_bucket(0)->used == 90);
}

TEST_CASE("venues.rate_limiter: accounts on their own threads spend one IP window") {
  // Each venue sends from its network thread; the shared window is touched under its lock.
  const auto ip = shared_rate("test-host-threads");
  RateLimiter a(1.0);
  RateLimiter b(1.0);
  a.share_ip(ip);
  b.share_ip(ip);
  REQUIRE(a.add_weight_bucket(1000, 60 * kSec));
  REQUIRE(b.add_weight_bucket(1000, 60 * kSec));
  std::atomic<int> sent{0};
  const auto spend = [&](RateLimiter& r) {
    for (int i = 0; i < 2000; ++i) {
      if (!r.can_send(1, kSec)) continue;
      r.on_sent(1, kSec);
      ++sent;
      static_cast<void>(r.weight_bucket(0));  // a status report, read concurrently
    }
  };
  std::thread ta(spend, std::ref(a));
  std::thread tb(spend, std::ref(b));
  ta.join();
  tb.join();
  // can_send and on_sent are two steps: each thread may pass the check once past the other's send.
  CHECK(sent.load() >= 1000);
  CHECK(sent.load() <= 1001);
  CHECK(a.weight_bucket(0)->used == static_cast<std::uint32_t>(sent.load()));
}

TEST_CASE("venues.rate_limiter: the published budget stops where can_send does") {
  // An account 88 into Binance's 100 per 10 s: the connector refuses past 90, so two are left.
  RateLimiter rl(0.9);
  REQUIRE(rl.add_order_bucket(100, 10 * kSec));
  REQUIRE(rl.add_order_bucket(200'000, 86'400 * kSec));
  const std::int64_t now = 5 * kSec;
  rl.on_headers(-1, 88, now);
  fastmm::OrderBudget b = budget_of(rl, now, 0);
  CHECK(b.orders_10s.limit == 100);
  CHECK(b.orders_10s.cap == 90);
  CHECK(b.orders_10s.remaining() == 2);
  CHECK(b.orders_1d.cap == 180'000);
  // A cancel (is_order false) spends weight, not the order windows.
  rl.on_sent(1, now, false);
  CHECK(budget_of(rl, now, 0).orders_10s.used == 88);
  // Any limit and threshold: the window has room exactly while can_send takes an order.
  for (const double t : {0.9, 0.5, 0.33, 1.0}) {
    for (const std::uint32_t limit : {1U, 7U, 10U, 50U, 100U, 1200U}) {
      RateLimiter r(t);
      REQUIRE(r.add_order_bucket(limit, 10 * kSec));
      for (std::uint32_t used = 0; used <= limit; ++used) {
        r.on_headers(-1, used, now);
        b = budget_of(r, now, 0);
        CHECK((b.orders_10s.remaining() > 0) == r.can_send(0, now, true));
      }
    }
  }
}
