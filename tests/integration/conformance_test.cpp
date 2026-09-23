// Protocol conformance of fastmm-sim-exchange, driven by the real BinanceVenue connector (not a
// scripted client): reference data, REST snapshot + depthUpdate sync, bookTicker / trade, the
// WS API user-data subscription, order.place / order.cancel / order.cancelReplace, the
// executionReport lifecycle with real fills against generator flow, openOrders reconciliation,
// the kill-switch cancel_all, and the documented Binance error codes.
#include "integration_util.hpp"

#include "fastmm/live/session.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/sim/server/binance_json.hpp"
#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/blocking_http.hpp"

#include <map>

using namespace fastmm;
using namespace fastmm::integration;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;

namespace {

Instrument btcusdt() {
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.base = "BTC";
  i.quote = "USDT";
  i.asset_class = AssetClass::Spot;
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_int(1);  // wrong on purpose: exchangeInfo must override it
  i.lot = Qty::from_decimal("0.001").value();
  i.min_qty = i.lot;
  i.min_notional = Notional::from_int(1);
  return i;
}

BinanceVenueConfig venue_config(const ServerFixture& fx, const char* secret = kApiSecret) {
  BinanceVenueConfig c;
  c.name = "sim";
  c.ws_url = fx.ws() + "/stream";
  c.ws_api_url = fx.ws() + "/ws-api/v3";
  c.rest_url = fx.http();
  c.credentials.api_key = kApiKey;
  c.credentials.secret.value = secret;
  c.user_stream = UserStreamMode::WsApi;
  c.http_timeout_ms = 3000;
  if (const char* d = std::getenv("FASTMM_IT_RAW_DIR")) c.record_raw_dir = d;
  return c;
}

ClientOrderId cid(std::uint64_t n) {
  return ClientOrderId{0x0001'0000'0000ULL + n};
}

OutNewOrderMsg new_order(
    ClientOrderId id, Side side, OrderType type, TimeInForce tif, Price px, Qty qty) {
  OutNewOrderMsg m{};
  init_header(m, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  m.cl_ord_id = id;
  m.side = side;
  m.type = type;
  m.tif = tif;
  m.price = px;
  m.qty = qty;
  return m;
}

// BinanceVenue + ring sinks on this thread's reactor (the connector's own threading model).
struct VenueHarness {
  InstrumentTable instruments;
  SymbolTable symbols;
  RecordingSink md{8U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor{test_net_backend()};
  std::unique_ptr<BinanceVenue> venue;
  Collected mdc;
  Collected oc;

  explicit VenueHarness(BinanceVenueConfig cfg) {
    REQUIRE(instruments.add(btcusdt()));
    venue = std::make_unique<BinanceVenue>(VenueId{0}, std::move(cfg));
  }
  ~VenueHarness() {
    venue->disconnect();
    reactor.run_once(0);
  }
  VenueHarness(const VenueHarness&) = delete;
  VenueHarness& operator=(const VenueHarness&) = delete;

  void connect() {
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {InstrumentId{0}};
    venue->subscribe(ids);
    venue->connect(reactor);
  }
  template <class Pred>
  bool pump(Pred pred, int timeout_ms = 15000) {
    return pump_until(
        reactor,
        [&] {
          mdc.take(md);
          oc.take(orders);
          return pred();
        },
        timeout_ms);
  }
  void send(const EventHeader& h) {
    REQUIRE(outbound.try_push(&h, h.len));
    venue->on_wake();
  }
  [[nodiscard]] std::size_t live_order_channels() const {
    return oc.count_if<ConnectionStateMsg>(
        EventType::ConnectionState,
        [](const ConnectionStateMsg& m) { return m.state == ConnState::Live; });
  }
  [[nodiscard]] std::size_t acks(ClientOrderId id) const {
    return oc.count_if<OrderAckMsg>(EventType::OrderAck,
                                    [id](const OrderAckMsg& m) { return m.cl_ord_id == id; });
  }
  [[nodiscard]] std::size_t cancel_acks(ClientOrderId id) const {
    return oc.count_if<OrderCancelAckMsg>(
        EventType::OrderCancelAck, [id](const OrderCancelAckMsg& m) { return m.cl_ord_id == id; });
  }
  [[nodiscard]] const OrderRejectMsg* reject(ClientOrderId id) const {
    const OrderRejectMsg* out = nullptr;
    for (const auto& m : oc.all) {
      if (Collected::type_of(m) == EventType::OrderReject &&
          Collected::as<OrderRejectMsg>(m).cl_ord_id == id)
        out = &Collected::as<OrderRejectMsg>(m);
    }
    return out;
  }
  [[nodiscard]] const OrderFillMsg* fill(ClientOrderId id) const {
    const OrderFillMsg* out = nullptr;
    for (const auto& m : oc.all) {
      if (Collected::type_of(m) == EventType::OrderFill &&
          Collected::as<OrderFillMsg>(m).cl_ord_id == id)
        out = &Collected::as<OrderFillMsg>(m);
    }
    return out;
  }
  [[nodiscard]] const OrderAckMsg* last_ack(ClientOrderId id) const {
    const OrderAckMsg* out = nullptr;
    for (const auto& m : oc.all) {
      if (Collected::type_of(m) == EventType::OrderAck &&
          Collected::as<OrderAckMsg>(m).cl_ord_id == id)
        out = &Collected::as<OrderAckMsg>(m);
    }
    return out;
  }
};

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

TEST_CASE(
    "sim_exchange: reference data and depth sync with book ticker and trades through "
    "BinanceVenue") {
  ServerFixture fx;
  VenueHarness h(venue_config(fx));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  const Instrument& inst = h.instruments.get(InstrumentId{0});
  CHECK(inst.tick == Price::from_decimal("0.01").value());
  CHECK(inst.lot == Qty::from_decimal("0.00001").value());
  CHECK(inst.min_qty == Qty::from_decimal("0.00001").value());
  CHECK(inst.max_qty == Qty::from_int(100));
  CHECK(inst.min_notional == Notional::from_int(5));
  h.connect();
  REQUIRE(h.pump([&] {
    return h.venue->md_feed()->synced_count() == 1 && h.mdc.count(EventType::BookDelta) >= 10 &&
           h.mdc.count(EventType::BookTicker) >= 1 && h.mdc.count(EventType::Trade) >= 1;
  }));

  // Snapshot first, then deltas that bracket and continue its lastUpdateId without gaps.
  std::map<std::int64_t, std::int64_t> bids;
  std::map<std::int64_t, std::int64_t> asks;
  std::uint64_t snapshot_id = 0;
  std::uint64_t prev_u = 0;
  std::size_t gaps = 0;
  std::size_t deltas = 0;
  auto apply = [](std::map<std::int64_t, std::int64_t>& side, std::span<const Level> levels) {
    for (const Level& l : levels) {
      if (l.qty.is_zero()) {
        side.erase(l.price.raw);
      } else {
        side[l.price.raw] = l.qty.raw;
      }
    }
  };
  for (const auto& m : h.mdc.all) {
    const EventType t = Collected::type_of(m);
    if (t == EventType::BookSnapshot) {
      const auto& d = Collected::as<BookDeltaMsg>(m);
      snapshot_id = d.last_update_id;
      bids.clear();
      asks.clear();
      apply(bids, d.bids());
      apply(asks, d.asks());
      prev_u = 0;
    } else if (t == EventType::BookDelta) {
      const auto& d = Collected::as<BookDeltaMsg>(m);
      REQUIRE(snapshot_id != 0);
      if (prev_u == 0) {
        CHECK(d.first_update_id <= snapshot_id + 1);
        CHECK(d.last_update_id >= snapshot_id + 1);
      } else if (d.first_update_id != prev_u + 1) {
        ++gaps;
      }
      prev_u = d.last_update_id;
      apply(bids, d.bids());
      apply(asks, d.asks());
      ++deltas;
    }
  }
  CHECK(deltas >= 10);
  CHECK(gaps == 0);
  for (const auto& m : h.mdc.all) {
    if (Collected::type_of(m) == EventType::ConnectionState &&
        Collected::as<ConnectionStateMsg>(m).state == ConnState::Resyncing)
      MESSAGE("Resyncing, SyncReason " << Collected::as<ConnectionStateMsg>(m).reason_code);
  }
  CHECK(h.venue->md_feed()->resync_count() == 0);
  REQUIRE_FALSE(bids.empty());
  REQUIRE_FALSE(asks.empty());
  CHECK(bids.rbegin()->first < asks.begin()->first);

  const auto* ticker = h.mdc.last<BookTickerMsg>(EventType::BookTicker);
  CHECK(ticker->bid_px < ticker->ask_px);
  CHECK(ticker->bid_qty.is_positive());
  const auto* trade = h.mdc.last<TradeMsg>(EventType::Trade);
  CHECK(trade->qty.is_positive());
  CHECK(trade->trade_id > 0);

  const sim::server::SimServerStats st = fx.server.stats();
  CHECK(st.md_sessions == 1);
  CHECK(st.depth_snapshots >= 1);
  CHECK(st.depth_updates >= 10);

  BlockingHttp http(fx.http());
  const HttpReply r = http.get("/api/v3/depth?symbol=BTCUSDT&limit=5");
  REQUIRE(r.ok());
  CHECK_FALSE(r.header("X-MBX-USED-WEIGHT-1M").empty());
  CHECK(contains(r.body, "\"lastUpdateId\":"));
}

// Live snapshots ("depth_snapshot = live", as on Binance): with 500 ms batches the REST
// lastUpdateId almost always lies inside the next depthUpdate [U, u], which the documented
// algorithm accepts as the first event (U <= lastUpdateId + 1 <= u). A clean start must not
// resync. Needs the BookSyncer first-delta fix from commit 687a8ae (core/book/book_syncer.hpp).
TEST_CASE("sim_exchange: live depth snapshot inside a pending batch syncs through BinanceVenue") {
  sim::server::SimServerConfig cfg = test_server_config();
  cfg.snapshot_at_flush = false;
  cfg.depth_update_ms = 500;
  ServerFixture fx(cfg);
  VenueHarness h(venue_config(fx));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  REQUIRE(h.pump([&] {
    return h.venue->md_feed()->resync_count() > 0 ||
           (h.venue->md_feed()->synced_count() == 1 && h.mdc.count(EventType::BookDelta) >= 2);
  }));
  CHECK(h.venue->md_feed()->synced_count() == 1);
  CHECK(h.venue->md_feed()->resync_count() == 0);
  CHECK(fx.server.stats().depth_snapshots == 1);
}

TEST_CASE("sim_exchange: order lifecycle over the WS API with real fills and reconciliation") {
  ServerFixture fx;
  VenueHarness h(venue_config(fx));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  REQUIRE(h.pump(
      [&] { return h.venue->md_feed()->synced_count() == 1 && h.live_order_channels() >= 2; }));
  sim::server::SimServerStats st = fx.server.stats();
  CHECK(st.user_subscriptions == 1);
  CHECK(st.api_sessions == 2);
  const Price bid = st.best_bid.price;
  const Price ask = st.best_ask.price;
  REQUIRE(bid.is_positive());
  REQUIRE(ask > bid);
  const Qty qty = Qty::from_decimal("0.001").value();

  // Passive LIMIT_MAKER: ack from the response and from executionReport NEW.
  const ClientOrderId o1 = cid(1);
  h.send(new_order(
             o1, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, bid - Price::from_int(100), qty)
             .hdr);
  REQUIRE(h.pump([&] { return h.acks(o1) >= 2; }));
  const OrderAckMsg* ack1 = h.last_ack(o1);
  REQUIRE(ack1 != nullptr);
  CHECK_FALSE(ack1->venue_order_id.view().empty());
  CHECK(fx.server.stats().open_orders == 1);

  // order.cancel by orderId: cancel ack from the response and from executionReport CANCELED.
  OutCancelMsg cancel{};
  init_header(cancel, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  cancel.cl_ord_id = o1;
  cancel.venue_order_id.assign(ack1->venue_order_id.view());
  h.send(cancel.hdr);
  REQUIRE(h.pump([&] { return h.cancel_acks(o1) >= 2; }));
  CHECK(fx.server.stats().open_orders == 0);

  // order.cancelReplace (STOP_ON_FAILURE): the original is cancelled, the replacement acked.
  const ClientOrderId o2 = cid(2);
  const ClientOrderId o3 = cid(3);
  h.send(new_order(
             o2, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, bid - Price::from_int(90), qty)
             .hdr);
  REQUIRE(h.pump([&] { return h.acks(o2) >= 1; }));
  OutReplaceMsg rep{};
  init_header(rep, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  rep.cl_ord_id = o3;
  rep.orig_cl_ord_id = o2;
  rep.venue_order_id.assign(h.last_ack(o2)->venue_order_id.view());
  rep.price = bid - Price::from_int(80);
  rep.qty = qty;
  h.send(rep.hdr);
  REQUIRE(h.pump([&] { return h.cancel_acks(o2) >= 1 && h.acks(o3) >= 1; }));
  st = fx.server.stats();
  CHECK(st.replaces == 1);
  CHECK(st.open_orders == 1);

  // LIMIT_MAKER that would take liquidity: -2010 "Order would immediately match and take."
  const ClientOrderId o4 = cid(4);
  h.send(new_order(
             o4, Side::Sell, OrderType::PostOnly, TimeInForce::Gtc, bid - Price::from_int(10), qty)
             .hdr);
  REQUIRE(h.pump([&] { return h.reject(o4) != nullptr; }));
  CHECK(h.reject(o4)->reason == RejectReason::PostOnlyWouldCross);
  CHECK(h.reject(o4)->venue_code == -2010);

  // Taker fill: LIMIT IOC through the generator's best ask.
  const ClientOrderId o5 = cid(5);
  h.send(new_order(o5, Side::Buy, OrderType::Limit, TimeInForce::Ioc, ask + Price::from_int(5), qty)
             .hdr);
  REQUIRE(h.pump([&] { return h.fill(o5) != nullptr; }));
  CHECK(h.fill(o5)->liquidity == Liquidity::Taker);
  CHECK(h.fill(o5)->price <= ask + Price::from_int(5));
  CHECK(h.fill(o5)->fee.is_positive());

  // Maker fill: rest at the current best ask and wait for a generator market buy.
  const Price touch = fx.server.stats().best_ask.price;
  const ClientOrderId o6 = cid(6);
  h.send(new_order(o6, Side::Sell, OrderType::PostOnly, TimeInForce::Gtc, touch, qty).hdr);
  REQUIRE(h.pump([&] { return h.fill(o6) != nullptr; }, 30000));
  CHECK(h.fill(o6)->liquidity == Liquidity::Maker);
  CHECK(h.fill(o6)->side == Side::Sell);
  CHECK(fx.server.stats().fills >= 2);

  // openOrders.status -> ReconcileMsg Begin / OpenOrder(o3) / End.
  const std::size_t reconciles = h.oc.count(EventType::Reconcile);
  h.venue->request_open_orders();
  REQUIRE(h.pump([&] {
    return h.oc.count_if<ReconcileMsg>(
               EventType::Reconcile,
               [](const ReconcileMsg& m) { return m.kind == ReconcileMsg::Kind::End; }) >= 1 &&
           h.oc.count(EventType::Reconcile) > reconciles;
  }));
  CHECK(h.oc.count_if<ReconcileMsg>(EventType::Reconcile, [&](const ReconcileMsg& m) {
    return m.kind == ReconcileMsg::Kind::OpenOrder && m.cl_ord_id == o3 &&
           m.price == bid - Price::from_int(80);
  }) == 1);

  // Kill switch over the independent blocking REST connection.
  CHECK(h.venue->cancel_all());
  CHECK(fx.server.stats().open_orders == 0);
  REQUIRE(h.pump([&] { return h.cancel_acks(o3) >= 1; }));  // executionReport CANCELED
  CHECK_FALSE(h.venue->fatal());
}

TEST_CASE("sim_exchange: a size-down amends in place and keeps the order id, a reprice does not") {
  // The point of order.amend.keepPriority for a quoter: shrinking a quote at the same price is
  // the common requote, and cancelReplace pays for it with the queue position. Here the
  // difference is visible as the venue's orderId — the identity of the entry in the book. The
  // queue consequence itself is in sim.matching ("replace keeps priority only for same price
  // and qty <= leaves" against "cancel then a new order ... goes to the back").
  ServerFixture fx;
  VenueHarness h(venue_config(fx));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  REQUIRE(h.pump(
      [&] { return h.venue->md_feed()->synced_count() == 1 && h.live_order_channels() >= 2; }));
  const Price bid = fx.server.stats().best_bid.price;
  REQUIRE(bid.is_positive());
  const Price px = bid - Price::from_int(100);
  const Qty big = Qty::from_decimal("0.004").value();
  const Qty small = Qty::from_decimal("0.002").value();

  const ClientOrderId o1 = cid(11);
  h.send(new_order(o1, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, px, big).hdr);
  REQUIRE(h.pump([&] { return h.acks(o1) >= 1; }));
  const std::string venue_id(h.last_ack(o1)->venue_order_id.view());
  REQUIRE_FALSE(venue_id.empty());

  // Same price, smaller quantity -> order.amend.keepPriority.
  const ClientOrderId o2 = cid(12);
  OutReplaceMsg down{};
  init_header(down, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  down.cl_ord_id = o2;
  down.orig_cl_ord_id = o1;
  down.venue_order_id.assign(venue_id);
  down.price = px;
  down.qty = small;
  h.send(down.hdr);
  REQUIRE(h.pump([&] { return h.acks(o2) >= 2; }));  // WS API response + executionReport REPLACED
  sim::server::SimServerStats st = fx.server.stats();
  CHECK(st.amends == 1);
  CHECK(st.replaces == 0);  // no cancelReplace was used
  CHECK(st.open_orders == 1);
  CHECK(h.venue->amends_sent() == 1);
  // Same order on the venue, under the engine's new id, and the ack says so.
  CHECK(h.last_ack(o2)->venue_order_id.view() == venue_id);
  CHECK((h.last_ack(o2)->flags & OrderAckMsg::kAmendedInPlace) != 0);
  CHECK(h.cancel_acks(o1) == 0);  // nothing was cancelled

  // A price change cannot be an amendment: cancelReplace, a new venue order, a new id.
  const ClientOrderId o3 = cid(13);
  OutReplaceMsg moved{};
  init_header(moved, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  moved.cl_ord_id = o3;
  moved.orig_cl_ord_id = o2;
  moved.venue_order_id.assign(venue_id);
  moved.price = px - Price::from_int(10);
  moved.qty = small;
  h.send(moved.hdr);
  REQUIRE(h.pump([&] { return h.acks(o3) >= 1 && h.cancel_acks(o2) >= 1; }));
  st = fx.server.stats();
  CHECK(st.amends == 1);
  CHECK(st.replaces == 1);
  CHECK(h.venue->amends_sent() == 1);
  CHECK(h.last_ack(o3)->venue_order_id.view() != venue_id);
  CHECK((h.last_ack(o3)->flags & OrderAckMsg::kAmendedInPlace) == 0);

  // A size-up at the same price is not one either (-2038 territory): cancelReplace again.
  const ClientOrderId o4 = cid(14);
  OutReplaceMsg up{};
  init_header(up, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  up.cl_ord_id = o4;
  up.orig_cl_ord_id = o3;
  up.venue_order_id.assign(h.last_ack(o3)->venue_order_id.view());
  up.price = moved.price;
  up.qty = big;
  h.send(up.hdr);
  REQUIRE(h.pump([&] { return h.acks(o4) >= 1; }));
  CHECK(fx.server.stats().replaces == 2);
  CHECK(h.venue->amends_sent() == 1);
  CHECK(h.venue->cancel_all());
}

TEST_CASE("sim_exchange: amend_keep_priority = false keeps every replace on cancelReplace") {
  ServerFixture fx;
  BinanceVenueConfig cfg = venue_config(fx);
  cfg.amend_keep_priority = false;
  VenueHarness h(std::move(cfg));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  REQUIRE(h.pump(
      [&] { return h.venue->md_feed()->synced_count() == 1 && h.live_order_channels() >= 2; }));
  const Price px = fx.server.stats().best_bid.price - Price::from_int(100);
  const ClientOrderId o1 = cid(21);
  h.send(new_order(o1,
                   Side::Buy,
                   OrderType::PostOnly,
                   TimeInForce::Gtc,
                   px,
                   Qty::from_decimal("0.004").value())
             .hdr);
  REQUIRE(h.pump([&] { return h.acks(o1) >= 1; }));
  const std::string venue_id(h.last_ack(o1)->venue_order_id.view());
  const ClientOrderId o2 = cid(22);
  OutReplaceMsg down{};
  init_header(down, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  down.cl_ord_id = o2;
  down.orig_cl_ord_id = o1;
  down.venue_order_id.assign(venue_id);
  down.price = px;
  down.qty = Qty::from_decimal("0.002").value();
  h.send(down.hdr);
  REQUIRE(h.pump([&] { return h.acks(o2) >= 1 && h.cancel_acks(o1) >= 1; }));
  CHECK(fx.server.stats().amends == 0);
  CHECK(fx.server.stats().replaces == 1);
  CHECK(h.venue->amends_sent() == 0);
  CHECK(h.last_ack(o2)->venue_order_id.view() != venue_id);
  CHECK(h.venue->cancel_all());
}

TEST_CASE(
    "sim_exchange: documented REST errors for signatures and timestamps and filters and limits") {
  ServerFixture fx;
  BlockingHttp http(fx.http());
  const std::string key_header = std::string("X-MBX-APIKEY: ") + kApiKey + "\r\n";
  auto signed_target =
      [](std::string_view path, const std::string& query, std::string_view secret) {
        return std::string(path) + "?" + query +
               "&signature=" + std::string(net::hmac_sha256_hex(secret, query).view());
      };
  auto order_query = [](std::int64_t ts, std::string_view side, std::string_view price) {
    return "symbol=BTCUSDT&side=" + std::string(side) +
           "&type=LIMIT_MAKER&quantity=0.001&price=" + std::string(price) +
           "&newOrderRespType=ACK&recvWindow=5000&timestamp=" + std::to_string(ts);
  };
  auto now = [&] { return fx.server.server_time_ms(); };

  HttpReply r = http.request(
      "POST",
      signed_target("/api/v3/order", order_query(now(), "BUY", "50000"), "wrong-secret"),
      key_header);
  CHECK(r.status == 400);
  CHECK(r.body == R"({"code":-1022,"msg":"Signature for this request is not valid."})");

  r = http.request(
      "POST",
      signed_target("/api/v3/order", order_query(now() - 10'000, "BUY", "50000"), kApiSecret),
      key_header);
  CHECK(r.status == 400);
  CHECK(r.body ==
        R"({"code":-1021,"msg":"Timestamp for this request is outside of the recvWindow."})");

  r = http.request(
      "POST",
      signed_target("/api/v3/order", order_query(now() + 5'000, "BUY", "50000"), kApiSecret),
      key_header);
  CHECK(r.status == 400);
  CHECK(contains(r.body, R"("code":-1021)"));
  CHECK(contains(r.body, "ahead of the server's time"));

  r = http.request("POST",
                   signed_target("/api/v3/order", order_query(now(), "BUY", "50000"), kApiSecret),
                   "X-MBX-APIKEY: nobody\r\n");
  CHECK(r.status == 401);
  CHECK(contains(r.body, R"("code":-2015)"));

  r = http.request("POST", "/api/v3/order?" + order_query(now(), "BUY", "50000"), key_header);
  CHECK(r.status == 400);
  CHECK(contains(r.body, R"("code":-1102)"));

  // A valid order: weight and order-count headers.
  r = http.request("POST",
                   signed_target("/api/v3/order", order_query(now(), "BUY", "50000"), kApiSecret),
                   key_header);
  REQUIRE(r.status == 200);
  CHECK(contains(r.body, R"("orderId":)"));
  CHECK(r.header("X-MBX-ORDER-COUNT-10S") == "1");
  CHECK_FALSE(r.header("X-MBX-USED-WEIGHT-1M").empty());

  // LIMIT_MAKER crossing the book.
  const Price ask = fx.server.stats().best_ask.price;
  sim::server::SimServerStats st{};
  std::string cross_px;
  sim::server::append_decimal(cross_px, ask + Price::from_int(10));
  r = http.request("POST",
                   signed_target("/api/v3/order", order_query(now(), "BUY", cross_px), kApiSecret),
                   key_header);
  CHECK(r.status == 400);
  CHECK(r.body == R"({"code":-2010,"msg":"Order would immediately match and take."})");

  r = http.request(
      "POST",
      signed_target("/api/v3/order", order_query(now(), "BUY", "50000.005"), kApiSecret),
      key_header);
  CHECK(r.body == R"({"code":-1013,"msg":"Filter failure: PRICE_FILTER"})");

  r = http.request("DELETE",
                   signed_target("/api/v3/order",
                                 "symbol=BTCUSDT&orderId=999999&timestamp=" + std::to_string(now()),
                                 kApiSecret),
                   key_header);
  CHECK(r.status == 400);
  CHECK(r.body == R"({"code":-2011,"msg":"Unknown order sent."})");

  // DELETE openOrders cancels the one open order, then answers -2011 (nothing open).
  const std::string cancel_all_query = "symbol=BTCUSDT&timestamp=";
  r = http.request(
      "DELETE",
      signed_target("/api/v3/openOrders", cancel_all_query + std::to_string(now()), kApiSecret),
      key_header);
  CHECK(r.status == 200);
  CHECK(contains(r.body, R"("status":"CANCELED")"));
  r = http.request(
      "DELETE",
      signed_target("/api/v3/openOrders", cancel_all_query + std::to_string(now()), kApiSecret),
      key_header);
  CHECK(r.status == 400);
  CHECK(contains(r.body, R"("code":-2011)"));

  // Injected rate limit: 429, Retry-After, -1003; the next request is served again.
  fx.server.rate_limit_next_requests(1);
  r = http.get("/api/v3/time");
  CHECK(r.status == 429);
  CHECK_FALSE(r.header("Retry-After").empty());
  CHECK(contains(r.body, R"("code":-1003)"));
  r = http.get("/api/v3/time");
  CHECK(r.status == 200);

  // Unresponsive REST: the request is swallowed and the client times out.
  fx.server.set_rest_unresponsive(true);
  {
    BlockingHttpOptions o;
    o.timeout_ms = 300;
    BlockingHttp slow(fx.http(), o);
    const HttpReply t = slow.get("/api/v3/time");
    CHECK(t.status == 0);
    CHECK_FALSE(t.error.empty());
  }
  fx.server.set_rest_unresponsive(false);

  st = fx.server.stats();
  CHECK(st.signature_errors == 1);
  CHECK(st.timestamp_errors == 2);
  CHECK(st.rate_limited == 1);
  CHECK(st.unanswered_rest == 1);
}

TEST_CASE("sim_exchange: connector reactions to -1022 and -1021 and rejected or delayed orders") {
  ServerFixture fx;
  {
    // A wrong secret: userDataStream.subscribe.signature answers -1022 and the connector goes
    // fatal.
    VenueHarness h(venue_config(fx, "wrong-secret"));
    REQUIRE(h.venue->load_reference_data(h.instruments));  // public endpoint
    h.connect();
    REQUIRE(h.pump([&] { return h.venue->fatal(); }));
    CHECK(fx.server.stats().signature_errors >= 1);
  }
  VenueHarness h(venue_config(fx));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  REQUIRE(h.pump(
      [&] { return h.venue->md_feed()->synced_count() == 1 && h.live_order_channels() >= 2; }));
  const Price far = fx.server.stats().best_bid.price - Price::from_int(200);
  const Qty qty = Qty::from_decimal("0.001").value();
  const sim::server::SimServerStats base = fx.server.stats();

  // -1021 once: the order is rejected and the connector re-reads the server time.
  fx.server.fail_next_timestamp();
  h.send(new_order(cid(10), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, far, qty).hdr);
  REQUIRE(h.pump([&] { return h.reject(cid(10)) != nullptr; }));
  CHECK(h.reject(cid(10))->venue_code == -1021);
  REQUIRE(h.pump([&] { return fx.server.stats().time_requests > base.time_requests; }));

  // Rejected by fault injection (-2010).
  fx.server.reject_next_orders(1);
  h.send(new_order(cid(11), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, far, qty).hdr);
  REQUIRE(h.pump([&] { return h.reject(cid(11)) != nullptr; }));
  CHECK(h.reject(cid(11))->venue_code == -2010);

  // Delayed acks: the response and its executionReport arrive after the configured delay.
  fx.server.set_ack_delay_ms(300);
  const std::int64_t t0 = net::Reactor::now_ns();
  h.send(new_order(cid(12), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, far, qty).hdr);
  REQUIRE(h.pump([&] { return h.acks(cid(12)) >= 1; }));
  CHECK(net::Reactor::now_ns() - t0 >= 280'000'000);
  fx.server.set_ack_delay_ms(0);
  CHECK(h.venue->cancel_all());
  CHECK_FALSE(h.venue->fatal());
}

TEST_CASE("sim_exchange: the generated book is deterministic for a seed") {
  sim::server::SimServerConfig cfg = test_server_config();
  const sim::server::SimExchangeServer a(cfg);
  const sim::server::SimExchangeServer b(cfg);
  const sim::server::SimServerStats sa = a.stats();
  const sim::server::SimServerStats sb = b.stats();
  CHECK(sa.best_bid == sb.best_bid);
  CHECK(sa.best_ask == sb.best_ask);
  CHECK(sa.last_update_id == sb.last_update_id);
  cfg.seed += 1;
  const sim::server::SimExchangeServer c(cfg);
  const sim::server::SimServerStats sc = c.stats();
  CHECK((sc.best_bid != sa.best_bid || sc.best_ask != sa.best_ask));

  std::string text;
  sim::server::append_decimal(text, Qty::from_decimal("0.001").value());
  CHECK(text == "0.00100000");
}

TEST_CASE("sim_exchange: Ed25519 key logs on once per connection and trades unsigned") {
  sim::server::SimServerConfig cfg = test_server_config();
  cfg.ed25519_public_key_pem = fastmm::test::fixture("binance/ed25519-test-public.pem");
  ServerFixture fx(std::move(cfg));
  BinanceVenueConfig vc = venue_config(fx);
  vc.credentials.secret.value.clear();
  vc.credentials.type = binance::KeyType::Ed25519;
  vc.credentials.private_key_pem.value = fastmm::test::fixture("binance/ed25519-test-private.pem");
  VenueHarness h(std::move(vc));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  // Order channel: session.logon, then Live. User channel: session.logon, then the unsigned
  // userDataStream.subscribe.
  REQUIRE(h.pump(
      [&] { return h.venue->md_feed()->synced_count() == 1 && h.live_order_channels() >= 2; }));
  sim::server::SimServerStats st = fx.server.stats();
  CHECK(st.session_logons == 2);
  CHECK(st.user_subscriptions == 1);
  CHECK(st.signature_errors == 0);
  const Price far = st.best_bid.price - Price::from_int(150);
  const Qty qty = Qty::from_decimal("0.001").value();

  // order.place without apiKey/signature: ack from the response and from executionReport NEW.
  h.send(new_order(cid(20), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, far, qty).hdr);
  REQUIRE(h.pump([&] { return h.acks(cid(20)) >= 2; }));
  OutCancelMsg cancel{};
  init_header(cancel, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  cancel.cl_ord_id = cid(20);
  cancel.venue_order_id.assign(h.last_ack(cid(20))->venue_order_id.view());
  h.send(cancel.hdr);
  REQUIRE(h.pump([&] { return h.cancel_acks(cid(20)) >= 2; }));

  // REST with an Ed25519 signature (percent-encoded base64): the kill switch.
  h.send(new_order(cid(21), Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, far, qty).hdr);
  REQUIRE(h.pump([&] { return h.acks(cid(21)) >= 1; }));
  CHECK(h.venue->cancel_all());
  CHECK(fx.server.stats().open_orders == 0);
  CHECK(fx.server.stats().signature_errors == 0);
  CHECK(fx.server.stats().session_logons == 2);
  CHECK_FALSE(h.venue->fatal());
}

TEST_CASE("sim_exchange: session.logon with an HMAC account is refused and fatal") {
  ServerFixture fx;  // HMAC account: an Ed25519 signature cannot verify
  BinanceVenueConfig vc = venue_config(fx);
  vc.credentials.secret.value.clear();
  vc.credentials.type = binance::KeyType::Ed25519;
  vc.credentials.private_key_pem.value = fastmm::test::fixture("binance/ed25519-test-private.pem");
  VenueHarness h(std::move(vc));
  REQUIRE(h.venue->load_reference_data(h.instruments));
  h.connect();
  REQUIRE(h.pump([&] { return h.venue->fatal(); }));
  CHECK(fx.server.stats().session_logons == 0);
  CHECK(fx.server.stats().signature_errors >= 1);
}

TEST_CASE("sim_exchange: resolve_venue_env takes Ed25519 keys without a secret") {
  Config cfg;
  VenueSection v;
  v.name = "binance";
  v.kind = "binance_spot";
  v.api_key = "ed-key";
  v.extra["key_type"] = "ed25519";
  cfg.venues.push_back(v);
  CHECK(live::resolve_venue_env(cfg, false, "test"));
  // A dry run keeps the API key for SBE market data (the stream needs it), not otherwise.
  cfg.venues[0].extra["md_format"] = "sbe";
  CHECK(live::resolve_venue_env(cfg, true, "test"));
  CHECK(cfg.venues[0].api_key == "ed-key");
  cfg.venues[0].extra.erase("md_format");
  CHECK(live::resolve_venue_env(cfg, true, "test"));
  CHECK(cfg.venues[0].api_key.empty());
  // HMAC keys still need the secret.
  VenueSection h;
  h.name = "hmac";
  h.kind = "binance_spot";
  h.api_key = "k";
  Config hc;
  hc.venues.push_back(h);
  CHECK_FALSE(live::resolve_venue_env(hc, false, "test"));
}
