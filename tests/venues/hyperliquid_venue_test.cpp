// HyperliquidVenue: config mapping, the registry entry, and the connector against a scripted
// in-process fake Hyperliquid (POST /info and /exchange, one WebSocket URL serving market data,
// the account streams and the posts): reference data (perpDexs, metaAndAssetCtxs, the agent check,
// the address budget), the l2Book snapshot, the account subscriptions, a post-only order rounded
// onto the price grid and acknowledged once although the reply and the stream both say so, a fill
// completed from the shadow, a cancel by oid, the address-budget refusal, a per-order error, the
// reconciliation and the blocking cancel_all.
#include "fastmm/venues/hyperliquid/hyperliquid_venue.hpp"

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
using namespace fastmm::venues::hyperliquid;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kKey = "0x4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318";
constexpr const char* kSigner = "0x2c7536e3605d9c16a7a3d7b1898e529396a65c23";
constexpr const char* kAccount = "0x00000000000000000000000000000000000000a1";
constexpr VenueId kVenue{7};
const InstrumentId kNvda{0};
constexpr const char* kCloid1 = "0x666d000000000000000000000000002a";

std::size_t count_of(std::string_view s, std::string_view needle) {
  std::size_t n = 0;
  for (std::size_t p = s.find(needle); p != std::string_view::npos; p = s.find(needle, p + 1)) ++n;
  return n;
}

// The action's "type" inside a post frame or an /exchange body.
std::string action_type(std::string_view t) {
  const std::string_view key = R"("action":{"type":")";
  const std::size_t p = t.find(key);
  if (p == std::string_view::npos) return {};
  const std::size_t start = p + key.size();
  return std::string(t.substr(start, t.find('"', start) - start));
}

std::string post_reply(const std::string& id, std::string_view type, std::string_view statuses) {
  return R"({"channel":"post","data":{"id":)" + id +
         R"(,"response":{"type":"action","payload":{"status":"ok","response":{"type":")" +
         std::string(type) + R"(","data":{"statuses":[)" + std::string(statuses) + "]}}}}}}";
}

std::string order_update(std::string_view cloid,
                         std::string_view status,
                         std::uint64_t oid,
                         const char* sz,
                         const char* orig) {
  return R"({"channel":"orderUpdates","data":[{"order":{"coin":"xyz:NVDA","side":"B","limitPx":"230.32","sz":")" +
         std::string(sz) + R"(","oid":)" + std::to_string(oid) +
         R"(,"timestamp":1791781700000,"origSz":")" + std::string(orig) + R"(","cloid":")" +
         std::string(cloid) + R"("},"status":")" + std::string(status) +
         R"(","statusTimestamp":1791781700100}]})";
}

struct Harness {
  FakeVenueServer srv;
  std::string perp_dexs = fastmm::test::fixture("hyperliquid/rest_perp_dexs.json");
  std::string meta = fastmm::test::fixture("hyperliquid/rest_meta_and_ctxs_xyz.json");
  std::string spot = fastmm::test::fixture("hyperliquid/rest_spot_clearinghouse.json");
  std::string book = fastmm::test::fixture("hyperliquid/md_l2book.json");
  std::mutex mu;
  std::string open_orders = "[]";
  std::string order_error;  // the next order's status is this error when set
  std::atomic<int> info_calls{0};
  std::atomic<int> open_order_calls{0};
  std::atomic<int> position_calls{0};
  std::atomic<int> spot_calls{0};
  std::atomic<int> fill_calls{0};
  std::atomic<int> rate_calls{0};
  std::atomic<int> posts{0};
  std::atomic<int> private_subs{0};
  std::atomic<int> exchange_cancels{0};
  net::WsSession* private_session = nullptr;  // server thread only

  ~Harness() { srv.stop(); }
  Harness() {
    srv.route("POST", "/info", [this](const net::HttpRequest& r) {
      ++info_calls;
      const std::string type = json_str(r.body, "type");
      srv.record("info", std::string(r.body));
      if (type == "perpDexs") return net::HttpServerResponse::json(200, perp_dexs);
      if (type == "metaAndAssetCtxs") return net::HttpServerResponse::json(200, meta);
      if (type == "extraAgents")
        return net::HttpServerResponse::json(200,
                                             std::string(R"([{"name":"FastMM","address":")") +
                                                 kSigner + R"(","validUntil":1806393075663}])");
      if (type == "userRateLimit") {
        ++rate_calls;
        return net::HttpServerResponse::json(
            200,
            R"({"cumVlm":"224.59","nRequestsUsed":6293,"nRequestsCap":10224,"nRequestsSurplus":0})");
      }
      if (type == "frontendOpenOrders") {
        ++open_order_calls;
        std::lock_guard<std::mutex> lock(mu);
        return net::HttpServerResponse::json(200, open_orders);
      }
      if (type == "clearinghouseState") {
        ++position_calls;
        return net::HttpServerResponse::json(
            200,
            R"({"marginSummary":{"accountValue":"0.0","totalNtlPos":"0.0","totalRawUsd":"0.0","totalMarginUsed":"0.0"},"withdrawable":"0.0","assetPositions":[{"type":"oneWay","position":{"coin":"xyz:NVDA","szi":"0.05","entryPx":"230.3"}}],"time":1791781700000})");
      }
      if (type == "spotClearinghouseState") {
        ++spot_calls;
        return net::HttpServerResponse::json(200, spot);
      }
      if (type == "userFillsByTime") {
        ++fill_calls;
        return net::HttpServerResponse::json(200, "[]");
      }
      return net::HttpServerResponse::json(400, R"("unknown info type")");
    });
    srv.route("POST", "/exchange", [this](const net::HttpRequest& r) {
      srv.record("exchange", std::string(r.body));
      const std::string type = action_type(r.body);
      if (type == "cancel") {
        ++exchange_cancels;
        return net::HttpServerResponse::json(
            200, R"({"status":"ok","response":{"type":"cancel","data":{"statuses":["success"]}}})");
      }
      return net::HttpServerResponse::json(200, R"({"status":"ok","response":{"type":"default"}})");
    });
    srv.on_ws_open("/ws", [](net::WsSession&) {});
    srv.on_ws_text("/ws", [this](net::WsSession& s, std::string_view t) {
      const std::string method = json_str(t, "method");
      if (method == "ping") {
        s.send_text(R"({"channel":"pong"})");
        return;
      }
      if (method == "subscribe") {
        const std::string type = json_str(t, "type");
        s.send_text(
            R"({"channel":"subscriptionResponse","data":{"method":"subscribe","subscription":)" +
            std::string(t.substr(t.find("\"subscription\":") + 15,
                                 t.size() - t.find("\"subscription\":") - 16)) +
            "}}");
        if (type == "l2Book") s.send_text(book);
        if (!json_str(t, "user").empty()) {
          ++private_subs;
          private_session = &s;
        }
        return;
      }
      if (method == "post") {
        ++posts;
        srv.record("post", std::string(t));
        const std::string id = json_int(t, "id");
        const std::string type = action_type(t);
        if (type == "order") {
          std::string statuses;
          const std::size_t n = count_of(t, R"("tif")");
          std::lock_guard<std::mutex> lock(mu);
          for (std::size_t i = 0; i < n; ++i) {
            if (!statuses.empty()) statuses += ',';
            if (!order_error.empty()) {
              statuses += R"({"error":")" + order_error + R"("})";
            } else {
              statuses += R"({"resting":{"oid":)" + std::to_string(5001 + i) + "}}";
            }
          }
          order_error.clear();
          s.send_text(post_reply(id, "order", statuses));
        } else if (type == "cancel" || type == "cancelByCloid") {
          std::string statuses;
          for (std::size_t i = 0; i < count_of(t, R"("o":)") + count_of(t, R"("cloid")"); ++i)
            statuses += i == 0 ? R"("success")" : R"(,"success")";
          s.send_text(post_reply(id, "cancel", statuses));
        }
        return;
      }
    });
    srv.start();
  }

  VenueSection section() const {
    VenueSection s;
    s.name = "hyperliquid";
    s.kind = "hyperliquid";
    s.testnet = false;
    s.ws_url = srv.ws_base() + "/ws";
    s.rest_url = srv.http_base();
    s.api_key = kAccount;
    s.api_secret = kKey;
    return s;
  }
};

}  // namespace

TEST_CASE("hyperliquid.venue: config mapping and defaults") {
  VenueSection s;
  s.name = "hl";
  s.testnet = false;
  s.api_key = "0x0000000000000000000000000000000000000002";
  s.extra["dex"] = "xyz";
  s.extra["bbo"] = "false";
  s.extra["dead_mans_switch_s"] = "30";
  s.extra["order_api"] = "rest";
  HyperliquidVenueConfig c = make_hyperliquid_config(s, true);
  CHECK(c.mainnet);
  CHECK(c.ws_url == "wss://api.hyperliquid.xyz/ws");
  CHECK(c.rest_url == "https://api.hyperliquid.xyz");
  CHECK(c.dex == "xyz");
  CHECK_FALSE(c.channels.bbo);
  CHECK(c.channels.l2_book);
  CHECK_FALSE(c.ws_order_api);
  CHECK(c.dead_mans_switch_s == 30);
  CHECK(c.address_reserve == 200);
  s.testnet = true;
  c = make_hyperliquid_config(s, true);
  CHECK_FALSE(c.mainnet);
  CHECK(c.rest_url == "https://api.hyperliquid-testnet.xyz");
  s.extra["dead_mans_switch_s"] = "5";
  CHECK_THROWS_AS(make_hyperliquid_config(s, true), std::invalid_argument);
  s.extra["dead_mans_switch_s"] = "0";
  s.api_key = "not-an-address";
  CHECK_THROWS_AS(make_hyperliquid_config(s, true), std::invalid_argument);
}

TEST_CASE("hyperliquid.venue: the registry resolves hyperliquid and its alias") {
  VenueRegistry r;
  register_builtin_venues(r);
  const VenueEntry* e = r.find("hyperliquid");
  REQUIRE(e != nullptr);
  CHECK(r.find("hl") == e);
  CHECK(e->caps.executions);
  CHECK(e->caps.positions);
  VenueSection s;
  s.name = "h";
  s.kind = "hl";
  VenueFactoryOptions opts;
  opts.dry_run = true;
  const std::unique_ptr<Venue> v = make_venue(VenueId{7}, s, opts, r);
  REQUIRE(v);
  CHECK(v->name() == "h");
  CHECK_FALSE(v->caps().user_stream);
  Config cfg;
  s.extra["no_such_key"] = "1";
  cfg.venues.push_back(s);
  CHECK_THROWS_WITH_AS(validate_venues(cfg, r), doctest::Contains("no_such_key"), ConfigError);
}

TEST_CASE("hyperliquid.venue: scripted fake exchange end to end") {
  Harness h;
  InstrumentTable instruments;
  Instrument inst = make_instrument("xyz:NVDA", 7, "NVDA", "USDC");
  REQUIRE(instruments.add(inst));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  const Seqlocked<TscCalibration> tsc(calibrate_tsc(milliseconds(10)));
  {
    HyperliquidVenue venue(kVenue, make_hyperliquid_config(h.section(), false));
    venue.set_tsc_calibration_source(&tsc);
    REQUIRE(venue.load_reference_data(instruments));
    // xyz is perp dex 1 and NVDA index 2 of its meta; the tick at its mark, the lot of
    // szDecimals 3.
    CHECK(venue.asset_of(kNvda) == 110002);
    CHECK(venue.sz_decimals_of(kNvda) == 3);
    CHECK(venue.coin_of(kNvda) == "xyz:NVDA");
    CHECK(instruments.get(kNvda).tick == Price::from_decimal("0.01").value());
    CHECK(instruments.get(kNvda).lot == Qty::from_decimal("0.001").value());
    CHECK(instruments.get(kNvda).min_notional == Notional::from_int(10));
    CHECK(instruments.get(kNvda).quote.view() == "USDC");
    CHECK(instruments.get(kNvda).asset_class == AssetClass::Perpetual);
    CHECK(venue.address_budget().known);
    CHECK(venue.address_budget().used == 6293);
    CHECK(venue.address_budget().cap == 10224);
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
    // The book from the l2Book snapshot, the three account streams, the order connection.
    REQUIRE(run_until([&] {
      return venue.status().books_synced == 1 && h.private_subs.load() == 3 &&
             venue.order_channel_live();
    }));
    bool snapshot = false;
    for (const auto& m : md.drain())
      snapshot = snapshot || RecordingSink::type_of(m) == EventType::BookSnapshot;
    CHECK(snapshot);
    // The start-up sweep: fills, open orders and positions per dex, the spot balances.
    REQUIRE(run_until([&] {
      return h.open_order_calls.load() >= 1 && h.position_calls.load() >= 1 &&
             h.fill_calls.load() >= 1 && h.spot_calls.load() >= 1;
    }));
    reactor.run_once(20);
    {
      bool position = false;
      bool end = false;
      bool balance = false;
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) == EventType::Reconcile) {
          const auto& r = RecordingSink::as<ReconcileMsg>(m);
          if (r.kind == ReconcileMsg::Kind::Position) {
            position = true;
            CHECK(r.position_qty == Qty::from_decimal("0.05").value());
          }
          if (r.kind == ReconcileMsg::Kind::End) end = true;
        }
        if (RecordingSink::type_of(m) == EventType::Balance) balance = true;
      }
      CHECK(position);
      CHECK(end);
      CHECK(balance);
    }
    // A post-only bid off the grid: rounded down to 230.32, cloid from the client id, one post.
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kNvda, kVenue);
    n.cl_ord_id = ClientOrderId{0x2a};
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.tif = TimeInForce::Gtc;
    n.price = Price::from_decimal("230.327").value();
    n.qty = Qty::from_decimal("0.05").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.posts.load() == 1; }));
    const std::string frame = h.srv.frames("post")[0];
    CHECK(
        frame.find(
            R"("action":{"type":"order","orders":[{"a":110002,"b":true,"p":"230.32","s":"0.05","r":false,"t":{"limit":{"tif":"Alo"}},"c":")" +
            std::string(kCloid1) + R"("}],"grouping":"na"})") != std::string::npos);
    CHECK(frame.find(R"("signature":{"r":"0x)") != std::string::npos);
    CHECK(venue.address_budget().used == 6294);
    std::size_t acks = 0;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderAck) continue;
        const auto& a = RecordingSink::as<OrderAckMsg>(m);
        CHECK(a.cl_ord_id == ClientOrderId{0x2a});
        CHECK(a.venue_order_id.view() == "5001");
        ++acks;
      }
      return acks == 1;
    }));
    // The stream's "open" repeats the ack: not forwarded again.
    h.srv.run_on_server([&] {
      if (h.private_session != nullptr)
        h.private_session->send_text(order_update(kCloid1, "open", 5001, "0.05", "0.05"));
    });
    reactor.run_once(50);
    venue.on_timer(net::Reactor::now_ns());
    for (const auto& m : orders.drain()) CHECK(RecordingSink::type_of(m) != EventType::OrderAck);
    // A fill on the stream: cum and leaves from the shadow.
    h.srv.run_on_server([&] {
      if (h.private_session != nullptr)
        h.private_session->send_text(
            std::string(R"({"channel":"userFills","data":{"user":")") + kAccount +
            R"(","fills":[{"coin":"xyz:NVDA","px":"230.32","sz":"0.02","side":"B","time":1791781701000,"startPosition":"0.0","dir":"Open Long","closedPnl":"0.0","hash":"0x0","oid":5001,"crossed":false,"fee":"0.0001","tid":42,"cloid":")" +
            kCloid1 + R"(","feeToken":"USDC","twapId":null}]}})");
    });
    bool filled = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderFill) continue;
        const auto& f = RecordingSink::as<OrderFillMsg>(m);
        CHECK(f.cl_ord_id == ClientOrderId{0x2a});
        CHECK(f.qty == Qty::from_decimal("0.02").value());
        CHECK(f.cum_qty == Qty::from_decimal("0.02").value());
        CHECK(f.leaves_qty == Qty::from_decimal("0.03").value());
        CHECK(f.liquidity == Liquidity::Maker);
        CHECK(f.exec_id.view() == "42");
        filled = true;
      }
      return filled;
    }));
    // Cancel: by the oid the reply gave; the stream's "canceled" is the cancel ack.
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kNvda, kVenue);
    c.cl_ord_id = ClientOrderId{0x2a};
    REQUIRE(outbound.try_push(&c, c.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.posts.load() == 2; }));
    CHECK(h.srv.frames("post")[1].find(
              R"("action":{"type":"cancel","cancels":[{"a":110002,"o":5001}]})") !=
          std::string::npos);
    h.srv.run_on_server([&] {
      if (h.private_session != nullptr)
        h.private_session->send_text(order_update(kCloid1, "canceled", 5001, "0.03", "0.05"));
    });
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
    // A per-order error in an accepted action: the reject names the reason.
    {
      std::lock_guard<std::mutex> lock(h.mu);
      h.order_error = "Order must have minimum value of $10.";
    }
    n.cl_ord_id = ClientOrderId{0x2b};
    n.qty = Qty::from_decimal("0.01").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    bool rejected = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderReject) continue;
        const auto& r = RecordingSink::as<OrderRejectMsg>(m);
        CHECK(r.cl_ord_id == ClientOrderId{0x2b});
        CHECK(r.reason == RejectReason::BelowMinNotional);
        rejected = true;
      }
      return rejected;
    }));
    // The address budget: a new order that would leave no more than the reserve is refused here.
    venue.set_address_budget(10224 - 150, 10224);
    const int posts_before = h.posts.load();
    n.cl_ord_id = ClientOrderId{0x2c};
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    bool refused = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderReject) continue;
        const auto& r = RecordingSink::as<OrderRejectMsg>(m);
        CHECK(r.reason == RejectReason::VenueRateLimit);
        CHECK(r.text.view() == "local rate limit: address budget");
        refused = true;
      }
      return refused;
    }));
    CHECK(h.posts.load() == posts_before);
    venue.on_timer(net::Reactor::now_ns());  // publishes the status
    CHECK(venue.status().refused_orders_1d == 1);
    // The blocking kill switch: the open orders of each dex, then one signed cancel action.
    {
      std::lock_guard<std::mutex> lock(h.mu);
      h.open_orders =
          R"([{"coin":"xyz:NVDA","side":"B","limitPx":"229.0","sz":"0.05","oid":777,"timestamp":1791781700000,"origSz":"0.05","cloid":null}])";
    }
    CHECK(venue.cancel_all());
    CHECK(h.exchange_cancels.load() == 1);
    REQUIRE_FALSE(h.srv.frames("exchange").empty());
    CHECK(h.srv.frames("exchange").back().find(R"("cancels":[{"a":110002,"o":777}])") !=
          std::string::npos);
    venue.disconnect();
  }
}
