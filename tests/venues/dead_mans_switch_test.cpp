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

TEST_CASE("venues.dms: a round counts only when every part is confirmed, from when it went out") {
  CountdownDriver d(30'000);  // refresh every 10 s
  const std::int64_t t0 = 5'000'000'000;
  REQUIRE(d.poll(t0) == CountdownDriver::Step::Refresh);
  // Two symbols: one confirmation is not the switch being up (the other symbol's countdown was
  // not pushed out).
  const std::uint32_t r1 = d.begin_round(t0, 2);
  d.went_out();
  d.confirmed(r1);
  CHECK_FALSE(d.ever_armed());
  d.confirmed(r1);
  CHECK(d.ever_armed());
  // Measured from the round's send time, not the confirmations': the venue's timer started no
  // earlier than that.
  CHECK(d.poll(t0 + 30'000 * kMs - 1) == CountdownDriver::Step::Refresh);
  // Next round: one symbol refused. The switch is still measured from the first round.
  const std::uint32_t r2 = d.begin_round(t0 + 10'000 * kMs, 2);
  d.went_out();
  d.confirmed(r2);
  d.confirmed(r1);  // a late reply to an older round counts for nothing
  CHECK(d.poll(t0 + 30'000 * kMs) == CountdownDriver::Step::Lapsed);
}

TEST_CASE("venues.dms: a lapse is reported once and nothing is refreshed after it") {
  CountdownDriver d(3'000);  // refresh every second
  const std::int64_t t0 = 1'000'000'000;
  REQUIRE(d.poll(t0) == CountdownDriver::Step::Refresh);
  const std::uint32_t r = d.begin_round(t0, 1);
  d.went_out();
  d.confirmed(r);
  CHECK(d.poll(t0 + 3'000 * kMs) == CountdownDriver::Step::Lapsed);
  CHECK(d.lapsed());
  // The venue's timer is left to run out: no refresh, no second kill, and a confirmation that
  // arrives now does not revive it.
  for (std::int64_t s = 1; s < 10; ++s)
    CHECK(d.poll(t0 + (3'000 + s * 1'000) * kMs) == CountdownDriver::Step::None);
  d.confirmed(r);
  CHECK(d.poll(t0 + 20'000 * kMs) == CountdownDriver::Step::None);
  // It still has to be stopped at shutdown: the last refresh may have reached the venue.
  CHECK(d.needs_stop());
  d.reset();
  CHECK_FALSE(d.lapsed());
  CHECK_FALSE(d.needs_stop());
  CHECK(d.poll(t0 + 20'000 * kMs) == CountdownDriver::Step::Refresh);
}

TEST_CASE("venues.dms: the stop is needed once a refresh went out, confirmed or not") {
  CountdownDriver d(30'000);
  CHECK_FALSE(d.needs_stop());  // nothing sent: nothing to stop
  static_cast<void>(d.begin_round(1, 1));
  CHECK_FALSE(d.needs_stop());  // a round nothing of which was sent
  d.went_out();
  CHECK(d.needs_stop());  // never confirmed, but its reply may be what was lost
  CHECK_FALSE(CountdownDriver(0).needs_stop());
}
