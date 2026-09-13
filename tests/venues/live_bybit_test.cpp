// Bybit v5 spot testnet (opt-in: FASTMM_LIVE_TESTS=1). Public part: reference data and a
// synchronised book. With FASTMM_BYBIT_API_KEY / FASTMM_BYBIT_API_SECRET: a post-only order
// far below the market is placed and cancelled.
// Endpoints: https://bybit-exchange.github.io/docs/v5/guide and .../v5/ws/connect
// (https://api-testnet.bybit.com, wss://stream-testnet.bybit.com/v5/{public/spot,private,trade}).
#include "live_test_util.hpp"

#include "fastmm/venues/bybit/bybit_venue.hpp"

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using namespace fastmm::venues::test;

TEST_CASE("live.bybit: testnet book sync, then place and cancel a far limit order") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against the Bybit testnet");
    return;
  }
  const std::string key = env_or_empty("FASTMM_BYBIT_API_KEY");
  const std::string secret = env_or_empty("FASTMM_BYBIT_API_SECRET");
  const bool with_keys = !key.empty() && !secret.empty();

  VenueSection s;
  s.name = "bybit-testnet";
  s.kind = "bybit";
  s.ws_url = "wss://stream-testnet.bybit.com/v5/public/spot";
  s.ws_api_url = "wss://stream-testnet.bybit.com/v5/trade";
  s.rest_url = "https://api-testnet.bybit.com";
  s.api_key = key;
  s.api_secret = secret;
  BybitVenueConfig cfg = make_bybit_config(s, !with_keys);
  cfg.cancel_on_order_channel_loss = false;

  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  BybitVenue venue(VenueId{0}, cfg);
  const auto ref = venue.load_reference_data(instruments);
  REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  venue.connect(reactor);

  Collected mdc;
  REQUIRE(pump_until(
      reactor,
      [&] {
        mdc.take(md);
        return venue.md_feed()->synced_count() == 1 && mdc.count(EventType::BookTicker) > 0;
      },
      30'000));
  const Price best_bid = mdc.last<BookTickerMsg>(EventType::BookTicker)->bid_px;
  REQUIRE(best_bid.is_positive());

  if (!with_keys) {
    MESSAGE("skipped order entry: FASTMM_BYBIT_API_KEY / FASTMM_BYBIT_API_SECRET not set");
    venue.disconnect();
    return;
  }
  Collected oc;
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        std::size_t live = 0;
        for (const auto& m : oc.all) {
          if (RecordingSink::type_of(m) == EventType::ConnectionState &&
              RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Live)
            ++live;
        }
        return live >= 2;
      },
      30'000));
  const OutNewOrderMsg n =
      far_passive_buy(instruments.get(InstrumentId{0}),
                      best_bid,
                      make_cl_ord_id(static_cast<std::uint16_t>(wall_now().ns % 60000), 1));
  REQUIRE(outbound.try_push(&n, n.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        return oc.count(EventType::OrderAck) > 0 || oc.count(EventType::OrderReject) > 0;
      },
      15'000));
  REQUIRE_MESSAGE(oc.count(EventType::OrderReject) == 0,
                  (oc.count(EventType::OrderReject)
                       ? std::string(oc.last<OrderRejectMsg>(EventType::OrderReject)->text.view())
                       : std::string()));
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = n.cl_ord_id;
  c.venue_order_id = oc.last<OrderAckMsg>(EventType::OrderAck)->venue_order_id;
  REQUIRE(outbound.try_push(&c, c.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        return oc.count(EventType::OrderCancelAck) > 0 ||
               oc.count(EventType::OrderCancelReject) > 0;
      },
      15'000));
  CHECK(oc.count(EventType::OrderCancelAck) > 0);
  CHECK(venue.cancel_all());
  CHECK_FALSE(venue.fatal());
  venue.disconnect();
}
