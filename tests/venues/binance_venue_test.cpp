// BinanceVenue against a scripted in-process fake Binance (REST + combined stream + WS API):
// reference data, clock, depth snapshot + buffered deltas, bookTicker/trade, user stream
// subscription, order.place -> ack + executionReport NEW/TRADE, order.cancel, openOrders
// reconciliation, a sequence gap -> resync, and the blocking kill-switch cancel_all.
#include "fastmm/venues/binance/binance_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/binance/generated/binance_stream_sbe.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kKey = "fake-key";
constexpr const char* kSecret = "fake-secret";

std::string depth_frame(std::uint64_t U, std::uint64_t u, const char* bid_px, const char* bid_qty) {
  return R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1789295134334,"s":"BTCUSDT","U":)" +
         std::to_string(U) + R"(,"u":)" + std::to_string(u) + R"(,"b":[[")" + bid_px + R"(",")" +
         bid_qty + R"("]],"a":[]}})";
}

std::string exec_report(
    const char* x, const char* status, const char* last_qty, const char* cum_qty, long trade_id) {
  return std::string(
             R"({"subscriptionId":0,"event":{"e":"executionReport","E":1789295200000,"s":"BTCUSDT","c":"fm000100000001","S":"BUY","o":"LIMIT_MAKER","f":"GTC","q":"0.00100000","p":"70000.00000000","P":"0.00000000","F":"0.00000000","g":-1,"C":"","x":")") +
         x + R"(","X":")" + status + R"(","r":"NONE","i":4293153,"l":")" + last_qty + R"(","z":")" +
         cum_qty + R"(","L":"70000.00000000","n":"0","N":null,"T":1789295199990,"t":)" +
         std::to_string(trade_id) +
         R"(,"I":8641984,"w":true,"m":true,"M":true,"O":1789295199990,"Z":"0","Y":"0","Q":"0","W":1789295199990,"V":"EXPIRE_MAKER"}})";
}

// Verifies "<params>&signature=<hex>" against HMAC-SHA256(secret, params).
bool signed_ok(std::string_view query) {
  const std::size_t p = query.rfind("&signature=");
  if (p == std::string_view::npos) return false;
  const std::string expected(net::hmac_sha256_hex(kSecret, query.substr(0, p)).view());
  return query.substr(p + 11) == expected;
}

struct Harness {
  FakeVenueServer srv;
  std::string exchange_info = fastmm::test::fixture("binance/exchange_info.json");
  std::atomic<int> depth_requests{0};
  std::atomic<std::uint64_t> depth_last_update_id{100};  // the snapshot's lastUpdateId
  std::atomic<int> cancel_all_ok{0};
  std::atomic<int> cancel_all_bad{0};
  std::atomic<bool> hold_place{false};   // order.place gets no answer (still in flight)
  std::atomic<bool> hold_trades{false};  // GET myTrades answers only once released (3 s at most)
  std::atomic<int> my_trades{0};         // GET myTrades requests seen
  std::mutex trades_mu;
  // GET myTrades answers (status, body), served in order; "[]" once they run out (trades_mu).
  std::vector<std::pair<int, std::string>> trades_replies;
  std::atomic<int> rest_open_orders{0};  // GET /api/v3/openOrders requests seen
  // GET /api/v3/order answers (status, body), served in order; 400 -2013 once they run out
  // (trades_mu).
  std::vector<std::pair<int, std::string>> order_replies;
  net::WsSession* user_session = nullptr;  // server thread only

  // The server thread reads this harness's members: stop it before they go.
  ~Harness() { srv.stop(); }
  Harness() {
    // The fixture's serverTime is from when it was recorded; the connector takes its clock from it
    // until GET /api/v3/time answers, and the execution replay starts at that clock. Now, as the
    // venue would say, or every replay would walk the days since the recording.
    const std::string recorded = R"("serverTime":1789295119614)";
    exchange_info.replace(exchange_info.find(recorded),
                          recorded.size(),
                          R"("serverTime":)" + std::to_string(wall_now().ns / 1'000'000));
    srv.route("GET", "/api/v3/exchangeInfo", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, exchange_info);
    });
    srv.route("GET", "/api/v3/time", [](const net::HttpRequest&) {
      return net::HttpServerResponse::json(
          200, R"({"serverTime":)" + std::to_string(wall_now().ns / 1'000'000) + "}");
    });
    srv.route("GET", "/api/v3/depth", [this](const net::HttpRequest& r) {
      ++depth_requests;
      srv.record("depth", std::string(r.query));
      return net::HttpServerResponse::json(
          200,
          R"({"lastUpdateId":)" + std::to_string(depth_last_update_id.load()) +
              R"(,"bids":[["70000.00000000","1.00000000"]],"asks":[["70000.10000000","2.00000000"]]})");
    });
    srv.route("DELETE", "/api/v3/openOrders", [this](const net::HttpRequest& r) {
      const bool ok = r.header("X-MBX-APIKEY") == kKey && signed_ok(r.query) &&
                      r.query.find("symbol=BTCUSDT") != std::string_view::npos;
      ++(ok ? cancel_all_ok : cancel_all_bad);
      return net::HttpServerResponse::json(200, "[]");
    });
    srv.route("GET", "/api/v3/myTrades", [this](const net::HttpRequest& r) {
      ++my_trades;
      srv.record("myTrades", std::string(r.query));
      // Blocks the fake's thread: the reply is in flight for as long as the test says.
      for (int i = 0; i < 600 && hold_trades.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      const std::lock_guard lock(trades_mu);
      if (trades_replies.empty()) return net::HttpServerResponse::json(200, "[]");
      const auto [status, body] = trades_replies.front();
      trades_replies.erase(trades_replies.begin());
      return status == 200 ? net::HttpServerResponse::json(200, body)
                           : net::HttpServerResponse::text(status, body);
    });
    srv.route("GET", "/api/v3/order", [this](const net::HttpRequest& r) {
      srv.record("order", std::string(r.query));
      const std::lock_guard lock(trades_mu);
      if (order_replies.empty())
        return net::HttpServerResponse::json(400,
                                             R"({"code":-2013,"msg":"Order does not exist."})");
      const auto [status, body] = order_replies.front();
      order_replies.erase(order_replies.begin());
      return status == 200 ? net::HttpServerResponse::json(200, body)
                           : net::HttpServerResponse::text(status, body);
    });
    srv.route("GET", "/api/v3/openOrders", [this](const net::HttpRequest&) {
      ++rest_open_orders;
      return net::HttpServerResponse::json(200, "[]");
    });
    srv.on_ws_open("/stream", [](net::WsSession& s) {
      s.send_text(depth_frame(95, 100, "69999.00", "9"));  // stale: u <= lastUpdateId
      s.send_text(depth_frame(101, 101, "70000.00", "1.5"));
    });
    srv.route("POST", "/api/v3/order", [this](const net::HttpRequest& r) {
      const std::string query(r.query);
      const std::size_t p = query.find("newClientOrderId=");
      const std::string id = p == std::string::npos
                                 ? std::string()
                                 : query.substr(p + 17, query.find('&', p) - p - 17);
      srv.record("rest_order", id);
      return net::HttpServerResponse::json(
          200,
          R"({"symbol":"BTCUSDT","orderId":4293160,"orderListId":-1,"clientOrderId":")" + id +
              R"(","transactTime":1789295199990})");
    });
    serve_ws_api(srv);
    srv.start();
  }

  // The WS API (order entry, the user stream, openOrders.status) on `server`.
  void serve_ws_api(FakeVenueServer& server) {
    server.on_ws_text("/ws-api/v3", [this, &server](net::WsSession& s, std::string_view t) {
      const std::string method = json_str(t, "method");
      const std::string id = json_str(t, "id");
      if (method == "userDataStream.subscribe.signature") {
        user_session = &s;
        s.send_text(R"({"id":")" + id + R"(","status":200,"result":{"subscriptionId":0}})");
      } else if (method == "order.place") {
        if (hold_place.load()) {
          server.record("held", json_str(t, "newClientOrderId"));
          return;
        }
        s.send_text(
            R"({"id":")" + id +
            R"(","status":200,"result":{"symbol":"BTCUSDT","orderId":4293153,"orderListId":-1,"clientOrderId":")" +
            json_str(t, "newClientOrderId") +
            R"(","transactTime":1789295199990},"rateLimits":[{"rateLimitType":"ORDERS","interval":"SECOND","intervalNum":10,"limit":50,"count":1},{"rateLimitType":"REQUEST_WEIGHT","interval":"MINUTE","intervalNum":1,"limit":6000,"count":3}]})");
        if (user_session != nullptr) {
          user_session->send_text(exec_report("NEW", "NEW", "0.00000000", "0.00000000", -1));
          user_session->send_text(
              exec_report("TRADE", "PARTIALLY_FILLED", "0.00040000", "0.00040000", 777));
        }
      } else if (method == "order.cancel") {
        s.send_text(
            R"({"id":")" + id +
            R"(","status":200,"result":{"symbol":"BTCUSDT","origClientOrderId":"fm000100000001","orderId":4293153,"orderListId":-1,"clientOrderId":"x1","transactTime":1789295200100,"price":"70000.00000000","origQty":"0.00100000","executedQty":"0.00040000","cummulativeQuoteQty":"28.00000000","status":"CANCELED","timeInForce":"GTC","type":"LIMIT_MAKER","side":"BUY"}})");
      } else if (method == "openOrders.status") {
        s.send_text(R"({"id":")" + id + R"(","status":200,"result":[]})");
      }
    });
  }

  BinanceVenueConfig config(bool dry_run) const {
    BinanceVenueConfig c;
    c.name = "fake-binance";
    c.ws_url = srv.ws_base() + "/stream";
    c.ws_api_url = srv.ws_base() + "/ws-api/v3";
    c.rest_url = srv.http_base();
    c.credentials.api_key = kKey;
    c.credentials.secret.value = kSecret;
    c.dry_run = dry_run;
    c.user_stream = dry_run ? UserStreamMode::None : UserStreamMode::WsApi;
    c.min_snapshot_interval_ns = 0;
    c.http_timeout_ms = 3000;
    return c;
  }
};

}  // namespace

TEST_CASE("binance.venue: scripted fake exchange end to end") {
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
  const Seqlocked<TscCalibration> tsc(calibrate_tsc(milliseconds(10)));
  {
    BinanceVenue venue(VenueId{0}, h.config(false));
    venue.set_tsc_calibration_source(&tsc);
    REQUIRE(venue.load_reference_data(instruments));
    CHECK(instruments.get(InstrumentId{0}).tick == Price::from_decimal("0.01").value());
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);

    Collected mdc;
    Collected oc;
    auto live_channels = [&] {
      std::size_t n = 0;
      for (const auto& m : oc.all) {
        if (RecordingSink::type_of(m) == EventType::ConnectionState &&
            RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Live)
          ++n;
      }
      return n;
    };
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      oc.take(orders);
      return venue.md_feed()->synced_count() == 1 && mdc.count(EventType::BookDelta) >= 1 &&
             live_channels() >= 2;
    }));
    CHECK(h.depth_requests.load() == 1);
    REQUIRE(h.srv.frames("depth").size() == 1);
    CHECK(h.srv.frames("depth")[0] == "symbol=BTCUSDT&limit=1000");
    // Snapshot first, then only the bracketing delta (the stale one was dropped).
    CHECK(RecordingSink::type_of(mdc.all.front()) == EventType::ConnectionState);  // md Live
    CHECK(mdc.count(EventType::BookSnapshot) == 1);
    CHECK(mdc.count(EventType::BookDelta) == 1);
    CHECK(mdc.last<BookDeltaMsg>(EventType::BookDelta)->first_update_id == 101);

    h.srv.send_to(
        "/stream",
        R"({"stream":"btcusdt@bookTicker","data":{"u":102,"s":"BTCUSDT","b":"70000.00000000","B":"1.50000000","a":"70000.10000000","A":"2.00000000"}})");
    h.srv.send_to(
        "/stream",
        R"({"stream":"btcusdt@trade","data":{"e":"trade","E":1789295134226,"s":"BTCUSDT","t":388510,"p":"70000.10000000","q":"0.00065000","T":1789295134225,"m":true,"M":true}})");
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      return mdc.count(EventType::BookTicker) == 1 && mdc.count(EventType::Trade) == 1;
    }));
    CHECK(mdc.last<TradeMsg>(EventType::Trade)->aggressor == Side::Sell);

    // Sequence gap -> Resyncing + a new snapshot request, answered with a snapshot inside the
    // update that revealed the gap (which is kept: it applies on the snapshot).
    h.depth_last_update_id = 150;
    h.srv.send_to("/stream", depth_frame(150, 151, "70000.00", "3"));
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      return h.depth_requests.load() == 2 && venue.md_feed()->synced_count() == 1;
    }));
    bool saw_resyncing = false;
    for (const auto& m : mdc.all) {
      if (RecordingSink::type_of(m) == EventType::ConnectionState &&
          RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Resyncing)
        saw_resyncing = true;
    }
    CHECK(saw_resyncing);
    CHECK(venue.md_feed()->resync_count() >= 1);

    // Order entry over the WS API; acks and fills from the response and the user stream. The
    // order carries the last trade's receive stamp, as the engine does for an order its
    // strategy placed in response to that trade; the cancel below carries none.
    const Cycles trigger_t0 = mdc.last<TradeMsg>(EventType::Trade)->hdr.t0_cycles;
    REQUIRE(trigger_t0.v != 0);
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    n.hdr.t0_cycles = trigger_t0;
    n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("70000").value();
    n.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    const Cycles before_wake = rdtscp();
    venue.on_wake();
    const Cycles after_wake = rdtscp();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 2 && oc.count(EventType::OrderFill) == 1;
    }));
    const auto api = h.srv.frames("/ws-api/v3");
    std::string place;
    for (const auto& f : api) {
      if (f.find("\"order.place\"") != std::string::npos) place = f;
    }
    REQUIRE_FALSE(place.empty());
    CHECK(place.find(R"("newClientOrderId":"fm000100000001")") != std::string::npos);
    CHECK(place.find(R"("newOrderRespType":"ACK")") != std::string::npos);
    CHECK(place.find(R"("type":"LIMIT_MAKER")") != std::string::npos);
    CHECK(place.find(R"("signature":")") != std::string::npos);
    const auto* ack = oc.last<OrderAckMsg>(EventType::OrderAck);
    CHECK(ack->cl_ord_id == n.cl_ord_id);
    CHECK(ack->venue_order_id.view() == "4293153");
    const auto* fill = oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(fill->qty == Qty::from_decimal("0.0004").value());
    CHECK(fill->leaves_qty == Qty::from_decimal("0.0006").value());
    CHECK(fill->liquidity == Liquidity::Maker);
    CHECK(fill->exec_id.view() == "777");

    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
    c.cl_ord_id = n.cl_ord_id;
    c.venue_order_id.assign("4293153");
    REQUIRE(outbound.try_push(&c, c.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderCancelAck) == 1;
    }));
    const auto* cx = oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck);
    CHECK(cx->cl_ord_id == n.cl_ord_id);
    CHECK(cx->cum_qty == Qty::from_decimal("0.0004").value());

    // Network-thread latency, published with the status: both messages were encoded and sent,
    // only the triggered order has a receive-to-wire sample, and it lies between the
    // receive-to-on_wake and receive-to-after-on_wake intervals.
    venue.on_timer(net::Reactor::now_ns());
    const VenueStatus st = venue.status();
    CHECK(st.order_encode.count == 2);
    CHECK(st.order_send.count == 2);
    CHECK(st.wire_tick_to_trade.count == 1);
    const TscClock conv(tsc.load());
    if (conv.calibration().use_tsc) {
      CHECK(st.wire_tick_to_trade.p50_ns >=
            static_cast<std::uint64_t>(conv.cycles_to_ns(before_wake - trigger_t0)));
      CHECK(st.wire_tick_to_trade.p50_ns <=
            static_cast<std::uint64_t>(conv.cycles_to_ns(after_wake - trigger_t0)));
      CHECK(st.wire_tick_to_trade.p99_ns >= st.wire_tick_to_trade.p50_ns);
      CHECK(st.order_encode.p50_ns > 0);
      CHECK(st.order_send.p50_ns > 0);
      CHECK(st.order_encode.p50_ns < 1'000'000'000);
      CHECK(st.order_send.p50_ns < 1'000'000'000);
    }

    // The connector already reconciled once on the first connect (a crashed session's orders would
    // be resting there); this is the snapshot the test asks for.
    const std::size_t reconciles = oc.count(EventType::Reconcile);
    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::Reconcile) == reconciles + 2;
    }));
    // Begin carries the last order id the venue sent before it asked for the snapshot.
    const auto* begin = oc.last_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
      return m.kind == ReconcileMsg::Kind::Begin;
    });
    REQUIRE(begin != nullptr);
    CHECK((begin->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK(begin->sent_watermark == n.cl_ord_id);

    // Kill switch from this (non-reactor) thread over an independent connection.
    CHECK(venue.cancel_all());
    CHECK(h.cancel_all_ok.load() == 1);
    CHECK(h.cancel_all_bad.load() == 0);
    CHECK_FALSE(venue.fatal());
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}

TEST_CASE("binance.venue: an order still in flight is above the snapshot's watermark") {
  // Session A, 12:05:28 UTC: an order was sent, a cancel reject asked for the open orders, and
  // the venue's reply did not list the order (sent 1 ms before, not answered yet). With the last
  // *sent* id as the watermark the engine cancelled it as gone and the connector dropped its
  // shadow; the watermark is now the last id sent before the first one still unanswered.
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    BinanceVenue venue(VenueId{0}, h.config(false));
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);
    Collected oc;
    auto live_channels = [&] {
      std::size_t n = 0;
      for (const auto& m : oc.all) {
        if (RecordingSink::type_of(m) == EventType::ConnectionState &&
            RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Live)
          ++n;
      }
      return n;
    };
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return live_channels() >= 2 && oc.count(EventType::Reconcile) >= 2;  // start-up sweep
    }));
    const auto place = [&](const char* id) {
      OutNewOrderMsg n{};
      init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
      n.cl_ord_id = decode_cl_ord_id(id).value();
      n.side = Side::Buy;
      n.type = OrderType::PostOnly;
      n.price = Price::from_decimal("70000").value();
      n.qty = Qty::from_decimal("0.001").value();
      REQUIRE(outbound.try_push(&n, n.hdr.len));
      venue.on_wake();
      return n.cl_ord_id;
    };
    const ClientOrderId answered = place("fm000100000001");
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 1;
    }));
    h.hold_place = true;
    place("fm000100000002");
    REQUIRE(pump_until(reactor, [&] { return h.srv.frames("held").size() == 1; }));
    CHECK(venue.shadow_count() == 2);

    const std::size_t reconciles = oc.count(EventType::Reconcile);
    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::Reconcile) == reconciles + 2;
    }));
    const auto* begin = oc.last_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
      return m.kind == ReconcileMsg::Kind::Begin;
    });
    REQUIRE(begin != nullptr);
    CHECK((begin->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK(begin->sent_watermark == answered);  // not the order in flight
    CHECK(venue.shadow_count() == 1);          // the in-flight order keeps its shadow
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}

TEST_CASE(
    "binance.venue: an order sent over REST while the order connection is down stays in flight") {
  // The order connection drops and a reconciliation is asked for; its execution replay goes out
  // over REST first, and an order sent then goes over REST too, queued behind it. The connection
  // comes back, and the replay's reply releases the snapshot, whose watermark is taken there,
  // before the order's reply. Losing the WebSocket connection settles what was sent on it, not a
  // REST request: the order must still hold the watermark back, or the snapshot (which may not
  // list it yet) has the engine cancel it as gone.
  Harness h;
  FakeVenueServer api;  // the WS API on its own thread, so it answers while REST is held
  h.serve_ws_api(api);
  api.start();
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    BinanceVenueConfig cfg = h.config(false);
    cfg.ws_api_url = api.ws_base() + "/ws-api/v3";
    cfg.cancel_on_order_channel_loss = false;
    cfg.http_timeout_ms = 8000;
    // A fixed reconnect delay, so the REST order goes out while the connection is down.
    cfg.backoff.base_ms = 800;
    cfg.backoff.max_ms = 800;
    cfg.backoff.jitter = 0.0;
    BinanceVenue venue(VenueId{0}, std::move(cfg));
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);
    Collected oc;
    const auto count_state = [&](ConnState state) {
      std::size_t n = 0;
      for (const auto& m : oc.all) {
        if (RecordingSink::type_of(m) == EventType::ConnectionState &&
            RecordingSink::as<ConnectionStateMsg>(m).state == state)
          ++n;
      }
      return n;
    };
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return count_state(ConnState::Live) >= 2 && oc.count(EventType::Reconcile) >= 2;
    }));
    const auto place = [&](const char* id) {
      OutNewOrderMsg n{};
      init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
      n.cl_ord_id = decode_cl_ord_id(id).value();
      n.side = Side::Buy;
      n.type = OrderType::PostOnly;
      n.price = Price::from_decimal("70000").value();
      n.qty = Qty::from_decimal("0.001").value();
      REQUIRE(outbound.try_push(&n, n.hdr.len));
      venue.on_wake();
      return n.cl_ord_id;
    };
    const ClientOrderId answered = place("fm000100000001");
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 1;
    }));

    api.close_sessions("/ws-api/v3");
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return count_state(ConnState::Disconnected) >= 2;  // order and user channels
    }));
    const std::size_t reconciles = oc.count(EventType::Reconcile);
    h.hold_trades = true;
    const int asked = h.my_trades.load();
    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] { return h.my_trades.load() == asked + 1; }));
    const ClientOrderId over_rest = place("fm000100000002");
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return count_state(ConnState::Live) >= 4;  // back, while the replay is still held
    }));
    h.hold_trades = false;
    REQUIRE(pump_until(
        reactor,
        [&] {
          oc.take(orders);
          return oc.count(EventType::Reconcile) >= reconciles + 2;
        },
        10000));
    REQUIRE(pump_until(reactor, [&] { return h.srv.frames("rest_order").size() == 1; }));
    CHECK(h.srv.frames("rest_order")[0] == "fm000100000002");
    // The first snapshot after the drop.
    const ReconcileMsg* begin = nullptr;
    std::size_t seen = 0;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) != EventType::Reconcile || seen++ < reconciles) continue;
      if (RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::Begin) {
        begin = &RecordingSink::as<ReconcileMsg>(m);
        break;
      }
    }
    REQUIRE(begin != nullptr);
    CHECK((begin->flags & ReconcileMsg::kSentWatermark) != 0);
    // Below the REST order: the snapshot (empty here) says nothing about it.
    CHECK(begin->sent_watermark == answered);
    CHECK(begin->sent_watermark.value < over_rest.value);
    venue.disconnect();
    reactor.run_once(0);
  }
  api.stop();
  h.srv.stop();
}

TEST_CASE("binance.venue: dry run opens market data only and refuses orders") {
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    BinanceVenue venue(VenueId{0}, h.config(true));
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);
    REQUIRE(pump_until(reactor, [&] { return venue.md_feed()->synced_count() == 1; }));
    CHECK(h.srv.open_count("/ws-api/v3") == 0);
    CHECK_FALSE(venue.caps().user_stream);
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    n.cl_ord_id = decode_cl_ord_id("fm000100000009").value();
    n.price = Price::from_decimal("70000").value();
    n.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    Collected oc;
    oc.take(orders);
    REQUIRE(oc.count(EventType::OrderReject) == 1);
    CHECK(oc.last<OrderRejectMsg>(EventType::OrderReject)->reason == RejectReason::VenueKilled);
    CHECK(venue.cancel_all());  // no-op without order entry
    CHECK(h.cancel_all_ok.load() == 0);
    venue.disconnect();
  }
  h.srv.stop();
}

namespace {

// One SBE frame (header + body) built with the generated stream_1_0 writers.
std::vector<std::byte> sbe_depth(std::int64_t first, std::int64_t last, std::int64_t bid_cents) {
  namespace ss = fastmm::venues::binance::sbe_stream;
  std::vector<std::byte> buf(512);
  ss::DepthDiffStreamEventWriter w(std::span<std::byte>(buf).subspan(8));
  w.set_event_time(1789295134334000);
  w.set_first_book_update_id(first);
  w.set_last_book_update_id(last);
  w.set_price_exponent(-2);
  w.set_qty_exponent(-8);
  auto bids = w.bids(1);
  bids[0].set_price(bid_cents);
  bids[0].set_qty(150000000);
  static_cast<void>(w.asks(0));
  static_cast<void>(w.set_symbol("BTCUSDT"));
  ss::DepthDiffStreamEventWriter::header().store(buf.data());
  buf.resize(8 + w.size_bytes());
  return buf;
}
std::vector<std::byte> sbe_trade(std::int64_t id) {
  namespace ss = fastmm::venues::binance::sbe_stream;
  std::vector<std::byte> buf(256);
  ss::TradesStreamEventWriter w(std::span<std::byte>(buf).subspan(8));
  w.set_event_time(1789295134226000);
  w.set_transact_time(1789295134225000);
  w.set_price_exponent(-2);
  w.set_qty_exponent(-8);
  auto t = w.trades(1);
  t[0].set_id(id);
  t[0].set_price(7000010);
  t[0].set_qty(65000);
  t[0].set_is_buyer_maker(ss::boolEnum::False);
  static_cast<void>(w.set_symbol("BTCUSDT"));
  ss::TradesStreamEventWriter::header().store(buf.data());
  buf.resize(8 + w.size_bytes());
  return buf;
}

}  // namespace

TEST_CASE("binance.venue: md_format sbe reads binary frames from the SBE stream url") {
  Harness h;
  h.srv.on_ws_open("/sbe/stream", [&h](net::WsSession& s) {
    h.srv.record("sbe-query", std::string(s.query()));
    s.send_text(R"({"id":null,"result":null})");  // text on an SBE stream: ignored
    s.send_binary(sbe_depth(95, 100, 6999900));   // stale: u <= lastUpdateId
    s.send_binary(sbe_depth(101, 101, 7000000));
  });
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    BinanceVenueConfig cfg = h.config(true);  // dry run: market data only
    cfg.md_format = MdFormat::Sbe;
    cfg.sbe_ws_url = h.srv.ws_base() + "/sbe/stream";
    cfg.credentials.secret.value.clear();
    cfg.credentials.type = KeyType::Ed25519;  // the API key alone opens SBE streams
    BinanceVenue venue(VenueId{0}, std::move(cfg));
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);
    Collected mdc;
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      return venue.md_feed()->synced_count() == 1 && mdc.count(EventType::BookDelta) >= 1;
    }));
    REQUIRE(h.srv.frames("sbe-query").size() == 1);
    CHECK(h.srv.frames("sbe-query")[0] == "streams=btcusdt@depth/btcusdt@bestBidAsk/btcusdt@trade");
    CHECK(h.srv.open_count("/stream") == 0);  // the JSON stream is not used
    CHECK(mdc.count(EventType::BookSnapshot) == 1);
    CHECK(mdc.last<BookDeltaMsg>(EventType::BookDelta)->first_update_id == 101);
    CHECK(mdc.last<BookDeltaMsg>(EventType::BookDelta)->bids()[0].price ==
          Price::from_decimal("70000").value());
    h.srv.send_binary_to("/sbe/stream", sbe_trade(388512));
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      return mdc.count(EventType::Trade) == 1;
    }));
    CHECK(mdc.last<TradeMsg>(EventType::Trade)->trade_id == 388512);
    CHECK(mdc.last<TradeMsg>(EventType::Trade)->aggressor == Side::Buy);
    CHECK(venue.md_feed()->stats().malformed == 0);
    venue.disconnect();
  }
  h.srv.stop();
}

TEST_CASE("binance.venue: a shutdown during the execution replay opens no REST connection") {
  // disconnect() resets the REST channel, which fails the myTrades request in flight. That used to
  // finish the replay and release the open-order snapshot it held: a GET openOrders on a fresh
  // connection, from a connector that had just been told to stop.
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  BinanceVenue venue(VenueId{0}, h.config(false));
  REQUIRE(venue.load_reference_data(instruments));
  REQUIRE(symbols.build(instruments));
  venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
  const InstrumentId ids[] = {InstrumentId{0}};
  venue.subscribe(ids);
  venue.connect(reactor);
  Collected oc;
  REQUIRE(pump_until(reactor, [&] {
    oc.take(orders);
    return oc.count(EventType::Reconcile) >= 2;  // the start-up sweep
  }));
  h.hold_trades = true;
  const int asked = h.my_trades.load();
  venue.request_open_orders();
  REQUIRE(pump_until(reactor, [&] { return h.my_trades.load() == asked + 1; }));
  venue.disconnect();
  // The reply the reset aborted is nobody's: not a failed query (Spot had no generation on its
  // replay's replies and counted, logged and retried it).
  CHECK(venue.status().execution_query_errors == 0);
  h.hold_trades = false;
  static_cast<void>(pump_until(reactor, [] { return false; }, 500));
  CHECK(h.rest_open_orders.load() == 0);
  oc.take(orders);
  const std::size_t reconciles = oc.count(EventType::Reconcile);
  CHECK(reconciles == 2);  // nothing after the sweep
  h.srv.stop();
}

namespace {

// One row of GET /api/v3/myTrades (rest-api.md "Account trade list").
std::string my_trade(long long id, long long time_ms, long long order_id = 4293153) {
  return R"({"symbol":"BTCUSDT","id":)" + std::to_string(id) + R"(,"orderId":)" +
         std::to_string(order_id) +
         R"(,"orderListId":-1,"price":"70000.00000000","qty":"0.00010000","quoteQty":"7.00000000","commission":"0.00000010","commissionAsset":"BTC","time":)" +
         std::to_string(time_ms) + R"(,"isBuyer":true,"isMaker":true,"isBestMatch":true})";
}

// GET /api/v3/order's answer (rest-api.md "Query order").
std::string order_object(long long order_id, const char* client_order_id) {
  return R"({"symbol":"BTCUSDT","orderId":)" + std::to_string(order_id) +
         R"(,"orderListId":-1,"clientOrderId":")" + client_order_id +
         R"(","price":"70000.00000000","origQty":"0.00010000","executedQty":"0.00010000","cummulativeQuoteQty":"7.00000000","status":"FILLED","timeInForce":"GTC","type":"LIMIT_MAKER","side":"BUY","stopPrice":"0.00000000","icebergQty":"0.00000000","time":1789295199990,"updateTime":1789295199990,"isWorking":true,"workingTime":1789295199990,"origQuoteOrderQty":"0.00000000","selfTradePreventionMode":"NONE"})";
}

// The client order id order_object() gives order 555.
ClientOrderId looked_up_id() {
  const ClientOrderId id = decode_cl_ord_id("fm000500000007").value_or(ClientOrderId{});
  REQUIRE(id.valid());
  return id;
}

// The replayed fills in `c` with this trade id, in the order they came.
std::vector<const OrderFillMsg*> replayed(const Collected& c, std::string_view exec_id) {
  std::vector<const OrderFillMsg*> out;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) != EventType::OrderFill) continue;
    const auto& f = RecordingSink::as<OrderFillMsg>(m);
    if ((f.flags & OrderFillMsg::kReplayed) != 0 && f.exec_id.view() == exec_id) out.push_back(&f);
  }
  return out;
}

std::string query_param(const std::string& query, const std::string& key) {
  const std::size_t p = query.find(key + "=");
  if (p == std::string::npos) return {};
  const std::size_t v = p + key.size() + 1;
  return query.substr(v, query.find('&', v) - v);
}

// A connector on the harness, resumed from `since` with `known` booked, up to its start-up sweep.
struct Resumed {
  InstrumentTable instruments;
  RecordingSink md{8U << 20};
  RecordingSink orders{4U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<BinanceVenue> venue;
  Collected oc;

  Resumed(Harness& h, std::int64_t since, const std::vector<std::string>& known) {
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
    venue = std::make_unique<BinanceVenue>(VenueId{0}, h.config(false));
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue->subscribe(ids);
    venue->resume_executions(since, known);
    venue->connect(reactor);
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::Reconcile) >= 2;  // the start-up sweep's Begin and End
    }));
  }
  ~Resumed() {
    venue->disconnect();
    reactor.run_once(0);
  }
  [[nodiscard]] bool first_exact() const {
    const auto* b = oc.first_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
      return m.kind == ReconcileMsg::Kind::Begin;
    });
    return b != nullptr && (b->flags & ReconcileMsg::kExecutionsExact) != 0;
  }
};

}  // namespace

TEST_CASE("binance.venue: a full myTrades page is full by the rows returned, not forwarded") {
  // 1000 trades (the limit), one of them booked by the session resumed from, and the next page
  // cannot be read. Spot counted the trades it forwarded, 999, took the page for the last and
  // called the replay exact; USD-M counted the rows. Now the next page is asked for at once, from
  // the id after the last, and the replay is exact only once it is in.
  Harness h;
  const long long now = wall_now().ns / 1'000'000;
  std::string page = "[";
  for (int i = 0; i < 1000; ++i) page += (i > 0 ? "," : "") + my_trade(5000 + i, now - 1000);
  page += "]";
  {
    const std::lock_guard lock(h.trades_mu);
    h.trades_replies = {{200, page}, {503, "Service Unavailable"}};
  }
  {
    Resumed r(h, now - 60'000, {"5000"});
    CHECK_FALSE(r.first_exact());
    CHECK(r.oc.count(EventType::OrderFill) == 999);
    const auto q = h.srv.frames("myTrades");
    REQUIRE(q.size() >= 2);
    CHECK(query_param(q[0], "startTime") == std::to_string(now - 60'000));
    CHECK(query_param(q[1], "fromId") == "6000");
  }
  h.srv.stop();
}

TEST_CASE("binance.venue: a replay more than 24 hours back walks 24-hour windows") {
  // myTrades takes a range of 24 hours at most. Spot clamped an older start to 24 hours ago, so a
  // restart after a longer outage never asked for the trades before that (and said so: never
  // exact). The history itself is kept.
  Harness h;
  constexpr std::int64_t kDay = 24LL * 3600 * 1000;
  const std::int64_t since = wall_now().ns / 1'000'000 - 30LL * 3600 * 1000;
  {
    Resumed r(h, since, {});
    CHECK(r.first_exact());
    const auto q = h.srv.frames("myTrades");
    REQUIRE(q.size() >= 2);
    CHECK(query_param(q[0], "startTime") == std::to_string(since));
    CHECK(query_param(q[0], "endTime") == std::to_string(since + kDay - 1));
    CHECK(query_param(q[1], "startTime") == std::to_string(since + kDay));
    CHECK(query_param(q[1], "endTime").empty());
  }
  h.srv.stop();
}

TEST_CASE("binance.venue: a replayed trade of an order this process never saw acked names it") {
  // myTrades names the order by orderId only. An order a session placed and never heard back
  // about (it died first; behind fastmm-gateway, a gateway restarted since) is not in the orderId
  // map, so its fill named no order. The connector now asks GET /api/v3/order, once per order,
  // before the fill goes out; an order that is not FastMM's is asked for once.
  Harness h;
  const long long now = wall_now().ns / 1'000'000;
  {
    const std::lock_guard lock(h.trades_mu);
    h.trades_replies = {{200,
                         "[" + my_trade(7001, now - 2000, 555) + "," +
                             my_trade(7002, now - 1500, 555) + "," +
                             my_trade(7003, now - 1000, 556) + "]"}};
    h.order_replies = {{200, order_object(555, "fm000500000007")},
                       {200, order_object(556, "web_7f3a9c")}};
  }
  {
    Resumed r(h, now - 60'000, {});
    CHECK(r.first_exact());
    const auto a = replayed(r.oc, "7001");
    const auto b = replayed(r.oc, "7002");
    const auto c = replayed(r.oc, "7003");
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    REQUIRE(c.size() == 1);
    CHECK(a[0]->cl_ord_id == looked_up_id());
    CHECK(b[0]->cl_ord_id == looked_up_id());
    CHECK(a[0]->venue_order_id.view() == "555");
    CHECK_FALSE(c[0]->cl_ord_id.valid());  // someone else's order on the account, for good
    CHECK((c[0]->flags & OrderFillMsg::kUnresolved) == 0);
    const auto q = h.srv.frames("order");
    REQUIRE(q.size() == 2);
    CHECK(query_param(q[0], "orderId") == "555");
    CHECK(query_param(q[0], "symbol") == "BTCUSDT");
    CHECK(query_param(q[1], "orderId") == "556");
  }
  h.srv.stop();
}

TEST_CASE("binance.venue: a failed order lookup is asked again at the next replay") {
  // The lookup fails: the fill goes out naming no order (as before), and the retry 5 s later asks
  // again and sends the fill once more, naming its order; the engine keeps one of the two.
  Harness h;
  const long long now = wall_now().ns / 1'000'000;
  {
    const std::lock_guard lock(h.trades_mu);
    h.trades_replies = {{200, "[" + my_trade(7001, now - 2000, 555) + "]"}};
    h.order_replies = {{503, "Service Unavailable"}, {200, order_object(555, "fm000500000007")}};
  }
  {
    Resumed r(h, now - 60'000, {});
    CHECK(r.first_exact());  // every execution was read
    {
      const auto first = replayed(r.oc, "7001");
      REQUIRE(first.size() == 1);
      CHECK_FALSE(first[0]->cl_ord_id.valid());
      CHECK((first[0]->flags & OrderFillMsg::kUnresolved) != 0);  // to be sent again
    }
    REQUIRE(pump_until(
        r.reactor,
        [&] {
          r.oc.take(r.orders);
          return replayed(r.oc, "7001").size() == 2;
        },
        10'000));
    const auto both = replayed(r.oc, "7001");
    CHECK(both[1]->cl_ord_id == looked_up_id());
    CHECK(both[1]->venue_order_id.view() == "555");
    CHECK((both[1]->flags & OrderFillMsg::kUnresolved) == 0);
    CHECK(h.srv.frames("order").size() == 2);
  }
  h.srv.stop();
}

TEST_CASE("binance.venue: with the order table full an order or replace is refused, not sent") {
  // Before, the order went out untracked: its reply carried no instrument and a replace of it was
  // refused as unknown; a replace that found no room went out and left its new order untracked.
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(4U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    BinanceVenue venue(VenueId{0}, h.config(false));
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue.subscribe(ids);
    venue.connect(reactor);
    Collected oc;
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::Reconcile) >= 2;  // the start-up sweep, over the order connection
    }));
    constexpr std::size_t kRoom = kShadowSlots - kShadowSlots / 8;
    const auto id_of = [](std::size_t k) {
      return make_cl_ord_id(2, static_cast<std::uint32_t>(k));
    };
    const auto push_new = [&](ClientOrderId id) {
      OutNewOrderMsg n{};
      init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
      n.cl_ord_id = id;
      n.side = Side::Buy;
      n.type = OrderType::PostOnly;
      n.price = Price::from_decimal("70000").value();
      n.qty = Qty::from_decimal("0.001").value();
      REQUIRE(outbound.try_push(&n, n.hdr.len));
    };
    // Acks from the replies (the fake's execution reports name another order).
    std::size_t acked = 0;
    const auto count_acks = [&] {
      oc.take(orders);
      acked = 0;
      for (const auto& m : oc.all) {
        if (RecordingSink::type_of(m) == EventType::OrderAck &&
            cl_ord_id_epoch(RecordingSink::as<OrderAckMsg>(m).cl_ord_id) == 2)
          ++acked;
      }
    };
    // In batches under the fixture's 50 orders per 10 s (each reply resets the count to 1).
    for (std::size_t k = 1; k <= kRoom; k += 40) {
      const std::size_t end = std::min(kRoom + 1, k + 40);
      for (std::size_t j = k; j < end; ++j) push_new(id_of(j));
      venue.on_wake();
      REQUIRE(pump_until(reactor, [&] {
        count_acks();
        return acked == end - 1;
      }));
    }
    REQUIRE(venue.shadow_count() == kRoom);

    push_new(id_of(kRoom + 1));
    OutReplaceMsg r{};
    init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
    r.cl_ord_id = id_of(kRoom + 2);
    r.orig_cl_ord_id = id_of(1);
    r.venue_order_id.assign("4293153");
    r.price = Price::from_decimal("70001").value();
    r.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&r, r.hdr.len));
    venue.on_wake();
    const auto refused = [&](ClientOrderId id) {
      return oc.first_if<OrderRejectMsg>(EventType::OrderReject, [&](const OrderRejectMsg& m) {
        return m.cl_ord_id == id && m.reason == RejectReason::OrderTableFull;
      });
    };
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return refused(id_of(kRoom + 1)) != nullptr && refused(id_of(kRoom + 2)) != nullptr;
    }));
    CHECK(refused(id_of(kRoom + 1))->hdr.instrument == InstrumentId{0});

    // The snapshot names none of them: swept, and the next order goes. Its reply comes after every
    // frame sent before it on the order connection.
    const std::size_t reconciles = oc.count(EventType::Reconcile);
    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::Reconcile) == reconciles + 2;
    }));
    CHECK(venue.shadow_count() == 0);
    push_new(id_of(kRoom + 3));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      count_acks();
      return acked == kRoom + 1;
    }));
    std::size_t places = 0;
    std::size_t replaces = 0;
    for (const auto& f : h.srv.frames("/ws-api/v3")) {
      places += f.find("\"order.place\"") != std::string::npos ? 1U : 0U;
      replaces += f.find("\"order.cancelReplace\"") != std::string::npos ||
                          f.find("\"order.amend") != std::string::npos
                      ? 1U
                      : 0U;
    }
    CHECK(places == kRoom + 1);
    CHECK(replaces == 0);
    venue.on_timer(net::Reactor::now_ns());
    CHECK(venue.status().shadows_refused == 2);
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}
