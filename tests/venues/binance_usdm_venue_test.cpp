// BinanceUsdmVenue against a scripted in-process fake Binance USDⓈ-M (REST + public/market streams
// + private listenKey stream + WS API): reference data and account checks, depth sync with a pu
// gap, order.place / order.modify / order.cancel with the venue client id kept across the modify,
// reconciliation with positions, the ACCOUNT_UPDATE position check, listenKey expiry, order-channel
// loss, the execution replay (GET /fapi/v1/userTrades), funding (GET /fapi/v1/income) and the
// blocking kill-switch cancel_all.
#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/registry.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using namespace fastmm::venues::test;

namespace {

Duration hours(std::int64_t h) {
  return seconds(h * 3600);
}

constexpr const char* kKey = "fake-key";
constexpr const char* kSecret = "fake-secret";
constexpr const char* kPrivatePath = "/private/ws/lk-test-0001";

std::string depth_frame(std::uint64_t U, std::uint64_t u, std::uint64_t pu, const char* bid_px) {
  return R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1789469121904,"T":1789469121836,"s":"BTCUSDT","ps":"BTCUSDT","U":)" +
         std::to_string(U) + R"(,"u":)" + std::to_string(u) + R"(,"pu":)" + std::to_string(pu) +
         R"(,"b":[[")" + bid_px + R"(","1.000"]],"a":[],"st":1}})";
}

std::string order_update(const char* client_id,
                         const char* x,
                         const char* q,
                         const char* l,
                         const char* z,
                         long trade_id) {
  return std::string(
             R"({"e":"ORDER_TRADE_UPDATE","E":1789469200000,"T":1789469199999,"o":{"s":"BTCUSDT","c":")") +
         client_id + R"(","S":"BUY","o":"LIMIT","f":"GTX","q":")" + q +
         R"(","p":"70000.00","ap":"0","sp":"0","x":")" + x + R"(","X":"NEW","i":4293153,"l":")" +
         l + R"(","z":")" + z +
         R"(","L":"70000.00","n":"0.0056","N":"USDT","T":1789469199999,"t":)" +
         std::to_string(trade_id) +
         R"(,"b":"0","a":"0","m":true,"R":false,"wt":"CONTRACT_PRICE","ot":"LIMIT","ps":"BOTH","cp":false,"rp":"0","pP":false,"si":0,"ss":0,"V":"EXPIRE_MAKER","pm":"NONE","gtd":0,"er":"0"}})";
}

std::string account_update(const char* amount) {
  return std::string(
             R"({"e":"ACCOUNT_UPDATE","E":1789469300000,"T":1789469299999,"a":{"m":"ORDER","B":[{"a":"USDT","wb":"5000","cw":"5000","bc":"0"}],"P":[{"s":"BTCUSDT","pa":")") +
         amount +
         R"(","ep":"70000.0","bep":"70000.0","cr":"0","up":"0","mt":"cross","iw":"0","ps":"BOTH"}]}})";
}

std::string percent_decode(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

const net::Ed25519Key& ed_public_key() {
  static const net::Ed25519Key k =
      net::Ed25519Key::from_public_pem(fastmm::test::fixture("binance/ed25519-test-public.pem"));
  return k;
}

// HMAC (kSecret) or Ed25519 (the test key pair) signature over the query before it.
bool signed_ok(std::string_view query) {
  const std::size_t p = query.rfind("&signature=");
  if (p == std::string_view::npos) return false;
  const std::string expected(net::hmac_sha256_hex(kSecret, query.substr(0, p)).view());
  if (query.substr(p + 11) == expected) return true;
  return ed_public_key().verify_base64(query.substr(0, p), percent_decode(query.substr(p + 11)));
}

std::string ws_result(const std::string& id, const std::string& result) {
  return R"({"id":")" + id + R"(","status":200,"result":)" + result +
         R"(,"rateLimits":[{"rateLimitType":"REQUEST_WEIGHT","interval":"MINUTE","intervalNum":1,"limit":2400,"count":4},{"rateLimitType":"ORDERS","interval":"SECOND","intervalNum":10,"limit":300,"count":1}]})";
}

struct Harness {
  FakeVenueServer srv;
  std::string exchange_info = fastmm::test::fixture("binance_usdm/exchange_info.json");
  bool hedge_mode = false;  // set before start
  std::atomic<int> depth_requests{0};
  std::atomic<std::uint64_t> depth_last_update_id{100};  // the snapshot's lastUpdateId
  std::atomic<int> listen_keys{0};
  std::atomic<int> reconcile_requests{0};
  std::atomic<int> unsigned_requests{0};
  std::atomic<int> cancel_all_ok{0};
  std::atomic<int> countdowns{0};       // POST /fapi/v1/countdownCancelAll with a window
  std::atomic<int> countdown_stops{0};  // ...with countdownTime=0
  std::atomic<bool> countdown_fails{false};
  std::atomic<bool> countdown_fails_eth{false};  // only ETHUSDT's countdown is refused
  std::atomic<int> cancel_all_limited{0};        // the next N allOpenOrders answer 429
  std::atomic<int> cancel_all_requests{0};
  std::atomic<bool> exchange_info_down{false};  // exchangeInfo answers 503
  // GET /fapi/v1/fundingInfo answer (set before load_reference_data); empty: 503.
  std::string funding_info = fastmm::test::fixture("binance_usdm/funding_info.json");
  std::atomic<bool> mark_price{false};  // the market stream sends a recorded markPriceUpdate
  std::atomic<int> user_trades_queries{0};
  std::atomic<int> user_trades_failures{0};  // the next N userTrades queries answer 503
  std::mutex trades_mu;
  std::string user_trades = "[]";  // GET /fapi/v1/userTrades answer (trades_mu)

  void set_user_trades(std::string body) {
    const std::lock_guard<std::mutex> lock(trades_mu);
    user_trades = std::move(body);
  }
  std::string income = "[]";                // GET /fapi/v1/income answer (trades_mu)
  std::atomic<int> income_queries{0};       // counted once the answer is fixed
  std::atomic<int> income_failures{0};      // the next N income queries answer 503
  std::atomic<int> listen_key_failures{0};  // the next N listenKey requests answer 503
  std::string open_orders = "[]";           // GET /fapi/v1/openOrders answer (trades_mu)
  void set_open_orders(std::string body) {
    const std::lock_guard<std::mutex> lock(trades_mu);
    open_orders = std::move(body);
  }
  void set_income(std::string body) {
    const std::lock_guard<std::mutex> lock(trades_mu);
    income = std::move(body);
  }
  std::atomic<bool> multi_assets{false};   // GET /fapi/v1/multiAssetsMargin
  std::atomic<bool> account_fails{false};  // GET /fapi/v3/account answers 500
  std::atomic<int> account_requests{0};
  std::string account = fastmm::test::fixture("binance_usdm/account_v3_single.json");  // trades_mu
  void set_account(std::string body) {
    const std::lock_guard<std::mutex> lock(trades_mu);
    account = std::move(body);
  }
  std::string order_reply;  // GET /fapi/v1/order answer; empty: -2013 (trades_mu)
  void set_order_reply(std::string body) {
    const std::lock_guard<std::mutex> lock(trades_mu);
    order_reply = std::move(body);
  }

  // The server thread reads this harness's members: stop it before they go.
  ~Harness() { srv.stop(); }
  // `order_limits` false: exchangeInfo allows far more orders than a test sends (the fixture's
  // 300 per 10 s would refuse most of them locally).
  explicit Harness(bool hedge = false, bool order_limits = true) : hedge_mode(hedge) {
    if (!order_limits) {
      for (const char* limit : {R"("limit":1200})", R"("limit":300})"}) {
        const std::size_t at = exchange_info.find(limit);
        REQUIRE(at != std::string::npos);
        exchange_info.replace(at, std::string_view(limit).size(), R"("limit":1000000})");
      }
    }
    srv.route("GET", "/fapi/v1/exchangeInfo", [this](const net::HttpRequest&) {
      if (exchange_info_down.load())
        return net::HttpServerResponse::text(503, "Service Unavailable");
      return net::HttpServerResponse::json(200, exchange_info);
    });
    srv.route("GET", "/fapi/v1/fundingInfo", [this](const net::HttpRequest& r) {
      srv.record("fundingInfo", std::string(r.query));
      if (funding_info.empty()) return net::HttpServerResponse::text(503, "Service Unavailable");
      return net::HttpServerResponse::json(200, funding_info);
    });
    srv.route("GET", "/fapi/v1/time", [](const net::HttpRequest&) {
      return net::HttpServerResponse::json(
          200, R"({"serverTime":)" + std::to_string(wall_now().ns / 1'000'000) + "}");
    });
    srv.route("GET", "/fapi/v1/depth", [this](const net::HttpRequest& r) {
      ++depth_requests;
      srv.record("depth", std::string(r.query));
      return net::HttpServerResponse::json(
          200,
          R"({"lastUpdateId":)" + std::to_string(depth_last_update_id.load()) +
              R"(,"E":1789469121900,"T":1789469121800,"bids":[["70000.00","1.000"]],"asks":[["70000.10","2.000"]]})");
    });
    auto signed_route = [this](const char* method, const char* path, std::string body) {
      srv.route(method, path, [this, path, body](const net::HttpRequest& r) {
        if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
        srv.record(path, std::string(r.query));
        return net::HttpServerResponse::json(200, body);
      });
    };
    signed_route("GET",
                 "/fapi/v1/positionSide/dual",
                 hedge_mode ? R"({"dualSidePosition":true})" : R"({"dualSidePosition":false})");
    signed_route(
        "GET",
        "/fapi/v1/symbolConfig",
        R"([{"symbol":"BTCUSDT","marginType":"CROSSED","isAutoAddMargin":false,"leverage":20,"maxNotionalValue":"1000000"}])");
    signed_route(
        "GET",
        "/fapi/v3/balance",
        R"([{"accountAlias":"x","asset":"USDT","balance":"5000","availableBalance":"5000"}])");
    srv.route("GET", "/fapi/v1/multiAssetsMargin", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      return net::HttpServerResponse::json(
          200,
          multi_assets.load() ? R"({"multiAssetsMargin":true})" : R"({"multiAssetsMargin":false})");
    });
    srv.route("GET", "/fapi/v3/account", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      ++account_requests;
      if (account_fails.load())
        return net::HttpServerResponse::json(500, R"({"code":-1001,"msg":"Internal error."})");
      const std::lock_guard<std::mutex> lock(trades_mu);
      return net::HttpServerResponse::json(200, account);
    });
    srv.route("GET", "/fapi/v1/openOrders", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      srv.record("/fapi/v1/openOrders", std::string(r.query));
      const std::lock_guard<std::mutex> lock(trades_mu);
      return net::HttpServerResponse::json(200, open_orders);
    });
    srv.route("GET", "/fapi/v1/userTrades", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      srv.record("userTrades", std::string(r.query));
      ++user_trades_queries;
      if (user_trades_failures.load() > 0) {
        --user_trades_failures;
        return net::HttpServerResponse::text(503, "Service Unavailable");
      }
      const std::lock_guard<std::mutex> lock(trades_mu);
      return net::HttpServerResponse::json(200, user_trades);
    });
    srv.route("GET", "/fapi/v1/order", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      srv.record("order", std::string(r.query));
      const std::lock_guard<std::mutex> lock(trades_mu);
      if (order_reply.empty())
        return net::HttpServerResponse::json(400,
                                             R"({"code":-2013,"msg":"Order does not exist."})");
      return net::HttpServerResponse::json(200, order_reply);
    });
    srv.route("GET", "/fapi/v1/income", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      srv.record("income", std::string(r.query));
      if (income_failures.load() > 0) {
        --income_failures;
        ++income_queries;
        return net::HttpServerResponse::text(503, "Service Unavailable");
      }
      std::string body;
      {
        const std::lock_guard<std::mutex> lock(trades_mu);
        body = income;
      }
      ++income_queries;
      return net::HttpServerResponse::json(200, body);
    });
    srv.route("GET", "/fapi/v3/positionRisk", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      ++reconcile_requests;
      return net::HttpServerResponse::json(
          200,
          R"([{"symbol":"BTCUSDT","positionSide":"BOTH","positionAmt":"0.002","entryPrice":"70000.0","markPrice":"70000.1","marginAsset":"USDT"},{"symbol":"ETHUSDT","positionSide":"BOTH","positionAmt":"1.0","entryPrice":"2400.0"}])");
    });
    srv.route("POST", "/fapi/v1/listenKey", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey) ++unsigned_requests;
      ++listen_keys;
      if (listen_key_failures.load() > 0) {
        --listen_key_failures;
        return net::HttpServerResponse::text(503, "Service Unavailable");
      }
      return net::HttpServerResponse::json(200, R"({"listenKey":"lk-test-0001"})");
    });
    srv.route("POST", "/fapi/v1/countdownCancelAll", [this](const net::HttpRequest& r) {
      if (r.header("X-MBX-APIKEY") != kKey || !signed_ok(r.query)) ++unsigned_requests;
      srv.record("countdown", std::string(r.query));
      const bool stop = r.query.find("countdownTime=0&") != std::string_view::npos;
      if (countdown_fails.load() ||
          (countdown_fails_eth.load() && r.query.find("symbol=ETHUSDT") != std::string_view::npos))
        return net::HttpServerResponse::json(
            400, R"({"code":-1130,"msg":"Data sent for parameter 'countdownTime' is not valid."})");
      if (stop) {
        ++countdown_stops;
      } else {
        ++countdowns;
      }
      return net::HttpServerResponse::json(200, R"({"symbol":"BTCUSDT","countdownTime":"3000"})");
    });
    srv.route("DELETE", "/fapi/v1/allOpenOrders", [this](const net::HttpRequest& r) {
      ++cancel_all_requests;
      if (cancel_all_limited.load() > 0) {
        --cancel_all_limited;
        auto limited = net::HttpServerResponse::json(
            429,
            R"({"code":-1003,"msg":"Too many requests; please use the websocket for live updates."})");
        limited.headers.emplace_back("Retry-After", "1");
        return limited;
      }
      if (r.header("X-MBX-APIKEY") == kKey && signed_ok(r.query) &&
          r.query.find("symbol=BTCUSDT") != std::string_view::npos)
        ++cancel_all_ok;
      return net::HttpServerResponse::json(
          200, R"({"code":200,"msg":"The operation of cancel all open order is done."})");
    });
    srv.on_ws_open("/public/stream", [](net::WsSession& s) {
      s.send_text(depth_frame(90, 95, 80, "69990.00"));     // u < lastUpdateId: dropped
      s.send_text(depth_frame(97, 104, 95, "69999.00"));    // brackets 100
      s.send_text(depth_frame(110, 112, 104, "70000.00"));  // pu == previous u
    });
    srv.on_ws_open("/market/stream", [this](net::WsSession& s) {
      s.send_text(
          R"({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1789469122045,"a":309896910,"s":"BTCUSDT","p":"70000.10","q":"0.010","nq":"0.010","f":1,"l":2,"T":1789469121938,"m":true,"st":1}})");
      if (mark_price.load()) s.send_text(fastmm::test::fixture("binance_usdm/mark_price.json"));
    });
    srv.on_ws_text("/ws-fapi/v1", [](net::WsSession& s, std::string_view t) {
      const std::string method = json_str(t, "method");
      const std::string id = json_str(t, "id");
      if (method == "session.logon") {
        // Verify the Ed25519 signature over the sorted params before accepting the session.
        const std::string payload = "apiKey=" + json_str(t, "apiKey") +
                                    "&recvWindow=" + json_int(t, "recvWindow") +
                                    "&timestamp=" + json_int(t, "timestamp");
        if (ed_public_key().verify_base64(payload, json_str(t, "signature"))) {
          s.send_text(ws_result(
              id,
              R"({"apiKey":"fake-key","authorizedSince":1,"connectedSince":1,"returnRateLimits":true,"serverTime":1})"));
        } else {
          s.send_text(
              R"({"id":")" + id +
              R"(","status":400,"error":{"code":-1022,"msg":"Signature for this request is not valid."}})");
        }
      } else if (method == "order.place") {
        s.send_text(ws_result(
            id,
            R"({"orderId":4293153,"symbol":"BTCUSDT","status":"NEW","clientOrderId":")" +
                json_str(t, "newClientOrderId") +
                R"(","price":"70000.00","origQty":"0.001","executedQty":"0","timeInForce":"GTX","type":"LIMIT","side":"BUY"})"));
      } else if (method == "order.modify") {
        s.send_text(ws_result(
            id,
            R"({"orderId":4293153,"symbol":"BTCUSDT","status":"PARTIALLY_FILLED","clientOrderId":"fm000100000001","price":"70001.00","origQty":")" +
                json_str(t, "quantity") + R"(","executedQty":"0.0004"})"));
      } else if (method == "order.cancel") {
        s.send_text(ws_result(
            id,
            R"({"orderId":4293153,"symbol":"BTCUSDT","status":"CANCELED","clientOrderId":"fm000100000001","origQty":"0.0014","executedQty":"0.0007"})"));
      }
    });
    srv.start();
  }

  [[nodiscard]] BinanceUsdmVenueConfig config(bool dry_run) const {
    BinanceUsdmVenueConfig c;
    c.name = "fake-usdm";
    c.ws_url = srv.ws_base();
    c.ws_api_url = srv.ws_base() + "/ws-fapi/v1";
    c.rest_url = srv.http_base();
    c.credentials.api_key = kKey;
    c.credentials.secret.value = kSecret;
    c.dry_run = dry_run;
    c.min_snapshot_interval_ns = 0;
    c.http_timeout_ms = 3000;
    c.position_settle_ms = 20;
    return c;
  }
};

ClientOrderId cid(const char* s) {
  return decode_cl_ord_id(s).value();
}

std::size_t live_states(const Collected& c) {
  std::size_t n = 0;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) == EventType::ConnectionState &&
        RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Live)
      ++n;
  }
  return n;
}

std::size_t reconcile_ends(const Collected& c) {
  std::size_t n = 0;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) == EventType::Reconcile &&
        RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::End)
      ++n;
  }
  return n;
}

void idle(net::Reactor& reactor, int ms) {
  static_cast<void>(pump_until(reactor, [] { return false; }, ms));
}

}  // namespace

TEST_CASE("binance_usdm.venue: scripted fake exchange end to end") {
  Harness h;
  InstrumentTable instruments;
  Instrument inst = make_instrument("BTCUSDT", 0, "BTC", "USDT");
  inst.tick = Price::from_decimal("1").value();  // wrong on purpose: exchangeInfo overrides it
  REQUIRE(instruments.add(inst));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    BinanceUsdmVenue venue(VenueId{0}, h.config(false));
    REQUIRE(venue.load_reference_data(instruments));
    const Instrument& loaded = instruments.get(InstrumentId{0});
    CHECK(loaded.tick == Price::from_decimal("0.10").value());
    CHECK(loaded.lot == Qty::from_decimal("0.0001").value());
    CHECK(loaded.min_notional == Notional::from_int(50));
    CHECK(loaded.asset_class == AssetClass::Perpetual);
    CHECK((loaded.flags & Instrument::kReduceOnlySupported) != 0);
    REQUIRE(h.srv.frames("/fapi/v1/symbolConfig").size() == 1);
    CHECK(h.srv.frames("/fapi/v1/symbolConfig")[0].find("symbol=BTCUSDT") != std::string::npos);
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);

    Collected mdc;
    Collected oc;
    // Market data synced, order and user channels live, first reconciliation done.
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      oc.take(orders);
      return venue.md_feed()->synced_count() == 1 && mdc.count(EventType::BookDelta) >= 2 &&
             mdc.count(EventType::Trade) == 1 && live_states(oc) >= 2 && reconcile_ends(oc) == 1;
    }));
    CHECK(h.listen_keys.load() == 1);
    CHECK(h.unsigned_requests.load() == 0);
    REQUIRE(h.srv.frames("depth").size() == 1);
    CHECK(h.srv.frames("depth")[0] == "symbol=BTCUSDT&limit=1000");
    CHECK(mdc.count(EventType::BookSnapshot) == 1);
    CHECK(mdc.count(EventType::BookDelta) == 2);
    CHECK(mdc.last<BookDeltaMsg>(EventType::BookDelta)->prev_update_id == 104);
    CHECK(mdc.last<TradeMsg>(EventType::Trade)->aggressor == Side::Sell);
    const auto* pos = oc.first_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
      return m.kind == ReconcileMsg::Kind::Position;
    });
    REQUIRE(pos != nullptr);  // ETHUSDT is not configured: one Position message
    CHECK(pos->hdr.instrument == InstrumentId{0});
    CHECK(pos->position_qty == Qty::from_decimal("0.002").value());
    CHECK(pos->avg_px == Price::from_decimal("70000").value());
    CHECK(oc.count(EventType::Reconcile) == 3);

    // pu gap -> Resyncing and a new snapshot, taken inside the update that revealed the gap (which
    // is kept: it applies on the snapshot).
    h.depth_last_update_id = 122;
    h.srv.send_to("/public/stream", depth_frame(120, 125, 118, "70000.00"));
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      return h.depth_requests.load() == 2 && venue.md_feed()->synced_count() == 1;
    }));
    const auto* resync = mdc.first_if<ConnectionStateMsg>(
        EventType::ConnectionState,
        [](const ConnectionStateMsg& m) { return m.state == ConnState::Resyncing; });
    REQUIRE(resync != nullptr);
    CHECK(resync->reason_code == static_cast<std::int32_t>(SyncReason::SequenceGap));

    // order.place: GTX post-only; ack from the response, NEW and TRADE from the user stream.
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    n.cl_ord_id = cid("fm000100000001");
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("70000").value();
    n.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 1;
    }));
    h.srv.send_to(kPrivatePath, order_update("fm000100000001", "NEW", "0.001", "0", "0", 0));
    h.srv.send_to(kPrivatePath,
                  order_update("fm000100000001", "TRADE", "0.001", "0.0004", "0.0004", 777));
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 2 && oc.count(EventType::OrderFill) == 1;
    }));
    std::string place;
    for (const auto& f : h.srv.frames("/ws-fapi/v1")) {
      if (f.find("\"order.place\"") != std::string::npos) place = f;
    }
    REQUIRE_FALSE(place.empty());
    CHECK(place.find(R"("timeInForce":"GTX")") != std::string::npos);
    CHECK(place.find(R"("type":"LIMIT")") != std::string::npos);
    CHECK(place.find(R"("signature":")") != std::string::npos);
    CHECK(oc.last<OrderAckMsg>(EventType::OrderAck)->venue_order_id.view() == "4293153");
    const auto* fill1 = oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(fill1->qty == Qty::from_decimal("0.0004").value());
    CHECK(fill1->leaves_qty == Qty::from_decimal("0.0006").value());
    CHECK(fill1->fee_asset == FeeAsset::Quote);

    // order.modify to a new engine id: the venue keeps fm000100000001, takes the total quantity,
    // and later events for it are reported for fm000100000002 counted from the modify.
    OutReplaceMsg r{};
    init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
    r.cl_ord_id = cid("fm000100000002");
    r.orig_cl_ord_id = n.cl_ord_id;
    r.venue_order_id.assign("4293153");
    r.price = Price::from_decimal("70001").value();
    r.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&r, r.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.first_if<OrderAckMsg>(EventType::OrderAck, [&](const OrderAckMsg& m) {
        return m.cl_ord_id == r.cl_ord_id;
      }) != nullptr;
    }));
    std::string modify;
    for (const auto& f : h.srv.frames("/ws-fapi/v1")) {
      if (f.find("\"order.modify\"") != std::string::npos) modify = f;
    }
    REQUIRE_FALSE(modify.empty());
    CHECK(modify.find(R"("orderId":4293153,"price":"70001","quantity":"0.0014")") !=
          std::string::npos);
    h.srv.send_to(kPrivatePath,
                  order_update("fm000100000001", "AMENDMENT", "0.0014", "0", "0.0004", 0));
    h.srv.send_to(kPrivatePath,
                  order_update("fm000100000001", "TRADE", "0.0014", "0.0003", "0.0007", 778));
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderFill) == 2;
    }));
    const auto* fill2 = oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(fill2->cl_ord_id == r.cl_ord_id);
    CHECK(fill2->qty == Qty::from_decimal("0.0003").value());
    CHECK(fill2->cum_qty == Qty::from_decimal("0.0003").value());
    CHECK(fill2->leaves_qty == Qty::from_decimal("0.0007").value());

    // ACCOUNT_UPDATE: 0.002 reconciled + 0.0007 filled agrees; a different amount corrects the
    // engine once no fill has arrived for position_settle_ms.
    h.srv.send_to(kPrivatePath, account_update("0.0027"));
    idle(reactor, 100);
    venue.on_timer(net::Reactor::now_ns());
    oc.take(orders);
    CHECK(oc.count(EventType::PositionUpdate) == 0);
    h.srv.send_to(kPrivatePath, account_update("0.0020"));
    idle(reactor, 100);
    venue.on_timer(net::Reactor::now_ns());
    oc.take(orders);
    REQUIRE(oc.count(EventType::PositionUpdate) == 1);
    CHECK(oc.last<PositionUpdateMsg>(EventType::PositionUpdate)->qty ==
          Qty::from_decimal("0.002").value());

    // order.cancel of the modified order: by order id, fills counted from the modify.
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
    c.cl_ord_id = r.cl_ord_id;
    c.venue_order_id.assign("4293153");
    REQUIRE(outbound.try_push(&c, c.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderCancelAck) >= 1;
    }));
    const auto* cx = oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck);
    CHECK(cx->cl_ord_id == r.cl_ord_id);
    CHECK(cx->cum_qty == Qty::from_decimal("0.0003").value());

    // Reconciliation on request: Begin carries the highest id sent.
    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return reconcile_ends(oc) == 2;
    }));
    const ReconcileMsg* begin2 = nullptr;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) == EventType::Reconcile &&
          RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::Begin)
        begin2 = &RecordingSink::as<ReconcileMsg>(m);
    }
    REQUIRE(begin2 != nullptr);
    CHECK(begin2->sent_watermark == r.cl_ord_id);

    // listenKeyExpired: a new key, a new user connection and another reconciliation.
    h.srv.send_to(kPrivatePath, fastmm::test::fixture("binance_usdm/listen_key_expired.json"));
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return h.listen_keys.load() == 2 && h.srv.open_count(kPrivatePath) == 2 &&
             reconcile_ends(oc) == 3;
    }));

    // Order channel lost: everything is cancelled over REST.
    h.srv.close_sessions("/ws-fapi/v1");
    REQUIRE(pump_until(reactor, [&] { return h.cancel_all_ok.load() >= 1; }));

    // Kill switch from this thread over an independent connection.
    const int before = h.cancel_all_ok.load();
    CHECK(venue.cancel_all());
    CHECK(h.cancel_all_ok.load() == before + 1);
    CHECK(h.unsigned_requests.load() == 0);
    CHECK_FALSE(venue.fatal());
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}

TEST_CASE("binance_usdm.venue: dry run opens market data only, hedge mode refuses to start") {
  {
    Harness h;
    InstrumentTable instruments;
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
    RecordingSink md(8U << 20);
    RecordingSink orders(1U << 20, SinkPolicy::Spin);
    MsgRing outbound(1U << 16);
    net::Reactor reactor;
    SymbolTable symbols;
    BinanceUsdmVenue venue(VenueId{0}, h.config(true));
    REQUIRE(venue.load_reference_data(instruments));
    CHECK(h.srv.frames("/fapi/v1/symbolConfig").empty());  // no signed requests in a dry run
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);
    REQUIRE(pump_until(reactor, [&] { return venue.md_feed()->synced_count() == 1; }));
    CHECK(h.srv.open_count("/ws-fapi/v1") == 0);
    CHECK(h.listen_keys.load() == 0);
    CHECK_FALSE(venue.caps().user_stream);
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    n.cl_ord_id = cid("fm000100000009");
    n.price = Price::from_decimal("70000").value();
    n.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    Collected oc;
    oc.take(orders);
    REQUIRE(oc.count(EventType::OrderReject) == 1);
    CHECK(oc.last<OrderRejectMsg>(EventType::OrderReject)->reason == RejectReason::VenueKilled);
    CHECK(venue.cancel_all());
    CHECK(h.cancel_all_ok.load() == 0);
    venue.disconnect();
    reactor.run_once(0);
    h.srv.stop();
  }
  {
    Harness h(/*hedge=*/true);
    InstrumentTable instruments;
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
    BinanceUsdmVenue venue(VenueId{0}, h.config(false));
    const auto loaded = venue.load_reference_data(instruments);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().find("hedge mode") != std::string::npos);
    h.srv.stop();
  }
}

TEST_CASE("binance_usdm.venue: an exchangeInfo larger than the streaming client's buffer loads") {
  // Production's exchangeInfo (no symbol filter) was 1.1 MB in 2026-09, past the 1 MiB default.
  Harness h;
  REQUIRE(h.exchange_info.front() == '{');
  h.exchange_info.insert(1, "\"pad\":\"" + std::string(std::size_t{2} << 20, 'x') + "\",");
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  BinanceUsdmVenue venue(VenueId{0}, h.config(true));
  const auto loaded = venue.load_reference_data(instruments);
  CHECK_MESSAGE(loaded.has_value(), (loaded ? std::string{} : loaded.error()));
  h.srv.stop();
}

TEST_CASE("binance_usdm.config: section mapping and factory registration") {
  VenueSection s;
  s.name = "usdm";
  s.kind = "binance_usdm";
  s.ws_url = "wss://demo-fstream.binance.com";
  s.ws_api_url = "wss://testnet.binancefuture.com/ws-fapi/v1";
  s.rest_url = "https://demo-fapi.binance.com";
  s.api_key = "k";
  s.api_secret = "s";
  s.supports_replace = true;
  s.recv_window_ms = 4000;
  s.extra["depth_limit"] = "300";
  s.extra["order_api"] = "rest";
  s.extra["position_from_account_update"] = "false";
  s.extra["stale_ms"] = "10000";
  const BinanceUsdmVenueConfig c = make_binance_usdm_config(s, false);
  CHECK(c.depth_limit == 500);  // the next valid limit
  CHECK_FALSE(c.ws_order_api);
  CHECK_FALSE(c.position_from_account_update);
  CHECK(c.stale_ms == 10000);
  CHECK(c.recv_window_ms == 4000);
  CHECK(c.supports_replace);
  CHECK(c.ws_private_url.empty());
  register_builtin_venues();
  REQUIRE(VenueRegistry::instance().find("binance_usdm") != nullptr);
  const auto v = make_venue(VenueId{3}, s, VenueFactoryOptions{true, {}});
  REQUIRE(v != nullptr);
  CHECK(v->name() == "usdm");
  CHECK(v->caps().supports_replace);
  CHECK_FALSE(v->caps().user_stream);  // dry run
  VenueSection ed = s;
  ed.extra["key_type"] = "ed25519";
  // Ed25519 needs a parsable private key (session.logon on the WS API order connection).
  CHECK_THROWS_AS(static_cast<void>(make_binance_usdm_config(ed, false)), std::invalid_argument);
  ed.extra["private_key_file"] =
      (fastmm::test::fixtures_dir() / "binance" / "ed25519-test-private.pem").string();
  CHECK(make_binance_usdm_config(ed, false).credentials.type == binance::KeyType::Ed25519);
  VenueSection no_url = s;
  no_url.rest_url.clear();
  CHECK_THROWS_AS(static_cast<void>(make_binance_usdm_config(no_url, false)),
                  std::invalid_argument);
}

TEST_CASE("binance_usdm.venue: Ed25519 key logs on to the WS API and sends unsigned orders") {
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    BinanceUsdmVenueConfig cfg = h.config(false);
    cfg.credentials.secret.value.clear();
    cfg.credentials.type = binance::KeyType::Ed25519;
    cfg.credentials.private_key_pem.value =
        fastmm::test::fixture("binance/ed25519-test-private.pem");
    BinanceUsdmVenue venue(VenueId{0}, std::move(cfg));
    REQUIRE(venue.load_reference_data(instruments));  // Ed25519-signed REST
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);
    Collected oc;
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return live_states(oc) >= 2 && reconcile_ends(oc) == 1;
    }));
    CHECK(h.unsigned_requests.load() == 0);
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    n.cl_ord_id = cid("fm000100000001");
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("70000").value();
    n.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 1;
    }));
    const auto frames = h.srv.frames("/ws-fapi/v1");
    REQUIRE(frames.size() >= 2);
    CHECK(frames[0].find("\"session.logon\"") != std::string::npos);
    std::string place;
    for (const auto& f : frames) {
      if (f.find("\"order.place\"") != std::string::npos) place = f;
    }
    REQUIRE_FALSE(place.empty());
    CHECK(place.find("apiKey") == std::string::npos);
    CHECK(place.find("signature") == std::string::npos);
    CHECK(place.find("\"timestamp\":") != std::string::npos);
    CHECK(venue.cancel_all());  // blocking REST, Ed25519 signature
    CHECK(h.cancel_all_ok.load() == 1);
    CHECK_FALSE(venue.fatal());
    venue.disconnect();
    reactor.run_once(0);
  }
}

// Fixture for the dead-man's-switch cases: everything a venue needs, nothing else.
namespace {
struct DmsFixture {
  Harness h;
  InstrumentTable instruments;
  RecordingSink md{8U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<BinanceUsdmVenue> venue;
  Collected oc;

  explicit DmsFixture(std::int64_t window_ms,
                      const std::function<void(BinanceUsdmVenue&)>& before_connect = {},
                      bool with_eth = false,
                      bool order_limits = true)
      : h(false, order_limits) {
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
    if (with_eth) REQUIRE(instruments.add(make_instrument("ETHUSDT", 0, "ETH", "USDT")));
    BinanceUsdmVenueConfig cfg = h.config(false);
    cfg.dead_mans_switch_ms = window_ms;
    venue = std::make_unique<BinanceUsdmVenue>(VenueId{0}, std::move(cfg));
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}, InstrumentId{1}};
    venue->subscribe(std::span<const InstrumentId>(ids, with_eth ? 2 : 1));
    if (before_connect) before_connect(*venue);
    venue->connect(reactor);
  }
  ~DmsFixture() {
    venue->disconnect();
    reactor.run_once(0);
  }
  DmsFixture(const DmsFixture&) = delete;
  DmsFixture& operator=(const DmsFixture&) = delete;

  template <class Pred>
  bool pump(Pred pred, int timeout_ms = 15000) {
    return pump_until(
        reactor,
        [&] {
          oc.take(orders);
          return pred();
        },
        timeout_ms);
  }
};
}  // namespace

TEST_CASE("binance_usdm.venue: countdownCancelAll is armed, refreshed and stopped on shutdown") {
  // The only thing that clears quotes after a SIGKILL, an OOM kill or a dead host is the venue's
  // own timer. It is armed per symbol from the housekeeping tick, refreshed at a third of the
  // window, and stopped when the session shuts down cleanly.
  DmsFixture f(1500);  // the refresh floor is 500 ms, so a refresh lands well inside the test
  REQUIRE(f.pump([&] { return f.h.countdowns.load() >= 2; }));  // armed once, then refreshed
  const auto frames = f.h.srv.frames("countdown");
  REQUIRE_FALSE(frames.empty());
  CHECK(frames[0].find("countdownTime=1500&") != std::string::npos);
  CHECK(frames[0].find("symbol=BTCUSDT") != std::string::npos);
  CHECK(f.h.unsigned_requests.load() == 0);
  CHECK(f.h.countdown_stops.load() == 0);

  // A clean shutdown stops the timer: the session cancels its own orders, so leaving a countdown
  // running against an account nobody is quoting is only a trap for the next run.
  f.venue->disconnect();
  REQUIRE(pump_until(f.reactor, [&] { return f.h.countdown_stops.load() == 1; }));
  CHECK(f.h.srv.frames("countdown").back().find("countdownTime=0&") != std::string::npos);
}

TEST_CASE("binance_usdm.venue: a lapsed dead man's switch kills the venue instead of requoting") {
  // The dangerous case is not the process dying, it is the process living while the switch
  // fails. The venue cancels everything, and a quoter that only notices empty slots puts the
  // quotes straight back, racing a kill switch it cannot see. The connector has to stop first.
  DmsFixture f(1200);
  REQUIRE(f.pump([&] { return f.h.countdowns.load() >= 1; }));
  CHECK_FALSE(f.venue->fatal());

  // Every refresh from here is refused, so after one window the venue has cancelled our orders.
  f.h.countdown_fails.store(true);
  REQUIRE(f.pump([&] { return f.venue->fatal(); }, 8000));
  const auto* kill = f.oc.first_if<ControlMsg>(EventType::Control, [](const ControlMsg& m) {
    return m.command == ControlCommand::TripVenueKill;
  });
  REQUIRE(kill != nullptr);
  CHECK(static_cast<KillReason>(kill->arg) == KillReason::DeadMansSwitchLost);

  // And the connector refuses to put a quote back.
  const std::size_t rejects = f.oc.count(EventType::OrderReject);
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.cl_ord_id = cid("fm000100000009");
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("70000").value();
  n.qty = Qty::from_decimal("0.001").value();
  REQUIRE(f.outbound.try_push(&n, n.hdr.len));
  f.venue->on_wake();
  REQUIRE(f.pump([&] { return f.oc.count(EventType::OrderReject) > rejects; }));
  CHECK(f.oc.last<OrderRejectMsg>(EventType::OrderReject)->reason == RejectReason::VenueKilled);
}

TEST_CASE("binance_usdm.venue: after a lapse the countdown is left to run out, not re-armed") {
  // The lapse killed the venue; the venue's own timer is what clears the account whatever the
  // local cancels manage. A refresh sent now would push that timer out again, so none goes out
  // until the session reconnects. (It used to re-arm on the very tick that reported the lapse.)
  DmsFixture f(1200);  // refreshed every 500 ms
  REQUIRE(f.pump([&] { return f.h.countdowns.load() >= 1; }));
  f.h.countdown_fails.store(true);
  REQUIRE(f.pump([&] { return f.venue->fatal(); }, 8000));
  f.h.countdown_fails.store(false);
  const std::size_t sent = f.h.srv.frames("countdown").size();
  idle(f.reactor, 2500);  // five refresh periods
  CHECK(f.h.srv.frames("countdown").size() == sent);
  // The shutdown still stops it: the last refresh may have reached the venue.
  f.venue->disconnect();
  CHECK(f.h.countdown_stops.load() == 1);
}

TEST_CASE("binance_usdm.venue: one symbol's refused refresh lapses the switch") {
  // The countdown is per symbol. When ETHUSDT's refresh is refused its countdown still runs from
  // the last one that was confirmed, and the venue cancels ETHUSDT's orders when it ends, however
  // well BTCUSDT's refreshes go. It used to count as armed if any one symbol answered.
  DmsFixture f(1200, {}, /*with_eth=*/true);
  REQUIRE(f.pump([&] { return f.h.countdowns.load() >= 2; }));  // both symbols armed
  CHECK_FALSE(f.venue->fatal());
  f.h.countdown_fails_eth.store(true);
  REQUIRE(f.pump([&] { return f.venue->fatal(); }, 8000));
  const auto* kill = f.oc.first_if<ControlMsg>(EventType::Control, [](const ControlMsg& m) {
    return m.command == ControlCommand::TripVenueKill;
  });
  REQUIRE(kill != nullptr);
  CHECK(static_cast<KillReason>(kill->arg) == KillReason::DeadMansSwitchLost);
  CHECK(f.h.countdowns.load() >= 3);  // BTCUSDT kept being confirmed meanwhile
}

TEST_CASE("binance_usdm.venue: the kill-switch cancel-all waits out a rate limit") {
  // A 429 on the kill path used to be final: the call returned false and the orders stayed on
  // the book. It is retried after the venue's Retry-After, within a deadline.
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  SymbolTable symbols;
  BinanceUsdmVenue venue(VenueId{0}, h.config(false));
  REQUIRE(venue.load_reference_data(instruments));
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  h.cancel_all_limited = 1;
  const std::int64_t t0 = net::Reactor::now_ns();
  CHECK(venue.cancel_all());
  const std::int64_t took_ms = (net::Reactor::now_ns() - t0) / 1'000'000;
  CHECK(h.cancel_all_requests.load() == 2);
  CHECK(h.cancel_all_ok.load() == 1);
  CHECK(took_ms >= 900);  // Retry-After: 1

  // A limit that does not lift is reported, not waited on forever.
  h.cancel_all_limited = 100;
  CHECK_FALSE(venue.cancel_all());
  CHECK(h.cancel_all_requests.load() == 6);  // the first and three retries
  h.srv.stop();
}

TEST_CASE("binance_usdm.venue: offline reference data still runs the account checks") {
  // allow_offline_reference_data keeps the configured tick and lot when exchangeInfo cannot be
  // had. It used to skip everything after it too, including the hedge-mode refusal.
  Harness h(/*hedge=*/true);
  h.exchange_info_down = true;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  BinanceUsdmVenueConfig cfg = h.config(false);
  cfg.allow_offline_reference_data = true;
  BinanceUsdmVenue venue(VenueId{0}, std::move(cfg));
  const auto loaded = venue.load_reference_data(instruments);
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error().find("hedge mode") != std::string::npos);
  h.srv.stop();

  // One-way mode: it loads, on the configured values.
  Harness h2;
  h2.exchange_info_down = true;
  InstrumentTable instruments2;
  REQUIRE(instruments2.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  BinanceUsdmVenueConfig cfg2 = h2.config(false);
  cfg2.allow_offline_reference_data = true;
  BinanceUsdmVenue venue2(VenueId{0}, std::move(cfg2));
  REQUIRE(venue2.load_reference_data(instruments2));
  CHECK(h2.srv.frames("/fapi/v1/positionSide/dual").size() == 1);
  h2.srv.stop();
}

TEST_CASE("binance_usdm.venue: markPrice on the market connection with fundingInfo's interval") {
  Harness h;
  h.mark_price = true;
  // BTCUSDT's interval adjusted to 4 h; ETHUSDT listed at 8 h.
  const std::string btc8 =
      R"("symbol":"BTCUSDT","adjustedFundingRateCap":"0.00300","adjustedFundingRateFloor":"-0.00300","fundingIntervalHours":8)";
  const std::size_t at = h.funding_info.find(btc8);
  REQUIRE(at != std::string::npos);
  h.funding_info.replace(at + btc8.size() - 1, 1, "4");
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  REQUIRE(instruments.add(make_instrument("ETHUSDT", 0, "ETH", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  BinanceUsdmVenue venue(VenueId{0}, h.config(true));
  REQUIRE(venue.load_reference_data(instruments));
  CHECK(h.srv.frames("fundingInfo").size() == 1);
  CHECK(instruments.get(InstrumentId{0}).asset_class == AssetClass::Perpetual);
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}, InstrumentId{1}};
  venue.subscribe(ids);
  CHECK(venue.md_feed()->funding_interval(InstrumentId{0}) == hours(4));
  CHECK(venue.md_feed()->funding_interval(InstrumentId{1}) == hours(8));
  venue.connect(reactor);
  Collected mc;
  REQUIRE(pump_until(reactor, [&] {
    mc.take(md);
    return mc.count(EventType::PerpState) == 1 && mc.count(EventType::Trade) == 1 &&
           h.srv.frames("upgrade:/public/stream").size() == 1;
  }));
  // The mark price streams ride the /market connection with aggTrade, not /public.
  const auto market = h.srv.frames("upgrade:/market/stream");
  REQUIRE(market.size() == 1);
  CHECK(market[0] ==
        "streams=btcusdt@aggTrade/ethusdt@aggTrade/btcusdt@markPrice@1s/ethusdt@markPrice@1s");
  const auto pub = h.srv.frames("upgrade:/public/stream");
  REQUIRE(pub.size() == 1);
  CHECK(pub[0].find("markPrice") == std::string::npos);
  const auto* m = mc.last<PerpStateMsg>(EventType::PerpState);
  REQUIRE(m != nullptr);
  CHECK(m->hdr.instrument == InstrumentId{0});
  CHECK(m->fields == (PerpStateMsg::kMark | PerpStateMsg::kIndex | PerpStateMsg::kFunding));
  CHECK(m->funding_interval == hours(4));
  venue.disconnect();
  reactor.run_once(0);
  h.srv.stop();
}

TEST_CASE("binance_usdm.venue: without fundingInfo the interval is 8 h, spot gets no markPrice") {
  // exchangeInfo and fundingInfo both down, offline reference data allowed: the instrument keeps
  // its configured asset class (spot here), which subscribes no mark price stream.
  Harness h;
  h.exchange_info_down = true;
  h.funding_info.clear();
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  Instrument eth = make_instrument("ETHUSDT", 0, "ETH", "USDT");
  eth.asset_class = AssetClass::Perpetual;
  REQUIRE(instruments.add(eth));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  SymbolTable symbols;
  BinanceUsdmVenueConfig cfg = h.config(true);
  cfg.allow_offline_reference_data = true;
  BinanceUsdmVenue venue(VenueId{0}, std::move(cfg));
  REQUIRE(venue.load_reference_data(instruments));
  CHECK(h.srv.frames("fundingInfo").size() == 1);
  CHECK(instruments.get(InstrumentId{0}).asset_class == AssetClass::Spot);
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}, InstrumentId{1}};
  venue.subscribe(ids);
  CHECK(venue.md_feed()->market_target() ==
        "/market/stream?streams=btcusdt@aggTrade/ethusdt@aggTrade/ethusdt@markPrice@1s");
  CHECK(venue.md_feed()->funding_interval(InstrumentId{1}) == hours(8));
  h.srv.stop();
}

TEST_CASE("binance_usdm.venue: a quote is charged what the endpoint costs, not one of everything") {
  // Placing and modifying cost 0 IP weight and 1 order; cancelling costs 1 IP weight and no
  // order. Charging 1 IP weight per quote spends half the 2400/minute IP budget on requests that
  // consume none of it, and the connector then refuses to quote long before the venue would.
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2; }));
  const auto used = [&](bool order) {
    const RateBucket* b =
        order ? f.venue->rate_limiter().order_bucket(0) : f.venue->rate_limiter().weight_bucket(0);
    REQUIRE(b != nullptr);
    return b->used;
  };
  const std::uint32_t weight0 = used(false);
  const std::uint32_t orders0 = used(true);

  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.cl_ord_id = cid("fm000100000001");
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("70000").value();
  n.qty = Qty::from_decimal("0.001").value();
  REQUIRE(f.outbound.try_push(&n, n.hdr.len));
  f.venue->on_wake();
  // Read before the response: the venue's own rateLimits[] come back with it and overwrite the
  // local estimate.
  CHECK(used(false) == weight0);     // the place costs no IP weight
  CHECK(used(true) == orders0 + 1);  // ...and one order
  REQUIRE(f.pump([&] { return f.oc.count(EventType::OrderAck) >= 1; }));

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = n.cl_ord_id;
  const std::uint32_t weight_before = used(false);
  const std::uint32_t orders_before = used(true);
  REQUIRE(f.outbound.try_push(&c, c.hdr.len));
  f.venue->on_wake();
  CHECK(used(false) == weight_before + 1);  // the cancel is the one that costs IP weight
  CHECK(used(true) == orders_before);       // ...and adds no order
  // Let the cancel's response land before the fixture tears the connections down.
  REQUIRE(f.pump([&] { return f.oc.count(EventType::OrderCancelAck) >= 1; }));
}

TEST_CASE("binance_usdm.venue: dead_mans_switch_ms = 0 leaves the venue timer alone") {
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2; }));
  idle(f.reactor, 2500);
  CHECK(f.h.countdowns.load() == 0);
  CHECK(f.h.countdown_stops.load() == 0);
}

// ---- execution replay (GET /fapi/v1/userTrades) ------------------------------------------------
namespace {

std::string user_trade(long id, const char* price, const char* qty, const char* commission) {
  return R"({"buyer":true,"commission":")" + std::string(commission) +
         R"(","commissionAsset":"USDT","id":)" + std::to_string(id) +
         R"(,"maker":true,"orderId":4293153,"price":")" + price + R"(","qty":")" + qty +
         R"(","quoteQty":"70.0","realizedPnl":"0","side":"BUY","positionSide":"BOTH","symbol":"BTCUSDT","time":)" +
         std::to_string(wall_now().ns / 1'000'000) + "}";
}

// The n-th (from 0) snapshot Begin in `c`, and its position in c.all.
std::size_t index_of_begin(const Collected& c, std::size_t n) {
  for (std::size_t i = 0; i < c.all.size(); ++i) {
    if (RecordingSink::type_of(c.all[i]) != EventType::Reconcile) continue;
    if (RecordingSink::as<ReconcileMsg>(c.all[i]).kind == ReconcileMsg::Kind::Begin && n-- == 0)
      return i;
  }
  return SIZE_MAX;
}

bool begin_exact(const Collected& c, std::size_t n) {
  const std::size_t i = index_of_begin(c, n);
  REQUIRE(i != SIZE_MAX);
  return (RecordingSink::as<ReconcileMsg>(c.all[i]).flags & ReconcileMsg::kExecutionsExact) != 0;
}

// Position in c.all of the first replayed fill with `exec_id`; SIZE_MAX if absent.
std::size_t index_of_fill(const Collected& c, std::string_view exec_id) {
  for (std::size_t i = 0; i < c.all.size(); ++i) {
    if (RecordingSink::type_of(c.all[i]) != EventType::OrderFill) continue;
    const auto& f = RecordingSink::as<OrderFillMsg>(c.all[i]);
    if ((f.flags & OrderFillMsg::kReplayed) != 0 && f.exec_id.view() == exec_id) return i;
  }
  return SIZE_MAX;
}

void place_order(DmsFixture& f, const char* id) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.cl_ord_id = cid(id);
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("70000").value();
  n.qty = Qty::from_decimal("0.001").value();
  REQUIRE(f.outbound.try_push(&n, n.hdr.len));
  f.venue->on_wake();
  REQUIRE(f.pump([&] { return f.oc.count(EventType::OrderAck) >= 1; }));
}

}  // namespace

TEST_CASE("binance_usdm.venue: a fill the user stream missed is booked from userTrades") {
  // The order fills completely while the private stream is down: the stream never reports it and
  // the open-order snapshot no longer names the order, so only the trade history can.
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1; }));
  const auto first = f.h.srv.frames("userTrades");
  REQUIRE(first.size() == 1);
  CHECK(first[0].find("symbol=BTCUSDT") != std::string::npos);
  CHECK(first[0].find("startTime=") != std::string::npos);  // from connect(), not 7 days back
  CHECK(first[0].find("fromId=") == std::string::npos);
  place_order(f, "fm000100000001");

  f.h.set_user_trades("[" + user_trade(901, "69999.90", "0.001", "0.02800000") + "]");
  f.h.srv.close_sessions(kPrivatePath);
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2; }));
  const std::size_t at = index_of_fill(f.oc, "901");
  REQUIRE(at != SIZE_MAX);
  CHECK(at < index_of_begin(f.oc, 1));  // booked before the snapshot is read
  const auto& fill = RecordingSink::as<OrderFillMsg>(f.oc.all[at]);
  CHECK(fill.cl_ord_id == cid("fm000100000001"));  // mapped back from orderId
  CHECK(fill.venue_order_id.view() == "4293153");
  CHECK(fill.side == Side::Buy);
  CHECK(fill.price == Price::from_decimal("69999.9").value());
  CHECK(fill.qty == Qty::from_decimal("0.001").value());
  CHECK(fill.fee == Notional::from_decimal("0.028").value());
  CHECK(fill.fee_asset == FeeAsset::Quote);
  CHECK(fill.liquidity == Liquidity::Maker);
  CHECK(std::abs(fill.hdr.exch_ts.ns - wall_now().ns) < 60'000'000'000);  // the trade's "time"
  CHECK(begin_exact(f.oc, 1));

  // The next replay carries on after the trade it forwarded.
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 3; }));
  const auto later = f.h.srv.frames("userTrades");
  REQUIRE(later.size() >= 3);
  CHECK(later.back().find("fromId=902") != std::string::npos);
  CHECK(later.back().find("startTime=") == std::string::npos);
  CHECK(f.h.unsigned_requests.load() == 0);
}

TEST_CASE("binance_usdm.venue: a snapshot is marked exact only after a complete replay") {
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1; }));
  CHECK(begin_exact(f.oc, 0));

  // A query that fails: the snapshot still goes out, but does not pass for exact.
  f.h.user_trades_failures.store(1);
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2; }));
  CHECK_FALSE(begin_exact(f.oc, 1));

  // A full page (limit 1000) is not proof that nothing is behind it.
  std::string page = "[";
  for (int i = 0; i < 1000; ++i) {
    if (i > 0) page += ",";
    page += user_trade(2000 + i, "70000.00", "0.001", "0.028");
  }
  page += "]";
  f.h.set_user_trades(page);
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 3; }));
  CHECK(index_of_fill(f.oc, "2999") < index_of_begin(f.oc, 2));
  CHECK_FALSE(begin_exact(f.oc, 2));

  // Complete again: exact again.
  f.h.set_user_trades("[]");
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 4; }));
  CHECK(begin_exact(f.oc, 3));
}

TEST_CASE("binance_usdm.venue: a failed userTrades query is retried from the housekeeping timer") {
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1; }));
  place_order(f, "fm000100000001");
  f.h.set_user_trades("[" + user_trade(905, "70000.00", "0.0004", "0.0112") + "]");
  f.h.user_trades_failures.store(1);
  const int queries = f.h.user_trades_queries.load();
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2; }));
  CHECK_FALSE(begin_exact(f.oc, 1));
  CHECK(index_of_fill(f.oc, "905") == SIZE_MAX);

  // No reconnect and no new reconciliation: the housekeeping timer asks again on its own.
  REQUIRE(f.pump([&] { return index_of_fill(f.oc, "905") != SIZE_MAX; }, 8000));
  CHECK(f.h.user_trades_queries.load() == queries + 2);
  CHECK(reconcile_ends(f.oc) == 2);  // only the executions are asked again, not the snapshot
  const auto& fill = RecordingSink::as<OrderFillMsg>(f.oc.all[index_of_fill(f.oc, "905")]);
  CHECK(fill.cl_ord_id == cid("fm000100000001"));
  CHECK(fill.fee == Notional::from_decimal("0.0112").value());
  // Published from the housekeeping tick.
  REQUIRE(f.pump([&] { return f.venue->status().executions_fetched >= 1; }));
  const VenueStatus st = f.venue->status();
  CHECK(st.execution_queries >= 3);
  CHECK(st.execution_query_errors == 1);
  CHECK(st.executions_fetched >= 1);
}

TEST_CASE(
    "binance_usdm.venue: a replayed trade of an order this process never saw acked names it") {
  // userTrades names the order by orderId only; one the orderId map does not hold (placed by a
  // session that died before the answer) is asked for with GET /fapi/v1/order before the fill
  // goes out, so the fill names its client order id.
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1; }));
  std::string row = user_trade(906, "70000.00", "0.001", "0.028");
  row.replace(row.find("4293153"), 7, "5550001");
  f.h.set_user_trades("[" + row + "]");
  f.h.set_order_reply(
      R"({"avgPrice":"70000.0","clientOrderId":"fm000500000007","cumQuote":"70.0","executedQty":"0.001","orderId":5550001,"origQty":"0.001","origType":"LIMIT","price":"70000.00","reduceOnly":false,"side":"BUY","positionSide":"BOTH","status":"FILLED","stopPrice":"0","closePosition":false,"symbol":"BTCUSDT","time":1789469199999,"timeInForce":"GTX","type":"LIMIT","updateTime":1789469199999,"workingType":"CONTRACT_PRICE","priceProtect":false,"priceMatch":"NONE","selfTradePreventionMode":"NONE","goodTillDate":0})");
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2; }));
  const std::size_t at = index_of_fill(f.oc, "906");
  REQUIRE(at != SIZE_MAX);
  CHECK(at < index_of_begin(f.oc, 1));  // named, then the snapshot
  const auto& fill = RecordingSink::as<OrderFillMsg>(f.oc.all[at]);
  CHECK(fill.cl_ord_id == cid("fm000500000007"));
  CHECK(fill.venue_order_id.view() == "5550001");
  CHECK(begin_exact(f.oc, 1));
  const auto q = f.h.srv.frames("order");
  REQUIRE(q.size() == 1);
  CHECK(q[0].find("orderId=5550001") != std::string::npos);
  CHECK(q[0].find("symbol=BTCUSDT") != std::string::npos);
  CHECK(f.h.unsigned_requests.load() == 0);
}

// ---- funding (GET /fapi/v1/income?incomeType=FUNDING_FEE) --------------------------------------
namespace {

// One income row, Binance's example shape ("Get Income History") with a FUNDING_FEE on `symbol`.
std::string income_row(const char* symbol, const char* income, long long tran_id, long long time) {
  return std::string(R"({"symbol":")") + symbol + R"(","incomeType":"FUNDING_FEE","income":")" +
         income + R"(","asset":"USDT","info":"FUNDING_FEE","time":)" + std::to_string(time) +
         R"(,"tranId":)" + std::to_string(tran_id) + R"(,"tradeId":""})";
}

// The balance-only ACCOUNT_UPDATE Binance pushes for funding on a crossed position ("User Data
// Streams": reason FUNDING_FEE, the symbol in a.S since 2026-08-07, no position P).
std::string funding_event(long long time) {
  return R"({"e":"ACCOUNT_UPDATE","E":)" + std::to_string(time + 1) + R"(,"T":)" +
         std::to_string(time) +
         R"(,"a":{"m":"FUNDING_FEE","S":"BTCUSDT","B":[{"a":"USDT","wb":"4999.625","cw":"4999.625","bc":"-0.375"}]}})";
}

std::vector<const FundingMsg*> funding_of(const Collected& c) {
  std::vector<const FundingMsg*> out;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) == EventType::Funding)
      out.push_back(&RecordingSink::as<FundingMsg>(m));
  }
  return out;
}

// The connector's start-up: its sweep done and its first income query answered.
void wait_started(DmsFixture& f) {
  REQUIRE(f.pump([&] {
    return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1 && f.h.income_queries.load() >= 1;
  }));
  idle(f.reactor, 50);  // the answer to that query is processed
}

}  // namespace

TEST_CASE("binance_usdm.venue: funding on the user stream is booked from the income history") {
  DmsFixture f(0);
  wait_started(f);
  const auto first = f.h.srv.frames("income");
  REQUIRE(first.size() == 1);
  CHECK(first[0].find("incomeType=FUNDING_FEE") != std::string::npos);
  CHECK(first[0].find("startTime=") != std::string::npos);  // from connect()
  CHECK(first[0].find("symbol=") == std::string::npos);     // every symbol in one query
  CHECK(funding_of(f.oc).empty());

  // The venue pays funding: the history has the payment (and one on a symbol not traded here),
  // and the stream says so without an id. Nothing else asks: the stream event does.
  const long long t = wall_now().ns / 1'000'000;
  f.h.set_income("[" + income_row("BTCUSDT", "-0.37500000", 9689322392, t) + "," +
                 income_row("ETHUSDT", "-1.2", 9689322393, t) + "]");
  f.h.srv.send_to(kPrivatePath, funding_event(t));
  REQUIRE(f.pump([&] { return funding_of(f.oc).size() == 1; }, 8000));
  CHECK(f.h.income_queries.load() == 2);
  const FundingMsg& m = *funding_of(f.oc)[0];
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == VenueId{0});
  CHECK(m.amount == Notional::from_decimal("-0.375").value());
  CHECK(m.asset.view() == "USDT");
  CHECK(m.funding_id.view() == "9689322392");
  CHECK(m.hdr.exch_ts == Timestamp{t * 1'000'000});
  CHECK((m.flags & FundingMsg::kReplayed) != 0);

  // The same event again (and every later query) finds the same row: not forwarded again.
  f.h.srv.send_to(kPrivatePath, funding_event(t));
  REQUIRE(f.pump([&] { return f.h.income_queries.load() == 3; }, 8000));
  idle(f.reactor, 100);
  CHECK(funding_of(f.oc).size() == 1);
  // The next query asks from no later than the payment's millisecond: the watermark stays the
  // settle margin behind the query before it, and the payment is known by its id.
  const std::string last = f.h.srv.frames("income").back();
  const std::size_t at = last.find("startTime=");
  REQUIRE(at != std::string::npos);
  CHECK(std::stoll(last.substr(at + 10)) <= t);
  CHECK(f.h.unsigned_requests.load() == 0);
  REQUIRE(f.pump([&] { return f.venue->status().funding_fetched == 1; }));
}

TEST_CASE("binance_usdm.venue: funding missed while the user stream was down is booked once") {
  DmsFixture f(0);
  wait_started(f);
  // Paid while the stream is down: the reconnect's reconciliation finds it.
  const long long t = wall_now().ns / 1'000'000;
  f.h.set_income("[" + income_row("BTCUSDT", "0.12", 555001, t) + "]");
  f.h.srv.close_sessions(kPrivatePath);
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2 && funding_of(f.oc).size() == 1; }));
  CHECK(funding_of(f.oc)[0]->funding_id.view() == "555001");
  CHECK(funding_of(f.oc)[0]->amount == Notional::from_decimal("0.12").value());
  // Its stream event arrives after all: the history is asked again, and nothing is new.
  const int queries = f.h.income_queries.load();
  f.h.srv.send_to(kPrivatePath, funding_event(t));
  REQUIRE(f.pump([&] { return f.h.income_queries.load() == queries + 1; }, 8000));
  idle(f.reactor, 100);
  CHECK(funding_of(f.oc).size() == 1);
}

TEST_CASE("binance_usdm.venue: a restart replays funding from its store's resume point") {
  // The earlier session stored payment 700001; 700002 was paid while nothing ran.
  const long long t = wall_now().ns / 1'000'000 - 3'600'000;
  DmsFixture f(0, [&](BinanceUsdmVenue& v) {
    v.resume_executions(t - 1'000, {std::string(kFundingIdPrefix) + "700001"});
  });
  f.h.set_income("[" + income_row("BTCUSDT", "-0.5", 700001, t) + "," +
                 income_row("BTCUSDT", "-0.7", 700002, t + 60'000) + "]");
  wait_started(f);
  const auto q = f.h.srv.frames("income");
  REQUIRE_FALSE(q.empty());
  CHECK(q[0].find("startTime=" + std::to_string(t - 1'000)) != std::string::npos);
  const auto got = funding_of(f.oc);
  REQUIRE(got.size() == 1);
  CHECK(got[0]->funding_id.view() == "700002");
}

TEST_CASE("binance_usdm.venue: an empty last income window moves the funding watermark") {
  // USD-M left its funding watermark where it was after an empty last window (OKX moved it), so
  // every later query asked from the connect time, and after a week in windows. Now it moves to
  // the query's start less the settle margin, as the execution replays do.
  const long long since = wall_now().ns / 1'000'000 - 3'600'000;
  DmsFixture f(0, [&](BinanceUsdmVenue& v) { v.resume_executions(since, {}); });
  wait_started(f);
  f.h.srv.send_to(kPrivatePath, funding_event(since + 1));
  REQUIRE(f.pump([&] { return f.h.income_queries.load() == 2; }, 8000));
  const auto q = f.h.srv.frames("income");
  REQUIRE(q.size() == 2);
  CHECK(q[0].find("startTime=" + std::to_string(since)) != std::string::npos);
  const std::size_t at = q[1].find("startTime=");
  REQUIRE(at != std::string::npos);
  CHECK(std::stoll(q[1].substr(at + 10)) > since + 1'800'000);
}

TEST_CASE("binance_usdm.venue: a failed income query is asked again from the timer") {
  DmsFixture f(0);
  wait_started(f);
  const long long t = wall_now().ns / 1'000'000;
  f.h.set_income("[" + income_row("BTCUSDT", "-0.375", 777, t) + "]");
  f.h.income_failures.store(1);
  f.h.srv.send_to(kPrivatePath, funding_event(t));
  REQUIRE(f.pump([&] { return funding_of(f.oc).size() == 1; }, 15000));
  CHECK(f.h.income_queries.load() == 3);  // the stream's, which failed, and the retry
}

// ---- the snapshot (ReconcileDriver) -----------------------------------------------------------

TEST_CASE(
    "binance_usdm.venue: the first snapshot is the start-up sweep, whatever was sent before") {
  // The user stream comes up late (its listenKey request fails once), after an order went out on
  // the order channel and was answered. Before ReconcileDriver the first snapshot carried that
  // order as its watermark and so judged this session's orders; every other connector's start-up
  // snapshot is a sweep, which judges none and finds the ones nobody holds.
  DmsFixture f(0);
  f.h.listen_key_failures = 1;
  const char* earlier = "fm000900000007";  // an order of a session that died
  f.h.set_open_orders(
      R"([{"orderId":4293153,"symbol":"BTCUSDT","status":"NEW","clientOrderId":"fm000100000001","price":"70000.00","origQty":"0.001","executedQty":"0","timeInForce":"GTX","type":"LIMIT","side":"BUY"},)"
      R"({"orderId":4293001,"symbol":"BTCUSDT","status":"NEW","clientOrderId":")" +
      std::string(earlier) +
      R"(","price":"69000.00","origQty":"0.002","executedQty":"0","timeInForce":"GTC","type":"LIMIT","side":"SELL"}])");
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 1; }));  // the order channel
  place_order(f, "fm000100000001");
  CHECK(reconcile_ends(f.oc) == 0);
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 1; }));
  CHECK(f.h.listen_keys.load() == 2);
  const std::size_t at = index_of_begin(f.oc, 0);
  REQUIRE(at != SIZE_MAX);
  const auto& begin = RecordingSink::as<ReconcileMsg>(f.oc.all[at]);
  CHECK((begin.flags & ReconcileMsg::kSentWatermark) != 0);
  CHECK_FALSE(begin.sent_watermark.valid());

  // What the engine does with it: the earlier session's order is unknown and cancelled; this
  // session's is judged by nothing, so an order of it that were missing would not be cancelled.
  Oms oms{1};
  NewOrderRequest req;
  req.instrument = InstrumentId{0};
  req.venue = VenueId{0};
  req.side = Side::Buy;
  req.price = Price::from_decimal("70000").value();
  req.qty = Qty::from_decimal("0.001").value();
  REQUIRE(oms.submit(req, cid("fm000100000001"), Timestamp{1}).has_value());
  REQUIRE(oms.submit(req, cid("fm000100000002"), Timestamp{1}).has_value());  // not listed
  oms.reconcile_begin(VenueId{0}, begin.sent_watermark);
  std::size_t unknown = 0;
  for (std::size_t i = at + 1; i < f.oc.all.size(); ++i) {
    if (RecordingSink::type_of(f.oc.all[i]) != EventType::Reconcile) continue;
    const auto& m = RecordingSink::as<ReconcileMsg>(f.oc.all[i]);
    if (m.kind != ReconcileMsg::Kind::OpenOrder) continue;
    if (oms.reconcile_open_order(m).action == OmsAction::CancelUnknown) {
      ++unknown;
      CHECK(m.cl_ord_id == cid(earlier));
      CHECK(m.venue_order_id.view() == "4293001");
    }
  }
  CHECK(unknown == 1);
  std::size_t cancelled = 0;
  oms.reconcile_end([&](const OmsUpdate&) { ++cancelled; }, VenueId{0});
  CHECK(cancelled == 0);
}

TEST_CASE("binance_usdm.venue: an order shadow whose terminal event was lost is dropped") {
  // Before ReconcileDriver only Binance Spot swept its shadows: an order that ended with nobody
  // told kept its entry in the 8192-slot table for the rest of the session.
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1; }));
  place_order(f, "fm000100000001");
  CHECK(f.venue->shadow_count() == 1);
  // The order is gone at the venue and no event said so: the snapshot does not name it.
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2; }));
  CHECK(RecordingSink::as<ReconcileMsg>(f.oc.all[index_of_begin(f.oc, 1)]).sent_watermark ==
        cid("fm000100000001"));
  CHECK(f.venue->shadow_count() == 0);
  REQUIRE(f.pump([&] { return f.venue->status().shadows_swept == 1; }));
}

namespace {

OutNewOrderMsg new_order(ClientOrderId id) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.cl_ord_id = id;
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("70000").value();
  n.qty = Qty::from_decimal("0.001").value();
  return n;
}

// Sends `ids` as New orders through the outbound ring and waits until the venue acked them all.
void place_all(DmsFixture& f, const std::vector<ClientOrderId>& ids) {
  const std::size_t acked = f.oc.count(EventType::OrderAck);
  for (const ClientOrderId id : ids) {
    const OutNewOrderMsg n = new_order(id);
    while (!f.outbound.try_push(&n, n.hdr.len)) {
      f.venue->on_wake();
      f.reactor.run_once(1);
    }
  }
  f.venue->on_wake();
  REQUIRE(f.pump([&] { return f.oc.count(EventType::OrderAck) == acked + ids.size(); }, 60000));
}

std::size_t frames_with(DmsFixture& f, std::string_view method) {
  std::size_t n = 0;
  for (const auto& t : f.h.srv.frames("/ws-fapi/v1")) n += t.find(method) != std::string::npos;
  return n;
}

}  // namespace

TEST_CASE("binance_usdm.venue: shadows of several engines' orders are swept in send order") {
  // Behind fastmm-gateway two engines send orders through one connector, their ids interleaved and
  // in send order only within each epoch. The sweep used to judge only the watermark's epoch, so
  // the other engine's orders whose end was lost stayed in the table for the session.
  DmsFixture f(0);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1; }));
  std::vector<ClientOrderId> ids;
  for (std::uint32_t k = 1; k <= 20; ++k) {
    ids.push_back(make_cl_ord_id(0x0007, 1000 + k));  // engine a: high sequence numbers
    ids.push_back(make_cl_ord_id(0x0003, k));         // engine b, the last to send
  }
  place_all(f, ids);
  CHECK(f.venue->shadow_count() == 40);
  // Every one ended at the venue with no event: the snapshot names none.
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2; }));
  CHECK(RecordingSink::as<ReconcileMsg>(f.oc.all[index_of_begin(f.oc, 1)]).sent_watermark ==
        ids.back());
  CHECK(f.venue->shadow_count() == 0);
  REQUIRE(f.pump([&] { return f.venue->status().shadows_swept == 40; }));
  CHECK(f.venue->status().shadows == 0);
}

TEST_CASE("binance_usdm.venue: with the order table full an order or modify is refused, not sent") {
  // 7168 orders the connector holds shadows for: working, or ended with no event and not swept yet
  // (another engine's, behind the gateway, before the sweep compared send order). Before, the
  // next order went out untracked. A modify did too, and the venue acked it: the engine took the
  // new id as the working order while the venue's later fills, still under the first client id,
  // were booked to the id it had replaced (and its cumulative quantity counted from the start).
  DmsFixture f(0, {}, false, /*order_limits=*/false);
  REQUIRE(f.pump([&] { return live_states(f.oc) >= 2 && reconcile_ends(f.oc) == 1; }));
  constexpr std::size_t kRoom = kShadowSlots - kShadowSlots / 8;
  std::vector<ClientOrderId> ids;
  for (std::uint32_t k = 1; k <= kRoom; ++k) ids.push_back(make_cl_ord_id(1, k));
  place_all(f, ids);
  REQUIRE(f.venue->shadow_count() == kRoom);
  REQUIRE(frames_with(f, "\"order.place\"") == kRoom);

  // One more order: refused with OrderTableFull, never sent.
  const ClientOrderId extra = make_cl_ord_id(1, kRoom + 1);
  const OutNewOrderMsg n = new_order(extra);
  REQUIRE(f.outbound.try_push(&n, n.hdr.len));
  // A modify of the first order to a new id: refused the same way, the original stays working.
  const ClientOrderId modified = make_cl_ord_id(1, kRoom + 2);
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  r.cl_ord_id = modified;
  r.orig_cl_ord_id = ids.front();
  r.venue_order_id.assign("4293153");
  r.price = Price::from_decimal("70001").value();
  r.qty = Qty::from_decimal("0.001").value();
  REQUIRE(f.outbound.try_push(&r, r.hdr.len));
  f.venue->on_wake();
  const auto refused = [&](ClientOrderId id) {
    return f.oc.first_if<OrderRejectMsg>(EventType::OrderReject, [&](const OrderRejectMsg& m) {
      return m.cl_ord_id == id && m.reason == RejectReason::OrderTableFull;
    });
  };
  REQUIRE(f.pump([&] { return refused(extra) != nullptr && refused(modified) != nullptr; }));
  CHECK(refused(extra)->hdr.instrument == InstrumentId{0});
  REQUIRE(f.pump([&] { return f.venue->status().shadows_refused == 2; }));

  // The snapshot names none of the orders, so they are swept: the table has room, the next order
  // goes. Its ack comes after every frame sent before it on the order connection.
  f.venue->request_open_orders();
  REQUIRE(f.pump([&] { return reconcile_ends(f.oc) == 2; }));
  CHECK(f.venue->shadow_count() == 0);
  place_all(f, {make_cl_ord_id(1, kRoom + 3)});
  CHECK(frames_with(f, "\"order.place\"") == kRoom + 1);
  CHECK(frames_with(f, "\"order.modify\"") == 0);
  CHECK(f.oc.first_if<OrderAckMsg>(EventType::OrderAck, [&](const OrderAckMsg& m) {
    return m.cl_ord_id == modified;
  }) == nullptr);
  CHECK(f.venue->status().shadows_refused == 2);
}

namespace {

std::vector<BalanceMsg> balances_of(const Collected& c) {
  std::vector<BalanceMsg> out;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) == EventType::Balance)
      out.push_back(RecordingSink::as<BalanceMsg>(m));
  }
  return out;
}

std::size_t balance_snapshots(const Collected& c) {
  std::size_t n = 0;
  for (const BalanceMsg& b : balances_of(c)) n += (b.flags & BalanceMsg::kSnapshotEnd) != 0 ? 1 : 0;
  return n;
}

// The rows of the last complete balance snapshot.
std::vector<BalanceMsg> last_snapshot(const Collected& c) {
  const std::vector<BalanceMsg> all = balances_of(c);
  std::vector<BalanceMsg> out;
  for (const BalanceMsg& b : all) {
    if ((b.flags & BalanceMsg::kSnapshot) == 0) continue;
    if (!out.empty() && (out.back().flags & BalanceMsg::kSnapshotEnd) != 0) out.clear();
    out.push_back(b);
  }
  return out;
}

Notional notional(const char* s) {
  return Notional::from_decimal(s).value();
}

// A connected BTCUSDT venue on `h`.
struct UsdmSession {
  InstrumentTable instruments;
  RecordingSink md{8U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<BinanceUsdmVenue> venue;
  Collected oc;
  std::int64_t connected_ms = 0;

  explicit UsdmSession(Harness& h) {
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
    venue = std::make_unique<BinanceUsdmVenue>(VenueId{0}, h.config(false));
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue->subscribe(ids);
    connected_ms = venue->venue_time_ms();
    venue->connect(reactor);
  }
  ~UsdmSession() {
    venue->disconnect();
    reactor.run_once(0);
  }
  UsdmSession(const UsdmSession&) = delete;
  UsdmSession& operator=(const UsdmSession&) = delete;

  template <class Pred>
  bool pump(Pred pred, int timeout_ms = 5000) {
    return pump_until(
        reactor,
        [&] {
          md.drain();
          oc.take(orders);
          return pred();
        },
        timeout_ms);
  }
};

}  // namespace

TEST_CASE("binance_usdm.venue: the start-up balance snapshot and an ACCOUNT_UPDATE refresh") {
  Harness h;
  UsdmSession s(h);
  // Order and user channels live, the reconciliation, then GET /fapi/v3/account: the documented
  // single-asset reply lists USDT and USDC; the engine keeps USDT (and BTC, which it lacks).
  REQUIRE(s.pump([&] {
    return live_states(s.oc) >= 2 && reconcile_ends(s.oc) == 1 && balance_snapshots(s.oc) == 1;
  }));
  const std::int64_t after_ms = s.venue->venue_time_ms();
  CHECK(h.account_requests.load() == 1);
  CHECK(h.unsigned_requests.load() == 0);
  std::vector<BalanceMsg> snap = last_snapshot(s.oc);
  REQUIRE(snap.size() == 1);
  const BalanceMsg& usdt = snap[0];
  CHECK(usdt.asset.view() == "USDT");
  CHECK(usdt.flags == (BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd));
  CHECK(usdt.free == notional("23.72469206"));    // availableBalance
  CHECK(usdt.locked.is_zero());                   // initialMargin
  CHECK(usdt.total == notional("23.72469206"));   // walletBalance
  CHECK(usdt.equity == notional("23.72469206"));  // marginBalance
  CHECK(usdt.maintenance.is_zero());              // maintMargin
  CHECK(usdt.hdr.exch_ts.ns >= s.connected_ms * 1'000'000);
  CHECK(usdt.hdr.exch_ts.ns <= after_ms * 1'000'000);

  // A fill moved the wallet: ACCOUNT_UPDATE B[] has no available balance or margin, so the
  // account is asked again (at most once a second) and its answer is the new snapshot.
  std::string after = fastmm::test::fixture("binance_usdm/account_v3_single.json");
  const std::string wallet = R"("walletBalance": "23.72469206")";
  after.replace(after.find(wallet), wallet.size(), R"("walletBalance": "23.70000000")");
  const std::string avail = R"("availableBalance": "23.72469206")";
  after.replace(after.find(avail), avail.size(), R"("availableBalance": "9.70000000")");
  const std::string im = R"("initialMargin": "0.00000000")";
  after.replace(after.find(im), im.size(), R"("initialMargin": "14.00000000")");
  h.set_account(after);
  h.srv.send_to(kPrivatePath, account_update("0.002"));
  REQUIRE(s.pump([&] { return balance_snapshots(s.oc) == 2; }));
  CHECK(h.account_requests.load() == 2);
  snap = last_snapshot(s.oc);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].free == notional("9.7"));
  CHECK(snap[0].locked == notional("14"));
  CHECK(snap[0].total == notional("23.7"));
  // Nothing forwarded from B[] itself: every BalanceMsg is a snapshot row.
  for (const BalanceMsg& b : balances_of(s.oc)) CHECK((b.flags & BalanceMsg::kSnapshot) != 0);
}

TEST_CASE("binance_usdm.venue: multi-assets mode reports the account row in USD") {
  Harness h;
  h.multi_assets = true;
  h.set_account(fastmm::test::fixture("binance_usdm/account_v3_multi.json"));
  UsdmSession s(h);
  REQUIRE(s.pump([&] { return reconcile_ends(s.oc) == 1 && balance_snapshots(s.oc) == 1; }));
  std::vector<BalanceMsg> snap = last_snapshot(s.oc);
  REQUIRE(snap.size() == 2);
  CHECK(snap[0].asset.view() == "USDT");
  CHECK(snap[0].flags == BalanceMsg::kSnapshot);
  CHECK(snap[0].free == notional("23.72469206"));  // maxWithdrawAmount, in USDT
  CHECK(snap[0].total == notional("23.72469206"));
  CHECK(snap[1].asset.view() == "USD");
  CHECK(snap[1].flags == (BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd | BalanceMsg::kAccount));
  CHECK(snap[1].free == notional("126.72469206"));    // availableBalance
  CHECK(snap[1].total == notional("126.72469206"));   // totalWalletBalance
  CHECK(snap[1].equity == notional("126.72469206"));  // totalMarginBalance
  CHECK(snap[1].locked.is_zero());                    // totalInitialMargin
  CHECK(snap[1].hdr.exch_ts == snap[0].hdr.exch_ts);

  // Switched to single-asset mode: the next snapshot has no account row.
  REQUIRE(s.pump([&] { return live_states(s.oc) >= 2; }));
  h.set_account(fastmm::test::fixture("binance_usdm/account_v3_single.json"));
  h.srv.send_to(kPrivatePath, R"({"e":"ACCOUNT_CONFIG_UPDATE","E":2,"T":1,"ai":{"j":false}})");
  REQUIRE(s.pump([&] { return balance_snapshots(s.oc) == 2; }));
  snap = last_snapshot(s.oc);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].asset.view() == "USDT");
  CHECK(snap[0].free == notional("23.72469206"));
}

TEST_CASE("binance_usdm.venue: a failed balance fetch does not hold up the order snapshot") {
  Harness h;
  h.account_fails = true;
  UsdmSession s(h);
  REQUIRE(s.pump([&] { return reconcile_ends(s.oc) == 1 && h.account_requests.load() >= 1; }));
  CHECK(balances_of(s.oc).empty());
  // Asked again after ReconcileDriver::kRetryNs (5 s).
  h.account_fails = false;
  REQUIRE(s.pump([&] { return balance_snapshots(s.oc) == 1; }, 10'000));
  CHECK(h.account_requests.load() >= 2);
  CHECK(last_snapshot(s.oc).size() == 1);
}
