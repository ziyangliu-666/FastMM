// Binance USDⓈ-M futures Demo Trading (opt-in: FASTMM_LIVE_TESTS=1). Public part: reference data
// and a synchronised book. With FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET (Demo Trading
// keys): a post-only order far below the market is placed and cancelled; an account without a
// futures margin balance rejects it, which skips the order steps.
// Hosts (never the live exchange): https://demo-fapi.binance.com, wss://demo-fstream.binance.com,
// wss://testnet.binancefuture.com/ws-fapi/v1
// (https://developers.binance.com/docs/derivatives/usds-margined-futures/general-info and
//  .../websocket-api-general-info).
#include "live_test_util.hpp"

#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using namespace fastmm::venues::test;

TEST_CASE("live.binance_usdm: book sync, then place and cancel a far post-only order") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against Binance USDⓈ-M Demo Trading");
    return;
  }
  const std::string key = env_or_empty("FASTMM_BINANCE_API_KEY");
  const std::string secret = env_or_empty("FASTMM_BINANCE_API_SECRET");
  const bool with_keys = !key.empty() && !secret.empty();

  BinanceUsdmVenueConfig cfg;
  cfg.name = "binance-usdm-demo";
  cfg.ws_url = "wss://demo-fstream.binance.com";
  cfg.ws_api_url = "wss://testnet.binancefuture.com/ws-fapi/v1";
  cfg.rest_url = "https://demo-fapi.binance.com";
  cfg.credentials.api_key = key;
  cfg.credentials.secret.value = secret;
  cfg.dry_run = !with_keys;
  cfg.cancel_on_order_channel_loss = false;
  cfg.stale_ms = 10'000;

  InstrumentTable instruments;
  Instrument inst = make_instrument("BTCUSDT", 0, "BTC", "USDT");
  inst.asset_class = AssetClass::Perpetual;
  REQUIRE(instruments.add(inst));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  BinanceUsdmVenue venue(VenueId{0}, cfg);
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
  CHECK(venue.md_feed()->resync_count() == 0);
  const Price best_bid = mdc.last<BookTickerMsg>(EventType::BookTicker)->bid_px;
  REQUIRE(best_bid.is_positive());

  if (!with_keys) {
    MESSAGE("skipped order entry: FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET not set");
    venue.disconnect();
    return;
  }
  Collected oc;
  // Order and user channels live, and the first reconciliation finished.
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        std::size_t live = 0;
        std::size_t ends = 0;
        for (const auto& m : oc.all) {
          if (RecordingSink::type_of(m) == EventType::ConnectionState &&
              RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Live)
            ++live;
          if (RecordingSink::type_of(m) == EventType::Reconcile &&
              RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::End)
            ++ends;
        }
        return live >= 2 && ends >= 1;
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
  if (oc.count(EventType::OrderReject) > 0) {
    const auto* rej = oc.last<OrderRejectMsg>(EventType::OrderReject);
    if (rej->reason == RejectReason::InsufficientBalance) {
      MESSAGE("skipped order entry: no futures margin balance (" << rej->text.view() << ")");
      venue.disconnect();
      return;
    }
    FAIL("order rejected: " << rej->venue_code << " " << rej->text.view());
  }
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
