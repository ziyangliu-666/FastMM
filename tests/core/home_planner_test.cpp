// HomePlanner (core/pool_homes.hpp): from the orders each instrument's home carried per 10 s window
// it moves at most plan_max_moves homes, off an account whose homes take more than half its window,
// spreads uneven shares and leaves even ones, follows a burst in the latest windows, gives first
// homes, never moves a pin, never moves onto an account that is not usable, and plans the same from
// the same input.
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
  Load l({60, 1, 1});
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

TEST_CASE("core.home_planner: a quiet pool spreads uneven shares, and leaves even ones") {
  HomePlanner p(kA, placement());
  Load l({20, 10, 5});  // 35 of A's 100, nothing on B and C: no window binds
  for (int w = 0; w < 60; ++w) {
    for (std::size_t i = 0; i < l.rate.size(); ++i) l.load.orders[i] += l.rate[i];
    p.sample(l.load);
  }
  const std::vector<HomeMove> moves = p.plan(l.load);
  REQUIRE(moves.size() >= 2);
  CHECK(moves[0].instrument == InstrumentId{0});
  CHECK(moves[0].from == kA);
  for (const HomeMove& m : moves) {
    for (std::size_t i = 0; i < 3; ++i) {
      if (l.load.instrument[i] == m.instrument) l.load.home[i] = m.to;
    }
  }
  // Each account now carries one instrument: the next plan leaves them.
  CHECK(l.load.home[0] != l.load.home[1]);
  CHECK(l.load.home[1] != l.load.home[2]);
  CHECK(l.load.home[0] != l.load.home[2]);
  CHECK(p.plan(l.load).empty());
}

TEST_CASE("core.home_planner: an account past half its window sheds its hottest instrument") {
  HomePlanner p(kA, placement());
  Load l({40, 30, 10});  // 80 of A's 100
  for (int w = 0; w < 60; ++w) {
    for (std::size_t i = 0; i < l.rate.size(); ++i) l.load.orders[i] += l.rate[i];
    p.sample(l.load);
  }
  const std::vector<HomeMove> moves = p.plan(l.load);
  // 40 off leaves A at 40 and the new home at 40: below half everywhere, and the shares closest.
  REQUIRE_FALSE(moves.empty());
  CHECK(moves[0].instrument == InstrumentId{0});
  CHECK(moves[0].from == kA);
  CHECK(moves[0].to != kA);
}

TEST_CASE("core.home_planner: a burst in the latest windows moves homes after a quiet hour") {
  HomePlanner p(kA, placement());
  Load l({10, 10, 20, 20});
  l.load.home[2] = kB;
  l.load.home[3] = kC;
  const auto windows = [&](int n, std::uint64_t a0, std::uint64_t a1) {
    for (int w = 0; w < n; ++w) {
      l.load.orders[0] += a0;
      l.load.orders[1] += a1;
      l.load.orders[2] += 20;
      l.load.orders[3] += 20;
      p.sample(l.load);
    }
  };
  windows(354, 10, 10);  // an hour at 20 of each account's 100
  CHECK(p.plan(l.load).empty());
  windows(6, 60, 50);  // a minute at 110
  const std::vector<HomeMove> moves = p.plan(l.load);
  REQUIRE_FALSE(moves.empty());
  CHECK(moves[0].from == kA);
}

TEST_CASE("core.home_planner: an account with a lower limit takes less") {
  HomePlanner p(kA, placement());
  Load l({30, 30, 1});
  l.load.home[2] = kB;
  l.load.limit_10s[1] = 50;  // B admits half what C does
  for (int w = 0; w < 60; ++w) {
    for (std::size_t i = 0; i < l.rate.size(); ++i) l.load.orders[i] += l.rate[i];
    p.sample(l.load);
  }
  const std::vector<HomeMove> moves = p.plan(l.load);
  REQUIRE_FALSE(moves.empty());
  CHECK(moves[0].from == kA);
  CHECK(moves[0].to == kC);
}
