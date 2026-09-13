// Binance Spot testnet (opt-in: FASTMM_LIVE_TESTS=1). Public part: reference data and a
// synchronised book. With FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET: a post-only
// order far below the market is placed and cancelled.
// Endpoints: https://developers.binance.com/docs/binance-spot-api-docs (testnet section):
// https://testnet.binance.vision, wss://stream.testnet.binance.vision,
// wss://ws-api.testnet.binance.vision/ws-api/v3.
#include "live_test_util.hpp"

#include "fastmm/venues/binance/binance_venue.hpp"

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using namespace fastmm::venues::test;

TEST_CASE("live.binance: testnet book sync, then place and cancel a far limit order") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against the Binance Spot testnet");
    return;
  }
  const std::string key = env_or_empty("FASTMM_BINANCE_API_KEY");
  const std::string secret = env_or_empty("FASTMM_BINANCE_API_SECRET");
  const bool with_keys = !key.empty() && !secret.empty();

  BinanceVenueConfig cfg;
  cfg.name = "binance-testnet";
  cfg.ws_url = "wss://stream.testnet.binance.vision/stream";
  cfg.ws_api_url = "wss://ws-api.testnet.binance.vision/ws-api/v3";
  cfg.rest_url = "https://testnet.binance.vision";
  cfg.credentials.api_key = key;
  cfg.credentials.secret.value = secret;
  cfg.dry_run = !with_keys;
  cfg.cancel_on_order_channel_loss = false;

  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  BinanceVenue venue(VenueId{0}, cfg);
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
  CHECK(mdc.count(EventType::BookSnapshot) >= 1);
  const Price best_bid = mdc.last<BookTickerMsg>(EventType::BookTicker)->bid_px;
  REQUIRE(best_bid.is_positive());

  if (!with_keys) {
    MESSAGE("skipped order entry: FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET not set");
    venue.disconnect();
    return;
  }
  Collected oc;
  const OutNewOrderMsg n =
      far_passive_buy(instruments.get(InstrumentId{0}),
                      best_bid,
                      make_cl_ord_id(static_cast<std::uint16_t>(wall_now().ns % 60000), 1));
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
  const auto* ack = oc.last<OrderAckMsg>(EventType::OrderAck);
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = n.cl_ord_id;
  c.venue_order_id = ack->venue_order_id;
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
