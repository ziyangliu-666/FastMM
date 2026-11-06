// HomePlanner (core/pool_homes.hpp): from the orders each instrument sent per 10 s window it moves
// at most plan_max_moves homes off the busiest account, gives first homes, never moves a pin, never
// moves onto an account that is not usable, and plans the same from the same input.
#include "fastmm/core/pool_homes.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace fastmm;

namespace {

constexpr VenueId kA{0};
constexpr VenueId kB{1};
constexpr VenueId kC{2};

PoolPlacement placement(std::uint32_t max_moves = 3) {
  PoolPlacement p;
  p.configured = true;
  p.plan_interval = seconds(3600);
  p.plan_lookback = seconds(3600);
  p.plan_max_moves = max_moves;
  return p;
}

// Six instruments, all homed on A, with `rate[i]` orders per window each.
struct Load {
  PoolLoad load;
  std::vector<std::uint64_t> rate;
  explicit Load(std::vector<std::uint64_t> r) : rate(std::move(r)) {
    load.members.venue[0] = kA;
    load.members.venue[1] = kB;
    load.members.venue[2] = kC;
    load.members.count = 3;
    for (std::size_t k = 0; k < 3; ++k) {
      load.usable[k] = true;
      load.limit_10s[k] = 100;
    }
    load.count = static_cast<std::uint32_t>(rate.size());
    for (std::size_t i = 0; i < rate.size(); ++i) {
      load.instrument[i] = InstrumentId{static_cast<std::uint32_t>(i)};
      load.home[i] = kA;
    }
  }
  // `n` windows of the rates; every tenth window at five times them.
  void run(HomePlanner& p, int n) {
    for (int w = 0; w < n; ++w) {
      for (std::size_t i = 0; i < rate.size(); ++i)
        load.orders[i] += rate[i] * (w % 10 == 0 ? 5 : 1);
      p.sample(load);
    }
  }
};

}  // namespace

TEST_CASE("core.home_planner: moves at most plan_max_moves homes off the busiest account") {
  HomePlanner p(kA, placement(2));
  Load l({20, 15, 10, 5, 5, 1});
  l.run(p, 100);
  CHECK(p.windows() == 100);
  const std::vector<HomeMove> moves = p.plan(l.load);
  REQUIRE(moves.size() == 2);
  for (const HomeMove& m : moves) {
    CHECK(m.from == kA);
    CHECK(m.to != kA);
  }
  CHECK(p.stats().moves == 2);
  // The same input plans the same moves.
  HomePlanner q(kA, placement(2));
  Load l2({20, 15, 10, 5, 5, 1});
  l2.run(q, 100);
  CHECK(q.plan(l2.load) == moves);
}

TEST_CASE("core.home_planner: a balanced split stays where it is") {
  HomePlanner p(kA, placement());
  Load l({10, 10, 10});
  l.load.home[1] = kB;
  l.load.home[2] = kC;
  l.run(p, 50);
  CHECK(p.plan(l.load).empty());
}

TEST_CASE(
    "core.home_planner: pins stay, an unusable account takes nothing, first homes are given") {
  std::vector<VenueId> pins(6);
  pins[0] = kA;
  HomePlanner p(kA, placement(5), pins);
  Load l({30, 10, 10, 10, 10, 10});
  l.load.usable[1] = false;  // B
  l.load.home[5] = VenueId{};
  l.run(p, 60);
  const std::vector<HomeMove> moves = p.plan(l.load);
  REQUIRE_FALSE(moves.empty());
  CHECK(moves[0].instrument == InstrumentId{5});  // the first home comes first
  CHECK_FALSE(moves[0].from.valid());
  CHECK(p.stats().assigned == 1);
  for (const HomeMove& m : moves) {
    CHECK(m.instrument != InstrumentId{0});
    CHECK(m.to != kB);
  }
}

TEST_CASE("core.home_planner: step closes a window every 10 s and plans every plan_interval") {
  PoolPlacement pp = placement();
  pp.plan_interval = seconds(60);
  HomePlanner p(kA, pp);
  Load l({20, 1, 1});
  std::int64_t now = seconds(1000).ns + 3;
  CHECK(p.step(now, l.load).empty());  // the baseline
  std::size_t planned = 0;
  for (int s = 1; s <= 130; ++s) {
    for (std::size_t i = 0; i < 3; ++i) l.load.orders[i] += l.rate[i];
    const std::vector<HomeMove> moves = p.step(now + seconds(s).ns, l.load);
    planned += moves.size();
    for (const HomeMove& m : moves) {
      for (std::size_t i = 0; i < 3; ++i) {
        if (l.load.instrument[i] == m.instrument) l.load.home[i] = m.to;
      }
    }
  }
  CHECK(p.stats().samples == 13);
  CHECK(p.stats().plans == 2);
  CHECK(planned >= 1);
}
