// BybitVenue with category = "linear" against a scripted fake Bybit v5: reference data for a
// perpetual, the position-mode check at start-up, the start-up sweep's positions, an order round
// trip, a fill missed on the private stream booked from execution/list, the position topic
// correcting the engine, and disconnect-cancel-all armed for derivatives. Wire formats as in
// bybit_linear_test.cpp (Bybit v5 docs read 2026-09-26); nothing here has met the real venue.
#include "fake_venue_util.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/bybit/bybit_venue.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kKey = "fake-key";
constexpr const char* kSecret = "fake-secret";
constexpr VenueId kVenue{1};
const InstrumentId kBtc{0};
constexpr long long kT = 1789299703000;

bool auth_ok(std::string_view t) {
  const std::string head = std::string(R"("args":[")") + kKey + "\",";
  const std::size_t p = t.find(head);
  if (p == std::string_view::npos) return false;
  const std::size_t e0 = p + head.size();
  const std::size_t comma = t.find(',', e0);
  const std::string_view expires = t.substr(e0, comma - e0);
  const std::size_t s0 = t.find('"', comma) + 1;
  const std::string_view sig = t.substr(s0, t.find('"', s0) - s0);
  return sig == net::hmac_sha256_hex(kSecret, "GET/realtime" + std::string(expires)).view();
}

bool rest_signed(const net::HttpRequest& r, std::string_view payload) {
  const std::string pre = std::string(r.header("X-BAPI-TIMESTAMP")) +
                          std::string(r.header("X-BAPI-API-KEY")) +
                          std::string(r.header("X-BAPI-RECV-WINDOW")) + std::string(payload);
  return r.header("X-BAPI-API-KEY") == kKey &&
         r.header("X-BAPI-SIGN") == net::hmac_sha256_hex(kSecret, pre).view();
}

std::string private_order(const char* link, const char* status, const char* cum) {
  return std::string(
             R"({"id":"x","topic":"order","creationTime":1789299700474,"data":[{"category":"linear","symbol":"BTCUSDT","orderId":"9aac161b-8ed6-450d-9cab-c5cc67c21784","orderLinkId":")") +
         link +
         R"(","side":"Sell","positionIdx":0,"orderType":"Limit","cancelType":"UNKNOWN","price":"60000.1","qty":"0.015","timeInForce":"PostOnly","orderStatus":")" +
         status + R"(","leavesQty":"0.015","cumExecQty":")" + cum +
         R"(","cumExecValue":"0","avgPrice":"","cumExecFee":"0","reduceOnly":false,"createdTime":"1789299700444","updatedTime":"1789299700457","rejectReason":"EC_NoError"}]})";
}

std::string private_execution(const char* id, const char* qty, const char* leaves) {
  return std::string(
             R"({"topic":"execution","id":"e1","creationTime":1789299703460,"data":[{"category":"linear","symbol":"BTCUSDT","execFee":"-0.0012","execId":")") +
         id + R"(","execPrice":"60000.1","execQty":")" + qty +
         R"(","execType":"Trade","execValue":"600.001","feeRate":"-0.00002","orderId":"9aac161b-8ed6-450d-9cab-c5cc67c21784","orderLinkId":"fm000100000001","orderPrice":"60000.1","orderQty":"0.015","side":"Sell","leavesQty":")" +
         leaves +
         R"(","execTime":"1789299703453","isMaker":true,"seq":140612148849390,"feeCurrency":""}]})";
}

std::string position_frame(const char* side, const char* size) {
  return std::string(
             R"({"id":"p1","topic":"position","creationTime":1789299703500,"data":[{"positionIdx":0,"symbol":"BTCUSDT","side":")") +
         side + R"(","size":")" + size +
         R"(","entryPrice":"60000.1","category":"linear","positionStatus":"Normal","updatedTime":"1789299703499","seq":140612148849390}]})";
}

// One row of GET /v5/execution/list for category=linear (a maker sell, rebate in USDT).
std::string exec_row(const char* id, const char* link, long long time_ms) {
  return std::string(
             R"({"symbol":"BTCUSDT","orderId":"9aac161b-8ed6-450d-9cab-c5cc67c21784","orderLinkId":")") +
         link +
         R"(","side":"Sell","orderPrice":"60000.1","orderQty":"0.015","leavesQty":"0.005","createType":"CreateByUser","orderType":"Limit","stopOrderType":"","execFee":"-0.0012","execId":")" +
         id +
         R"(","execPrice":"60000.1","execQty":"0.01","execType":"Trade","execValue":"600.001","execTime":")" +
         std::to_string(time_ms) +
         R"(","feeCurrency":"","isMaker":true,"feeRate":"-0.00002","markPrice":"60001","closedSize":"0","seq":1})";
}

std::string exec_page(const std::vector<std::string>& rows) {
  std::string list;
  for (const std::string& r : rows) list += (list.empty() ? "" : ",") + r;
  return R"({"retCode":0,"retMsg":"OK","result":{"nextPageCursor":"","category":"linear","list":[)" +
         list + R"(]},"retExtInfo":{},"time":1789299704000})";
}

constexpr const char* kNoOpenOrders =
    R"({"retCode":0,"retMsg":"OK","result":{"list":[],"nextPageCursor":"","category":"linear"},"retExtInfo":{},"time":1789299704000})";

struct Harness {
  FakeVenueServer srv;
  std::string instruments_info = fastmm::test::fixture("bybit/linear_instruments_info.json");
  std::string server_time = fastmm::test::fixture("bybit/server_time.json");
  std::string snapshot = fastmm::test::fixture("bybit/orderbook50_snapshot.json");
  // GET /v5/position/list by symbol (the start-up mode check) and by settle coin (reconciliation).
  std::string mode_reply = fastmm::test::fixture("bybit/linear_position_list.json");
  std::string positions_reply = fastmm::test::fixture("bybit/linear_position_list.json");
  std::string open_orders = kNoOpenOrders;
  std::mutex mu;
  std::string execution_page = exec_page({});
  std::atomic<int> signed_ok{0};
  std::atomic<int> signed_bad{0};
  std::atomic<int> dcp_ok{0};
  std::atomic<int> cancel_all_ok{0};
  std::atomic<int> auth_failures{0};
  net::WsSession* private_session = nullptr;  // server thread only

  void count_signature(const net::HttpRequest& r, std::string_view payload) {
    ++(rest_signed(r, payload) ? signed_ok : signed_bad);
  }

  Harness() {
    srv.route("GET", "/v5/market/time", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, server_time);
    });
    srv.route("GET", "/v5/market/instruments-info", [this](const net::HttpRequest& r) {
      srv.record("instruments", std::string(r.query));
      return net::HttpServerResponse::json(200, instruments_info);
    });
    srv.route("GET", "/v5/position/list", [this](const net::HttpRequest& r) {
      count_signature(r, r.query);
      srv.record("positions", std::string(r.query));
      const std::lock_guard lock(mu);
      const bool by_symbol = r.query.find("symbol=") != std::string_view::npos;
      return net::HttpServerResponse::json(200, by_symbol ? mode_reply : positions_reply);
    });
    srv.route("GET", "/v5/order/realtime", [this](const net::HttpRequest& r) {
      count_signature(r, r.query);
      srv.record("open_orders", std::string(r.query));
      const std::lock_guard lock(mu);
      return net::HttpServerResponse::json(200, open_orders);
    });
    srv.route("GET", "/v5/execution/list", [this](const net::HttpRequest& r) {
      count_signature(r, r.query);
      srv.record("executions", std::string(r.query));
      const std::lock_guard lock(mu);
      return net::HttpServerResponse::json(200, execution_page);
    });
    srv.route("POST", "/v5/order/disconnected-cancel-all", [this](const net::HttpRequest& r) {
      srv.record("dcp", std::string(r.body));
      if (rest_signed(r, r.body)) ++dcp_ok;
      return net::HttpServerResponse::json(
          200,
          R"({"retCode":0,"retMsg":"success","result":{},"retExtInfo":{},"time":1789299700000})");
    });
    srv.route("POST", "/v5/order/cancel-all", [this](const net::HttpRequest& r) {
      srv.record("cancel_all", std::string(r.body));
      if (rest_signed(r, r.body)) ++cancel_all_ok;
      return net::HttpServerResponse::json(
          200,
          R"({"retCode":0,"retMsg":"OK","result":{"list":[],"success":"1"},"retExtInfo":{},"time":1789299704000})");
    });
    srv.on_ws_text("/v5/public/linear", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      if (op == "subscribe") {
        s.send_text(R"({"success":true,"ret_msg":"subscribe","conn_id":"c1","req_id":")" +
                    json_str(t, "req_id") + R"(","op":"subscribe"})");
        if (t.find("orderbook.50.BTCUSDT") != std::string_view::npos) s.send_text(snapshot);
      }
    });
    srv.on_ws_text("/v5/private", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      if (op == "auth") {
        const bool ok = auth_ok(t);
        if (!ok) ++auth_failures;
        s.send_text(std::string(R"({"success":)") + (ok ? "true" : "false") +
                    R"(,"ret_msg":"","op":"auth","conn_id":"p1"})");
      } else if (op == "subscribe") {
        private_session = &s;
        srv.record("private_subscribe", std::string(t));
        s.send_text(R"({"success":true,"ret_msg":"","op":"subscribe","conn_id":"p1","req_id":")" +
                    json_str(t, "req_id") + R"("})");
      }
    });
    srv.on_ws_text("/v5/trade", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      const std::string req = json_str(t, "reqId");
      if (op == "auth") {
        s.send_text(auth_ok(t) ? R"({"retCode":0,"retMsg":"OK","op":"auth","connId":"t1"})"
                               : R"({"retCode":10004,"retMsg":"Invalid sign","op":"auth"})");
        return;
      }
      if (op != "order.create" && op != "order.amend" && op != "order.cancel") return;
      srv.record("trade", std::string(t));
      s.send_text(
          R"({"reqId":")" + req + R"(","retCode":0,"retMsg":"OK","op":")" + op +
          R"(","data":{"orderId":"9aac161b-8ed6-450d-9cab-c5cc67c21784","orderLinkId":"fm000100000001"},"retExtInfo":{},"header":{"X-Bapi-Limit":"10","X-Bapi-Limit-Status":"9","X-Bapi-Limit-Reset-Timestamp":"1789299700208"},"connId":"t1"})");
      if (private_session == nullptr) return;
      if (op == "order.create") {
        private_session->send_text(private_order("fm000100000001", "New", "0"));
        private_session->send_text(private_execution("ex-l1", "0.005", "0.01"));
      } else if (op == "order.cancel") {
        private_session->send_text(private_order("fm000100000001", "Cancelled", "0.005"));
      }
    });
    srv.start();
  }

  VenueSection section(bool with_keys = true) const {
    VenueSection s;
    s.name = "fake-bybit-linear";
    s.kind = "bybit";
    s.ws_url = srv.ws_base() + "/v5/public/linear";
    s.ws_api_url = srv.ws_base() + "/v5/trade";
    s.rest_url = srv.http_base();
    s.extra["category"] = "linear";
    s.extra["ws_private_url"] = srv.ws_base() + "/v5/private";
    if (with_keys) {
      s.api_key = kKey;
      s.api_secret = kSecret;
    }
    s.supports_replace = true;
    return s;
  }
};

// A BTCUSDT perpetual configured as a spot instrument with a USDC quote: reference data has to
// make it a USDT-settled perpetual.
Instrument configured_btcusdt() {
  Instrument i = make_instrument("BTCUSDT", 1, "", "USDC");
  return i;
}

struct Live {
  InstrumentTable instruments;
  RecordingSink md{8U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<BybitVenue> venue;
  Collected oc;

  Live(Harness& h,
       const VenueSection& section,
       const std::function<void(BybitVenue&)>& before_connect = {}) {
    REQUIRE(instruments.add(configured_btcusdt()));
    venue = std::make_unique<BybitVenue>(kVenue, make_bybit_config(section, false));
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kBtc};
    venue->subscribe(ids);
    if (before_connect) before_connect(*venue);
    venue->connect(reactor);
    static_cast<void>(h);
  }
  ~Live() {
    venue->disconnect();
    reactor.run_once(0);
  }
  // The start-up sweep is the connector's own first reconciliation: wait for its End.
  void wait_for_sweep() {
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.first_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
        return m.kind == ReconcileMsg::Kind::End;
      }) != nullptr;
    }));
  }
  std::vector<const ReconcileMsg*> positions() const {
    std::vector<const ReconcileMsg*> out;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) == EventType::Reconcile &&
          RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::Position)
        out.push_back(&RecordingSink::as<ReconcileMsg>(m));
    }
    return out;
  }
};

}  // namespace

TEST_CASE("bybit_linear.venue: config takes the category and rejects an unknown one") {
  VenueSection s;
  s.name = "bybit";
  s.ws_url = "wss://stream-testnet.bybit.com/v5/public/linear";
  s.rest_url = "https://api-testnet.bybit.com";
  CHECK(make_bybit_config(s, true).category == BybitCategory::Spot);  // default: unchanged
  s.extra["category"] = "linear";
  const BybitVenueConfig c = make_bybit_config(s, true);
  CHECK(c.category == BybitCategory::Linear);
  CHECK(c.ws_private_url == "wss://stream-testnet.bybit.com/v5/private");
  CHECK(c.ws_trade_url == "wss://stream-testnet.bybit.com/v5/trade");
  CHECK(c.position_from_stream);
  s.extra["category"] = "inverse";
  CHECK_THROWS_AS(static_cast<void>(make_bybit_config(s, true)), std::invalid_argument);
}

TEST_CASE("bybit_linear.venue: reference data makes a USDT-settled perpetual") {
  Harness h;
  {
    InstrumentTable instruments;
    REQUIRE(instruments.add(configured_btcusdt()));
    BybitVenue venue(kVenue, make_bybit_config(h.section(), false));
    REQUIRE(venue.load_reference_data(instruments));
    const Instrument& in = instruments.get(kBtc);
    CHECK(in.asset_class == AssetClass::Perpetual);
    CHECK(in.contract_multiplier == Qty::from_int(1));
    CHECK_FALSE(in.inverse());
    CHECK((in.flags & Instrument::kReduceOnlySupported) != 0);
    CHECK(in.enabled());
    CHECK(in.tick == Price::from_decimal("0.1").value());
    CHECK(in.lot == Qty::from_decimal("0.001").value());
    CHECK(in.min_qty == Qty::from_decimal("0.001").value());
    CHECK(in.max_qty == Qty::from_int(1190));
    CHECK(in.min_notional == Notional::from_int(5));
    CHECK(in.base.view() == "BTC");
    CHECK(in.quote.view() == "USDT");  // the settle coin, not the configured USDC
    CHECK(in.settlement_ccy() == "USDT");
    const auto q = h.srv.frames("instruments");
    REQUIRE(q.size() == 1);
    CHECK(q[0] == "category=linear&symbol=BTCUSDT");
    // With keys, the position mode is read per symbol, signed.
    const auto p = h.srv.frames("positions");
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "category=linear&symbol=BTCUSDT&limit=200");
    CHECK(h.signed_ok.load() == 1);
    CHECK(h.signed_bad.load() == 0);
    CHECK_FALSE(venue.refused_account_settings());
  }
  // A dry run has no keys to ask with.
  {
    InstrumentTable instruments;
    REQUIRE(instruments.add(configured_btcusdt()));
    BybitVenue venue(kVenue, make_bybit_config(h.section(false), true));
    REQUIRE(venue.load_reference_data(instruments));
    CHECK(h.srv.frames("positions").size() == 1);
  }
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: a contract that is not a perpetual is refused") {
  Harness h;
  const std::string perp = "\"LinearPerpetual\"";
  h.instruments_info.replace(h.instruments_info.find(perp), perp.size(), "\"LinearFutures\"");
  InstrumentTable instruments;
  REQUIRE(instruments.add(configured_btcusdt()));
  BybitVenue venue(kVenue, make_bybit_config(h.section(), false));
  const auto r = venue.load_reference_data(instruments);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("LinearFutures") != std::string::npos);
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: a symbol in hedge mode is refused at start-up") {
  Harness h;
  h.mode_reply = fastmm::test::fixture("bybit/linear_position_list_hedge.json");
  InstrumentTable instruments;
  REQUIRE(instruments.add(configured_btcusdt()));
  BybitVenue venue(kVenue, make_bybit_config(h.section(), false));
  const auto r = venue.load_reference_data(instruments);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("hedge mode") != std::string::npos);
  // fastmm-live exits 3 for this, not 4: retrying does not help.
  CHECK(venue.refused_account_settings());

  // A mode that cannot be read is refused as well.
  Harness h2;
  h2.mode_reply =
      R"({"retCode":10003,"retMsg":"API key is invalid.","result":{},"retExtInfo":{},"time":1789299704000})";
  InstrumentTable instruments2;
  REQUIRE(instruments2.add(configured_btcusdt()));
  BybitVenue venue2(kVenue, make_bybit_config(h2.section(), false));
  const auto r2 = venue2.load_reference_data(instruments2);
  REQUIRE_FALSE(r2);
  CHECK(r2.error().find("10003") != std::string::npos);
  CHECK(venue2.refused_account_settings());
  h.srv.stop();
  h2.srv.stop();
}

TEST_CASE("bybit_linear.venue: the start-up sweep reports the venue position") {
  Harness h;
  {
    Live l(h, h.section());
    l.wait_for_sweep();
    const auto pos = l.positions();
    REQUIRE(pos.size() == 1);
    CHECK(pos[0]->hdr.instrument == kBtc);
    CHECK(pos[0]->hdr.venue == kVenue);
    CHECK(pos[0]->position_qty == Qty::from_decimal("-0.015").value());
    CHECK(pos[0]->avg_px == Price::from_decimal("60123.45").value());
    // Both lists are asked for by settle coin, signed, after the execution replay.
    const auto oo = h.srv.frames("open_orders");
    REQUIRE(oo.size() == 1);
    CHECK(oo[0] == "category=linear&settleCoin=USDT&limit=50");
    const auto p = h.srv.frames("positions");
    REQUIRE(p.size() == 2);  // the mode check, then the sweep
    CHECK(p[1] == "category=linear&settleCoin=USDT&limit=200");
    const auto ex = h.srv.frames("executions");
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].rfind("category=linear&startTime=", 0) == 0);
    CHECK(h.signed_bad.load() == 0);

    // A flat account lists nothing by settle coin: the next reconciliation reports zero.
    {
      const std::lock_guard lock(h.mu);
      h.positions_reply = kNoOpenOrders;
    }
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.positions().size() == 2;
    }));
    CHECK(l.positions()[1]->position_qty.is_zero());
  }
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: a failed position query emits no snapshot") {
  Harness h;
  h.positions_reply =
      R"({"retCode":10006,"retMsg":"Too many visits!","result":{},"retExtInfo":{},"time":1789299704000})";
  {
    Live l(h, h.section());
    REQUIRE(pump_until(l.reactor, [&] { return h.srv.frames("positions").size() == 2; }));
    for (int i = 0; i < 40; ++i) l.reactor.run_once(5);
    l.oc.take(l.orders);
    // Oms::reconcile_end() would cancel what the snapshot does not name: nothing goes out.
    CHECK(l.oc.count(EventType::Reconcile) == 0);
  }
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: order round trip with positionIdx, reduceOnly and fills") {
  Harness h;
  {
    Live l(h, h.section());
    l.wait_for_sweep();
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kBtc, kVenue);
    n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
    n.side = Side::Sell;
    n.type = OrderType::PostOnly;
    n.reduce_only = 1;
    n.price = Price::from_decimal("60000.1").value();
    n.qty = Qty::from_decimal("0.015").value();
    REQUIRE(l.outbound.try_push(&n, n.hdr.len));
    l.venue->on_wake();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderAck) >= 2 && l.oc.count(EventType::OrderFill) == 1;
    }));
    const auto frames = h.srv.frames("trade");
    REQUIRE(frames.size() == 1);
    CHECK(
        frames[0].find(
            R"("args":[{"category":"linear","symbol":"BTCUSDT","side":"Sell","orderType":"Limit","qty":"0.015","price":"60000.1","timeInForce":"PostOnly","orderLinkId":"fm000100000001","positionIdx":0,"reduceOnly":true}])") !=
        std::string::npos);
    CHECK(l.oc.last<OrderAckMsg>(EventType::OrderAck)->cl_ord_id == n.cl_ord_id);
    const auto* f = l.oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(f->cl_ord_id == n.cl_ord_id);
    CHECK(f->side == Side::Sell);
    CHECK(f->qty == Qty::from_decimal("0.005").value());
    CHECK(f->cum_qty == Qty::from_decimal("0.005").value());
    CHECK(f->fee == Notional::from_decimal("-0.0012").value());
    CHECK(f->fee_asset == FeeAsset::Quote);  // USDT, the settle coin
    CHECK(f->liquidity == Liquidity::Maker);

    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kBtc, kVenue);
    c.cl_ord_id = n.cl_ord_id;
    c.venue_order_id.assign("9aac161b-8ed6-450d-9cab-c5cc67c21784");
    REQUIRE(l.outbound.try_push(&c, c.hdr.len));
    l.venue->on_wake();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderCancelAck) == 1;
    }));
    CHECK(l.oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck)->cum_qty ==
          Qty::from_decimal("0.005").value());
    CHECK(h.srv.frames("trade")[1].find(R"("op":"order.cancel","args":[{"category":"linear")") !=
          std::string::npos);

    CHECK(l.venue->cancel_all());
    CHECK(h.cancel_all_ok.load() == 1);
    const auto ca = h.srv.frames("cancel_all");
    REQUIRE(ca.size() == 1);
    CHECK(ca[0] == R"({"category":"linear","symbol":"BTCUSDT"})");
    CHECK_FALSE(l.venue->fatal());
  }
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: a fill the private stream missed is booked from execution/list") {
  Harness h;
  h.execution_page = exec_page({exec_row("ex-missed", "fm000100000001", kT)});
  {
    Live l(h, h.section());
    l.wait_for_sweep();
    const auto* f =
        l.oc.first_if<OrderFillMsg>(EventType::OrderFill, [](const OrderFillMsg&) { return true; });
    REQUIRE(f != nullptr);
    CHECK((f->flags & OrderFillMsg::kReplayed) != 0);
    CHECK(f->exec_id.view() == "ex-missed");
    CHECK(f->cl_ord_id == decode_cl_ord_id("fm000100000001").value());
    CHECK(f->side == Side::Sell);
    CHECK(f->qty == Qty::from_decimal("0.01").value());
    CHECK(f->fee == Notional::from_decimal("-0.0012").value());
    CHECK(f->fee_asset == FeeAsset::Quote);  // the spot rule would say Quote for a sell too
    CHECK(f->hdr.exch_ts == Timestamp{kT * 1'000'000});
    // Booked before the snapshot, whose Begin says the replay was complete.
    const auto* begin = l.oc.first_if<ReconcileMsg>(
        EventType::Reconcile,
        [](const ReconcileMsg& m) { return m.kind == ReconcileMsg::Kind::Begin; });
    REQUIRE(begin != nullptr);
    CHECK((begin->flags & ReconcileMsg::kExecutionsExact) != 0);
    std::size_t fill_at = 0;
    std::size_t begin_at = 0;
    for (std::size_t i = 0; i < l.oc.all.size(); ++i) {
      const auto t = RecordingSink::type_of(l.oc.all[i]);
      if (t == EventType::OrderFill) fill_at = i;
      if (t == EventType::Reconcile && begin_at == 0) begin_at = i;
    }
    CHECK(fill_at < begin_at);
    const auto ex = h.srv.frames("executions");
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].rfind("category=linear&startTime=", 0) == 0);
  }
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: the position topic corrects the engine only when it differs") {
  Harness h;
  {
    Live l(h, h.section());
    l.wait_for_sweep();  // tracked = venue = -0.015
    const std::size_t before = l.oc.count(EventType::PositionUpdate);
    // The venue agrees with the fills: nothing to correct.
    h.srv.send_to("/v5/private", position_frame("Sell", "0.015"));
    for (int i = 0; i < 20; ++i) l.reactor.run_once(5);
    l.venue->on_timer(net::Reactor::now_ns() + 2'000'000'000);
    l.oc.take(l.orders);
    CHECK(l.oc.count(EventType::PositionUpdate) == before);
    // A fill the engine books moves both sides alike: -0.015 - 0.005 on each, nothing to correct.
    h.srv.send_to("/v5/private", private_execution("ex-p1", "0.005", "0.01"));
    h.srv.send_to("/v5/private", position_frame("Sell", "0.02"));
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderFill) == 1;
    }));
    for (int i = 0; i < 20; ++i) l.reactor.run_once(5);
    l.venue->on_timer(net::Reactor::now_ns() + 2'000'000'000);
    l.oc.take(l.orders);
    CHECK(l.oc.count(EventType::PositionUpdate) == before);
    // A liquidation or another client moved the position: the engine takes the venue's.
    h.srv.send_to("/v5/private", position_frame("Buy", "0.004"));
    for (int i = 0; i < 20; ++i) l.reactor.run_once(5);
    l.venue->on_timer(net::Reactor::now_ns());  // not settled yet
    l.oc.take(l.orders);
    CHECK(l.oc.count(EventType::PositionUpdate) == before);
    l.venue->on_timer(net::Reactor::now_ns() + 2'000'000'000);
    l.oc.take(l.orders);
    REQUIRE(l.oc.count(EventType::PositionUpdate) == before + 1);
    const auto* p = l.oc.last<PositionUpdateMsg>(EventType::PositionUpdate);
    CHECK(p->hdr.instrument == kBtc);
    CHECK(p->qty == Qty::from_decimal("0.004").value());
    CHECK(p->avg_px == Price::from_decimal("60000.1").value());
    // Once corrected, the same value does not correct again.
    h.srv.send_to("/v5/private", position_frame("Buy", "0.004"));
    for (int i = 0; i < 20; ++i) l.reactor.run_once(5);
    l.venue->on_timer(net::Reactor::now_ns() + 2'000'000'000);
    l.oc.take(l.orders);
    CHECK(l.oc.count(EventType::PositionUpdate) == before + 1);
  }
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: a hedge-mode position during the session stops new orders") {
  Harness h;
  {
    Live l(h, h.section());
    l.wait_for_sweep();
    CHECK_FALSE(l.venue->fatal());
    h.srv.send_to(
        "/v5/private",
        R"({"id":"p1","topic":"position","creationTime":1789299703500,"data":[{"positionIdx":1,"symbol":"BTCUSDT","side":"Buy","size":"0.01","entryPrice":"60000.1","category":"linear","updatedTime":"1789299703499"}]})");
    REQUIRE(pump_until(l.reactor, [&] { return l.venue->fatal(); }));
    l.oc.take(l.orders);
    const ControlMsg* kill = l.oc.last<ControlMsg>(EventType::Control);
    REQUIRE(kill != nullptr);
    CHECK(kill->command == ControlCommand::TripVenueKill);
  }
  h.srv.stop();
}

TEST_CASE("bybit_linear.venue: disconnect-cancel-all is armed for derivatives") {
  Harness h;
  {
    VenueSection s = h.section();
    s.extra["dead_mans_switch_s"] = "30";
    Live l(h, s);
    REQUIRE(pump_until(l.reactor, [&] { return h.dcp_ok.load() == 1; }));
    const auto dcp = h.srv.frames("dcp");
    REQUIRE(dcp.size() == 1);
    CHECK(dcp[0] == R"({"product":"DERIVATIVES","timeWindow":30})");
    const auto subs = h.srv.frames("private_subscribe");
    REQUIRE_FALSE(subs.empty());
    CHECK(
        subs[0] ==
        R"({"req_id":"private","op":"subscribe","args":["order","execution","position","dcp.future"]})");
  }
  h.srv.stop();
  // Without the switch: no dcp topic and no wallet, the position topic instead.
  Harness h2;
  {
    Live l(h2, h2.section());
    l.wait_for_sweep();
    const auto subs = h2.srv.frames("private_subscribe");
    REQUIRE_FALSE(subs.empty());
    CHECK(subs[0] ==
          R"({"req_id":"private","op":"subscribe","args":["order","execution","position"]})");
    CHECK(h2.srv.frames("dcp").empty());
  }
  h2.srv.stop();
}
