// BybitVenue against a scripted in-process fake Bybit v5 (REST + public/private/trade
// WebSockets): reference data, subscribe + snapshot/delta sync, u == 1 resubscribe, auth
// signatures on both private channels, order.create -> ack + order New, order.amend with the
// orderLinkId alias, execution fill, order.cancel -> Cancelled, open-order reconciliation
// and the blocking kill-switch cancel_all.
#include "fastmm/venues/bybit/bybit_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"

#include <atomic>
#include <string>

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

struct Harness {
  FakeVenueServer srv;
  std::string instruments_info = fastmm::test::fixture("bybit/instruments_info.json");
  std::string server_time = fastmm::test::fixture("bybit/server_time.json");
  std::string snapshot = fastmm::test::fixture("bybit/orderbook50_snapshot.json");
  std::string delta = fastmm::test::fixture("bybit/orderbook50_delta.json");
  std::string top = fastmm::test::fixture("bybit/orderbook1_snapshot.json");
  std::string trade = fastmm::test::fixture("bybit/public_trade.json");
  std::string open_orders = fastmm::test::fixture("bybit/rest_open_orders.json");
  std::atomic<int> snapshots_sent{0};
  std::atomic<int> auth_failures{0};
  std::atomic<int> cancel_all_ok{0};
  std::atomic<int> cancel_all_bad{0};
  std::atomic<int> open_orders_ok{0};
  net::WsSession* private_session = nullptr;  // server thread only

  Harness() {
    srv.route("GET", "/v5/market/time", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, server_time);
    });
    srv.route("GET", "/v5/market/instruments-info", [this](const net::HttpRequest& r) {
      srv.record("instruments", std::string(r.query));
      return net::HttpServerResponse::json(200, instruments_info);
    });
    srv.route("GET", "/v5/order/realtime", [this](const net::HttpRequest& r) {
      if (rest_signed(r, r.query)) ++open_orders_ok;
      return net::HttpServerResponse::json(200, open_orders);
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
