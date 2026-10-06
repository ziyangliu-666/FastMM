// Venue::defer_private on Binance Spot (a warm standby, fastmm-live --standby): connect() reads
// market data only and nothing of the account; enable_private() opens order entry, the user
// stream, the execution replay and the start-up sweep.
#include "integration_util.hpp"

using namespace fastmm;
using namespace fastmm::integration;

TEST_CASE("integration.binance: defer_private holds the account's channels until enable_private") {
  ServerFixture fx;
  VenueHarness h(venue_config(fx));
  REQUIRE(h.venue->defer_private());
  h.connect();
  REQUIRE(h.pump([&] { return h.venue->status().books_synced == 1; }));
  h.pump([] { return false; }, 1500);  // a housekeeping tick or so
  sim::server::SimServerStats s = fx.server.stats();
  INFO(describe(h.venue->status(), s));
  CHECK(s.md_sessions == 1);
  CHECK(s.api_sessions_opened == 0);
  CHECK(s.user_subscriptions == 0);
  CHECK(s.open_orders_queries == 0);
  CHECK(s.my_trades_queries == 0);
  CHECK(h.oc.count(EventType::Reconcile) == 0);
  CHECK(h.live_order_channels() == 0);

  h.venue->enable_private();
  REQUIRE(h.pump([&] {
    return fx.server.stats().user_subscriptions == 1 && h.live_order_channels() >= 1 &&
           h.oc.count_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
             return m.kind == ReconcileMsg::Kind::End;
           }) >= 1;
  }));
  s = fx.server.stats();
  CHECK(s.api_sessions_opened >= 1);
  CHECK(s.open_orders_queries >= 1);
  CHECK_FALSE(h.venue->defer_private());  // only before connect()
}
