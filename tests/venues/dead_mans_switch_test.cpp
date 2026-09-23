#include "fastmm/venues/dead_mans_switch.hpp"

#include "test_support.hpp"

using namespace fastmm::venues;

namespace {
constexpr std::int64_t kMs = 1'000'000;
}

TEST_CASE("venues.dms: a disabled switch never asks for anything") {
  CountdownSwitch off;
  CHECK_FALSE(off.enabled());
  CHECK_FALSE(off.due(1'000'000'000));
  CHECK_FALSE(off.expired(1'000'000'000'000));
  CHECK(CountdownSwitch(0).refresh_period_ns() == 0);
}

TEST_CASE("venues.dms: the refresh runs at a fraction of the window, with a floor") {
  const CountdownSwitch s(60'000);  // 60 s window, refreshed every 20 s
  CHECK(s.enabled());
  CHECK(s.window_ms() == 60'000);
  CHECK(s.refresh_period_ns() == 20'000 * kMs);
  // Windows small enough that a third of them is sub-second are clamped: refreshing faster is
  // rate limit spent for nothing.
  CHECK(CountdownSwitch(900).refresh_period_ns() == CountdownSwitch::kMinPeriodNs);
  CHECK(CountdownSwitch(30'000, 1, 2).refresh_period_ns() == 15'000 * kMs);
}

TEST_CASE("venues.dms: due paces retries, expired measures from the venue's confirmation") {
  CountdownSwitch s(30'000);  // refresh every 10 s
  const std::int64_t t0 = 5'000'000'000;
  CHECK(s.due(t0));  // nothing sent yet: arm now
  CHECK_FALSE(s.ever_armed());

  // A request went out but the venue has not answered: no second request for a whole period,
  // and the window is not running, so nothing has expired.
  s.attempted(t0);
  CHECK_FALSE(s.due(t0 + 9'999 * kMs));
  CHECK_FALSE(s.expired(t0 + 60'000 * kMs));
  CHECK(s.due(t0 + 10'000 * kMs));

  // The venue answered: the window runs from here.
  s.armed(t0 + 100 * kMs);
  CHECK(s.ever_armed());
  CHECK_FALSE(s.expired(t0 + 30'099 * kMs));
  CHECK(s.expired(t0 + 30'100 * kMs));

  // A later confirmation pushes it out again.
  s.attempted(t0 + 10'000 * kMs);
  s.armed(t0 + 10'100 * kMs);
  CHECK_FALSE(s.expired(t0 + 30'100 * kMs));
  CHECK(s.expired(t0 + 40'100 * kMs));

  // Disarm forgets both clocks: the next tick arms from scratch and cannot report an expiry it
  // has already acted on.
  s.disarm();
  CHECK(s.due(t0 + 40'100 * kMs));
  CHECK_FALSE(s.expired(t0 + 40'100 * kMs));
  CHECK_FALSE(s.ever_armed());
}
