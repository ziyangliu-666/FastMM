// LighterVenue: config mapping, the registry entry, and the connector against a scripted
// in-process fake Lighter (the REST endpoints and one WebSocket URL serving market data, the
// account channels and jsonapi/sendtx): reference data (orderBookDetails, the API key check, the
// nonce), the order_book snapshot, the account subscriptions with the auth token, a post-only
// order signed with the next nonce and SkipNonce (its signature checked against the key), the ack
// from the account stream once, a fill with its fee, a cancel by client order index, a refused
// transaction, a post-only the venue ended, the volume-quota refusal, a modify applied in place,
// the dead man's switch, the reconciliation and the blocking cancel_all.
#include "fastmm/venues/lighter/lighter_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/registry.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::lighter;
using namespace fastmm::venues::test;

namespace {

// The golden-vector key of lighter_signing_test.cpp: it controls no account.
constexpr const char* kKey =
    "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728";
constexpr const char* kPub =
    "710c8cd2201061fa5570d20852d90ddabd6c496825b639d6c6486ecb1f859b63f237dee54f3f42b4";
constexpr std::int64_t kAccount = 281474976499586;
constexpr VenueId kVenue{9};
const InstrumentId kNvda{0};

std::vector<std::uint8_t> unbase64(std::string_view s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<std::uint8_t> out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    const int v = val(c);
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

std::int64_t num(std::string_view json, std::string_view key) {
  return std::stoll(json_int(json, key));
}

std::string order_update(std::int64_t client, const char* status, const char* filled) {
  return R"({"channel":"account_all_orders:281474976499586","orders":{"110":[{"order_index":)" +
         std::to_string(900000 + client) + R"(,"client_order_index":)" + std::to_string(client) +
         R"(,"market_index":110,"initial_base_amount":"0.050","price":"226.300","remaining_base_amount":"0.050","filled_base_amount":")" +
         filled + R"(","is_ask":false,"status":")" + status +
         R"(","transaction_time":1791835627257555}]},"type":"update/account_all_orders"})";
}

std::string trade_update(std::int64_t client, const char* size, const char* usd) {
  return std::string(
             R"({"channel":"account_all_trades:281474976499586","trades":{"110":[{"trade_id":)") +
         std::to_string(70000 + client) + R"(,"market_id":110,"size":")" + size +
         R"(","price":"226.300","usd_amount":")" + usd + R"(","ask_id":5,"bid_id":)" +
         std::to_string(900000 + client) + R"(,"ask_client_id":0,"bid_client_id":)" +
         std::to_string(client) +
         R"(,"ask_account_id":11,"bid_account_id":281474976499586,"is_maker_ask":false,"maker_fee":40,"taker_fee":280,"timestamp":1791835628000}]},"type":"update/account_all_trades"})";
}

struct Harness {
  FakeVenueServer srv;
  std::string details = fastmm::test::fixture("lighter/rest_order_book_details.json");
  std::string book = fastmm::test::fixture("lighter/md_order_book_snapshot.json");
  std::mutex mu;
  std::string tx_error;  // the next transaction's error when set
  std::string active_orders = R"({"code":200,"orders":[]})";
  std::atomic<int> sendtx{0};
  std::atomic<int> rest_sendtx{0};
  std::atomic<int> active_calls{0};
  std::atomic<int> account_calls{0};
  std::atomic<int> private_subs{0};
  std::atomic<bool> auth_seen{false};
  std::atomic<std::int64_t> quota{900};
  net::WsSession* private_session = nullptr;  // server thread only

  ~Harness() { srv.stop(); }
  Harness() {
    srv.route("GET", "/api/v1/orderBookDetails", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, details);
    });
    srv.route("GET", "/api/v1/apikeys", [](const net::HttpRequest& r) {
      CHECK(r.param("account_index") == "281474976499586");
      return net::HttpServerResponse::json(
          200,
          std::string(
              R"({"code":200,"api_keys":[{"account_index":281474976499586,"api_key_index":4,"nonce":77,"public_key":")") +
              kPub + R"(","transaction_time":1}]})");
    });
    srv.route("GET", "/api/v1/nextNonce", [](const net::HttpRequest& r) {
      CHECK(r.param("api_key_index") == "4");
      return net::HttpServerResponse::json(200, R"({"code":200,"nonce":77})");
    });
    srv.route("GET", "/api/v1/accountActiveOrders", [this](const net::HttpRequest& r) {
      ++active_calls;
      if (r.header("Authorization").starts_with("1")) auth_seen = true;
      std::lock_guard<std::mutex> lock(mu);
      return net::HttpServerResponse::json(200, active_orders);
    });
    srv.route("GET", "/api/v1/account", [this](const net::HttpRequest&) {
      ++account_calls;
      return net::HttpServerResponse::json(
          200,
          R"({"code":200,"total":1,"accounts":[{"index":281474976499586,"collateral":"1000.000000","available_balance":"900.000000","total_asset_value":"1001.5","positions":[{"market_id":110,"symbol":"NVDA","sign":-1,"position":"0.050","avg_entry_price":"226.100"}]}]})");
    });
    srv.route("POST", "/api/v1/sendTx", [this](const net::HttpRequest& r) {
      ++rest_sendtx;
      srv.record("rest_sendtx", std::string(r.body));
      return net::HttpServerResponse::json(
          200,
          R"({"code":200,"message":"{}","tx_hash":"ab","predicted_execution_time_ms":1,"volume_quota_remaining":800})");
    });
    srv.on_ws_open("/stream", [](net::WsSession& s) {
      s.send_text(R"({"session_id":"x","type":"connected"})");
    });
    srv.on_ws_text("/stream", [this](net::WsSession& s, std::string_view t) {
      const std::string type = json_str(t, "type");
      if (type == "ping") {
        s.send_text(R"({"type":"pong"})");
        return;
      }
      if (type == "subscribe") {
        const std::string channel = json_str(t, "channel");
        if (channel == "order_book/110") s.send_text(book);
        if (channel.starts_with("account_all_orders/")) {
          if (!json_str(t, "auth").empty()) auth_seen = true;
          ++private_subs;
          private_session = &s;
          s.send_text(
              R"({"channel":"account_all_orders:281474976499586","orders":{},"type":"subscribed/account_all_orders"})");
        }
        if (channel.starts_with("account_all_trades/")) {
          ++private_subs;
          s.send_text(
              R"({"channel":"account_all_trades:281474976499586","trades":{},"type":"subscribed/account_all_trades"})");
        }
        return;
      }
      if (type == "jsonapi/sendtx") {
        ++sendtx;
        srv.record("sendtx", std::string(t));
        const std::string id = json_str(t, "id");
        std::lock_guard<std::mutex> lock(mu);
        if (!tx_error.empty()) {
          s.send_text(R"({"error":{"code":21705,"message":")" + tx_error + R"("},"id":")" + id +
                      R"("})");
          tx_error.clear();
          return;
        }
        s.send_text(R"({"id":")" + id + R"(","code":200,"tx_hash":"ab","volume_quota_remaining":)" +
                    std::to_string(quota.load()) + "}");
      }
    });
    srv.start();
  }

  void to_private(const std::string& text) {
    srv.run_on_server([&] {
      if (private_session != nullptr) private_session->send_text(text);
    });
  }

  VenueSection section() const {
    VenueSection s;
    s.name = "lighter";
    s.kind = "lighter";
    s.testnet = false;
    s.ws_url = srv.ws_base() + "/stream";
    s.rest_url = srv.http_base();
    s.api_key = std::to_string(kAccount);
    s.api_secret = kKey;
    s.supports_replace = true;
    s.extra["api_key_index"] = "4";
    s.extra["dead_mans_switch_s"] = "310";
    s.extra["latency_log_s"] = "0";
    return s;
  }
};

// The CreateOrder a sendtx frame carries, rebuilt from its tx_info: its signature must verify
// against the key over the transaction's hash.
bool create_order_verifies(std::string_view frame) {
  CreateOrderTx tx;
  tx.h.account_index = num(frame, "AccountIndex");
  tx.h.api_key_index = static_cast<std::uint8_t>(num(frame, "ApiKeyIndex"));
  tx.h.expired_at = num(frame, "ExpiredAt");
  tx.h.nonce = num(frame, "Nonce");
  if (frame.find(R"("L2TxAttributes":{"4":1})") != std::string_view::npos)
    static_cast<void>(tx.h.attributes.add(kAttrSkipNonce, 1));
  tx.market_index = static_cast<std::int16_t>(num(frame, "MarketIndex"));
  tx.client_order_index = num(frame, "ClientOrderIndex");
  tx.base_amount = num(frame, "BaseAmount");
  tx.price = static_cast<std::uint32_t>(num(frame, "Price"));
  tx.is_ask = static_cast<std::uint8_t>(num(frame, "IsAsk"));
  tx.type = static_cast<std::uint8_t>(num(frame, "Type"));
  tx.time_in_force = static_cast<std::uint8_t>(num(frame, "TimeInForce"));
  tx.reduce_only = static_cast<std::uint8_t>(num(frame, "ReduceOnly"));
  tx.trigger_price = static_cast<std::uint32_t>(num(frame, "TriggerPrice"));
  tx.order_expiry = num(frame, "OrderExpiry");
  const std::vector<std::uint8_t> raw = unbase64(json_str(frame, "Sig"));
  if (raw.size() != 80) return false;
  SigBytes sig{};
  std::copy(raw.begin(), raw.end(), sig.begin());
  Fp5Bytes pk{};
  if (!from_hex(kPub, pk)) return false;
  return verify(pk, tx_hash(tx, kMainnetChainId), sig);
}

}  // namespace

TEST_CASE("lighter.venue: config mapping and defaults") {
  VenueSection s;
  s.name = "lt";
  s.testnet = false;
  s.api_key = "702384";
  s.api_secret = kKey;
  s.extra["api_key_index"] = "4";
  s.extra["ticker"] = "false";
  s.extra["order_api"] = "rest";
  s.extra["dead_mans_switch_s"] = "600";
  LighterVenueConfig c = make_lighter_config(s, true);
  CHECK(c.chain_id == kMainnetChainId);
  CHECK(c.ws_url == "wss://mainnet.zklighter.elliot.ai/stream");
  CHECK(c.rest_url == "https://mainnet.zklighter.elliot.ai");
  CHECK(c.account_index == 702384);
  CHECK(c.api_key_index == 4);
  CHECK_FALSE(c.channels.ticker);
  CHECK(c.channels.order_book);
  CHECK_FALSE(c.ws_order_api);
  CHECK(c.skip_nonce);
  CHECK_FALSE(c.emit_ack_from_response);
  CHECK(c.dead_mans_switch_s == 600);
  s.testnet = true;
  c = make_lighter_config(s, true);
  CHECK(c.chain_id == kTestnetChainId);
  CHECK(c.rest_url == "https://testnet.zklighter.elliot.ai");
  s.extra["dead_mans_switch_s"] = "60";  // the venue wants 5 minutes at the least
  CHECK_THROWS_AS(make_lighter_config(s, true), std::invalid_argument);
  s.extra["dead_mans_switch_s"] = "0";
  s.extra["api_key_index"] = "255";
  CHECK_THROWS_AS(make_lighter_config(s, true), std::invalid_argument);
  s.extra.erase("api_key_index");
  CHECK_THROWS_AS(make_lighter_config(s, true), std::invalid_argument);  // a key needs its slot
  s.extra["api_key_index"] = "4";
  s.api_key = "0xabc";
  CHECK_THROWS_AS(make_lighter_config(s, true), std::invalid_argument);
}

TEST_CASE("lighter.venue: price and size as the venue's integers") {
  CHECK(LighterVenue::price_units(Price::from_decimal("226.3004").value(), 3, true) == 226300);
  CHECK(LighterVenue::price_units(Price::from_decimal("226.3004").value(), 3, false) == 226301);
  CHECK(LighterVenue::price_units(Price::from_decimal("226.3").value(), 3, false) == 226300);
  CHECK(LighterVenue::price_units(Price::from_decimal("5000000").value(), 3, true) == 0);  // > 2^32
  CHECK(LighterVenue::size_units(Qty::from_decimal("0.0509").value(), 3) == 50);
  CHECK(LighterVenue::size_units(Qty::from_decimal("0.0009").value(), 3) == 0);
}

TEST_CASE("lighter.venue: the registry resolves lighter and its alias") {
  VenueRegistry r;
  register_builtin_venues(r);
  const VenueEntry* e = r.find("lighter");
  REQUIRE(e != nullptr);
  CHECK(r.find("zklighter") == e);
  CHECK(e->caps.positions);
  CHECK_FALSE(e->caps.executions);
  VenueSection s;
  s.name = "l";
  s.kind = "zklighter";
  VenueFactoryOptions opts;
  opts.dry_run = true;
  const std::unique_ptr<Venue> v = make_venue(VenueId{9}, s, opts, r);
  REQUIRE(v);
  CHECK(v->name() == "l");
  CHECK_FALSE(v->caps().user_stream);
  Config cfg;
  s.extra["no_such_key"] = "1";
  cfg.venues.push_back(s);
  CHECK_THROWS_WITH_AS(validate_venues(cfg, r), doctest::Contains("no_such_key"), ConfigError);
}

TEST_CASE("lighter.venue: scripted fake exchange end to end") {
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("NVDA", 9, "NVDA", "USDC")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  const Seqlocked<TscCalibration> tsc(calibrate_tsc(milliseconds(10)));
  {
    LighterVenue venue(kVenue, make_lighter_config(h.section(), false));
    venue.set_tsc_calibration_source(&tsc);
    REQUIRE(venue.load_reference_data(instruments));
    CHECK(venue.market_of(kNvda) == 110);
    CHECK(venue.size_decimals_of(kNvda) == 3);
    CHECK(venue.price_decimals_of(kNvda) == 3);
    CHECK(instruments.get(kNvda).tick == Price::from_decimal("0.001").value());
    CHECK(instruments.get(kNvda).lot == Qty::from_decimal("0.001").value());
    CHECK(instruments.get(kNvda).min_qty == Qty::from_decimal("0.025").value());
    CHECK(instruments.get(kNvda).min_notional == Notional::from_int(10));
    CHECK(instruments.get(kNvda).asset_class == AssetClass::Perpetual);
    CHECK(venue.next_nonce() == 77);
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kNvda};
    venue.subscribe(ids);
    venue.connect(reactor);
    auto run_until = [&](auto pred, int ms = 4000) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (!pred() && std::chrono::steady_clock::now() < deadline) {
        reactor.run_once(5);
        venue.on_timer(net::Reactor::now_ns());
      }
      return pred();
    };
    // The book, the two account channels (the orders one with the token), order entry.
    REQUIRE(run_until([&] {
      return venue.status().books_synced == 1 && h.private_subs.load() == 2 &&
             venue.order_channel_live() && venue.private_channel_live();
    }));
    CHECK(h.auth_seen.load());
    // The start-up sweep: open orders (authenticated), then the account's positions.
    REQUIRE(run_until([&] { return h.active_calls.load() >= 1 && h.account_calls.load() >= 1; }));
    reactor.run_once(20);
    {
      bool position = false;
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::Reconcile) continue;
        const auto& r = RecordingSink::as<ReconcileMsg>(m);
        if (r.kind == ReconcileMsg::Kind::Position) {
          position = true;
          CHECK(r.position_qty == Qty::from_decimal("-0.05").value());
          CHECK(r.avg_px == Price::from_decimal("226.1").value());
        }
      }
      CHECK(position);
    }
    // The dead man's switch: a scheduled CancelAll on the trade connection.
    REQUIRE(run_until([&] { return h.sendtx.load() >= 1; }));
    {
      const std::string f = h.srv.frames("sendtx")[0];
      CHECK(f.find(R"("tx_type":16)") != std::string::npos);
      CHECK(f.find(R"("TimeInForce":1)") != std::string::npos);
      const std::int64_t when = num(f, "Time");
      CHECK(when - wall_now().ns / 1'000'000 > 300'000);
      CHECK(num(f, "Nonce") == 77);
    }
    // A post-only bid off the grid: the price rounded down, the client index the id itself.
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kNvda, kVenue);
    n.cl_ord_id = ClientOrderId{42};
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.tif = TimeInForce::Gtc;
    n.price = Price::from_decimal("226.3004").value();
    n.qty = Qty::from_decimal("0.05").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.sendtx.load() == 2; }));
    {
      const std::string f = h.srv.frames("sendtx")[1];
      CHECK(f.find(R"("type":"jsonapi/sendtx")") != std::string::npos);
      CHECK(f.find(R"("tx_type":14)") != std::string::npos);
      CHECK(
          f.find(
              R"("tx_info":{"AccountIndex":281474976499586,"ApiKeyIndex":4,"MarketIndex":110,"ClientOrderIndex":42,"BaseAmount":50,"Price":226300,"IsAsk":0,"Type":0,"TimeInForce":2,"ReduceOnly":0,"TriggerPrice":0,"OrderExpiry":)") !=
          std::string::npos);
      CHECK(f.find(R"("Nonce":78,)") != std::string::npos);
      CHECK(f.find(R"("L2TxAttributes":{"4":1}})") != std::string::npos);
      CHECK(create_order_verifies(f));
    }
    // Accepted is not an ack; the account stream's "open" is, once.
    reactor.run_once(30);
    for (const auto& m : orders.drain()) CHECK(RecordingSink::type_of(m) != EventType::OrderAck);
    h.to_private(order_update(42, "open", "0.000"));
    h.to_private(order_update(42, "open", "0.000"));
    std::size_t acks = 0;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderAck) continue;
        const auto& a = RecordingSink::as<OrderAckMsg>(m);
        CHECK(a.cl_ord_id == ClientOrderId{42});
        CHECK(a.venue_order_id.view() == "900042");
        ++acks;
      }
      return acks >= 1;
    }));
    reactor.run_once(30);
    for (const auto& m : orders.drain()) CHECK(RecordingSink::type_of(m) != EventType::OrderAck);
    CHECK(acks == 1);
    // A fill: cum and leaves from the shadow, the fee from the maker rate.
    h.to_private(trade_update(42, "0.020", "4.526000"));
    bool filled = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderFill) continue;
        const auto& f = RecordingSink::as<OrderFillMsg>(m);
        CHECK(f.cl_ord_id == ClientOrderId{42});
        CHECK(f.side == Side::Buy);
        CHECK(f.qty == Qty::from_decimal("0.02").value());
        CHECK(f.cum_qty == Qty::from_decimal("0.02").value());
        CHECK(f.leaves_qty == Qty::from_decimal("0.03").value());
        CHECK(f.liquidity == Liquidity::Maker);
        CHECK(f.fee == Notional::from_decimal("0.00018104").value());
        CHECK(f.exec_id.view() == "70042");
        filled = true;
      }
      return filled;
    }));
    // Cancel by the client order index; the stream's "canceled" is the cancel ack.
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kNvda, kVenue);
    c.cl_ord_id = ClientOrderId{42};
    REQUIRE(outbound.try_push(&c, c.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.sendtx.load() == 3; }));
    {
      const std::string f = h.srv.frames("sendtx")[2];
      CHECK(f.find(R"("tx_type":15)") != std::string::npos);
      CHECK(f.find(R"("MarketIndex":110,"Index":42,)") != std::string::npos);
      CHECK(f.find(R"("Nonce":79,)") != std::string::npos);
    }
    h.to_private(order_update(42, "canceled", "0.020"));
    bool cancelled = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderCancelAck) continue;
        CHECK(RecordingSink::as<OrderCancelAckMsg>(m).cum_qty == Qty::from_decimal("0.02").value());
        cancelled = true;
      }
      return cancelled;
    }));
    CHECK(venue.shadow_count() == 0);
    // A refused transaction: the reject names the reason.
    {
      std::lock_guard<std::mutex> lock(h.mu);
      h.tx_error = "not enough margin to create the order";
    }
    n.cl_ord_id = ClientOrderId{43};
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    bool rejected = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderReject) continue;
        const auto& r = RecordingSink::as<OrderRejectMsg>(m);
        CHECK(r.cl_ord_id == ClientOrderId{43});
        CHECK(r.reason == RejectReason::InsufficientBalance);
        CHECK(r.venue_code == 21705);
        rejected = true;
      }
      return rejected;
    }));
    // A post-only the venue ends before it rests: a reject, not an expiry.
    n.cl_ord_id = ClientOrderId{44};
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.sendtx.load() == 5; }));
    reactor.run_once(20);
    h.to_private(order_update(44, "canceled-post-only", "0.000"));
    rejected = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        CHECK(RecordingSink::type_of(m) != EventType::OrderAck);
        if (RecordingSink::type_of(m) != EventType::OrderReject) continue;
        CHECK(RecordingSink::as<OrderRejectMsg>(m).reason == RejectReason::PostOnlyWouldCross);
        rejected = true;
      }
      return rejected;
    }));
    CHECK(venue.shadow_count() == 0);
    // A modify, applied in place: an ack with kAmendedInPlace, and the order's later events
    // (which name the first client index) under the new id.
    n.cl_ord_id = ClientOrderId{45};
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.sendtx.load() == 6; }));
    h.to_private(order_update(45, "open", "0.000"));
    bool opened = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain())
        opened = opened || RecordingSink::type_of(m) == EventType::OrderAck;
      return opened;
    }));
    OutReplaceMsg rp{};
    init_header(rp, EventType::OutReplace, kNvda, kVenue);
    rp.cl_ord_id = ClientOrderId{46};
    rp.orig_cl_ord_id = ClientOrderId{45};
    rp.price = Price::from_decimal("226.2").value();
    rp.qty = Qty::from_decimal("0.04").value();
    REQUIRE(outbound.try_push(&rp, rp.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.sendtx.load() == 7; }));
    {
      const std::string f = h.srv.frames("sendtx")[6];
      CHECK(f.find(R"("tx_type":17)") != std::string::npos);
      CHECK(
          f.find(
              R"("MarketIndex":110,"Index":45,"BaseAmount":40,"Price":226200,"TriggerPrice":0,)") !=
          std::string::npos);
    }
    bool amended = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderAck) continue;
        const auto& a = RecordingSink::as<OrderAckMsg>(m);
        CHECK(a.cl_ord_id == ClientOrderId{46});
        CHECK((a.flags & OrderAckMsg::kAmendedInPlace) != 0);
        amended = true;
      }
      return amended;
    }));
    h.to_private(trade_update(45, "0.040", "9.048000"));
    filled = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderFill) continue;
        const auto& f = RecordingSink::as<OrderFillMsg>(m);
        CHECK(f.cl_ord_id == ClientOrderId{46});
        CHECK(f.leaves_qty.is_zero());
        filled = true;
      }
      return filled;
    }));
    CHECK(venue.shadow_count() == 0);
    // The volume quota: a new order that would leave no more than the reserve is refused here.
    venue.set_quota(40);
    const int before = h.sendtx.load();
    n.cl_ord_id = ClientOrderId{47};
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    bool refused = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderReject) continue;
        const auto& r = RecordingSink::as<OrderRejectMsg>(m);
        CHECK(r.reason == RejectReason::VenueRateLimit);
        CHECK(r.text.view() == "local rate limit: volume quota");
        refused = true;
      }
      return refused;
    }));
    CHECK(h.sendtx.load() == before);
    // The blocking kill switch: one CancelAll now over REST.
    CHECK(venue.cancel_all());
    REQUIRE(h.rest_sendtx.load() == 1);
    {
      const std::string body = h.srv.frames("rest_sendtx")[0];
      CHECK(body.starts_with("tx_type=16&tx_info="));
      CHECK(body.find("%22TimeInForce%22%3A0%2C%22Time%22%3A0") != std::string::npos);
    }
    // A clean stop aborts the scheduled cancel.
    venue.disconnect();
    REQUIRE(h.rest_sendtx.load() == 2);
    CHECK(h.srv.frames("rest_sendtx")[1].find("%22TimeInForce%22%3A2") != std::string::npos);
  }
}
