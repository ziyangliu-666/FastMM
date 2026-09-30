// Binance Spot testnet or Demo Mode (opt-in: FASTMM_LIVE_TESTS=1). Public part: reference data
// and a synchronised book. With FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET: a post-only
// order far below the market is placed and cancelled.
// FASTMM_BINANCE_ENV selects the environment (never the live exchange):
//   testnet (default)  https://testnet.binance.vision, wss://stream.testnet.binance.vision,
//                      wss://ws-api.testnet.binance.vision/ws-api/v3
//   demo               https://demo-api.binance.com, wss://demo-stream.binance.com,
//                      wss://demo-ws-api.binance.com/ws-api/v3
// (https://developers.binance.com/docs/binance-spot-api-docs, testnet section, and
//  .../demo-mode/general-info). Testnet and Demo Mode keys are different keys.
#include "live_test_util.hpp"

#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/decimal.hpp"

#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using namespace fastmm::venues::test;

TEST_CASE("live.binance: book sync, then place and cancel a far limit order") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against the Binance Spot testnet or demo");
    return;
  }
  const std::string key = env_or_empty("FASTMM_BINANCE_API_KEY");
  const std::string secret = env_or_empty("FASTMM_BINANCE_API_SECRET");
  const bool with_keys = !key.empty() && !secret.empty();

  const std::string env = env_or_empty("FASTMM_BINANCE_ENV");
  BinanceVenueConfig cfg;
  if (env == "demo") {
    cfg.name = "binance-demo";
    cfg.ws_url = "wss://demo-stream.binance.com/stream";
    cfg.ws_api_url = "wss://demo-ws-api.binance.com/ws-api/v3";
    cfg.rest_url = "https://demo-api.binance.com";
  } else {
    REQUIRE_MESSAGE((env.empty() || env == "testnet"),
                    "FASTMM_BINANCE_ENV must be testnet or demo, got '" << env << "'");
    cfg.name = "binance-testnet";
    cfg.ws_url = "wss://stream.testnet.binance.vision/stream";
    cfg.ws_api_url = "wss://ws-api.testnet.binance.vision/ws-api/v3";
    cfg.rest_url = "https://testnet.binance.vision";
  }
  MESSAGE("environment: " << cfg.name << " (" << cfg.rest_url << ")");
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

namespace {

// The text of `"key":"<value>"` in `obj`; empty when absent. Test helper for raw replies.
std::string raw_field(std::string_view obj, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":\"";
  const std::size_t p = obj.find(needle);
  if (p == std::string_view::npos) return {};
  const std::size_t start = p + needle.size();
  return std::string(obj.substr(start, obj.find('"', start) - start));
}

// The {...} of the balances[] / assets[] entry naming `asset`; empty when absent.
std::string_view raw_entry(std::string_view json, std::string_view asset) {
  const std::string needle = "\"asset\":\"" + std::string(asset) + "\"";
  const std::size_t p = json.find(needle);
  if (p == std::string_view::npos) return {};
  const std::size_t open = json.rfind('{', p);
  return json.substr(open, json.find('}', p) - open + 1);
}

std::string text(Notional v) {
  return std::string(DecimalText(v).view());
}

}  // namespace

// Read-only: the connector's start-up balance snapshot against a GET /api/v3/account of our own.
TEST_CASE("live.binance: the balance snapshot equals the account reply") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against the Binance Spot testnet or demo");
    return;
  }
  const std::string key = env_or_empty("FASTMM_BINANCE_API_KEY");
  const std::string secret = env_or_empty("FASTMM_BINANCE_API_SECRET");
  if (key.empty() || secret.empty()) {
    MESSAGE("skipped: FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET not set");
    return;
  }
  const std::string env = env_or_empty("FASTMM_BINANCE_ENV");
  BinanceVenueConfig cfg;
  if (env == "demo") {
    cfg.name = "binance-demo";
    cfg.ws_url = "wss://demo-stream.binance.com/stream";
    cfg.ws_api_url = "wss://demo-ws-api.binance.com/ws-api/v3";
    cfg.rest_url = "https://demo-api.binance.com";
  } else {
    REQUIRE_MESSAGE((env.empty() || env == "testnet"),
                    "FASTMM_BINANCE_ENV must be testnet or demo, got '" << env << "'");
    cfg.name = "binance-testnet";
    cfg.ws_url = "wss://stream.testnet.binance.vision/stream";
    cfg.ws_api_url = "wss://ws-api.testnet.binance.vision/ws-api/v3";
    cfg.rest_url = "https://testnet.binance.vision";
  }
  cfg.credentials.api_key = key;
  cfg.credentials.secret.value = secret;
  cfg.cancel_on_order_channel_loss = false;
  cfg.record_raw_dir = env_or_empty("FASTMM_LIVE_RAW_DIR");  // the frames, for fixtures

  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  REQUIRE(instruments.add(make_instrument("ETHUSDT", 0, "ETH", "USDT")));
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
  const InstrumentId ids[] = {InstrumentId{0}, InstrumentId{1}};
  venue.subscribe(ids);
  venue.connect(reactor);

  Collected oc;
  std::vector<BalanceMsg> snap;
  REQUIRE(pump_until(
      reactor,
      [&] {
        md.drain();
        oc.take(orders);
        snap.clear();
        for (const auto& m : oc.all) {
          if (RecordingSink::type_of(m) != EventType::Balance) continue;
          snap.push_back(RecordingSink::as<BalanceMsg>(m));
          if ((snap.back().flags & BalanceMsg::kSnapshotEnd) != 0) return true;
        }
        return false;
      },
      30'000));

  // The same account, asked directly.
  const Signer signer(cfg.credentials);
  BinanceOrderEncoder enc(signer, symbols, cfg.recv_window_ms);
  RestRequest rr;
  REQUIRE(enc.encode_rest_account(venue.venue_time_ms(), rr));
  BlockingHttp http(cfg.rest_url);
  const HttpReply raw = http.request(
      "GET", std::string(rr.path) + "?" + std::string(rr.query.view()), api_key_header(key));
  REQUIRE_MESSAGE(raw.ok(), "GET /api/v3/account: HTTP " << raw.status);

  MESSAGE("asset | raw free / locked | BalanceMsg free / locked / total / flags");
  for (const std::string asset : {"BTC", "ETH", "USDT"}) {
    const std::string_view entry = raw_entry(raw.body, asset);
    const BalanceMsg* row = nullptr;
    for (const BalanceMsg& b : snap) {
      if (b.asset.view() == asset) row = &b;
    }
    if (entry.empty()) {
      // omitZeroBalances: not listed, so not held; the snapshot does not name it either.
      MESSAGE(asset << " | (not listed) | "
                    << std::string(row == nullptr ? "(not named)" : "named"));
      CHECK(row == nullptr);
      continue;
    }
    const std::string free_s = raw_field(entry, "free");
    const std::string locked_s = raw_field(entry, "locked");
    REQUIRE(row != nullptr);
    MESSAGE(asset << " | " << free_s << " / " << locked_s << " | " << text(row->free) << " / "
                  << text(row->locked) << " / " << text(row->total) << " / " << int{row->flags});
    CHECK(row->free == Notional::from_decimal(free_s).value());
    CHECK(row->locked == Notional::from_decimal(locked_s).value());
    CHECK(row->total == row->free + row->locked);
    CHECK((row->flags & BalanceMsg::kSnapshot) != 0);
  }
  CHECK(snap.back().hdr.exch_ts.ns > 0);

  // The stream: a far post-only buy locks its notional in USDT (outboundAccountPosition), its
  // cancel releases it. FASTMM_LIVE_BALANCE_ORDER=1 only: it places an order.
  if (env_or_empty("FASTMM_LIVE_BALANCE_ORDER") != "1") {
    MESSAGE("stream part skipped: set FASTMM_LIVE_BALANCE_ORDER=1 to place and cancel an order");
    venue.disconnect();
    return;
  }
  const auto stream_usdt = [&]() -> const BalanceMsg* {
    const BalanceMsg* out = nullptr;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) != EventType::Balance) continue;
      const auto& b = RecordingSink::as<BalanceMsg>(m);
      if (b.flags == 0 && b.asset.view() == "USDT") out = &b;
    }
    return out;
  };
  Price best_bid{};
  REQUIRE(pump_until(
      reactor,
      [&] {
        for (const auto& m : md.drain()) {
          if (RecordingSink::type_of(m) == EventType::BookTicker)
            best_bid = RecordingSink::as<BookTickerMsg>(m).bid_px;
        }
        oc.take(orders);
        return best_bid.is_positive();
      },
      30'000));
  const Instrument& btc = instruments.get(InstrumentId{0});
  const OutNewOrderMsg n = far_passive_buy(
      btc, best_bid, make_cl_ord_id(static_cast<std::uint16_t>(wall_now().ns % 60000), 1));
  REQUIRE(outbound.try_push(&n, n.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        md.drain();
        oc.take(orders);
        const BalanceMsg* u = stream_usdt();
        return oc.count(EventType::OrderReject) > 0 || (u != nullptr && u->locked.is_positive());
      },
      15'000));
  REQUIRE(oc.count(EventType::OrderReject) == 0);
  const BalanceMsg locked = *stream_usdt();
  const BalanceMsg* snap_usdt = nullptr;
  for (const BalanceMsg& b : snap) {
    if (b.asset.view() == "USDT") snap_usdt = &b;
  }
  REQUIRE(snap_usdt != nullptr);
  MESSAGE("order " << text(btc.notional(n.price, n.qty)) << " USDT: stream USDT free "
                   << text(locked.free) << " locked " << text(locked.locked) << " at "
                   << locked.hdr.exch_ts.ns / 1'000'000);
  CHECK(locked.locked == btc.notional(n.price, n.qty));
  CHECK(locked.free + locked.locked == snap_usdt->total);
  const auto* ack = oc.last<OrderAckMsg>(EventType::OrderAck);
  REQUIRE(ack != nullptr);
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = n.cl_ord_id;
  c.venue_order_id = ack->venue_order_id;
  REQUIRE(outbound.try_push(&c, c.hdr.len));
  venue.on_wake();
  REQUIRE(pump_until(
      reactor,
      [&] {
        md.drain();
        oc.take(orders);
        const BalanceMsg* u = stream_usdt();
        return oc.count(EventType::OrderCancelAck) > 0 && u != nullptr && u->locked.is_zero();
      },
      15'000));
  MESSAGE("after the cancel: stream USDT free " << text(stream_usdt()->free) << " locked "
                                                << text(stream_usdt()->locked));
  CHECK(stream_usdt()->free == snap_usdt->free);
  CHECK_FALSE(venue.fatal());
  venue.disconnect();
}
