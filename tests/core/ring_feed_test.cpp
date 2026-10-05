// RingFeed's per-ring budget: how many messages one ring gives before the next ring's turn.
#include "test_support.hpp"

#include "fastmm/core/transport.hpp"

#include <vector>

using namespace fastmm;

namespace {

void push(MsgRing& ring, std::uint64_t seq) {
  ControlMsg m{};
  init_header(m, EventType::Control);
  m.hdr.seq = seq;
  REQUIRE(ring.try_push(&m, m.hdr.len));
}

// Ring a holds 100..104, ring b 200..204; the order the feed gives them.
std::vector<std::uint64_t> drain_order(RingFeed& feed, MsgRing& a, MsgRing& b) {
  for (std::uint64_t i = 0; i < 5; ++i) push(a, 100 + i);
  for (std::uint64_t i = 0; i < 5; ++i) push(b, 200 + i);
  std::vector<std::uint64_t> seen;
  while (const EventHeader* h = feed.next()) {
    seen.push_back(h->seq);
    feed.release();
  }
  return seen;
}

}  // namespace

TEST_CASE("core.ring_feed: the default budget drains a ring's burst before the next ring") {
  MsgRing a(1U << 16);
  MsgRing b(1U << 16);
  RingFeed feed;
  REQUIRE(feed.add_ring(&a));
  REQUIRE(feed.add_ring(&b));
  CHECK(feed.budget_per_ring() == kFeedBudgetPerRing);
  CHECK(drain_order(feed, a, b) ==
        std::vector<std::uint64_t>{100, 101, 102, 103, 104, 200, 201, 202, 203, 204});
}

TEST_CASE("core.ring_feed: a budget of 1 takes the rings in turn") {
  MsgRing a(1U << 16);
  MsgRing b(1U << 16);
  RingFeed feed;
  REQUIRE(feed.add_ring(&a));
  REQUIRE(feed.add_ring(&b));
  feed.set_budget_per_ring(1);
  CHECK(drain_order(feed, a, b) ==
        std::vector<std::uint64_t>{100, 200, 101, 201, 102, 202, 103, 203, 104, 204});
}

TEST_CASE("core.ring_feed: a budget of 2, an empty ring skipped, 0 taken as 1") {
  MsgRing a(1U << 16);
  MsgRing empty(1U << 16);
  MsgRing b(1U << 16);
  RingFeed feed;
  REQUIRE(feed.add_ring(&a));
  REQUIRE(feed.add_ring(&empty));
  REQUIRE(feed.add_ring(&b));
  feed.set_budget_per_ring(2);
  CHECK(drain_order(feed, a, b) ==
        std::vector<std::uint64_t>{100, 101, 200, 201, 102, 103, 202, 203, 104, 204});
  feed.set_budget_per_ring(0);
  CHECK(feed.budget_per_ring() == 1);
}
