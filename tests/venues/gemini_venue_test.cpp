// GeminiVenue against a scripted fake Gemini (REST v1 and the WebSocket API): config mapping,
// reference data, the signed order connection with cancelOnDisconnect, the start-up sweep, order
// entry (ack, fill, cancel, reject, post-only would take, a cancel before the ack), the trade
// replay after a disconnect, funding, the heartbeat, the kill-path cancel-all through a rate limit
// and a book gap. Wire formats as in the gemini_* unit tests (docs read 2026-09-30); only the
// public market data has met the real venue.
#include "fastmm/venues/gemini/gemini_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/net/crypto.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gemini;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kKey = "account-fake-key";
constexpr const char* kSecret = "fake-secret";
constexpr VenueId kVenue{1};
const InstrumentId kBtc{0};

std::int64_t now_ms() {
  return wall_now().ns / 1'000'000;
}

// A request signed as Gemini checks it: the payload names the path, the signature is the
// HMAC-SHA384 of the base64 payload.
bool rest_signed(const net::HttpRequest& r) {
  const std::string b64(r.header("X-GEMINI-PAYLOAD"));
  std::string payload;
  if (!net::base64_decode(b64, payload)) return false;
  return r.header("X-GEMINI-APIKEY") == kKey && json_str(payload, "request") == r.path &&
         !json_int(payload, "nonce").empty() && r.body.empty() &&
         r.header("X-GEMINI-SIGNATURE") == net::hmac_sha384_hex(kSecret, b64).view();
}

std::string payload_of(const net::HttpRequest& r) {
  std::string payload;
  static_cast<void>(net::base64_decode(std::string(r.header("X-GEMINI-PAYLOAD")), payload));
  return payload;
}

std::string order_event(const std::string& status,
                        long long order_id,
                        const std::string& cl,
                        const std::string& extra = "") {
  return R"({"e":"orderUpdate","E":1790730000000000000,"s":"BTCGUSDPERP","i":)" +
         std::to_string(order_id) + R"(,"c":")" + cl + R"(","X":")" + status + "\"" + extra +
         R"(,"T":1790730000000000000})";
}

std::string trade_row(long long tid, long long order_id, const std::string& cl, long long ts) {
  return R"({"price":"83000.50","amount":"0.0004","timestamp":)" + std::to_string(ts / 1000) +
         R"(,"timestampms":)" + std::to_string(ts) +
         R"(,"type":"Buy","aggressor":false,"fee_currency":"GUSD","fee_amount":"0.01","tid":)" +
         std::to_string(tid) + R"(,"order_id":")" + std::to_string(order_id) +
         R"(","client_order_id":")" + cl +
         R"(","exchange":"gemini","is_auction_fill":false,"break":"","symbol":"BTCGUSDPERP"})";
}

std::string snapshot(long long id) {
  return R"({"e":"depthUpdate","E":1790730770578214134,"s":"btcgusdperp","U":)" +
         std::to_string(id) + R"(,"u":)" + std::to_string(id) +
         R"(,"b":[["83417.500","0.1266"],["83415.000","0.1210"]],"a":[["83456.000","0.1210"]]})";
}

struct Harness {
  FakeVenueServer srv;
  std::mutex mu;  // guards the replies below, which tests change while the server runs
  std::string details = fastmm::test::fixture("gemini/symbol_details_btcgusdperp.json");
  std::string open_orders = "[]";
  std::string positions = R"({"openPositions":[]})";
  std::string funding = "[]";
  std::string orders_check_reply = "[]";                  // /v1/orders at start-up and in snapshots
  std::vector<std::pair<long long, std::string>> trades;  // time ms, row
  int cancel_session_429 = 0;  // the next cancel/session requests answer 429
  bool heartbeat_ok = true;
  // order.place behaviour, keyed by clientOrderId (server thread only).
  std::map<std::string, std::string> place_mode;  // "reject" | "take" | "fill" | "hold"
  std::map<long long, std::string> held;          // order id -> clientOrderId, NEW not sent
  std::map<long long, long long> filled;          // order id -> cum, 1e-4 units
  long long next_order_id = 73797746498585000;
  long long next_tid = 1893456012054000;
  net::WsSession* order_session = nullptr;  // server thread only
  std::atomic<int> signed_ok{0};
  std::atomic<int> signed_bad{0};
  std::atomic<int> upgrades_ok{0};
  std::atomic<int> upgrades_bad{0};
  std::vector<long long> nonces;  // upgrade nonces, in order (under mu)

  void count(const net::HttpRequest& r) { ++(rest_signed(r) ? signed_ok : signed_bad); }
  std::string reply(const std::string& s) {
    const std::lock_guard lock(mu);
    return s;
  }
  // A fill streamed on the order connection now and listed by mytrades (server thread).
  void fill(long long order_id, const std::string& cl, bool stream) {
    const long long tid = next_tid++;
    const long long ts = now_ms();
    {
      const std::lock_guard lock(mu);
      trades.emplace_back(ts, trade_row(tid, order_id, cl, ts));
    }
    filled[order_id] += 4;
    if (stream && order_session != nullptr) {
      order_session->send_text(order_event(
          "PARTIALLY_FILLED",
          order_id,
          cl,
          R"(,"S":"BUY","q":"0.0010","z":")" +
              std::string(filled[order_id] >= 10 ? "0" : "0.0006") +
              R"(","Z":"0.0004","L":"83000.50","t":)" + std::to_string(tid) + R"(,"m":true)"));
    }
  }

  Harness() {
    srv.route("GET", "/v1/symbols/details/btcgusdperp", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, reply(details));
    });
    srv.route("POST", "/v1/orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "orders");
      return net::HttpServerResponse::json(200, reply(open_orders));
    });
    srv.route("POST", "/v1/positions", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "positions");
      return net::HttpServerResponse::json(200, reply(positions));
    });
    srv.route("POST", "/v1/mytrades", [this](const net::HttpRequest& r) {
      count(r);
      const std::string p = payload_of(r);
      srv.record("rest", "mytrades");
      srv.record("mytrades", p);
      const long long since = std::stoll(json_int(p, "timestamp"));
      std::string body = "[";
      const std::lock_guard lock(mu);
      std::vector<std::pair<long long, std::string>> rows = trades;
      std::sort(
          rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      bool first = true;
      for (const auto& [ts, row] : rows) {
        if (ts < since) continue;
        if (!first) body += ',';
        first = false;
        body += row;
      }
      body += ']';
      return net::HttpServerResponse::json(200, body);
    });
    srv.route("POST", "/v1/perpetuals/fundingPayment", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "funding");
      srv.record("funding", std::string(r.query));
      return net::HttpServerResponse::json(200, reply(funding));
    });
    srv.route("POST", "/v1/order/cancel/session", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "cancel/session");
      const std::lock_guard lock(mu);
      if (cancel_session_429 > 0) {
        --cancel_session_429;
        return net::HttpServerResponse::json(
            429, R"({"result":"error","reason":"RateLimit","message":"Too Many Requests"})");
      }
      return net::HttpServerResponse::json(
          200,
          R"({"result":"ok","details":{"cancelledOrders":[73797746498585000],"cancelRejects":[]}})");
    });
    srv.route("POST", "/v1/order/cancel", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "cancel");
      srv.record("rest_cancel", payload_of(r));
      return net::HttpServerResponse::json(
          200,
          R"({"order_id":")" + json_int(payload_of(r), "order_id") +
              R"(","symbol":"btcgusdperp","side":"buy","type":"exchange limit","is_live":false,"is_cancelled":true,"executed_amount":"0","original_amount":"0.001","price":"83000.50"})");
    });
    srv.route("POST", "/v1/heartbeat", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "heartbeat");
      const std::lock_guard lock(mu);
      return heartbeat_ok ? net::HttpServerResponse::json(200, R"({"result":"ok"})")
                          : net::HttpServerResponse::json(
                                503, R"({"result":"error","reason":"System","message":"down"})");
    });
    srv.on_upgrade([this](std::string_view path, std::string_view, const net::HttpHeaders& h) {
      if (path != "/orders") return true;
      const std::string nonce(h.get("X-GEMINI-NONCE"));
      const std::string b64(h.get("X-GEMINI-PAYLOAD"));
      const bool ok = h.get("X-GEMINI-APIKEY") == kKey && !nonce.empty() &&
                      b64 == net::base64_encode(nonce) &&
                      h.get("X-GEMINI-SIGNATURE") == net::hmac_sha384_hex(kSecret, b64).view();
      ++(ok ? upgrades_ok : upgrades_bad);
      if (ok) {
        const std::lock_guard lock(mu);
        nonces.push_back(std::stoll(nonce));
      }
      return ok;  // Gemini answers 401; the fake refuses the upgrade
    });
    srv.on_ws_text("/md", [this](net::WsSession& s, std::string_view t) {
      const std::string method = json_str(t, "method");
      const std::string id = json_str(t, "id");
      if (method == "ping" || method == "time") {
        s.send_text(R"({"id":")" + id + R"(","status":200,"result":{"serverTime":)" +
                    std::to_string(now_ms()) + "}}");
        return;
      }
      srv.record("md", std::string(t));
      if (method == "unsubscribe") {
        s.send_text(R"({"id":")" + id + R"(","status":200})");
        return;
      }
      if (method != "subscribe") return;
      s.send_text(R"({"id":")" + id + R"(","status":200})");
      s.send_text(snapshot(id == "md" ? 1000 : 2000));
    });
    srv.on_ws_text("/orders", [this](net::WsSession& s, std::string_view t) {
      const std::string method = json_str(t, "method");
      const std::string id = json_str(t, "id");
      if (method == "ping") {
        s.send_text(R"({"id":"ping","status":200})");
        return;
      }
      srv.record("orders", std::string(t));
      if (method == "subscribe") {
        order_session = &s;
        s.send_text(R"({"id":")" + id + R"(","status":200})");
        return;
      }
      if (method == "order.place") {
        const std::string cl = json_str(t, "clientOrderId");
        const std::string mode = place_mode.contains(cl) ? place_mode[cl] : "";
        if (mode == "reject") {
          s.send_text(R"({"id":")" + id +
                      R"(","status":400,"error":{"code":-2010,"msg":"InsufficientFunds"}})");
          return;
        }
        const long long oid = next_order_id++;
        if (mode == "hold") {  // the reply names no order either
          s.send_text(R"({"id":")" + id + R"(","status":200,"result":{}})");
          held[oid] = cl;
          return;
        }
        s.send_text(R"({"id":")" + id + R"(","status":200,"result":{"orderId":")" +
                    std::to_string(oid) + R"("}})");
        s.send_text(order_event("NEW", oid, cl, R"(,"S":"BUY","q":"0.0010","z":"0.0010")"));
        if (mode == "take")
          s.send_text(order_event("CANCELED", oid, cl, R"(,"r":"MakerOrCancelWouldTake")"));
        if (mode == "fill") fill(oid, cl, true);
        return;
      }
      if (method == "order.cancel") {
        const long long oid = std::stoll(json_int(t, "orderId"));
        s.send_text(R"({"id":")" + id + R"(","status":200,"result":{}})");
        const std::string cum = filled.contains(oid) ? "0.000" + std::to_string(filled[oid]) : "";
        s.send_text(order_event("CANCELED",
                                oid,
                                json_str(t, "id").substr(1),
                                cum.empty() ? "" : R"(,"Z":")" + cum + "\""));
      }
    });
    srv.start();
  }
  // The server thread reads the members above: stop it before they go.
  ~Harness() { srv.stop(); }

  // Sends the NEW events held back (server thread).
  void release_held() {
    srv.run_on_server([this] {
      for (const auto& [oid, cl] : held)
        order_session->send_text(
            order_event("NEW", oid, cl, R"(,"S":"BUY","q":"0.0010","z":"0.0010")"));
      held.clear();
    });
  }

  VenueSection section(bool with_keys = true) const {
    VenueSection s;
    s.name = "fake-gemini";
    s.kind = "gemini";
    s.ws_url = srv.ws_base() + "/md";
    s.ws_api_url = srv.ws_base() + "/orders";
    s.rest_url = srv.http_base();
    s.testnet = true;
    if (with_keys) {
      s.api_key = kKey;
      s.api_secret = kSecret;
    }
    s.extra["ping_interval_ms"] = "1000";
    return s;
  }
  std::vector<std::string> rest() { return srv.frames("rest"); }
  std::size_t count_rest(const std::string& what) {
    const auto r = rest();
    return static_cast<std::size_t>(std::count(r.begin(), r.end(), what));
  }
};

Instrument configured_perp() {
  Instrument i = make_instrument("btcgusdperp", 1, "BTC", "USD");
  i.tick = Price::from_decimal("0.01").value();
  return i;
}

struct Live {
  InstrumentTable instruments;
  RecordingSink md{8U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<GeminiVenue> venue;
  Collected oc;
  Collected mc;

  explicit Live(const VenueSection& section) {
    REQUIRE(instruments.add(configured_perp()));
    venue = std::make_unique<GeminiVenue>(kVenue, make_gemini_config(section, false));
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kBtc};
    venue->subscribe(ids);
    venue->connect(reactor);
  }
  ~Live() {
    venue->disconnect();
    reactor.run_once(0);
  }
  [[nodiscard]] std::size_t ends() {
    oc.take(orders);
    std::size_t n = 0;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) == EventType::Reconcile &&
          RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::End)
        ++n;
    }
    return n;
  }
  // The start-up sweep is the first End; the order channel is live once orders@account is
  // subscribed, which is also what starts the sweep.
  void wait_for_sweep() {
    REQUIRE(
        pump_until(reactor, [&] { return ends() >= 1 && venue->order_channel_live(); }, 10'000));
  }
  void pump(int iterations = 20) {
    for (int i = 0; i < iterations; ++i) reactor.run_once(5);
    oc.take(orders);
  }
  template <class M>
  std::vector<const M*> all(EventType t) const {
    std::vector<const M*> out;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) == t) out.push_back(&RecordingSink::as<M>(m));
    }
    return out;
  }
  std::vector<const ReconcileMsg*> reconcile(ReconcileMsg::Kind k) const {
    std::vector<const ReconcileMsg*> out;
    for (const ReconcileMsg* m : all<ReconcileMsg>(EventType::Reconcile)) {
      if (m->kind == k) out.push_back(m);
    }
    return out;
  }
  void push(const EventHeader& h) {
    REQUIRE(outbound.try_push(&h, h.len));
    venue->on_wake();
  }
  template <class Pred>
  void until(Pred pred, int timeout_ms = 5000) {
    REQUIRE(pump_until(
        reactor,
        [&] {
          oc.take(orders);
          return pred();
        },
        timeout_ms));
  }
};

OutNewOrderMsg new_order(const char* cl, OrderType type = OrderType::PostOnly) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, kBtc, kVenue);
  n.cl_ord_id = decode_cl_ord_id(cl).value();
  n.side = Side::Buy;
  n.type = type;
  n.price = Price::from_decimal("83000.5").value();
  n.qty = Qty::from_decimal("0.001").value();
  return n;
}

OutCancelMsg cancel_of(const char* cl, const char* venue_id = "") {
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, kBtc, kVenue);
  c.cl_ord_id = decode_cl_ord_id(cl).value();
  c.venue_order_id.assign(venue_id);
  return c;
}

}  // namespace

TEST_CASE("gemini.venue: config mapping, hosts and keys") {
  VenueSection s;
  s.name = "gemini";
  s.kind = "gemini";
  GeminiVenueConfig c = make_gemini_config(s, false);
  CHECK(c.ws_md_url == "wss://ws.sandbox.gemini.com");  // the sandbox by default
  CHECK(c.ws_order_url == c.ws_md_url);
  CHECK(c.rest_url == "https://api.sandbox.gemini.com");
  CHECK(c.sandbox);
  CHECK(c.cancel_on_disconnect);
  CHECK_FALSE(c.heartbeat);
  s.extra["cancel_on_disconnect"] = "false";
  s.extra["heartbeat"] = "true";
  s.extra["ping_interval_ms"] = "100";
  c = make_gemini_config(s, false);
  CHECK_FALSE(c.cancel_on_disconnect);
  CHECK(c.heartbeat);
  CHECK(c.ping_interval_ms == 1000);  // clamped
  // A production host with testnet = true, or the reverse, is refused.
  s.ws_url = "wss://ws.gemini.com";
  CHECK_THROWS_AS(static_cast<void>(make_gemini_config(s, false)), std::invalid_argument);
  s.testnet = false;
  CHECK_THROWS_AS(static_cast<void>(make_gemini_config(s, false)), std::invalid_argument);  // rest
  s.rest_url = "https://api.gemini.com";
  c = make_gemini_config(s, false);
  CHECK_FALSE(c.sandbox);
  s.ws_url = "wss://ws.sandbox.gemini.com";
  CHECK_THROWS_AS(static_cast<void>(make_gemini_config(s, false)), std::invalid_argument);
  // The WebSocket API takes account-scoped keys only.
  VenueSection m;
  m.name = "gemini";
  m.api_key = "master-abc";
  m.api_secret = "s";
  CHECK_THROWS_AS(static_cast<void>(make_gemini_config(m, false)), std::invalid_argument);
  CHECK_NOTHROW(static_cast<void>(make_gemini_config(m, true)));
}

TEST_CASE("gemini.venue: reference data, a linear perpetual of 1 BTC per contract") {
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(configured_perp()));
  GeminiVenue venue(kVenue, make_gemini_config(h.section(), false));
  REQUIRE(venue.load_reference_data(instruments));
  const Instrument& in = instruments.get(kBtc);
  CHECK(in.asset_class == AssetClass::Perpetual);
  CHECK(in.contract_multiplier == Qty::from_int(1));
  CHECK_FALSE(in.inverse());
  CHECK((in.flags & Instrument::kReduceOnlySupported) == 0);
  CHECK(in.enabled());
  CHECK(in.tick == Price::from_decimal("0.5").value());
  CHECK(in.lot == Qty::from_decimal("0.0001").value());
  CHECK(in.min_qty == Qty::from_decimal("0.0001").value());
  CHECK(in.base.view() == "BTC");
  CHECK(in.quote.view() == "GUSD");  // quote_currency, not the configured USD
  CHECK(in.notional(Price::from_int(80000), Qty::from_decimal("0.01").value()) ==
        Notional::from_int(800));
  // With keys the key is checked, signed.
  CHECK(h.rest() == std::vector<std::string>{"orders"});
  CHECK(h.signed_ok.load() == 1);
  CHECK(h.signed_bad.load() == 0);

  // An inverse perpetual is refused, a closed market disabled, a refused key exits 3.
  {
    const std::lock_guard lock(h.mu);
    h.details.replace(h.details.find("linear"), 6, "inverse");
  }
  InstrumentTable i2;
  REQUIRE(i2.add(configured_perp()));
  GeminiVenue v2(kVenue, make_gemini_config(h.section(), false));
  const auto r2 = v2.load_reference_data(i2);
  REQUIRE_FALSE(r2);
  CHECK(r2.error().find("inverse") != std::string::npos);
  {
    const std::lock_guard lock(h.mu);
    h.details = fastmm::test::fixture("gemini/symbol_details_btcgusdperp.json");
    h.details.replace(h.details.find("\"open\""), 6, "\"closed\"");
  }
  InstrumentTable i3;
  REQUIRE(i3.add(configured_perp()));
  GeminiVenue v3(kVenue, make_gemini_config(h.section(false), true));
  REQUIRE(v3.load_reference_data(i3));
  CHECK_FALSE(i3.get(kBtc).enabled());
}

TEST_CASE("gemini.venue: a refused key exits 3, an unreachable venue does not") {
  Harness h;
  FakeVenueServer bad;
  bad.route("GET", "/v1/symbols/details/btcgusdperp", [](const net::HttpRequest&) {
    return net::HttpServerResponse::json(
        200, fastmm::test::fixture("gemini/symbol_details_btcgusdperp.json"));
  });
  bad.route("POST", "/v1/orders", [](const net::HttpRequest&) {
    return net::HttpServerResponse::json(
        400, R"({"result":"error","reason":"InvalidSignature","message":"InvalidSignature"})");
  });
  bad.start();
  VenueSection s = h.section();
  s.rest_url = bad.http_base();
  InstrumentTable i1;
  REQUIRE(i1.add(configured_perp()));
  GeminiVenue refused(kVenue, make_gemini_config(s, false));
  const auto r = refused.load_reference_data(i1);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("InvalidSignature") != std::string::npos);
  CHECK(refused.refused_account_settings());
  bad.stop();
  // 502 without an envelope: not a setting.
  FakeVenueServer down;
  down.route("GET", "/v1/symbols/details/btcgusdperp", [](const net::HttpRequest&) {
    return net::HttpServerResponse::json(
        200, fastmm::test::fixture("gemini/symbol_details_btcgusdperp.json"));
  });
  down.route("POST", "/v1/orders", [](const net::HttpRequest&) {
    return net::HttpServerResponse::text(502, "<html>502 Bad Gateway</html>");
  });
  down.start();
  s.rest_url = down.http_base();
  InstrumentTable i2;
  REQUIRE(i2.add(configured_perp()));
  GeminiVenue unreachable(kVenue, make_gemini_config(s, false));
  REQUIRE_FALSE(unreachable.load_reference_data(i2));
  CHECK_FALSE(unreachable.refused_account_settings());
  down.stop();
}

TEST_CASE("gemini.venue: signed upgrade with cancelOnDisconnect, then the start-up sweep") {
  Harness h;
  {
    const std::lock_guard lock(h.mu);
    // An order an earlier session left resting, one on another symbol, and a short position.
    h.open_orders =
        R"([{"order_id":"555","client_order_id":"fm000000000007","symbol":"btcgusdperp","side":"sell","type":"exchange limit","price":"84000.00","original_amount":"0.002","executed_amount":"0.0005","remaining_amount":"0.0015","is_live":true},{"order_id":"556","client_order_id":"x","symbol":"ethgusdperp","side":"buy","price":"3000","original_amount":"1","executed_amount":"0"}])";
    h.positions =
        R"({"openPositions":[{"symbol":"btcgusdperp","instrument_type":"perp","quantity":"-0.0025","average_cost":"83412.5","mark_price":"83400"}]})";
  }
  {
    Live l(h.section());
    l.wait_for_sweep();
    // The order connection authenticated at the upgrade, with cancelOnDisconnect; the market
    // data asked for full snapshots.
    CHECK(h.upgrades_ok.load() == 1);
    CHECK(h.upgrades_bad.load() == 0);
    CHECK(h.srv.frames("upgrade:/orders") == std::vector<std::string>{"cancelOnDisconnect=true"});
    CHECK(h.srv.frames("upgrade:/md") == std::vector<std::string>{"snapshot=-1"});
    CHECK(h.srv.frames("orders")[0] ==
          R"({"id":"orders","method":"subscribe","params":["orders@account"]})");
    const auto begins = l.reconcile(ReconcileMsg::Kind::Begin);
    REQUIRE(begins.size() == 1);
    // The sweep says nothing about this session's orders: an empty watermark.
    CHECK((begins[0]->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK_FALSE(begins[0]->sent_watermark.valid());
    CHECK((begins[0]->flags & ReconcileMsg::kExecutionsExact) != 0);
    const auto oo = l.reconcile(ReconcileMsg::Kind::OpenOrder);
    REQUIRE(oo.size() == 1);  // the ETH order is not subscribed
    CHECK(oo[0]->cl_ord_id == decode_cl_ord_id("fm000000000007").value());
    CHECK(oo[0]->venue_order_id.view() == "555");
    CHECK(oo[0]->side == Side::Sell);
    CHECK(oo[0]->state == OrderState::PartiallyFilled);
    CHECK(oo[0]->price == Price::from_int(84000));
    CHECK(oo[0]->orig_qty == Qty::from_decimal("0.002").value());
    CHECK(oo[0]->cum_qty == Qty::from_decimal("0.0005").value());
    const auto pos = l.reconcile(ReconcileMsg::Kind::Position);
    REQUIRE(pos.size() == 1);
    CHECK(pos[0]->position_qty == Qty::from_decimal("-0.0025").value());
    CHECK(pos[0]->avg_px == Price::from_decimal("83412.5").value());
    // Key check, trades, then orders and positions; all signed.
    const auto rest = h.rest();
    REQUIRE(rest.size() >= 4);
    CHECK(rest[0] == "orders");
    CHECK(std::find(rest.begin(), rest.end(), "mytrades") != rest.end());
    CHECK(rest[rest.size() - 2] == "orders");
    CHECK(rest.back() == "positions");
    CHECK(h.signed_bad.load() == 0);
    const std::string mt = h.srv.frames("mytrades")[0];
    CHECK(json_str(mt, "symbol") == "btcgusdperp");
    CHECK(json_int(mt, "limit_trades") == "500");

    // A flat account lists nothing: the next reconciliation reports zero.
    {
      const std::lock_guard lock(h.mu);
      h.positions = R"({"openPositions":[]})";
    }
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() == 2; }));
    CHECK(l.reconcile(ReconcileMsg::Kind::Position)[1]->position_qty.is_zero());
  }
  // Without cancel_on_disconnect the URL does not ask for it.
  Harness h2;
  VenueSection s = h2.section();
  s.extra["cancel_on_disconnect"] = "false";
  {
    Live l(s);
    l.wait_for_sweep();
    CHECK(h2.srv.frames("upgrade:/orders") == std::vector<std::string>{""});
  }
}

TEST_CASE("gemini.venue: order lifecycle: ack, fill, cancel, reject, post-only that would take") {
  Harness h;
  h.srv.run_on_server([&] {
    h.place_mode["fm000100000001"] = "fill";
    h.place_mode["fm000100000002"] = "reject";
    h.place_mode["fm000100000003"] = "take";
  });
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.oc.all.clear();
    BookedPosition book;
    const OutNewOrderMsg n1 = new_order("fm000100000001");
    book.submit(n1.cl_ord_id, kBtc, kVenue, n1.side, n1.price, n1.qty);
    l.push(n1.hdr);
    l.until([&] {
      return l.oc.count(EventType::OrderAck) == 1 && l.oc.count(EventType::OrderFill) == 1;
    });
    CHECK(
        h.srv.frames("orders")[1] ==
        R"({"id":"nfm000100000001","method":"order.place","params":{"symbol":"btcgusdperp","side":"BUY","type":"LIMIT","timeInForce":"MOC","price":"83000.5","quantity":"0.001","clientOrderId":"fm000100000001"}})");
    const auto* ack = l.oc.last<OrderAckMsg>(EventType::OrderAck);
    CHECK(ack->cl_ord_id == n1.cl_ord_id);
    CHECK(ack->hdr.instrument == kBtc);
    CHECK(ack->venue_order_id.view() == "73797746498585000");
    const auto* f = l.oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(f->cl_ord_id == n1.cl_ord_id);
    CHECK(f->exec_id.view() == "1893456012054000");
    CHECK(f->qty == Qty::from_decimal("0.0004").value());
    CHECK(f->leaves_qty == Qty::from_decimal("0.0006").value());
    CHECK(f->side == Side::Buy);
    CHECK(f->liquidity == Liquidity::Maker);
    // The cancel names the venue's id; the CANCELED event carries what filled.
    l.push(cancel_of("fm000100000001", "73797746498585000").hdr);
    l.until([&] { return l.oc.count(EventType::OrderCancelAck) == 1; });
    CHECK(
        h.srv.frames("orders")[2] ==
        R"({"id":"cfm000100000001","method":"order.cancel","params":{"orderId":73797746498585000}})");
    CHECK(l.oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck)->cum_qty ==
          Qty::from_decimal("0.0004").value());
    CHECK(l.venue->shadow_count() == 0);
    // A rejected placement: 400 with -2010 and the reason.
    const OutNewOrderMsg n2 = new_order("fm000100000002");
    book.submit(n2.cl_ord_id, kBtc, kVenue, n2.side, n2.price, n2.qty);
    l.push(n2.hdr);
    l.until([&] { return l.oc.count(EventType::OrderReject) == 1; });
    const auto* rej = l.oc.last<OrderRejectMsg>(EventType::OrderReject);
    CHECK(rej->cl_ord_id == n2.cl_ord_id);
    CHECK(rej->reason == RejectReason::InsufficientBalance);
    CHECK(rej->venue_code == -2010);
    CHECK_FALSE(l.venue->fatal());
    // A post-only that would take: accepted, then cancelled by the venue -> expired.
    const OutNewOrderMsg n3 = new_order("fm000100000003");
    book.submit(n3.cl_ord_id, kBtc, kVenue, n3.side, n3.price, n3.qty);
    l.push(n3.hdr);
    l.until([&] { return l.oc.count(EventType::OrderExpired) == 1; });
    CHECK(l.oc.last<OrderExpiredMsg>(EventType::OrderExpired)->cl_ord_id == n3.cl_ord_id);
    book.drain(l.oc);
    CHECK(book.position == Qty::from_decimal("0.0004").value());
    CHECK(book.oms.open_count() == 0);
    CHECK(l.venue->shadow_count() == 0);
    // No amend: a replace is refused locally.
    OutReplaceMsg r{};
    init_header(r, EventType::OutReplace, kBtc, kVenue);
    r.cl_ord_id = decode_cl_ord_id("fm000100000009").value();
    r.orig_cl_ord_id = n1.cl_ord_id;
    l.push(r.hdr);
    l.until([&] { return l.oc.count(EventType::OrderReject) == 2; });
    CHECK_FALSE(l.venue->caps().supports_replace);
    CHECK(h.signed_bad.load() == 0);
  }
}

TEST_CASE("gemini.venue: a cancel sent before the venue named the order goes out after its NEW") {
  Harness h;
  h.srv.run_on_server([&] { h.place_mode["fm000100000001"] = "hold"; });
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.push(new_order("fm000100000001").hdr);
    REQUIRE(pump_until(l.reactor, [&] { return h.srv.frames("orders").size() == 2; }));
    l.pump();
    l.push(cancel_of("fm000100000001").hdr);  // no venue id yet
    l.pump();
    CHECK(h.srv.frames("orders").size() == 2);  // nothing to name it by: held
    h.release_held();
    l.until([&] { return l.oc.count(EventType::OrderCancelAck) == 1; });
    CHECK(
        h.srv.frames("orders")[2] ==
        R"({"id":"cfm000100000001","method":"order.cancel","params":{"orderId":73797746498585000}})");
  }
}

TEST_CASE("gemini.venue: a fill made while the order connection was down is booked once") {
  Harness h;
  h.srv.run_on_server([&] { h.place_mode["fm000100000001"] = "fill"; });
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.oc.all.clear();
    BookedPosition book;
    const OutNewOrderMsg n = new_order("fm000100000001");
    book.submit(n.cl_ord_id, kBtc, kVenue, n.side, n.price, n.qty);
    l.push(n.hdr);
    l.until([&] { return l.oc.count(EventType::OrderFill) == 1; });
    // The connection drops; a second fill happens while it is down (only mytrades has it).
    h.srv.close_sessions("/orders");
    REQUIRE(pump_until(l.reactor, [&] { return !l.venue->order_channel_live(); }));
    h.srv.run_on_server([&] {
      h.order_session = nullptr;
      h.fill(73797746498585000, "fm000100000001", false);
    });
    // Back: the reconciliation replays mytrades first (both fills), then the snapshot.
    const std::size_t ends = l.ends();
    REQUIRE(pump_until(
        l.reactor, [&] { return l.ends() > ends && l.venue->order_channel_live(); }, 15'000));
    const auto fills = l.all<OrderFillMsg>(EventType::OrderFill);
    REQUIRE(fills.size() == 3);  // streamed, then the two replayed
    CHECK((fills[1]->flags & OrderFillMsg::kReplayed) != 0);
    CHECK((fills[2]->flags & OrderFillMsg::kReplayed) != 0);
    CHECK(fills[1]->exec_id.view() == "1893456012054000");
    CHECK(fills[2]->exec_id.view() == "1893456012054001");
    CHECK(fills[2]->cl_ord_id == n.cl_ord_id);
    CHECK(fills[2]->fee == Notional::from_decimal("0.01").value());
    CHECK(fills[2]->fee_asset == FeeAsset::Quote);
    book.drain(l.oc);
    // 0.0004 streamed + 0.0004 replayed, not 0.0012.
    CHECK(book.position == Qty::from_decimal("0.0008").value());
    CHECK(book.oms.stats().duplicates >= 1);
    CHECK(h.upgrades_ok.load() == 2);
    {
      // A fresh nonce for the reconnect, above the first.
      const std::lock_guard lock(h.mu);
      REQUIRE(h.nonces.size() == 2);
      CHECK(h.nonces[1] > h.nonces[0]);
    }
    // The next replay reads from the watermark: nothing is forwarded twice.
    l.venue->request_open_orders();
    const std::size_t e2 = l.ends();
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() > e2; }));
    CHECK(l.all<OrderFillMsg>(EventType::OrderFill).size() == 3);
    // The drop also cancelled the key's orders over REST.
    CHECK(h.count_rest("cancel/session") >= 1);
  }
}

TEST_CASE("gemini.venue: funding payments are booked once") {
  Harness h;
  {
    const std::lock_guard lock(h.mu);
    const long long t = now_ms() + 1000;
    h.funding =
        R"([{"eventType":"Hourly Funding Transfer","hourlyFundingTransfer":{"eventType":"Hourly Funding Transfer","timestamp":)" +
        std::to_string(t) +
        R"(,"assetCode":"GUSD","action":"Debit","quantity":{"currency":"GUSD","value":"0.25"},"instrumentSymbol":"BTCGUSDPERP"}},{"eventType":"Hourly Funding Transfer","hourlyFundingTransfer":{"timestamp":)" +
        std::to_string(t) +
        R"(,"assetCode":"GUSD","action":"Credit","quantity":{"currency":"GUSD","value":"0.1"},"instrumentSymbol":"ETHGUSDPERP"}}])";
  }
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.until([&] { return l.oc.count(EventType::Funding) >= 1; });
    auto got = l.all<FundingMsg>(EventType::Funding);
    REQUIRE(got.size() == 1);  // ETH is not traded here
    CHECK(got[0]->amount == Notional::from_decimal("-0.25").value());
    CHECK(got[0]->asset.view() == "GUSD");
    CHECK((got[0]->flags & FundingMsg::kReplayed) != 0);
    CHECK(h.srv.frames("funding")[0].rfind("since=", 0) == 0);
    l.venue->request_open_orders();
    const std::size_t e = l.ends();
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() > e; }));
    l.pump();
    CHECK(l.all<FundingMsg>(EventType::Funding).size() == 1);
    CHECK(h.srv.frames("funding").size() >= 2);
  }
}

TEST_CASE("gemini.venue: kill-path cancel-all waits out a rate limit") {
  Harness h;
  {
    const std::lock_guard lock(h.mu);
    h.cancel_session_429 = 1;
  }
  {
    Live l(h.section());
    l.wait_for_sweep();
    CHECK(l.venue->cancel_all());
    CHECK(h.count_rest("cancel/session") == 2);
    CHECK(h.signed_bad.load() == 0);
    // A limit that never lifts gives up.
    {
      const std::lock_guard lock(h.mu);
      h.cancel_session_429 = 100;
    }
    CHECK_FALSE(l.venue->cancel_all());
    CHECK(h.count_rest("cancel/session") == 6);  // 1 + 3 retries
  }
}

TEST_CASE("gemini.venue: the heartbeat is refreshed, and a lapse kills the venue") {
  Harness h;
  VenueSection s = h.section();
  s.extra["heartbeat"] = "true";
  {
    Live l(s);
    l.wait_for_sweep();
    REQUIRE(pump_until(l.reactor, [&] { return h.count_rest("heartbeat") >= 1; }));
    l.pump();
    // Refreshed after half the window.
    l.venue->on_timer(net::Reactor::now_ns() + 16'000'000'000);
    REQUIRE(pump_until(l.reactor, [&] { return h.count_rest("heartbeat") >= 2; }));
    l.pump();
    CHECK_FALSE(l.venue->fatal());
    // The venue stops accepting it: the window runs out, the venue kills itself.
    {
      const std::lock_guard lock(h.mu);
      h.heartbeat_ok = false;
    }
    l.venue->on_timer(net::Reactor::now_ns() + 20'000'000'000);
    REQUIRE(pump_until(l.reactor, [&] { return h.count_rest("heartbeat") >= 3; }));
    l.pump();
    l.venue->on_timer(net::Reactor::now_ns() + 64'000'000'000);
    CHECK(l.venue->fatal());
    l.oc.take(l.orders);
    const ControlMsg* kill = l.oc.last<ControlMsg>(EventType::Control);
    REQUIRE(kill != nullptr);
    CHECK(kill->command == ControlCommand::TripVenueKill);
    CHECK(kill->arg == static_cast<std::uint64_t>(KillReason::DeadMansSwitchLost));
  }
}

TEST_CASE("gemini.venue: a book gap resubscribes the depth stream for a new snapshot") {
  Harness h;
  {
    Live l(h.section(false));
    REQUIRE(pump_until(l.reactor, [&] {
      return l.venue->md_feed() != nullptr && l.venue->md_feed()->synced_count() == 1;
    }));
    CHECK(json_str(h.srv.frames("md")[0], "method") == "subscribe");
    // 1000 -> U 1050: a gap.
    h.srv.send_to(
        "/md",
        R"({"e":"depthUpdate","E":1790730770606322654,"s":"btcgusdperp","U":1050,"u":1060,"b":[],"a":[["83473.500","0.3633"]]})");
    REQUIRE(pump_until(l.reactor, [&] {
      for (const std::string& f : h.srv.frames("md")) {
        if (json_str(f, "method") == "unsubscribe") return true;
      }
      return false;
    }));
    REQUIRE(pump_until(l.reactor, [&] { return l.venue->md_feed()->synced_count() == 1; }));
    CHECK(l.venue->md_feed()->resync_count() == 1);
    l.mc.take(l.md);
    bool resyncing = false;
    for (const auto& m : l.mc.all) {
      resyncing =
          resyncing || (RecordingSink::type_of(m) == EventType::ConnectionState &&
                        RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Resyncing);
    }
    CHECK(resyncing);
    CHECK(l.mc.count(EventType::BookSnapshot) == 2);
    // The clock offset came from the `time` method.
    CHECK(l.venue->clock_offset_ms() < 5000);
    CHECK(l.venue->clock_offset_ms() > -5000);
  }
}
