// BybitVenue against a scripted in-process fake Bybit v5 (REST + public/private/trade
// WebSockets): reference data, subscribe + snapshot/delta sync, u == 1 resubscribe, auth
// signatures on both private channels, order.create -> ack + order New, order.amend with the
// orderLinkId alias, execution fill, order.cancel -> Cancelled, open-order reconciliation
// the blocking kill-switch cancel_all and the execution replay (GET /v5/execution/list).
#include "fastmm/venues/bybit/bybit_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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

// {"op":"auth","args":["key",expires,"sig"]}
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

// X-BAPI-SIGN == HMAC(secret, timestamp + key + recv_window + payload)
bool rest_signed(const net::HttpRequest& r, std::string_view payload) {
  const std::string pre = std::string(r.header("X-BAPI-TIMESTAMP")) +
                          std::string(r.header("X-BAPI-API-KEY")) +
                          std::string(r.header("X-BAPI-RECV-WINDOW")) + std::string(payload);
  return r.header("X-BAPI-API-KEY") == kKey &&
         r.header("X-BAPI-SIGN") == net::hmac_sha256_hex(kSecret, pre).view();
}

std::string private_order(const char* link, const char* status, const char* cum) {
  return std::string(
             R"({"id":"x","topic":"order","creationTime":1789299700474,"data":[{"category":"spot","symbol":"BTCUSDT","orderId":"2012345678901234567","orderLinkId":")") +
         link +
         R"(","side":"Buy","orderType":"Limit","cancelType":"UNKNOWN","price":"60000.1","qty":"0.001","timeInForce":"PostOnly","orderStatus":")" +
         status + R"(","leavesQty":"0.001","cumExecQty":")" + cum +
         R"(","cumExecValue":"0","avgPrice":"","cumExecFee":"0","createdTime":"1789299700444","updatedTime":"1789299700457","rejectReason":"EC_NoError"}]})";
}

// One row of GET /v5/execution/list (a spot maker buy of 0.0004 BTC, fee in BTC).
std::string exec_row(const char* id,
                     const char* link,
                     const char* price,
                     const char* fee,
                     long long time_ms,
                     const char* type = "Trade",
                     const char* symbol = "BTCUSDT") {
  return std::string(R"({"symbol":")") + symbol +
         R"(","orderId":"2012345678901234567","orderLinkId":")" + link +
         R"(","side":"Buy","orderPrice":"60000.1","orderQty":"0.001","leavesQty":"0",)"
         R"("orderType":"Limit","stopOrderType":"","execFee":")" +
         fee + R"(","feeCurrency":"BTC","execId":")" + id + R"(","execPrice":")" + price +
         R"(","execQty":"0.0004","execType":")" + type + R"(","execValue":"24","execTime":")" +
         std::to_string(time_ms) +
         R"(","isMaker":true,"feeRate":"0.001","markPrice":"","closedSize":"","seq":1})";
}

std::string exec_page(const std::vector<std::string>& rows, const char* cursor) {
  std::string list;
  for (const std::string& r : rows) list += (list.empty() ? "" : ",") + r;
  return R"({"retCode":0,"retMsg":"OK","result":{"nextPageCursor":")" + std::string(cursor) +
         R"(","category":"spot","list":[)" + list + R"(]},"retExtInfo":{},"time":1789299704000})";
}

struct Harness {
  FakeVenueServer srv;
  std::string instruments_info = fastmm::test::fixture("bybit/instruments_info.json");
  std::string server_time = fastmm::test::fixture("bybit/server_time.json");
  std::string snapshot = fastmm::test::fixture("bybit/orderbook50_snapshot.json");
  std::string delta = fastmm::test::fixture("bybit/orderbook50_delta.json");
  std::string top = fastmm::test::fixture("bybit/orderbook1_snapshot.json");
  std::string trade = fastmm::test::fixture("bybit/public_trade.json");
  // One entry per GET /v5/order/realtime, in request order; the last one repeats.
  std::vector<std::string> open_orders_pages{fastmm::test::fixture("bybit/rest_open_orders.json")};
  std::atomic<int> snapshots_sent{0};
  std::atomic<int> auth_failures{0};
  std::atomic<int> cancel_all_ok{0};
  std::atomic<int> cancel_all_bad{0};
  std::atomic<int> open_orders_ok{0};
  std::atomic<int> open_orders_calls{0};
  std::atomic<int> rest_cancels{0};
  std::atomic<int> dcp_ok{0};
  // GET /v5/execution/list: one entry per answered request, in order; the last one repeats. The
  // next `executions_failing` requests are answered with retCode 10006 instead.
  std::mutex exec_mu;
  std::vector<std::string> execution_pages{exec_page({}, "")};
  std::size_t execution_served = 0;
  std::atomic<int> executions_failing{0};
  std::atomic<int> executions_calls{0};
  std::atomic<int> executions_ok{0};
  std::atomic<bool> dcp_refused{false};       // "DCP feature is only available for Ins clients"
  net::WsSession* private_session = nullptr;  // server thread only

  explicit Harness(std::vector<std::string> pages = {}) {
    if (!pages.empty()) open_orders_pages = std::move(pages);
    srv.route("GET", "/v5/market/time", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, server_time);
    });
    srv.route("GET", "/v5/market/instruments-info", [this](const net::HttpRequest& r) {
      srv.record("instruments", std::string(r.query));
      return net::HttpServerResponse::json(200, instruments_info);
    });
    srv.route("GET", "/v5/order/realtime", [this](const net::HttpRequest& r) {
      if (rest_signed(r, r.query)) ++open_orders_ok;
      srv.record("open_orders", std::string(r.query));
      const std::size_t i = static_cast<std::size_t>(open_orders_calls++);
      return net::HttpServerResponse::json(
          200, open_orders_pages[std::min(i, open_orders_pages.size() - 1)]);
    });
    srv.route("GET", "/v5/execution/list", [this](const net::HttpRequest& r) {
      if (rest_signed(r, r.query)) ++executions_ok;
      srv.record("executions", std::string(r.query));
      ++executions_calls;
      if (executions_failing.load() > 0) {
        --executions_failing;
        return net::HttpServerResponse::json(
            200,
            R"({"retCode":10006,"retMsg":"Too many visits!","result":{},"retExtInfo":{},"time":1789299704000})");
      }
      const std::lock_guard lock(exec_mu);
      const std::size_t i = std::min(execution_served++, execution_pages.size() - 1);
      return net::HttpServerResponse::json(200, execution_pages[i]);
    });
    srv.route("POST", "/v5/order/cancel", [this](const net::HttpRequest&) {
      ++rest_cancels;
      return net::HttpServerResponse::json(
          200,
          R"({"retCode":0,"retMsg":"OK","result":{"orderId":"2012345678901234567","orderLinkId":"fm000100000002"},"retExtInfo":{},"time":1789299704000})");
    });
    srv.route("POST", "/v5/order/disconnected-cancel-all", [this](const net::HttpRequest& r) {
      srv.record("dcp", std::string(r.body));
      if (dcp_refused.load())
        return net::HttpServerResponse::json(
            200,
            R"({"retCode":10005,"retMsg":"Permission denied","result":{},"retExtInfo":{},"time":1789299700000})");
      if (rest_signed(r, r.body)) ++dcp_ok;
      return net::HttpServerResponse::json(
          200,
          R"({"retCode":0,"retMsg":"success","result":{},"retExtInfo":{},"time":1789299700000})");
    });
    srv.route("POST", "/v5/order/cancel-all", [this](const net::HttpRequest& r) {
      const bool ok =
          rest_signed(r, r.body) && r.body == R"({"category":"spot","symbol":"BTCUSDT"})";
      ++(ok ? cancel_all_ok : cancel_all_bad);
      return net::HttpServerResponse::json(
          200,
          R"({"retCode":0,"retMsg":"OK","result":{"list":[],"success":"1"},"retExtInfo":{},"time":1789299704000})");
    });
    srv.on_ws_text("/v5/public/spot", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      const std::string req = json_str(t, "req_id");
      if (op == "subscribe") {
        s.send_text(R"({"success":true,"ret_msg":"subscribe","conn_id":"c1","req_id":")" + req +
                    R"(","op":"subscribe"})");
        if (t.find("orderbook.50.BTCUSDT") != std::string_view::npos) {
          s.send_text(snapshot);
          ++snapshots_sent;
          if (snapshots_sent.load() == 1) {
            s.send_text(delta);
            s.send_text(top);
            s.send_text(trade);
          }
        }
      } else if (op == "unsubscribe") {
        s.send_text(R"({"success":true,"ret_msg":"","conn_id":"c1","req_id":")" + req +
                    R"(","op":"unsubscribe"})");
      } else if (op == "ping") {
        s.send_text(R"({"success":true,"ret_msg":"pong","conn_id":"c1","op":"ping"})");
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
        const bool ok = auth_ok(t);
        if (!ok) ++auth_failures;
        s.send_text(ok ? R"({"retCode":0,"retMsg":"OK","op":"auth","connId":"t1"})"
                       : R"({"retCode":10004,"retMsg":"Invalid sign","op":"auth","connId":"t1"})");
        return;
      }
      if (op != "order.create" && op != "order.amend" && op != "order.cancel") return;
      s.send_text(
          R"({"reqId":")" + req + R"(","retCode":0,"retMsg":"OK","op":")" + op +
          R"(","data":{"orderId":"2012345678901234567","orderLinkId":"fm000100000001"},"retExtInfo":{},"header":{"X-Bapi-Limit":"20","X-Bapi-Limit-Status":"19","X-Bapi-Limit-Reset-Timestamp":"1789299700208"},"connId":"t1"})");
      if (private_session == nullptr) return;
      if (op == "order.create") {
        private_session->send_text(private_order("fm000100000001", "New", "0"));
      } else if (op == "order.amend") {
        // Venue keeps the original orderLinkId after an amend.
        private_session->send_text(
            R"({"topic":"execution","id":"e1","creationTime":1789299703460,"data":[{"category":"spot","symbol":"BTCUSDT","execFee":"0","execId":"ex-1","execPrice":"60000.2","execQty":"0.0005","execType":"Trade","execValue":"30.0001","feeRate":"0","orderId":"2012345678901234567","orderLinkId":"fm000100000001","orderPrice":"60000.2","orderQty":"0.002","side":"Buy","leavesQty":"0.0015","execTime":"1789299703453","isMaker":true,"seq":1}]})");
      } else {
        private_session->send_text(private_order("fm000100000001", "Cancelled", "0.0005"));
      }
    });
    srv.start();
  }

  VenueSection section(bool with_keys) const {
    VenueSection s;
    s.name = "fake-bybit";
    s.kind = "bybit";
    s.ws_url = srv.ws_base() + "/v5/public/spot";
    s.ws_api_url = srv.ws_base() + "/v5/trade";
    s.rest_url = srv.http_base();
    if (with_keys) {
      s.api_key = kKey;
      s.api_secret = kSecret;
    }
    s.supports_replace = true;
    return s;
  }
};

}  // namespace

TEST_CASE("bybit.venue: config mapping derives the private and trade URLs") {
  VenueSection s;
  s.name = "bybit";
  s.ws_url = "wss://stream-testnet.bybit.com/v5/public/spot";
  s.rest_url = "https://api-testnet.bybit.com";
  s.extra["depth"] = "200";
  s.extra["order_api"] = "rest";
  const BybitVenueConfig c = make_bybit_config(s, true);
  CHECK(c.ws_private_url == "wss://stream-testnet.bybit.com/v5/private");
  CHECK(c.ws_trade_url == "wss://stream-testnet.bybit.com/v5/trade");
  CHECK(c.depth == 200);
  CHECK_FALSE(c.ws_order_api);
  CHECK(c.dry_run);
}

TEST_CASE("bybit.venue: scripted fake exchange end to end") {
  Harness h;
  InstrumentTable instruments;
  // Venue id 1 on purpose (the id is a runtime value, not an index assumption).
  Instrument inst = make_instrument("BTCUSDT", 1, "BTC", "USDT");
  REQUIRE(instruments.add(inst));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  const Seqlocked<TscCalibration> tsc(calibrate_tsc(milliseconds(10)));
  {
    BybitVenueConfig cfg = make_bybit_config(h.section(true), false);
    cfg.ws_private_url = h.srv.ws_base() + "/v5/private";
    BybitVenue venue(kVenue, cfg);
    venue.set_tsc_calibration_source(&tsc);
    REQUIRE(venue.load_reference_data(instruments));
    CHECK(instruments.get(kBtc).tick == Price::from_decimal("0.1").value());
    CHECK(instruments.get(kBtc).lot == Qty::from_decimal("0.000001").value());
    REQUIRE(h.srv.frames("instruments").size() == 1);
    CHECK(h.srv.frames("instruments")[0] == "category=spot&symbol=BTCUSDT");
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kBtc};
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
      return venue.md_feed()->synced_count() == 1 && mdc.count(EventType::Trade) == 1 &&
             live_channels() >= 2;
    }));
    CHECK(h.auth_failures.load() == 0);
    CHECK(mdc.count(EventType::BookSnapshot) == 1);
    CHECK(mdc.count(EventType::BookDelta) == 1);
    CHECK(mdc.count(EventType::BookTicker) == 1);
    const auto sub = h.srv.frames("/v5/public/spot");
    REQUIRE_FALSE(sub.empty());
    CHECK(
        sub[0] ==
        R"({"req_id":"md0","op":"subscribe","args":["orderbook.50.BTCUSDT","orderbook.1.BTCUSDT","publicTrade.BTCUSDT"]})");

    // u == 1 on a delta: resync + unsubscribe/subscribe -> new snapshot.
    h.srv.send_to("/v5/public/spot", fastmm::test::fixture("bybit/orderbook50_delta_u1.json"));
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      return h.snapshots_sent.load() == 2 && venue.md_feed()->synced_count() == 1 &&
             mdc.count(EventType::BookSnapshot) == 2;
    }));
    CHECK(venue.md_feed()->resync_count() == 1);

    // The new order carries the last snapshot's receive stamp (as if the strategy placed it in
    // response to that book update); the amend and the cancel below carry none.
    const Cycles trigger_t0 = mdc.last<BookDeltaMsg>(EventType::BookSnapshot)->hdr.t0_cycles;
    REQUIRE(trigger_t0.v != 0);
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kBtc, kVenue);
    n.hdr.t0_cycles = trigger_t0;
    n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("60000.1").value();
    n.qty = Qty::from_decimal("0.001").value();
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    const Cycles before_wake = rdtscp();
    venue.on_wake();
    const Cycles after_wake = rdtscp();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 2;
    }));
    CHECK(oc.last<OrderAckMsg>(EventType::OrderAck)->cl_ord_id == n.cl_ord_id);
    CHECK(oc.last<OrderAckMsg>(EventType::OrderAck)->venue_order_id.view() ==
          "2012345678901234567");

    // Amend in place: ack for the new id; later events for the old orderLinkId map to it.
    OutReplaceMsg rp{};
    init_header(rp, EventType::OutReplace, kBtc, kVenue);
    rp.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
    rp.orig_cl_ord_id = n.cl_ord_id;
    rp.venue_order_id.assign("2012345678901234567");
    rp.price = Price::from_decimal("60000.2").value();
    rp.qty = Qty::from_decimal("0.002").value();
    REQUIRE(outbound.try_push(&rp, rp.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderFill) == 1;
    }));
    CHECK(oc.last<OrderAckMsg>(EventType::OrderAck)->cl_ord_id == rp.cl_ord_id);
    const auto* fill = oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(fill->cl_ord_id == rp.cl_ord_id);
    CHECK(fill->qty == Qty::from_decimal("0.0005").value());
    CHECK(fill->cum_qty == Qty::from_decimal("0.0005").value());
    CHECK(fill->liquidity == Liquidity::Maker);

    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kBtc, kVenue);
    c.cl_ord_id = rp.cl_ord_id;
    c.venue_order_id.assign("2012345678901234567");
    REQUIRE(outbound.try_push(&c, c.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderCancelAck) == 1;
    }));
    const auto* cx = oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck);
    CHECK(cx->cl_ord_id == rp.cl_ord_id);
    CHECK(cx->cum_qty == Qty::from_decimal("0.0005").value());
    std::string cancel_frame;
    for (const auto& f : h.srv.frames("/v5/trade")) {
      if (f.find("order.cancel") != std::string::npos) cancel_frame = f;
    }
    CHECK(cancel_frame.find(R"("orderId":"2012345678901234567")") != std::string::npos);

    // Network-thread latency: three messages encoded and sent, one receive-to-wire sample
    // bracketed by the receive-to-on_wake and receive-to-after-on_wake intervals.
    venue.on_timer(net::Reactor::now_ns());
    const VenueStatus st = venue.status();
    CHECK(st.order_encode.count == 3);
    CHECK(st.order_send.count == 3);
    CHECK(st.wire_tick_to_trade.count == 1);
    const TscClock conv(tsc.load());
    if (conv.calibration().use_tsc) {
      CHECK(st.wire_tick_to_trade.p50_ns >=
            static_cast<std::uint64_t>(conv.cycles_to_ns(before_wake - trigger_t0)));
      CHECK(st.wire_tick_to_trade.p50_ns <=
            static_cast<std::uint64_t>(conv.cycles_to_ns(after_wake - trigger_t0)));
      CHECK(st.order_encode.p50_ns > 0);
      CHECK(st.order_send.p50_ns > 0);
      CHECK(st.order_send.p50_ns < 1'000'000'000);
    }

    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::Reconcile) == 3;  // Begin, BTCUSDT order, End (ETHUSDT skipped)
    }));
    // Begin carries the last order id the venue sent before it asked for the snapshot.
    const auto* begin = oc.first_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
      return m.kind == ReconcileMsg::Kind::Begin;
    });
    REQUIRE(begin != nullptr);
    CHECK((begin->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK(begin->sent_watermark == rp.cl_ord_id);
    CHECK(h.open_orders_ok.load() == 1);

    CHECK(venue.cancel_all());
    CHECK(h.cancel_all_ok.load() == 1);
    CHECK(h.cancel_all_bad.load() == 0);
    CHECK_FALSE(venue.fatal());
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: a failed private authentication kills this venue, once") {
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 1, "BTC", "USDT")));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    VenueSection s = h.section(true);
    s.api_secret = "not-the-secret";
    BybitVenueConfig cfg = make_bybit_config(s, false);
    cfg.ws_private_url = h.srv.ws_base() + "/v5/private";
    BybitVenue venue(kVenue, cfg);
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kBtc};
    venue.subscribe(ids);
    venue.connect(reactor);
    REQUIRE(pump_until(reactor, [&] { return venue.fatal(); }));
    CHECK(h.auth_failures.load() >= 1);
    // Both authenticated channels fail; the engine is asked only once.
    for (int i = 0; i < 40; ++i) reactor.run_once(5);
    Collected oc;
    oc.take(orders);
    REQUIRE(oc.count(EventType::Control) == 1);
    const ControlMsg* kill = oc.last<ControlMsg>(EventType::Control);
    CHECK(kill->command == ControlCommand::TripVenueKill);
    CHECK(kill->hdr.venue == kVenue);
    CHECK(static_cast<KillReason>(kill->arg) == KillReason::VenueFatal);
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}

namespace {

// A connected BybitVenue on `h`, with its sinks and reactor.
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
    REQUIRE(instruments.add(make_instrument("BTCUSDT", 1, "BTC", "USDT")));
    BybitVenueConfig cfg = make_bybit_config(section, false);
    cfg.ws_private_url = h.srv.ws_base() + "/v5/private";
    venue = std::make_unique<BybitVenue>(kVenue, cfg);
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kBtc};
    venue->subscribe(ids);
    if (before_connect) before_connect(*venue);
    venue->connect(reactor);
  }
  ~Live() {
    venue->disconnect();
    reactor.run_once(0);
  }
  std::size_t live_channels() {
    oc.take(orders);
    std::size_t n = 0;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) == EventType::ConnectionState &&
          RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Live)
        ++n;
    }
    return n;
  }
  void spin(int iterations) {
    for (int i = 0; i < iterations; ++i) reactor.run_once(5);
    oc.take(orders);
  }
};

}  // namespace

TEST_CASE("bybit.venue: an open-order reply with retCode != 0 reconciles nothing") {
  // HTTP 200 with retCode 10006 (rate limit). Emitting Begin/End around it would make
  // Oms::reconcile_end() cancel every order still resting at the venue.
  Harness h(
      {R"({"retCode":10006,"retMsg":"Too many visits!","result":{},"retExtInfo":{},"time":1789299704000})"});
  {
    Live l(h, h.section(true));
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] { return h.open_orders_calls.load() == 1; }));
    l.spin(40);
    CHECK(l.oc.count(EventType::Reconcile) == 0);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: the open-order snapshot follows nextPageCursor") {
  auto page = [](const char* link, const char* cursor) {
    return std::string(R"({"retCode":0,"retMsg":"OK","result":{"category":"spot","list":[)"
                       R"({"orderId":"2012345678901234567","orderLinkId":")") +
           link +
           R"(","symbol":"BTCUSDT","price":"60000.1","qty":"0.001","side":"Buy",)"
           R"("orderStatus":"New","cumExecQty":"0","leavesQty":"0.001"}],"nextPageCursor":")" +
           cursor + R"("},"retExtInfo":{},"time":1789299704000})";
  };
  Harness h({page("fm000100000001", "cursor-2"), page("fm000100000002", "")});
  {
    Live l(h, h.section(true));
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Reconcile) == 4;  // Begin, two orders, End
    }));
    CHECK(h.open_orders_calls.load() == 2);
    const auto queries = h.srv.frames("open_orders");
    REQUIRE(queries.size() == 2);
    CHECK(queries[0].find("cursor=") == std::string::npos);
    CHECK(queries[1].find("&cursor=cursor-2") != std::string::npos);
    CHECK(h.open_orders_ok.load() == 2);  // both pages signed
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: a cancel still goes out after a venue-fatal error") {
  Harness h;
  {
    VenueSection s = h.section(true);
    s.api_secret = "not-the-secret";  // both authenticated channels fail -> fatal
    Live l(h, s);
    REQUIRE(pump_until(l.reactor, [&] { return l.venue->fatal(); }));
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kBtc, kVenue);
    c.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
    c.venue_order_id.assign("2012345678901234567");
    REQUIRE(l.outbound.try_push(&c, c.hdr.len));
    l.venue->on_wake();
    REQUIRE(pump_until(l.reactor, [&] { return h.rest_cancels.load() == 1; }));
    l.spin(10);
    // The cancel was sent, not refused with "venue fatal".
    CHECK(l.oc.count(EventType::OrderCancelReject) == 0);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: a batch whose write fails rejects its orders") {
  Harness h;
  {
    Live l(h, h.section(true));
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    h.srv.close_sessions("/v5/trade");
    // The reactor is not run from here on: the venue still believes the trade channel is live, so
    // the orders are encoded into the corked connection and uncork() is what discovers that the
    // batch never left. Writing to a closed peer fails on the write after the RST arrives.
    std::uint32_t id = 0x100;
    bool rejected = false;
    for (int attempt = 0; attempt < 20 && !rejected; ++attempt) {
      for (int i = 0; i < 2; ++i) {
        OutNewOrderMsg n{};
        init_header(n, EventType::OutNewOrder, kBtc, kVenue);
        n.cl_ord_id = ClientOrderId{++id};
        n.side = Side::Buy;
        n.type = OrderType::PostOnly;
        n.price = Price::from_decimal("60000.1").value();
        n.qty = Qty::from_decimal("0.001").value();
        REQUIRE(l.outbound.try_push(&n, n.hdr.len));
      }
      l.venue->on_wake();
      l.oc.take(l.orders);
      rejected = l.oc.first_if<OrderRejectMsg>(EventType::OrderReject, [](const OrderRejectMsg& m) {
        return m.text.view() == "order batch not written";
      }) != nullptr;
      if (!rejected) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(rejected);
    // Both orders of the failed batch are rejected, not silently counted as sent.
    std::size_t batch_rejects = 0;
    for (const auto& m : l.oc.all) {
      if (RecordingSink::type_of(m) == EventType::OrderReject &&
          RecordingSink::as<OrderRejectMsg>(m).text.view() == "order batch not written")
        ++batch_rejects;
    }
    CHECK(batch_rejects == 2);
    // ...and taken back out of orders_sent: only the batches that were written count.
    l.venue->disconnect();
    CHECK(l.venue->status().orders_sent == (id - 0x100) - batch_rejects);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: disconnect-cancel-all is set once and needs the dcp topic to fire") {
  // Bybit's dead man's switch is not a countdown the client refreshes. The window is an account
  // setting, Bybit starts the clock itself once every private connection that subscribed a
  // `dcp.*` topic is gone, and without that subscription the setting does nothing at all.
  Harness h;
  {
    VenueSection s = h.section(true);
    s.extra["dead_mans_switch_s"] = "30";
    Live l(h, s);
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2 && h.dcp_ok.load() == 1; }));
    const auto dcp = h.srv.frames("dcp");
    REQUIRE(dcp.size() == 1);
    CHECK(dcp[0] == R"({"product":"SPOT","timeWindow":30})");
    const auto subs = h.srv.frames("private_subscribe");
    REQUIRE_FALSE(subs.empty());
    CHECK(subs[0].find("\"dcp.spot\"") != std::string::npos);
    // Nothing to refresh: the setting persists, so it is not sent again.
    l.spin(60);
    CHECK(h.dcp_ok.load() == 1);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: dead_mans_switch_s is off by default and refusal is not fatal") {
  Harness h;
  {
    Live l(h, h.section(true));  // no dead_mans_switch_s
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    l.spin(40);
    CHECK(h.dcp_ok.load() == 0);
    CHECK(h.srv.frames("dcp").empty());
    // And no dcp topic, because there is no switch to arm.
    const auto subs = h.srv.frames("private_subscribe");
    REQUIRE_FALSE(subs.empty());
    CHECK(subs[0].find("dcp") == std::string::npos);
  }
  h.srv.stop();

  // Bybit grants DCP to institutional accounts only, so an ordinary key is refused. That has to
  // be loud but survivable: the connector keeps quoting without a venue-side switch.
  Harness h2;
  h2.dcp_refused.store(true);
  {
    VenueSection s = h2.section(true);
    s.extra["dead_mans_switch_s"] = "10";
    Live l(h2, s);
    REQUIRE(pump_until(l.reactor, [&] { return !h2.srv.frames("dcp").empty(); }));
    l.spin(40);
    CHECK(h2.dcp_ok.load() == 0);
    CHECK_FALSE(l.venue->fatal());
    CHECK(l.live_channels() >= 2);
  }
  h2.srv.stop();
}

TEST_CASE("bybit.venue: the dcp window is clamped to what the venue accepts") {
  auto window = [](const char* value) {
    VenueSection s;
    s.name = "bybit";
    s.kind = "bybit";
    s.ws_url = "wss://stream-testnet.bybit.com/v5/public/spot";
    s.rest_url = "https://api-testnet.bybit.com";
    s.extra["dead_mans_switch_s"] = value;
    return make_bybit_config(s, false).dead_mans_switch_s;
  };
  CHECK(window("0") == 0);   // off
  CHECK(window("-5") == 0);  // off, not a negative window
  CHECK(window("1") == kMinDcpWindowS);
  CHECK(window("10") == 10);
  CHECK(window("9000") == kMaxDcpWindowS);
}

namespace {

constexpr long long kT = 1789299703000;  // execTime of the rows below, ms

std::vector<const OrderFillMsg*> fills_of(const Collected& oc) {
  std::vector<const OrderFillMsg*> out;
  for (const auto& m : oc.all) {
    if (RecordingSink::type_of(m) == EventType::OrderFill)
      out.push_back(&RecordingSink::as<OrderFillMsg>(m));
  }
  return out;
}

// The Begin of every snapshot, in order.
std::vector<const ReconcileMsg*> begins_of(const Collected& oc) {
  std::vector<const ReconcileMsg*> out;
  for (const auto& m : oc.all) {
    if (RecordingSink::type_of(m) == EventType::Reconcile &&
        RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::Begin)
      out.push_back(&RecordingSink::as<ReconcileMsg>(m));
  }
  return out;
}

bool exact(const ReconcileMsg* begin) {
  return (begin->flags & ReconcileMsg::kExecutionsExact) != 0;
}

std::string param(const std::string& query, const std::string& key) {
  const std::size_t p = query.find(key + "=");
  if (p == std::string::npos) return {};
  const std::size_t v = p + key.size() + 1;
  return query.substr(v, query.find('&', v) - v);
}

}  // namespace

TEST_CASE("bybit.venue: a fill the private stream missed is booked from execution/list first") {
  Harness h;
  h.execution_pages = {
      exec_page({exec_row("ex-9", "fm000100000001", "60010.5", "0.0000004", kT)}, "")};
  {
    Live l(h, h.section(true));
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    CHECK(h.executions_calls.load() == 0);  // nothing to replay before a reconciliation
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Reconcile) == 3;
    }));
    const auto fills = fills_of(l.oc);
    REQUIRE(fills.size() == 1);
    const OrderFillMsg* f = fills[0];
    CHECK((f->flags & OrderFillMsg::kReplayed) != 0);
    CHECK(f->exec_id.view() == "ex-9");
    CHECK(f->cl_ord_id == decode_cl_ord_id("fm000100000001").value());
    CHECK(f->venue_order_id.view() == "2012345678901234567");
    CHECK(f->side == Side::Buy);
    CHECK(f->price == Price::from_decimal("60010.5").value());
    CHECK(f->qty == Qty::from_decimal("0.0004").value());
    CHECK(f->fee == Notional::from_decimal("0.0000004").value());
    CHECK(f->fee_asset == FeeAsset::Base);
    CHECK(f->liquidity == Liquidity::Maker);
    // The fill is in before the snapshot that no longer names its order.
    std::size_t fill_at = 0;
    std::size_t begin_at = 0;
    for (std::size_t i = 0; i < l.oc.all.size(); ++i) {
      const auto t = RecordingSink::type_of(l.oc.all[i]);
      if (t == EventType::OrderFill) fill_at = i;
      if (t == EventType::Reconcile && begin_at == 0) begin_at = i;
    }
    CHECK(fill_at < begin_at);
    const auto begins = begins_of(l.oc);
    REQUIRE(begins.size() == 1);
    CHECK(exact(begins[0]));
    const auto queries = h.srv.frames("executions");
    REQUIRE(queries.size() == 1);
    CHECK(queries[0].rfind("category=spot&startTime=", 0) == 0);
    CHECK(queries[0].find("&limit=100") != std::string::npos);
    CHECK(queries[0].find("endTime") == std::string::npos);  // the last window is open-ended
    CHECK(h.executions_ok.load() == 1);                      // signed
    l.venue->on_timer(net::Reactor::now_ns());
    const VenueStatus st = l.venue->status();
    CHECK(st.execution_queries == 1);
    CHECK(st.executions_fetched == 1);
    CHECK(st.execution_query_errors == 0);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: execution/list pages are followed and emitted oldest first") {
  Harness h;
  h.execution_pages = {
      // Newest first, as Bybit sends them; a funding row and another symbol are not our fills.
      exec_page({exec_row("ex-3", "fm000100000001", "60003", "0", kT + 3),
                 exec_row("fund-1", "", "60002", "0", kT + 2, "Funding"),
                 exec_row("eth-1", "", "3000", "0", kT + 2, "Trade", "ETHUSDT"),
                 exec_row("ex-2", "fm000100000001", "60002", "0", kT + 2)},
                "cur-2"),
      exec_page({exec_row("ex-1", "fm000100000001", "60001", "0", kT + 1)}, ""),
      // The next replay starts at the newest execTime seen: ex-3 comes back and is not repeated.
      exec_page({exec_row("ex-4", "fm000100000001", "60004", "0", kT + 4),
                 exec_row("ex-3", "fm000100000001", "60003", "0", kT + 3)},
                "")};
  {
    Live l(h, h.section(true));
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Reconcile) == 3;
    }));
    auto fills = fills_of(l.oc);
    REQUIRE(fills.size() == 3);
    CHECK(fills[0]->exec_id.view() == "ex-1");
    CHECK(fills[1]->exec_id.view() == "ex-2");
    CHECK(fills[2]->exec_id.view() == "ex-3");
    CHECK(fills[0]->price == Price::from_decimal("60001").value());
    CHECK(exact(begins_of(l.oc)[0]));
    auto queries = h.srv.frames("executions");
    REQUIRE(queries.size() == 2);
    CHECK(queries[0].find("cursor=") == std::string::npos);
    CHECK(param(queries[1], "cursor") == "cur-2");
    CHECK(param(queries[1], "startTime") == param(queries[0], "startTime"));
    CHECK(h.executions_ok.load() == 2);

    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Reconcile) == 6;
    }));
    fills = fills_of(l.oc);
    REQUIRE(fills.size() == 4);
    CHECK(fills[3]->exec_id.view() == "ex-4");
    queries = h.srv.frames("executions");
    REQUIRE(queries.size() == 3);
    CHECK(param(queries[2], "startTime") == std::to_string(kT + 3));
    CHECK(queries[2].find("cursor=") == std::string::npos);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: the snapshot is exact only when the execution replay completed") {
  Harness h;
  h.executions_failing = 1;  // retCode 10006 on the first query
  {
    Live l(h, h.section(true));
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Reconcile) == 3;
    }));
    // The snapshot still comes, but says it is an estimate.
    auto begins = begins_of(l.oc);
    REQUIRE(begins.size() == 1);
    CHECK_FALSE(exact(begins[0]));
    CHECK(h.executions_calls.load() == 1);

    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Reconcile) == 6;
    }));
    begins = begins_of(l.oc);
    REQUIRE(begins.size() == 2);
    CHECK(exact(begins[1]));
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: a failed execution query is retried from the housekeeping timer") {
  Harness h;
  h.executions_failing = 1;
  h.execution_pages = {
      exec_page({exec_row("ex-7", "fm000100000001", "60007", "0.0000004", kT)}, "")};
  {
    Live l(h, h.section(true));
    REQUIRE(pump_until(l.reactor, [&] { return l.live_channels() >= 2; }));
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Reconcile) == 3;
    }));
    CHECK(l.oc.count(EventType::OrderFill) == 0);
    // No reconnect and no new reconciliation: the timer asks again and books the fill.
    l.venue->on_timer(net::Reactor::now_ns());
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderFill) == 1;
    }));
    CHECK(fills_of(l.oc)[0]->exec_id.view() == "ex-7");
    CHECK(h.executions_calls.load() == 2);
    CHECK(l.oc.count(EventType::Reconcile) == 3);  // the snapshot is not repeated
    l.venue->on_timer(net::Reactor::now_ns());
    const VenueStatus st = l.venue->status();
    CHECK(st.execution_queries == 2);
    CHECK(st.execution_query_errors == 1);
    CHECK(st.executions_fetched == 1);
    CHECK(h.executions_calls.load() == 2);  // succeeded: nothing more to retry
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: a resumed session replays from the store and skips what it booked") {
  Harness h;
  h.execution_pages = {exec_page({exec_row("ex-new", "fm000100000001", "60005", "0", kT + 5),
                                  exec_row("ex-known", "fm000100000001", "60004", "0", kT + 4)},
                                 "")};
  std::int64_t since = 0;  // the venue clock follows the fixture's server time
  {
    Live l(h, h.section(true), [&](BybitVenue& v) {
      since = v.venue_time_ms() - 3'600'000;
      v.resume_executions(since, {"ex-known"});
    });
    // No reconciliation asked for: the replay runs once the private channel is up.
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderFill) == 1;
    }));
    CHECK(fills_of(l.oc)[0]->exec_id.view() == "ex-new");
    const auto queries = h.srv.frames("executions");
    REQUIRE(queries.size() == 1);
    CHECK(param(queries[0], "startTime") == std::to_string(since));
    l.spin(20);
    CHECK(l.oc.count(EventType::Reconcile) == 0);
    CHECK(l.oc.count(EventType::OrderFill) == 1);
  }
  h.srv.stop();
}

TEST_CASE("bybit.venue: a replay longer than 7 days walks 7-day windows") {
  Harness h;
  const std::int64_t week = 7LL * 24 * 3600 * 1000;
  std::int64_t since = 0;
  {
    Live l(h, h.section(true), [&](BybitVenue& v) {
      since = v.venue_time_ms() - 10LL * 24 * 3600 * 1000;
      v.resume_executions(since, {});
    });
    REQUIRE(pump_until(l.reactor, [&] { return h.executions_calls.load() == 2; }));
    l.spin(10);
    const auto queries = h.srv.frames("executions");
    REQUIRE(queries.size() == 2);
    CHECK(param(queries[0], "startTime") == std::to_string(since));
    CHECK(param(queries[0], "endTime") == std::to_string(since + week));
    CHECK(param(queries[1], "startTime") == std::to_string(since + week));
    CHECK(queries[1].find("endTime") == std::string::npos);
  }
  h.srv.stop();
}
