// Recovery under repetition. The scenarios in recovery_test.cpp break a session once and check it
// comes back; this one keeps breaking it, with order flow running throughout, and checks the same
// invariants after every fault. What it is looking for is drift: an order the two sides stop
// agreeing on, a position that separates from the venue's, an id used twice, a reconciliation that
// stops converging - the failures that a single round trip is too short to show.
//
// Default duration is a few seconds so it runs with every other test. FASTMM_SOAK_SECONDS makes it
// as long as you want: `FASTMM_SOAK_SECONDS=1800 ctest -R recovery_soak`.
#include "integration_util.hpp"

#include "fastmm/core/oms.hpp"
#include "fastmm/core/position.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;

namespace {

const Qty kLot = Qty::from_decimal("0.001").value();

std::int64_t soak_seconds() {
  if (const char* s = std::getenv("FASTMM_SOAK_SECONDS")) {
    const std::int64_t v = std::atoll(s);
    if (v > 0) return v;
  }
  return 4;
}

sim::server::SimServerConfig quiet_server() {
  sim::server::SimServerConfig c = test_server_config();
  c.generator.market_rate_per_s = 0.0;  // only the test takes a resting order
  return c;
}

struct Harness : VenueHarness {
  std::size_t reconciles = 0;

  explicit Harness(const ServerFixture& fx) : VenueHarness(venue_config(fx)) {
    REQUIRE(venue->load_reference_data(instruments));
    connect();
    REQUIRE(pump([&] {
      return venue->md_feed()->synced_count() == 1 && live_order_channels() >= 2 &&
             ends() >= 1;
    }));
    reconciles = ends();
  }

  [[nodiscard]] std::size_t ends() const {
    return oc.count_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& r) {
      return r.kind == ReconcileMsg::Kind::End;
    });
  }
};

}  // namespace

TEST_CASE("recovery_soak: repeated faults with order flow leave no drift") {
  ServerFixture fx(quiet_server());
  Harness h(fx);
  OmsMirror m(h.instruments);

  const auto orders_agree = [&] {
    std::vector<std::string> venue_ids = fx.server.open_client_order_ids();
    std::vector<std::string> engine_ids;
    for (const ClientOrderId id : m.open_ids()) engine_ids.emplace_back(encode_cl_ord_id(id).view());
    std::sort(venue_ids.begin(), venue_ids.end());
    std::sort(engine_ids.begin(), engine_ids.end());
    return venue_ids == engine_ids;
  };

  const Price tick = Price::from_decimal("0.01").value();
  std::mt19937_64 rng(20260924);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(soak_seconds());
  std::uint64_t next_id = 1;
  std::uint64_t rounds = 0;
  std::uint64_t faults = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    ++rounds;
    // The previous round's fault may still be healing: quote only once the book is synced again.
    REQUIRE(h.pump(
        [&] {
          m.drain(h.oc);
          return h.venue->md_feed()->synced_count() == 1 && !h.venue->fatal();
        },
        20000));
    // Two orders on the book, far enough behind the touch that only the test moves them.
    const std::size_t rejects_before = h.oc.count(EventType::OrderReject);
    for (int i = 0; i < 2; ++i) {
      const ClientOrderId id{next_id++};
      const Price px = Price::from_raw(fx.server.stats().best_bid.price.raw -
                                       tick.raw * (100 + 20 * i + static_cast<int>(rounds % 5)));
      const OutNewOrderMsg o =
          new_order(id, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, px, kLot);
      m.submit(o);
      h.send(o.hdr);
      // An order placed just after a reconnect can end three ways: acknowledged, rejected, or
      // resolved by the reconciliation that follows. The soak does not care which - it cares that
      // the two sides agree about it afterwards.
      static_cast<void>(h.pump(
          [&] {
            h.venue->on_wake();
            m.drain(h.oc);
            return h.acks(id) >= 2 || h.oc.count(EventType::OrderReject) > rejects_before;
          },
          3000));
    }

    const std::vector<std::string> open = fx.server.open_client_order_ids();
    // Fills during an outage are held back until execution-history recovery lands: a fill the
    // private stream never delivered does not reach the position through the REST cancel-all that
    // follows an order-channel drop (NOTES.md). Faults here are the ones recovery already handles.
    const int fault = static_cast<int>(rng() % 3);
    MESSAGE("round " << rounds << " fault " << fault);
    switch (fault) {
      case 0:  // both WS API connections go
        fx.server.drop_ws_api_connections(true);
        break;
      case 1:  // the venue bans us for a moment
        fx.server.ban_next_requests(2);
        fx.server.drop_ws_api_connections(true);
        break;
      default:  // market data cut
        fx.server.drop_market_data_connections();
        break;
    }
    ++faults;

    // Whatever broke, the session has to come back: a reconciliation lands and the two sides agree.
    const std::size_t before = h.ends();
    REQUIRE(h.pump(
        [&] {
          m.drain(h.oc);
          return h.ends() > before || orders_agree();
        },
        20000));
    m.drain(h.oc);
    h.reconciles = h.ends();

    REQUIRE(h.pump([&] {
      m.drain(h.oc);
      return orders_agree();
    }));
    CHECK(fx.server.stats().duplicate_client_order_ids == 0);
    CHECK(m.position() == fx.server.stats().position);

    // Clear the book for the next round through the engine's own path.
    // cancel_all() refuses while the venue has us banned (418): retry until it takes, the way an
    // operator would.
    REQUIRE(h.pump(
        [&] {
          h.venue->on_wake();
          m.drain(h.oc);
          if (fx.server.stats().open_orders == 0) return true;
          static_cast<void>(h.venue->cancel_all());
          return false;
        },
        20000));
    m.drain(h.oc);
  }

  MESSAGE("soak: " << rounds << " rounds, " << faults << " faults, " << m.synthetic_fills()
                   << " synthetic fill(s), position " << m.position().raw);
  CHECK(rounds >= 1);
  CHECK_FALSE(h.venue->fatal());
  // The venue is empty by the last round's cleanup; the engine's cancel acks may still be in the
  // sink. The invariant is that the two sides converge, so wait for it rather than sample it.
  CHECK(h.pump([&] {
    m.drain(h.oc);
    return orders_agree();
  }));
  CHECK(m.position() == fx.server.stats().position);
}
