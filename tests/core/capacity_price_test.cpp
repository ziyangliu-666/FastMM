// The capacity price (core/capacity_price.hpp) on its own: the threshold, the account windows and
// their fill credit, the learned values, the day price and when priced-out levels are re-checked.
#include "fastmm/core/capacity_price.hpp"

#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

using namespace fastmm;

namespace {

constexpr std::int64_t kSec = 1'000'000'000;
const VenueId kAccount{0};
const InstrumentId kInst{0};

CapacityConfig config(std::int64_t orders_10s = 10) {
  CapacityConfig c;
  c.mode = CapacityMode::Enforce;
  c.value = CapacityValue::Learned;
  c.orders_10s = orders_10s;
  c.fill_credit = true;
  c.min_notional = 5.0;
  c.max_notional = 1200.0;
  return c;
}

}  // namespace

TEST_CASE("capacity: the threshold is flat below z0 and reaches max over min notional when full") {
  CapacityThreshold t;
  t.configure(240.0);
  CHECK(t.z0() == doctest::Approx(1.0 / (1.0 + std::log(240.0))));
  CHECK(t.at(0.0) == 1.0);
  CHECK(t.at(0.1) == 1.0);
  CHECK(t.at(1.0) == doctest::Approx(240.0));
  CHECK(t.at(2.0) == doctest::Approx(240.0));
  CHECK(t.at(-1.0) == 1.0);
  // (R e)^z / e above z0, never below 1, rising.
  CHECK(t.at(0.5) == doctest::Approx(std::exp((0.5 * (std::log(240.0) + 1.0)) - 1.0)));
  double last = 0.0;
  for (int k = 0; k <= 100; ++k) {
    const double h = t.at(k / 100.0);
    CHECK(h >= last);
    last = h;
  }
}

TEST_CASE("capacity: demand is bucketed in quarter octaves of the floor") {
  CHECK(CapacityDayPrice::bucket(1.0) == CapacityDayPrice::kZero);
  CHECK(CapacityDayPrice::bucket(1.18) == CapacityDayPrice::kZero);
  CHECK(CapacityDayPrice::bucket(1.19) == CapacityDayPrice::kZero + 1);
  CHECK(CapacityDayPrice::bucket(2.0) == CapacityDayPrice::kZero + 4);
  CHECK(CapacityDayPrice::bucket(0.5) == CapacityDayPrice::kZero - 4);
  CHECK(CapacityDayPrice::bucket(0.0) == 0);
  CHECK(CapacityDayPrice::bucket(std::numeric_limits<double>::infinity()) ==
        CapacityDayPrice::kBuckets - 1);
  for (int b = 1; b < CapacityDayPrice::kBuckets - 1; ++b)
    CHECK(CapacityDayPrice::bucket(CapacityDayPrice::edge(b) * 1.0001) == b);
}

TEST_CASE(
    "capacity: the window's share prices the next order, and a first fill gives a slot back") {
  CapacityPrice c;
  c.configure(config());
  const std::int64_t t0 = 1000 * kSec;
  const double floor = c.floor();
  CHECK(floor == doctest::Approx(5.0 * CapacityPrice::kPriorYield * 1e-4));
  CHECK(c.price(kAccount, kAccount, t0) == doctest::Approx(floor));
  for (int k = 0; k < 8; ++k) c.on_send(kAccount, kAccount, kInst, Side::Buy, 100.0, t0);
  CapacityQuote q = c.quote(kAccount, kAccount, t0);
  CHECK(q.window_used == 8);
  CHECK(q.window_share == doctest::Approx(0.8));
  CHECK(q.price == doctest::Approx(floor * c.threshold().at(0.8)));
  CHECK(q.price > 50.0 * floor);
  // The order's first fill: the venue counts it out of the window.
  c.on_fill(kAccount, kInst, Side::Buy, 100.0, 100.0, true, t0 + 1);
  CHECK(c.quote(kAccount, kAccount, t0 + 1).window_used == 7);
  CHECK(c.recheck_due(t0 + 1));
  c.rechecked(t0 + 1);
  CHECK_FALSE(c.recheck_due(t0 + 2));
  // A later fill of the same order gives nothing back.
  c.on_fill(kAccount, kInst, Side::Buy, 100.0, 50.0, false, t0 + 2);
  CHECK(c.quote(kAccount, kAccount, t0 + 2).window_used == 7);
  // The window rolls over at the next 10 s boundary: empty, and a re-check is due.
  const std::int64_t next = ((t0 / (10 * kSec)) + 1) * 10 * kSec;
  CHECK_FALSE(c.recheck_due(next - 1));
  CHECK(c.recheck_due(next));
  CHECK(c.quote(kAccount, kAccount, next).window_used == 0);
  CHECK(c.price(kAccount, kAccount, next) == doctest::Approx(c.floor()));
}

TEST_CASE("capacity: learned values follow the fills of the instrument, side and size class") {
  CapacityPrice c;
  c.configure(config());
  const std::int64_t t0 = 1000 * kSec;
  // Before anything is learned: the prior yield at margin_bps.
  CHECK(c.learned_value(kInst, Side::Buy, 600.0) ==
        doctest::Approx(600.0 * CapacityPrice::kPriorYield * 1e-4));
  // Large bids that never fill, small asks that always do.
  for (int k = 0; k < 400; ++k) {
    c.on_send(kAccount, kAccount, kInst, Side::Buy, 600.0, t0);
    c.on_send(kAccount, kAccount, kInst, Side::Sell, 8.0, t0);
    c.on_fill(kAccount, kInst, Side::Sell, 8.0, 8.0, true, t0);
  }
  const double bid = c.learned_value(kInst, Side::Buy, 600.0);
  const double ask = c.learned_value(kInst, Side::Sell, 8.0);
  CHECK(bid < 600.0 * CapacityPrice::kPriorYield * 1e-4 * 0.2);
  CHECK(ask > 8.0 * 0.5 * 1e-4);
  // Another instrument leans on the size class: its small asks are worth more than its prior.
  const InstrumentId other{1};
  CHECK(c.learned_value(other, Side::Sell, 8.0) > 8.0 * CapacityPrice::kPriorYield * 1e-4);
}

TEST_CASE("capacity: decide counts enforced refusals apart from observed ones") {
  CapacityPrice c;
  c.configure(config());
  CHECK(c.decide(kAccount, 2.0, 1.0, false, true));
  CHECK_FALSE(c.decide(kAccount, 0.5, 1.0, false, true));
  CHECK_FALSE(c.decide(kAccount, 0.5, 1.0, true, false));
  // Unstated (NaN) and infinite values pass any price.
  CHECK(c.decide(kAccount, std::numeric_limits<double>::quiet_NaN(), 1e9, false, true));
  CHECK(c.decide(kAccount, std::numeric_limits<double>::infinity(), 1e9, false, true));
  CHECK(c.totals().decided == 5);
  CHECK(c.totals().priced_out_new == 1);
  CHECK(c.totals().observed_move == 1);
  CHECK(c.enforces(true));
  CapacityConfig n = config();
  n.mode = CapacityMode::New;
  c.configure(n);
  CHECK(c.enforces(false));
  CHECK_FALSE(c.enforces(true));
}

TEST_CASE("capacity: the day price is zero while the day fits and keeps the best part when not") {
  // A Tuesday 12:00 UTC.
  const std::int64_t noon = (20'000LL * 86'400 + 12 * 3600) * kSec;
  CapacityDayPrice d;
  const double floor = 1.0;
  // One minute of demand: 100 actions at 1 L, 10 at 10 L, 1 at 1000 L.
  const auto minute = [&] {
    for (int k = 0; k < 100; ++k) d.note(1.0);
    for (int k = 0; k < 10; ++k) d.note(10.0);
    d.note(1000.0);
  };
  minute();
  d.tick(noon, 0, floor, 0);
  CHECK(d.price() == 0.0);  // no budget
  minute();
  d.tick(noon + 60 * kSec, 1'000'000'000, floor, 0);
  CHECK(d.price() == 0.0);  // the rest of the day fits
  // A budget of a few orders for the rest of the day: only the top of the demand goes.
  minute();
  d.tick(noon + 120 * kSec, 500, floor, 0);
  CHECK(d.price() > 1.0);
  CHECK(d.price() <= 1000.0);
  // What is sent counts against the budget; the day rolls it over at midnight.
  for (int k = 0; k < 10; ++k) d.note_sent();
  CHECK(d.used() == 10);
  d.tick(noon + 12 * 3600 * kSec, 500, floor, 0);
  CHECK(d.used() == 0);
}

TEST_CASE("capacity: the minute tick hands each active account's minute over and clears it") {
  CapacityPrice c;
  c.configure(config());
  const std::int64_t t0 = 1000 * kSec;
  int calls = 0;
  c.tick(t0, [&](VenueId, const CapacityMinute&) { ++calls; });  // the first tick arms the clock
  CHECK_FALSE(c.tick_due(t0 + 1));
  c.on_send(kAccount, kAccount, kInst, Side::Buy, 100.0, t0 + 1);
  CHECK(c.decide(kAccount, 1.0, 0.5, false, true));
  CHECK_FALSE(c.decide(kAccount, 0.1, 0.5, false, true));
  const std::int64_t next = ((t0 / (60 * kSec)) + 1) * 60 * kSec;
  CHECK(c.tick_due(next));
  CapacityMinute seen{};
  c.tick(next, [&](VenueId v, const CapacityMinute& m) {
    ++calls;
    CHECK(v == kAccount);
    seen = m;
  });
  CHECK(calls == 1);
  CHECK(seen.decided == 2);
  CHECK(seen.sent == 1);
  CHECK(seen.priced_out_new == 1);
  CHECK(seen.peak_used == 1);
  CHECK(seen.price_min == doctest::Approx(0.5));
  c.tick(next + 60 * kSec, [&](VenueId, const CapacityMinute&) { ++calls; });
  CHECK(calls == 1);  // nothing happened that minute
}

TEST_CASE("capacity: an account outside [capacity] venues is neither priced nor counted") {
  CapacityConfig c = config();
  c.accounts = 1U;  // account 0 only
  CapacityPrice cp;
  cp.configure(c);
  const VenueId other{1};
  CHECK(cp.prices(kAccount));
  CHECK_FALSE(cp.prices(other));
  CHECK_FALSE(cp.quote(other, other, 1000 * kSec).on);
  for (int k = 0; k < 5; ++k) cp.on_send(other, other, kInst, Side::Buy, 100.0, 1000 * kSec);
  CHECK(cp.quote(kAccount, kAccount, 1000 * kSec).window_used == 0);
}

TEST_CASE("capacity: a side refused again in the same window is recorded once") {
  CapacityPrice c;
  c.configure(config());
  const std::int64_t t0 = 1000 * kSec;
  for (int k = 0; k < 5; ++k)
    CHECK_FALSE(c.decide(kAccount, 0.1, 1.0, false, true, kInst, Side::Buy, t0 + k));
  CHECK(c.totals().priced_out_new == 1);
  CHECK(c.totals().decided == 1);
  // The other side, and the next window, are new demand.
  CHECK_FALSE(c.decide(kAccount, 0.1, 1.0, false, true, kInst, Side::Sell, t0));
  CHECK_FALSE(c.decide(kAccount, 0.1, 1.0, false, true, kInst, Side::Buy, t0 + 10 * kSec));
  CHECK(c.totals().priced_out_new == 3);
  // A pass is always recorded.
  CHECK(c.decide(kAccount, 2.0, 1.0, false, true, kInst, Side::Buy, t0 + 10 * kSec));
  CHECK(c.totals().decided == 4);
}
