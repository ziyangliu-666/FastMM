// BinanceUsdmVenue against a scripted in-process fake Binance USDⓈ-M (REST + public/market streams
// + private listenKey stream + WS API): reference data and account checks, depth sync with a pu
// gap, order.place / order.modify / order.cancel with the venue client id kept across the modify,
// reconciliation with positions, the ACCOUNT_UPDATE position check, listenKey expiry, order-channel
// loss and the blocking kill-switch cancel_all.
#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/venue_factory.hpp"

#include <atomic>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using namespace fastmm::venues::test;

namespace {

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
  std::atomic<int> listen_keys{0};
  std::atomic<int> reconcile_requests{0};
  std::atomic<int> unsigned_requests{0};
  std::atomic<int> cancel_all_ok{0};

  explicit Harness(bool hedge = false) : hedge_mode(hedge) {
    srv.route("GET", "/fapi/v1/exchangeInfo", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, exchange_info);
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
          R"({"lastUpdateId":100,"E":1789469121900,"T":1789469121800,"bids":[["70000.00","1.000"]],"asks":[["70000.10","2.000"]]})");
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
    signed_route("GET", "/fapi/v1/openOrders", "[]");
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
      return net::HttpServerResponse::json(200, R"({"listenKey":"lk-test-0001"})");
    });
    srv.route("DELETE", "/fapi/v1/allOpenOrders", [this](const net::HttpRequest& r) {
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
    srv.on_ws_open("/market/stream", [](net::WsSession& s) {
      s.send_text(
          R"({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":1789469122045,"a":309896910,"s":"BTCUSDT","p":"70000.10","q":"0.010","nq":"0.010","f":1,"l":2,"T":1789469121938,"m":true,"st":1}})");
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

    // pu gap -> Resyncing and a new snapshot.
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
  CHECK(venue_kind("binance_usdm") == VenueKind::BinanceUsdm);
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
