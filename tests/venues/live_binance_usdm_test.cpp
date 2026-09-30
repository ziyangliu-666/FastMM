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
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/decimal.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

namespace {

// The text of `"key":"<value>"` in `obj`; empty when absent. Test helper for raw replies.
std::string raw_field(std::string_view obj, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\":\"";
  const std::size_t p = obj.find(needle);
  if (p == std::string_view::npos) return {};
  const std::size_t start = p + needle.size();
  return std::string(obj.substr(start, obj.find('"', start) - start));
}

// The {...} of the assets[] entry naming `asset`; empty when absent.
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

Notional dec(const std::string& s) {
  return Notional::from_decimal(s).value();
}

}  // namespace

// Read-only: the connector's start-up balance snapshot against a GET /fapi/v3/account of our own.
TEST_CASE("live.binance_usdm: the balance snapshot equals the account reply") {
  if (!live_tests_enabled()) {
    MESSAGE("skipped: set FASTMM_LIVE_TESTS=1 to run against Binance USDⓈ-M Demo Trading");
    return;
  }
  const std::string key = env_or_empty("FASTMM_BINANCE_API_KEY");
  const std::string secret = env_or_empty("FASTMM_BINANCE_API_SECRET");
  if (key.empty() || secret.empty()) {
    MESSAGE("skipped: FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET not set");
    return;
  }
  BinanceUsdmVenueConfig cfg;
  cfg.name = "binance-usdm-demo";
  cfg.ws_url = "wss://demo-fstream.binance.com";
  cfg.ws_api_url = "wss://testnet.binancefuture.com/ws-fapi/v1";
  cfg.rest_url = "https://demo-fapi.binance.com";
  cfg.credentials.api_key = key;
  cfg.credentials.secret.value = secret;
  cfg.cancel_on_order_channel_loss = false;
  cfg.dead_mans_switch_ms = 0;  // read-only: nothing sent that changes the account
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

  // The same account and its mode, asked directly.
  const binance::Signer signer(cfg.credentials);
  BlockingHttp http(cfg.rest_url);
  const auto signed_get = [&](std::string_view path) {
    binance::RestRequest rr;
    REQUIRE(BinanceUsdmOrderEncoder::encode_rest_signed_get(
        signer, cfg.recv_window_ms, path, {}, venue.venue_time_ms(), 5, rr));
    return http.request("GET",
                        std::string(rr.path) + "?" + std::string(rr.query.view()),
                        binance::api_key_header(key));
  };
  const HttpReply mode = signed_get("/fapi/v1/multiAssetsMargin");
  REQUIRE_MESSAGE(mode.ok(), "GET multiAssetsMargin: HTTP " << mode.status);
  const bool multi = mode.body.find("true") != std::string::npos;
  const HttpReply raw = signed_get("/fapi/v3/account");
  REQUIRE_MESSAGE(raw.ok(), "GET /fapi/v3/account: HTTP " << raw.status);
  MESSAGE("multi-assets mode: " << std::string(multi ? "on" : "off"));
  if (const std::string dir = env_or_empty("FASTMM_LIVE_RAW_DIR"); !dir.empty()) {
    std::ofstream(dir + "/binance-usdm-demo-account.json") << raw.body << "\n";  // for fixtures
  }

  MESSAGE(
      "asset | raw availableBalance / maxWithdrawAmount / initialMargin / walletBalance / "
      "marginBalance / maintMargin | BalanceMsg free / locked / total / equity / maintenance / "
      "flags");
  for (const std::string asset : {"BTC", "USDT"}) {
    const std::string_view entry = raw_entry(raw.body, asset);
    const BalanceMsg* row = nullptr;
    for (const BalanceMsg& b : snap) {
      if (b.asset.view() == asset && (b.flags & BalanceMsg::kAccount) == 0) row = &b;
    }
    if (entry.empty()) {
      MESSAGE(asset << " | (not listed) | "
                    << std::string(row == nullptr ? "(not named)" : "named"));
      CHECK(row == nullptr);
      continue;
    }
    REQUIRE(row != nullptr);
    const std::string avail = raw_field(entry, "availableBalance");
    const std::string maxw = raw_field(entry, "maxWithdrawAmount");
    MESSAGE(asset << " | " << avail << " / " << maxw << " / " << raw_field(entry, "initialMargin")
                  << " / " << raw_field(entry, "walletBalance") << " / "
                  << raw_field(entry, "marginBalance") << " / " << raw_field(entry, "maintMargin")
                  << " | " << text(row->free) << " / " << text(row->locked) << " / "
                  << text(row->total) << " / " << text(row->equity) << " / "
                  << text(row->maintenance) << " / " << int{row->flags});
    CHECK(row->free == dec(multi ? maxw : avail));
    CHECK(row->locked == dec(raw_field(entry, "initialMargin")));
    CHECK(row->total == dec(raw_field(entry, "walletBalance")));
    CHECK(row->equity == dec(raw_field(entry, "marginBalance")));
    CHECK(row->maintenance == dec(raw_field(entry, "maintMargin")));
  }
  const BalanceMsg* account = nullptr;
  for (const BalanceMsg& b : snap) {
    if ((b.flags & BalanceMsg::kAccount) != 0) account = &b;
  }
  if (multi) {
    REQUIRE(account != nullptr);
    const std::string_view totals =
        std::string_view(raw.body).substr(0, raw.body.find("\"assets\""));
    MESSAGE("USD (account) | " << raw_field(totals, "availableBalance") << " / "
                               << raw_field(totals, "totalInitialMargin") << " / "
                               << raw_field(totals, "totalWalletBalance") << " / "
                               << raw_field(totals, "totalMarginBalance") << " / "
                               << raw_field(totals, "totalMaintMargin") << " | "
                               << text(account->free) << " / " << text(account->locked) << " / "
                               << text(account->total) << " / " << text(account->equity) << " / "
                               << text(account->maintenance) << " / " << int{account->flags});
    CHECK(account->asset.view() == "USD");
    CHECK(account->free == dec(raw_field(totals, "availableBalance")));
    CHECK(account->total == dec(raw_field(totals, "totalWalletBalance")));
  } else {
    CHECK(account == nullptr);
  }
  CHECK(snap.back().hdr.exch_ts.ns > 0);
  CHECK_FALSE(venue.fatal());
  venue.disconnect();
}
