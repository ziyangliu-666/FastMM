// End to end: fastmm-sim-exchange in-process on ephemeral ports + the fastmm-live engine path
// (BinanceVenue on its reactor thread, Engine<BasicMM, TscClock, LiveTransport, RingFeed> on its
// own thread, configs/sim-local(-tls).toml), over TCP and TLS, and the 6.7 failure handling:
// a skipped depthUpdate, a market-data drop and an order-channel loss.
#include "integration_util.hpp"

using namespace fastmm;
using namespace fastmm::integration;

namespace {

void check_session(ServerFixture& fx, LiveEngine& live) {
  CHECK(live.kill_pushed());
  CHECK(live.cancel_all_ok());
  const auto& es = live.engine().stats();
  CHECK(es.orders_sent >= 1);
  CHECK(es.fills >= 1);
  CHECK(es.kills == 1);
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  const sim::server::SimServerStats ss = fx.server.stats();
  const RiskLimits& lim = live.engine().risk().limits();
  CHECK(ss.max_order_qty <= lim.max_order_qty);
  CHECK(ss.max_abs_position <= lim.max_position);
  CHECK(ss.max_open_orders <= lim.max_open_orders);
  CHECK(live.engine().position(InstrumentId{0}).qty == ss.position);
  MESSAGE("server: orders=" << ss.orders_accepted << " replaces=" << ss.replaces
                            << " cancels=" << ss.cancels << " fills=" << ss.fills << " rejects="
                            << ss.orders_rejected << " max_open=" << ss.max_open_orders);
}

}  // namespace

TEST_CASE("sim_exchange e2e: live engine over TCP quotes and gets filled and shuts down cleanly") {
  const std::size_t fds_before = open_fd_count();
  {
    ServerFixture fx;
    LiveEngine live(sim_local_config(fx, false));
    live.start();
    REQUIRE(wait_until(
        [&] {
          const sim::server::SimServerStats s = fx.server.stats();
          return s.fills >= 2 && s.orders_accepted >= 3;
        },
        30000));
    CHECK(wait_until([&] { return live.venue().status().books_synced == 1; }, 5000));
    live.stop();
    check_session(fx, live);
  }
  CHECK(open_fd_count() == fds_before);
}

TEST_CASE("sim_exchange e2e: live engine over TLS verified against the fixture certificate") {
  const std::size_t fds_before = open_fd_count();
  {
    ServerFixture fx(test_server_config(true));
    LiveEngine live(sim_local_config(fx, true));
    CHECK_FALSE(live.config().venues[0].insecure_tls);
    live.start();
    REQUIRE(wait_until([&] { return fx.server.stats().fills >= 1; }, 30000));
    live.stop();
    check_session(fx, live);
  }
  CHECK(open_fd_count() == fds_before);
}

TEST_CASE("sim_exchange e2e: injected depth gap triggers a resync and the engine book recovers") {
  ServerFixture fx;
  LiveEngine live(sim_local_config(fx, false));
  live.start();
  auto status = [&] { return live.venue().status(); };
  REQUIRE(wait_until(
      [&] { return status().books_synced == 1 && fx.server.stats().open_orders >= 1; }, 20000));

  // The connector detects U != prev_u + 1, reports Resyncing (the engine clears the book and
  // pulls quotes), re-snapshots over REST and resumes.
  const std::uint64_t resyncs_before = status().resyncs;
  const std::uint64_t snapshots_before = fx.server.stats().depth_snapshots;
  fx.server.mark();
  fx.server.skip_next_depth_update();
  REQUIRE(wait_until([&] { return fx.server.stats().depth_snapshots > snapshots_before; }, 10000));
  REQUIRE(wait_until(
      [&] {
        const venues::VenueStatus s = status();
        return s.resyncs > resyncs_before && s.books_synced == 1;
      },
      10000));
  // BasicMM only quotes on a valid book: new orders prove the engine book recovered.
  REQUIRE(wait_until([&] { return fx.server.stats().orders_since_mark >= 1; }, 15000));
  const sim::server::SimServerStats s = fx.server.stats();
  CHECK(s.depth_updates_skipped == 1);
  CHECK(s.min_open_orders_since_mark == 0);
  live.stop();
  CHECK(live.cancel_all_ok());
  CHECK(live.engine().stats().book_updates > 0);
  // Diagnostic only: a Stale episode (slow sanitizer builds) clears the book near shutdown.
  WARN_MESSAGE(live.engine().book(InstrumentId{0}).is_valid(), "engine book not valid at shutdown");
}

TEST_CASE("sim_exchange e2e: market-data drop pulls quotes and order-channel loss cancels all") {
  ServerFixture fx;
  LiveEngine live(sim_local_config(fx, false));
  live.start();
  auto status = [&] { return live.venue().status(); };
  REQUIRE(wait_until(
      [&] { return status().books_synced == 1 && fx.server.stats().open_orders >= 1; }, 20000));

  // 1. Market-data connection dropped: quotes are pulled, the stream reconnects and resyncs.
  const std::uint64_t open_before_md = fx.server.stats().open_orders;
  fx.server.mark();
  fx.server.drop_market_data_connections();
  REQUIRE(wait_until(
      [&] {
        const sim::server::SimServerStats x = fx.server.stats();
        return x.md_sessions_opened_since_mark >= 1 && x.md_sessions == 1;
      },
      20000));
  CHECK(wait_until(
      [&] {
        const venues::VenueStatus v = status();
        return v.md == venues::ChannelState::Live && v.books_synced == 1;
      },
      10000));
  sim::server::SimServerStats s = fx.server.stats();
  CHECK(s.md_connections_dropped == 1);
  CHECK(s.min_open_orders_since_mark == 0);
  // Quotes can all have filled by the time of the drop; then there is nothing to cancel.
  if (open_before_md > 0) CHECK(s.cancels_since_mark >= 1);
  const bool md_resumed = wait_until([&] { return fx.server.stats().open_orders >= 1; }, 10000);

  // 2. Order channel (the WS API connection without the user stream) lost: the connector
  //    cancels everything over REST and reconnects the channel.
  const std::uint64_t open_before = fx.server.stats().open_orders;
  fx.server.mark();
  fx.server.drop_ws_api_connections(false);
  REQUIRE(wait_until(
      [&] {
        const sim::server::SimServerStats x = fx.server.stats();
        return x.cancel_all_since_mark >= 1 && x.api_sessions_opened_since_mark >= 1 &&
               x.api_sessions == 2;
      },
      15000));
  CHECK(wait_until([&] { return status().order == venues::ChannelState::Live; }, 10000));
  const bool reconciled =
      wait_until([&] { return fx.server.stats().open_orders_queries_since_mark >= 1; }, 3000);
  const bool resumed = wait_until([&] { return fx.server.stats().orders_since_mark >= 1; }, 10000);
  s = fx.server.stats();
  CHECK(s.api_connections_dropped == 1);
  if (open_before > 0) CHECK(s.min_open_orders_since_mark == 0);

  live.stop();
  CHECK(live.cancel_all_ok());
  CHECK(wait_until([&] { return fx.server.stats().open_orders == 0; }, 5000));
  const auto& es = live.engine().stats();
  MESSAGE("engine: events=" << es.events << " book_updates=" << es.book_updates
                            << " orders=" << es.orders_sent << " cancels=" << es.cancels_sent
                            << " replaces=" << es.replaces_sent << " fills=" << es.fills
                            << " risk_rejects=" << es.risk_rejects
                            << " crossed_pulls=" << es.crossed_pulls
                            << " book_valid=" << live.engine().book(InstrumentId{0}).is_valid()
                            << " | server after order-channel loss: cancel_all="
                            << s.cancel_all_since_mark << " open_orders_queries="
                            << s.open_orders_queries_since_mark << " orders=" << s.orders_since_mark
                            << " cancel_rejects=" << s.cancel_rejects);
  // Strategies requote as soon as the venue is Live again (on_connection), and the connector
  // reconciles open orders after the order channel reconnects (plan 6.7).
  CHECK_MESSAGE(md_resumed, "quoting did not resume within 10 s of the market-data reconnect");
  CHECK_MESSAGE(resumed, "quoting did not resume within 10 s of the order-channel reconnect");
  CHECK_MESSAGE(reconciled, "no openOrders reconciliation after the order channel came back");
}
