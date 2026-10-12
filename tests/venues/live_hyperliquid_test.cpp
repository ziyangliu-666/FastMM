// Hyperliquid, against the real venue (opt-in: FASTMM_LIVE_TESTS=1). Two checks:
//
//   live.hyperliquid: reference data   no keys; the meta of FASTMM_HL_COIN's dex (default
//                                      xyz:NVDA): asset id, szDecimals, tick at the mark, lot.
//   live.hyperliquid: one ALO ...      places ONE post-only buy at 80 % of the best bid (the
//                                      smallest size above 10 USDC), reads its orderStatus (the
//                                      venue's timestamps), cancels it, and prints the times:
//                                      send -> reply, send -> block time of "open" on the stream,
//                                      send -> cancel ack. Needs FASTMM_HL_PLACE_ORDER=1 on top,
//                                      FASTMM_HL_ACCOUNT (the master address) and the signing key
//                                      in the variable FASTMM_HL_KEY_ENV names (default
//                                      HL_API_PRIVATE_KEY). FASTMM_HL_TESTNET=1 for the testnet.
//                                      Spends two requests of the address budget.
//
// FASTMM_HL_RECORD_DIR records the raw frames of every connection (RawRecorder).
#include "live_test_util.hpp"

#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/hyperliquid/hyperliquid_venue.hpp"

#include <fmt/format.h>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::hyperliquid;
using namespace fastmm::venues::test;

namespace {

std::string coin_under_test() {
  std::string c = env_or_empty("FASTMM_HL_COIN");
  return c.empty() ? std::string("xyz:NVDA") : c;
}

VenueSection live_section() {
  VenueSection s;
  s.name = "hyperliquid-live";
  s.kind = "hyperliquid";
  s.testnet = env_or_empty("FASTMM_HL_TESTNET") == "1";
  s.api_key = env_or_empty("FASTMM_HL_ACCOUNT");
  std::string key_env = env_or_empty("FASTMM_HL_KEY_ENV");
  if (key_env.empty()) key_env = "HL_API_PRIVATE_KEY";
  s.api_secret = env_or_empty(key_env.c_str());
  s.extra["latency_log_s"] = "0";
  return s;
}

}  // namespace

TEST_CASE("live.hyperliquid: reference data of the coin's dex") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to read Hyperliquid's meta");
    return;
  }
  VenueSection s = live_section();
  s.api_key.clear();
  s.api_secret.clear();
  HyperliquidVenue venue(VenueId{0}, make_hyperliquid_config(s, true));
  InstrumentTable instruments;
  const std::string coin = coin_under_test();
  REQUIRE(instruments.add(make_instrument(coin.c_str(), 0, "X", "USDC")));
  const auto ref = venue.load_reference_data(instruments);
  REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
  const Instrument& i = instruments.get(InstrumentId{0});
  MESSAGE(fmt::format("{}: asset {} szDecimals {} tick {} lot {} min notional {}",
                      venue.coin_of(InstrumentId{0}),
                      venue.asset_of(InstrumentId{0}),
                      venue.sz_decimals_of(InstrumentId{0}),
                      DecimalText(i.tick).view(),
                      DecimalText(i.lot).view(),
                      DecimalText(i.min_notional).view()));
  CHECK(venue.asset_of(InstrumentId{0}) > 0);
  CHECK(i.tick.is_positive());
}

TEST_CASE("live.hyperliquid: public market data through the connector") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to read Hyperliquid's market data");
    return;
  }
  VenueSection s = live_section();
  s.api_key.clear();
  s.api_secret.clear();
  HyperliquidVenueConfig cfg = make_hyperliquid_config(s, true);
  cfg.record_raw_dir = env_or_empty("FASTMM_HL_RECORD_DIR");
  HyperliquidVenue venue(VenueId{0}, cfg);
  InstrumentTable instruments;
  const std::string coin = coin_under_test();
  REQUIRE(instruments.add(make_instrument(coin.c_str(), 0, "X", "USDC")));
  REQUIRE(venue.load_reference_data(instruments));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  venue.connect(reactor);
  Collected mc;
  REQUIRE(pump_until(
      reactor,
      [&] {
        mc.take(md);
        venue.on_timer(net::Reactor::now_ns());
        return mc.count(EventType::BookSnapshot) > 0 && mc.count(EventType::BookTicker) > 0;
      },
      30'000));
  const auto* b = mc.last<BookDeltaMsg>(EventType::BookSnapshot);
  const auto* t = mc.last<BookTickerMsg>(EventType::BookTicker);
  MESSAGE(fmt::format("{}: book {}x{} levels, bbo {} / {}, {} trade(s); block time - receive {} ms",
                      coin,
                      b->bid_count,
                      b->ask_count,
                      DecimalText(t->bid_px).view(),
                      DecimalText(t->ask_px).view(),
                      mc.count(EventType::Trade),
                      (t->hdr.exch_ts.ns - t->hdr.recv_ts.ns) / 1'000'000));
  CHECK(t->bid_px < t->ask_px);
  CHECK(venue.status().books_synced == 1);
  venue.disconnect();
}

TEST_CASE("live.hyperliquid: one ALO far from the market, its status and its cancel") {
  if (!live_tests_enabled() || env_or_empty("FASTMM_HL_PLACE_ORDER") != "1") {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 and FASTMM_HL_PLACE_ORDER=1 to place one order");
    return;
  }
  const VenueSection s = live_section();
  if (s.api_key.empty() || s.api_secret.empty()) {
    MESSAGE("skipped: FASTMM_HL_ACCOUNT and the signing key (FASTMM_HL_KEY_ENV) not set");
    return;
  }
  HyperliquidVenueConfig cfg = make_hyperliquid_config(s, false);
  cfg.record_raw_dir = env_or_empty("FASTMM_HL_RECORD_DIR");
  cfg.channels.trades = false;
  const std::string account = cfg.account_address;
  const std::string rest_url = cfg.rest_url;
  HyperliquidVenue venue(VenueId{0}, cfg);
  InstrumentTable instruments;
  const std::string coin = coin_under_test();
  REQUIRE(instruments.add(make_instrument(coin.c_str(), 0, "X", "USDC")));
  const auto ref = venue.load_reference_data(instruments);
  REQUIRE_MESSAGE(ref, (ref ? std::string() : ref.error()));
  MESSAGE(fmt::format("address budget before: {} of {} used",
                      venue.address_budget().used,
                      venue.address_budget().cap));
  RecordingSink md(16U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  venue.connect(reactor);

  // The touch, and every connection up.
  Price best_bid{};
  REQUIRE(pump_until(
      reactor,
      [&] {
        for (const auto& m : md.drain()) {
          if (RecordingSink::type_of(m) == EventType::BookTicker)
            best_bid = RecordingSink::as<BookTickerMsg>(m).bid_px;
        }
        venue.on_timer(net::Reactor::now_ns());
        return best_bid.is_positive() && venue.order_channel_live() &&
               venue.status().user == ChannelState::Live;
      },
      30'000));
  Collected oc;
  pump_until(
      reactor,
      [&] {
        oc.take(orders);
        return false;
      },
      2000);  // the start-up sweep

  const Instrument& inst = instruments.get(InstrumentId{0});
  const OutNewOrderMsg n =
      far_passive_buy(inst, best_bid, ClientOrderId{(std::uint64_t{0x7e} << 32) | 1});
  MESSAGE(fmt::format("placing ALO buy {} @ {} (best bid {})",
                      DecimalText(n.qty).view(),
                      DecimalText(n.price).view(),
                      DecimalText(best_bid).view()));
  const std::int64_t sent_ms = wall_now().ns / 1'000'000;
  REQUIRE(outbound.try_push(&n, n.hdr.len));
  venue.on_wake();
  std::string oid;
  std::int64_t ack_ms = 0;
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        if (const auto* a = oc.first_if<OrderAckMsg>(
                EventType::OrderAck,
                [&](const OrderAckMsg& m) { return m.cl_ord_id == n.cl_ord_id; })) {
          oid = std::string(a->venue_order_id.view());
          ack_ms = a->hdr.recv_ts.ns / 1'000'000;
        }
        const auto* rej = oc.first_if<OrderRejectMsg>(
            EventType::OrderReject,
            [&](const OrderRejectMsg& m) { return m.cl_ord_id == n.cl_ord_id; });
        if (rej != nullptr) FAIL("rejected: " << std::string(rej->text.view()));
        return !oid.empty();
      },
      15'000));
  MESSAGE(fmt::format("acked oid {} after {} ms", oid, ack_ms - sent_ms));

  // The venue's own record of it: status and timestamps.
  {
    BlockingHttp http(rest_url);
    const HttpReply r =
        http.request("POST",
                     "/info",
                     "Content-Type: application/json\r\n",
                     fmt::format(R"({{"type":"orderStatus","user":"{}","oid":{}}})", account, oid));
    REQUIRE(r.ok());
    OrderStatusRow st;
    CHECK(decode_order_status(r.body, st).empty());
    MESSAGE(fmt::format("orderStatus {}: placed (block time) {} = send + {} ms, status time {}",
                        st.status,
                        st.placed_ms,
                        st.placed_ms - sent_ms,
                        st.status_ms));
    CHECK(st.status == "open");
  }

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, inst.id, inst.venue);
  c.cl_ord_id = n.cl_ord_id;
  const std::int64_t cancel_ms = wall_now().ns / 1'000'000;
  REQUIRE(outbound.try_push(&c, c.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        oc.take(orders);
        return oc.first_if<OrderCancelAckMsg>(EventType::OrderCancelAck,
                                              [&](const OrderCancelAckMsg& m) {
                                                return m.cl_ord_id == n.cl_ord_id;
                                              }) != nullptr;
      },
      15'000));
  const auto* ca = oc.first_if<OrderCancelAckMsg>(
      EventType::OrderCancelAck,
      [&](const OrderCancelAckMsg& m) { return m.cl_ord_id == n.cl_ord_id; });
  MESSAGE(fmt::format("cancel acked after {} ms (venue time {} = cancel send + {} ms)",
                      ca->hdr.recv_ts.ns / 1'000'000 - cancel_ms,
                      ca->hdr.exch_ts.ns / 1'000'000,
                      ca->hdr.exch_ts.ns / 1'000'000 - cancel_ms));
  CHECK(ca->cum_qty.is_zero());
  CHECK_FALSE(venue.fatal());
  CHECK(venue.shadow_count() == 0);
  MESSAGE(fmt::format("address budget after: {} of {} used (counted locally)",
                      venue.address_budget().used,
                      venue.address_budget().cap));
  venue.disconnect();
}
