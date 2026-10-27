// AlpacaVenue against a scripted fake Alpaca: reference data, the key headers on every request and
// the auth frame on both streams, the start-up sweep with positions, an order placed, filled in
// part, replaced and cancelled with its states from trade_updates in binary frames, rejects, the
// fill replay after the trading stream dropped (no fill booked twice), the cancel-all on that loss
// and the blocking kill-switch cancel-all, and quotes into the book. Formats as recorded on the
// paper account (alpaca_test.cpp); the real account: scripts/alpaca-paper-check.sh.
#include "fastmm/venues/alpaca/alpaca_venue.hpp"

#include "fake_venue_util.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::alpaca;
using namespace fastmm::venues::test;

namespace {

constexpr VenueId kVenue{0};
const InstrumentId kSpy{0};
constexpr const char* kKey = "PKFAKEKEY";
constexpr const char* kSecret = "fakesecret";
constexpr const char* kOldOrder = "aaaaaaaa-2222-4333-8444-000000000007";
constexpr const char* kWebOrder = "bbbbbbbb-2222-4333-8444-000000000009";

std::string order_id(int n) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "11111111-2222-4333-8444-%012d", n);
  return buf;
}

struct FakeOrder {
  std::string id;
  std::string client;
  std::string side = "buy";
  std::string qty = "2";
  std::string filled = "0";
  std::string price = "765.37";
  std::string status = "new";
  std::string replaced_by;
};

std::string order_json(const FakeOrder& o) {
  return R"({"id":")" + o.id + R"(","client_order_id":")" + o.client +
         R"(","created_at":"2026-10-27T13:31:15.430700416Z","replaced_by":)" +
         (o.replaced_by.empty() ? std::string("null") : "\"" + o.replaced_by + "\"") +
         R"(,"symbol":"SPY","asset_class":"us_equity","qty":")" + o.qty + R"(","filled_qty":")" +
         o.filled + R"(","type":"limit","side":")" + o.side +
         R"(","time_in_force":"day","limit_price":")" + o.price + R"(","status":")" + o.status +
         R"(","extended_hours":false})";
}

std::string open_order(const std::string& id, const std::string& client) {
  FakeOrder o;
  o.id = id;
  o.client = client;
  return order_json(o);
}

std::string fill_activity(const std::string& exec, const std::string& order, const char* qty) {
  return R"({"id":"20261027093000000::)" + exec +
         R"(","activity_type":"FILL","transaction_time":")" +
         std::string(coinbase::format_time_ms(wall_now().ns / 1'000'000).view()) +
         R"(","type":"partial_fill","price":"765.37","qty":")" + qty +
         R"(","side":"buy","symbol":"SPY","leaves_qty":"1","order_id":")" + order +
         R"(","cum_qty":"1","order_status":"partially_filled"})";
}

std::vector<std::byte> bytes(const std::string& s) {
  std::vector<std::byte> b(s.size());
  std::memcpy(b.data(), s.data(), s.size());
  return b;
}

struct Harness {
  FakeVenueServer srv;
  std::mutex mu;
  std::string open_orders = "[]";
  std::string positions = R"([{"symbol":"SPY","qty":"3","avg_entry_price":"760.5","side":"long"}])";
  std::string activities = "[]";
  std::string post_error;  // non-empty: POST /v2/orders answers 403 with this message
  bool fill_on_new = true;
  std::atomic<int> keyed{0};
  std::atomic<int> unkeyed{0};
  std::atomic<int> deletes{0};
  // Server thread only.
  std::map<std::string, FakeOrder> orders;
  int next_order = 1;
  int next_exec = 1;

  void count(const net::HttpRequest& r) {
    ++(r.header("APCA-API-KEY-ID") == kKey && r.header("APCA-API-SECRET-KEY") == kSecret ? keyed
                                                                                         : unkeyed);
  }
  std::string get(const std::string& s) {
    const std::lock_guard lock(mu);
    return s;
  }
  void push(const std::string& event, const FakeOrder& o, const std::string& extra = {}) {
    const std::string f = R"({"stream":"trade_updates","data":{"event":")" + event +
                          R"(","timestamp":"2026-10-27T13:30:00.000123456Z")" + extra +
                          R"(,"order":)" + order_json(o) + "}}";
    srv.send_binary_here("/stream", bytes(f));
  }

  Harness() {
    srv.route("GET", "/v2/clock", [this](const net::HttpRequest& r) {
      count(r);
      std::string now(coinbase::format_time_ms(wall_now().ns / 1'000'000).view());
      now.pop_back();  // the venue's time with its offset, "+00:00" for UTC
      return net::HttpServerResponse::json(
          200,
          R"({"is_open":true,"next_close":"2026-10-27T16:00:00-04:00","next_open":"2026-10-28T09:30:00-04:00","timestamp":")" +
              now + R"(+00:00"})");
    });
    srv.route("GET", "/v2/assets/SPY", [this](const net::HttpRequest& r) {
      count(r);
      return net::HttpServerResponse::json(
          200,
          R"({"symbol":"SPY","status":"active","tradable":true,"shortable":true,"fractionable":false})");
    });
    srv.route("GET", "/v2/orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "open?" + std::string(r.query));
      return net::HttpServerResponse::json(200, get(open_orders));
    });
    srv.route("GET", "/v2/positions", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "positions");
      return net::HttpServerResponse::json(200, get(positions));
    });
    srv.route("GET", "/v2/account/activities/FILL", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "fills?" + std::string(r.query));
      return net::HttpServerResponse::json(200, get(activities));
    });
    srv.route("POST", "/v2/orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("new", std::string(r.body));
      {
        const std::lock_guard lock(mu);
        if (!post_error.empty())
          return net::HttpServerResponse::json(
              403, R"({"code":40310000,"message":")" + post_error + "\"}");
      }
      FakeOrder o;
      o.id = order_id(next_order++);
      o.client = json_str(r.body, "client_order_id");
      o.qty = json_str(r.body, "qty");
      o.price = json_str(r.body, "limit_price");
      o.side = json_str(r.body, "side");
      o.status = "pending_new";
      const std::string reply = order_json(o);
      o.status = "new";
      orders[o.id] = o;
      push("new", o);
      if (fill_on_new) {
        o.filled = "1";
        o.status = "partially_filled";
        orders[o.id] = o;
        push("partial_fill",
             o,
             R"(,"execution_id":"e)" + std::to_string(next_exec++) +
                 R"(","price":"765.37","qty":"1","position_qty":"4")");
      }
      return net::HttpServerResponse::json(200, reply);
    });
    srv.fallback([this](const net::HttpRequest& r) {
      count(r);
      const std::string path(r.path);
      const std::string prefix = "/v2/orders/";
      if (path.rfind(prefix, 0) != 0) return net::HttpServerResponse::json(404, "{}");
      const std::string id = path.substr(prefix.size());
      srv.record("rest", std::string(r.method) + " " + id);
      auto it = orders.find(id);
      if (r.method == "DELETE") {
        ++deletes;
        if (it == orders.end())
          return net::HttpServerResponse::json(
              404, R"({"code":40410000,"message":"order not found for )" + id + "\"}");
        if (it->second.status != "canceled" && it->second.status != "replaced") {
          it->second.status = "canceled";
          push("canceled", it->second);
        }
        return net::HttpServerResponse::json(204, "");
      }
      if (r.method == "PATCH") {
        srv.record("patch", std::string(r.body));
        if (it == orders.end() || it->second.status == "canceled")
          return net::HttpServerResponse::json(
              422, R"({"code":42210000,"message":"order is not open"})");
        FakeOrder n;
        n.id = order_id(next_order++);
        n.client = json_str(r.body, "client_order_id");
        n.qty = json_str(r.body, "qty");
        n.price = json_str(r.body, "limit_price");
        n.side = it->second.side;
        it->second.status = "replaced";
        it->second.replaced_by = n.id;
        orders[n.id] = n;
        push("replaced", it->second);
        return net::HttpServerResponse::json(200, order_json(n));
      }
      if (r.method == "GET") {
        const std::string client = id == kOldOrder ? "fm000000000007" : "web-1";
        FakeOrder o;
        o.id = id;
        o.client = client;
        return net::HttpServerResponse::json(200, order_json(o));
      }
      return net::HttpServerResponse::json(405, "{}");
    });
    srv.on_ws_text("/stream", [](net::WsSession& s, std::string_view t) {
      if (t.find(R"("action":"auth")") != std::string_view::npos) {
        const bool ok = json_str(t, "key") == kKey && json_str(t, "secret") == kSecret;
        s.send_binary(bytes(
            std::string(R"({"stream":"authorization","data":{"action":"authenticate","status":")") +
            (ok ? "authorized" : "unauthorized") + R"("}})"));
      } else if (t.find(R"("action":"listen")") != std::string_view::npos) {
        s.send_binary(bytes(R"({"stream":"listening","data":{"streams":["trade_updates"]}})"));
      }
    });
    srv.on_ws_open("/v2/iex", [](net::WsSession& s) {
      s.send_text(R"([{"T":"success","msg":"connected"}])");
    });
    srv.on_ws_text("/v2/iex", [](net::WsSession& s, std::string_view t) {
      if (t.find(R"("action":"auth")") != std::string_view::npos) {
        s.send_text(R"([{"T":"success","msg":"authenticated"}])");
      } else if (t.find(R"("action":"subscribe")") != std::string_view::npos) {
        s.send_text(R"([{"T":"subscription","trades":["SPY"],"quotes":["SPY"]}])");
      }
    });
    srv.start();
  }
  ~Harness() { srv.stop(); }

  VenueSection section() const {
    VenueSection s;
    s.name = "fake-alpaca";
    s.kind = "alpaca";
    s.ws_url = srv.ws_base() + "/v2/iex";
    s.rest_url = srv.http_base();
    s.api_key = kKey;
    s.api_secret = kSecret;
    s.extra["ws_private_url"] = srv.ws_base() + "/stream";
    return s;
  }
  void set(std::string& field, std::string v) {
    const std::lock_guard lock(mu);
    field = std::move(v);
  }
};

struct Live {
  InstrumentTable instruments;
  RecordingSink md{1U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<AlpacaVenue> venue;
  Collected oc;
  Collected mc;

  explicit Live(const VenueSection& section) {
    Instrument i = make_instrument("SPY", 0, "", "");
    i.tick = Price::from_decimal("0.05").value();
    REQUIRE(instruments.add(i));
    venue = std::make_unique<AlpacaVenue>(kVenue, make_alpaca_config(section, false));
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, nullptr);
    const InstrumentId ids[] = {kSpy};
    venue->subscribe(ids);
    venue->connect(reactor);
  }
  ~Live() {
    venue->disconnect();
    reactor.run_once(0);
  }
  template <class Pred>
  void wait(Pred pred, int timeout_ms = 5000) {
    REQUIRE(pump_until(
        reactor,
        [&] {
          oc.take(orders);
          mc.take(md);
          return pred();
        },
        timeout_ms));
  }
  std::size_t reconciles() const {
    std::size_t n = 0;
    for (const auto& m : oc.all)
      n += RecordingSink::type_of(m) == EventType::Reconcile &&
                   RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::End
               ? 1U
               : 0U;
    return n;
  }
  void send(const EventHeader& h) {
    const EventHeader* batch[] = {&h};
    venue->send_now(batch);
  }
};

OutNewOrderMsg new_order(std::uint32_t seq, Side side = Side::Buy) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, kSpy, kVenue);
  n.cl_ord_id = make_cl_ord_id(1, seq);
  n.side = side;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("765.37").value();
  n.qty = Qty::from_int(2);
  return n;
}

}  // namespace

TEST_CASE("alpaca.venue: reference data, sweep, order lifecycle, replay and cancel-all") {
  Harness h;
  h.open_orders =
      "[" + open_order(kOldOrder, "fm000000000007") + "," + open_order(kWebOrder, "web-1") + "]";
  Live l(h.section());
  const Instrument& in = l.instruments.get(kSpy);
  CHECK(in.tick == Price::from_decimal("0.01").value());
  CHECK(in.lot == Qty::from_int(1));
  CHECK(in.asset_class == AssetClass::Equity);
  CHECK(in.quote.view() == "USD");

  // Start-up sweep: both open orders and the position, then the book from the first quote.
  l.wait([&] { return l.reconciles() >= 1 && l.venue->order_channel_live(); }, 10'000);
  std::size_t open = 0;
  const ReconcileMsg* pos = nullptr;
  for (const auto& m : l.oc.all) {
    if (RecordingSink::type_of(m) != EventType::Reconcile) continue;
    const auto& r = RecordingSink::as<ReconcileMsg>(m);
    if (r.kind == ReconcileMsg::Kind::OpenOrder) ++open;
    if (r.kind == ReconcileMsg::Kind::Position) pos = &r;
  }
  CHECK(open == 2);
  REQUIRE(pos != nullptr);
  CHECK(pos->position_qty == Qty::from_int(3));
  CHECK(pos->avg_px == Price::from_decimal("760.5").value());
  h.set(h.open_orders, "[]");
  // The engine cancels the earlier session's order the sweep found: by its venue id.
  OutCancelMsg old{};
  init_header(old, EventType::OutCancel, kSpy, kVenue);
  old.cl_ord_id = decode_cl_ord_id("fm000000000007").value();
  old.venue_order_id.assign(kOldOrder);
  l.send(old.hdr);
  l.wait([&] {
    const auto rest = h.srv.frames("rest");
    return std::find(rest.begin(), rest.end(), std::string("DELETE ") + kOldOrder) != rest.end();
  });
  h.srv.send_to(
      "/v2/iex",
      R"([{"T":"q","S":"SPY","bx":"V","bp":765.3,"bs":2,"ax":"V","ap":765.4,"as":3,"c":["R"],"z":"B","t":"2026-10-27T13:30:00.5Z"}])");
  l.wait([&] { return l.mc.count(EventType::BookSnapshot) >= 1; });

  // New: acknowledged by the reply, filled in part on the stream.
  const OutNewOrderMsg n = new_order(1);
  l.send(n.hdr);
  l.wait([&] { return l.oc.count(EventType::OrderFill) >= 1; });
  const auto news = h.srv.frames("new");
  REQUIRE(news.size() == 1);
  CHECK(json_str(news[0], "type") == "limit");
  CHECK(json_str(news[0], "time_in_force") == "day");
  const auto* fill = l.oc.last<OrderFillMsg>(EventType::OrderFill);
  CHECK(fill->cl_ord_id == n.cl_ord_id);
  CHECK(fill->exec_id.view() == "e1");
  CHECK(fill->qty == Qty::from_int(1));
  CHECK(fill->cum_qty == Qty::from_int(1));
  CHECK(fill->leaves_qty == Qty::from_int(1));
  CHECK(l.oc.first_if<OrderAckMsg>(EventType::OrderAck, [&](const OrderAckMsg& a) {
    return a.cl_ord_id == n.cl_ord_id && a.venue_order_id.view() == order_id(1);
  }) != nullptr);

  // Replace: PATCH the venue's order; the reply acknowledges the new client id.
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, kSpy, kVenue);
  r.cl_ord_id = make_cl_ord_id(1, 2);
  r.orig_cl_ord_id = n.cl_ord_id;
  r.price = Price::from_decimal("765.35").value();
  r.qty = Qty::from_int(1);
  l.send(r.hdr);
  l.wait([&] {
    return l.oc.first_if<OrderAckMsg>(EventType::OrderAck, [&](const OrderAckMsg& a) {
      return a.cl_ord_id == r.cl_ord_id;
    }) != nullptr;
  });
  CHECK(h.srv.frames("patch").at(0) == R"({"qty":"1","limit_price":"765.35","client_order_id":")" +
                                           std::string(encode_cl_ord_id(r.cl_ord_id).view()) +
                                           "\"}");

  // Cancel the replacement: DELETE by its venue id, the cancel from the stream.
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, kSpy, kVenue);
  c.cl_ord_id = r.cl_ord_id;
  l.send(c.hdr);
  l.wait([&] { return l.oc.count(EventType::OrderCancelAck) >= 1; });
  CHECK(l.oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck)->cl_ord_id == r.cl_ord_id);

  // A replace of an order that is over is refused by the venue: a reject of the new id.
  OutReplaceMsg r2 = r;
  r2.cl_ord_id = make_cl_ord_id(1, 3);
  r2.orig_cl_ord_id = make_cl_ord_id(1, 2);
  l.send(r2.hdr);
  l.wait([&] { return l.oc.count(EventType::OrderReject) >= 1; });

  // A refusal of a new order.
  h.set(h.post_error, "potential wash trade detected. use complex orders");
  const OutNewOrderMsg n4 = new_order(4, Side::Sell);
  l.send(n4.hdr);
  l.wait([&] {
    return l.oc.first_if<OrderRejectMsg>(EventType::OrderReject, [&](const OrderRejectMsg& m) {
      return m.cl_ord_id == n4.cl_ord_id;
    }) != nullptr;
  });
  CHECK(l.oc.last<OrderRejectMsg>(EventType::OrderReject)->reason ==
        RejectReason::SelfTradePrevention);
  h.set(h.post_error, "");

  // The trading stream drops: the open orders are cancelled over REST; on its return the fill
  // activities are replayed (e1 again, and e9 the stream never sent), then a snapshot.
  const OutNewOrderMsg n5 = new_order(5);
  l.send(n5.hdr);
  l.wait([&] { return l.oc.count(EventType::OrderFill) >= 2; });
  h.set(h.open_orders,
        "[" + open_order(order_id(3), std::string(encode_cl_ord_id(n5.cl_ord_id).view())) + "]");
  h.set(h.activities,
        "[" + fill_activity("e9", order_id(3), "1") + "," + fill_activity("e1", order_id(1), "1") +
            "]");
  const std::size_t before = l.reconciles();
  const int deletes = h.deletes.load();
  h.srv.close_sessions("/stream");
  l.wait([&] { return h.deletes.load() > deletes; });
  l.wait([&] { return l.reconciles() > before && l.venue->order_channel_live(); }, 15'000);
  std::vector<std::string> replayed;
  for (const auto& m : l.oc.all) {
    if (RecordingSink::type_of(m) != EventType::OrderFill) continue;
    const auto& f = RecordingSink::as<OrderFillMsg>(m);
    if ((f.flags & OrderFillMsg::kReplayed) != 0) {
      replayed.emplace_back(f.exec_id.view());
      if (f.exec_id.view() == "e9") CHECK(f.cl_ord_id == n5.cl_ord_id);
    }
  }
  CHECK(replayed == std::vector<std::string>{"e1", "e9"});
  CHECK(h.unkeyed.load() == 0);
  CHECK(h.keyed.load() > 10);

  // Kill switch: lists the open orders and cancels each, from another connection.
  h.set(h.open_orders, "[" + open_order(order_id(5), "fm000000000099") + "]");
  const int d2 = h.deletes.load();
  CHECK_FALSE(l.venue->cancel_all());  // the fake keeps listing it: three rounds, then false
  CHECK(h.deletes.load() == d2 + 3);
  h.set(h.open_orders, "[]");
  CHECK(l.venue->cancel_all());
}

TEST_CASE("alpaca.venue: refused keys stop order entry") {
  Harness h;
  VenueSection s = h.section();
  s.api_secret = "wrong";
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("SPY", 0, "SPY", "USD")));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  AlpacaVenue v(kVenue, make_alpaca_config(s, false));
  RecordingSink md;
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  net::Reactor reactor;
  v.attach(symbols, instruments, md.sink, orders.sink, nullptr);
  const InstrumentId ids[] = {kSpy};
  v.subscribe(ids);
  v.connect(reactor);
  REQUIRE(pump_until(reactor, [&] { return v.fatal(); }, 10'000));
  v.disconnect();
  reactor.run_once(0);
}
