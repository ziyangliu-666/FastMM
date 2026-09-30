// CoinbaseExchangeVenue against a scripted fake Coinbase Exchange: config mapping, reference data,
// the start-up sweep (paged), order entry over REST with its events from the user channel,
// rejects, cancels, fills, the fill replay after the user channel dropped (no fill booked twice,
// an order named by lookup), an engine-requested reconciliation, the kill-path cancel-all through
// a rate limit, a lost trade on the feed, a dry run, and the balances (the snapshot after the
// sweep, the balance channel, a refresh after a fill, a failed fetch). Wire formats as in the
// coinbase_* unit tests (docs read 2026-09-30); only the public feed and public REST have met the
// real venue.
#include "fastmm/venues/coinbase/coinbase_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/net/crypto.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::coinbase;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kKey = "fake-key";
// base64 of the bytes 0..63.
constexpr const char* kSecret =
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+Pw==";
constexpr const char* kPass = "fake-pass";
constexpr VenueId kVenue{1};
const InstrumentId kBtc{0};
// An order of an earlier session, and one of another client on the same profile.
constexpr const char* kOldOrder = "aaaaaaaa-77a8-460a-b958-000000000007";
constexpr const char* kForeignOrder = "bbbbbbbb-77a8-460a-b958-000000000009";

std::string b64_hmac(std::string_view data) {
  std::string key;
  REQUIRE(net::base64_decode(kSecret, key));
  std::array<std::uint8_t, net::kSha256Size> mac{};
  REQUIRE(net::hmac_sha256(key, data, mac));
  return net::base64_encode(std::span<const std::uint8_t>(mac));
}

bool rest_signed(const net::HttpRequest& r) {
  const std::string pre = std::string(r.header("CB-ACCESS-TIMESTAMP")) + std::string(r.method) +
                          std::string(r.target) + std::string(r.body);
  return r.header("CB-ACCESS-KEY") == kKey && r.header("CB-ACCESS-PASSPHRASE") == kPass &&
         r.header("CB-ACCESS-SIGN") == b64_hmac(pre);
}

bool ws_signed(std::string_view t) {
  return json_str(t, "key") == kKey && json_str(t, "passphrase") == kPass &&
         json_str(t, "signature") == b64_hmac(json_str(t, "timestamp") + "GET/users/self/verify");
}

std::string oid_of(std::uint32_t seq) {
  return std::string(encode_client_oid(make_cl_ord_id(1, seq)).view());
}

// The venue's order id for the fake's n-th order.
std::string order_id(int n) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "d50ec984-77a8-460a-b958-%012d", n);
  return buf;
}

std::string order_row(const std::string& id,
                      const std::string& oid,
                      const char* product,
                      const char* filled = "0") {
  return R"({"id":")" + id + R"(","price":"60000.10000000","size":"0.30000000","product_id":")" +
         product +
         R"(","side":"sell","type":"limit","time_in_force":"GTC","post_only":true,"created_at":"2026-09-30T01:00:00.000Z","filled_size":")" +
         filled + R"(","status":"open","settled":false,"client_oid":")" + oid + R"("})";
}

std::string fill_row(long long trade_id, const std::string& order, long long ts_ms) {
  return R"({"created_at":")" + std::string(format_time_ms(ts_ms).view()) + R"(","trade_id":)" +
         std::to_string(trade_id) + R"(,"product_id":"BTC-USD","order_id":")" + order +
         R"(","user_id":"u","profile_id":"p","liquidity":"M","price":"60000.10000000","size":"0.10000000","fee":"24.0000400000000000","side":"sell","settled":true})";
}

const std::string kSnapshot =
    R"({"type":"snapshot","product_id":"BTC-USD","asks":[["60000.20","5"]],"bids":[["60000.10","3"]],"time":"2026-09-30T01:41:50Z"})";

struct Order {
  std::string id;
  std::string oid;
  std::string size;
  double filled = 0;
};

struct Harness {
  FakeVenueServer srv;
  std::mutex mu;  // guards the replies below, which tests change while the server runs
  std::string product = fastmm::test::fixture("coinbase/product_btc_usd_sandbox.json");
  std::string open_orders = "[]";
  std::string open_orders_page2 = "[]";  // served for after=page2
  bool paged = false;
  std::string fills = "[]";
  std::vector<int> cancel_all_status;  // the next DELETE /orders answers, then 200 []
  std::string accounts = "[]";         // GET /accounts
  int accounts_status = 200;
  std::string new_reply;  // non-empty: POST /orders answers this (status 200)
  int new_status = 200;
  int cancel_status = 200;
  bool fill_on_new = true;  // a new order is matched for 0.1 at once
  std::atomic<int> signed_ok{0};
  std::atomic<int> signed_bad{0};
  std::atomic<int> ws_bad{0};
  std::atomic<int> no_agent{0};
  // Server thread only.
  net::WsSession* user = nullptr;
  std::map<std::string, Order> orders;  // by client_oid
  int next_order = 1;
  long long next_trade = 5001;

  void count(const net::HttpRequest& r) {
    ++(rest_signed(r) ? signed_ok : signed_bad);
    if (r.header("User-Agent").empty()) ++no_agent;
  }
  std::string reply(const std::string& s) {
    const std::lock_guard lock(mu);
    return s;
  }
  void push(const std::string& t) {
    if (user != nullptr) user->send_text(t);
  }
  std::string match_push(const Order& o, const char* size) {
    return R"({"type":"match","trade_id":)" + std::to_string(next_trade++) +
           R"(,"sequence":50,"maker_order_id":")" + o.id +
           R"(","taker_order_id":"132fb6ae-456b-4654-b4e0-d681ac05cea1","time":"2026-09-30T01:41:51.000001Z","product_id":"BTC-USD","size":")" +
           size +
           R"(","price":"60000.1","side":"sell","user_id":"u","profile_id":"p","maker_fee_rate":"0.004"})";
  }
  std::string done_push(const Order& o, const char* reason, const char* cancel_reason = nullptr) {
    return R"({"type":"done","time":"2026-09-30T01:41:52Z","product_id":"BTC-USD","sequence":10,"price":"60000.1","order_id":")" +
           o.id + R"(","reason":")" + reason + R"(","side":"sell","remaining_size":"0.2")" +
           (cancel_reason != nullptr ? std::string(R"(,"cancel_reason":")") + cancel_reason + "\""
                                     : "") +
           "}";
  }

  Harness() {
    srv.route("GET", "/time", [this](const net::HttpRequest& r) {
      if (r.header("User-Agent").empty()) ++no_agent;
      return net::HttpServerResponse::json(200, fastmm::test::fixture("coinbase/server_time.json"));
    });
    srv.route("GET", "/products/BTC-USD", [this](const net::HttpRequest& r) {
      if (r.header("User-Agent").empty()) ++no_agent;
      srv.record("rest", "products");
      return net::HttpServerResponse::json(200, reply(product));
    });
    srv.route("GET", "/orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "orders?" + std::string(r.query));
      const std::lock_guard lock(mu);
      if (r.query.find("after=page2") != std::string_view::npos)
        return net::HttpServerResponse::json(200, open_orders_page2);
      net::HttpServerResponse resp = net::HttpServerResponse::json(200, open_orders);
      if (paged) resp.headers.emplace_back("CB-AFTER", "page2");
      return resp;
    });
    srv.route("GET", "/accounts", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("accounts", std::string(r.target));
      const std::lock_guard lock(mu);
      if (accounts_status != 200)
        return net::HttpServerResponse::json(accounts_status, R"({"message":"Internal error"})");
      return net::HttpServerResponse::json(200, accounts);
    });
    srv.route("GET", "/fills", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "fills?" + std::string(r.query));
      srv.record("fills", std::string(r.query));
      return net::HttpServerResponse::json(200, reply(fills));
    });
    srv.route("GET", std::string("/orders/") + kOldOrder, [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "lookup");
      return net::HttpServerResponse::json(
          200,
          order_row(
              kOldOrder, std::string(encode_client_oid(make_cl_ord_id(0, 7)).view()), "BTC-USD"));
    });
    srv.route("GET", std::string("/orders/") + kForeignOrder, [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "lookup");
      return net::HttpServerResponse::json(
          200, order_row(kForeignOrder, "d50ec974-76a2-454b-66f1-35b1ea8c0000", "BTC-USD"));
    });
    srv.route("POST", "/orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("new", std::string(r.body));
      const std::string oid = json_str(r.body, "client_oid");
      {
        const std::lock_guard lock(mu);
        if (new_status != 200)
          return net::HttpServerResponse::json(new_status, R"({"message":"Insufficient funds"})");
        if (!new_reply.empty()) return net::HttpServerResponse::json(200, new_reply);
      }
      Order& o = orders[oid];
      o.id = order_id(next_order++);
      o.oid = oid;
      o.size = json_str(r.body, "size");
      push(
          R"({"type":"received","time":"2026-09-30T01:41:50.5Z","product_id":"BTC-USD","sequence":1,"order_id":")" +
          o.id + R"(","size":")" + o.size + R"(","price":")" + json_str(r.body, "price") +
          R"(","side":")" + json_str(r.body, "side") + R"(","order_type":"limit","client_oid":")" +
          oid + R"(","user_id":"u","profile_id":"p"})");
      const bool fill = [this] {
        const std::lock_guard lock(mu);
        return fill_on_new;
      }();
      if (fill) {
        push(match_push(o, "0.1"));
        o.filled += 0.1;
      }
      return net::HttpServerResponse::json(
          200,
          R"({"id":")" + o.id + R"(","price":")" + json_str(r.body, "price") + R"(","size":")" +
              o.size +
              R"(","product_id":"BTC-USD","side":"sell","type":"limit","time_in_force":"GTC","post_only":true,"created_at":"2026-09-30T01:41:50.5Z","fill_fees":"0","filled_size":"0","executed_value":"0","status":"pending","settled":false})");
    });
    for (std::uint32_t seq = 1; seq <= 4; ++seq) {
      const std::string oid = oid_of(seq);
      srv.route("DELETE", "/orders/client:" + oid, [this, oid](const net::HttpRequest& r) {
        count(r);
        srv.record("cancel", std::string(r.target));
        {
          const std::lock_guard lock(mu);
          if (cancel_status != 200)
            return net::HttpServerResponse::json(cancel_status, R"({"message":"NotFound"})");
        }
        const auto it = orders.find(oid);
        if (it == orders.end())
          return net::HttpServerResponse::json(404, R"({"message":"NotFound"})");
        push(done_push(it->second, "canceled"));
        return net::HttpServerResponse::json(200, "\"" + it->second.id + "\"");
      });
    }
    srv.route("DELETE", "/orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("cancel_all", std::string(r.query));
      const std::lock_guard lock(mu);
      if (!cancel_all_status.empty()) {
        const int st = cancel_all_status.front();
        cancel_all_status.erase(cancel_all_status.begin());
        if (st == 429)
          return net::HttpServerResponse::json(429, R"({"message":"Private rate limit exceeded"})");
        return net::HttpServerResponse::json(200, R"([")" + order_id(99) + R"("])");
      }
      return net::HttpServerResponse::json(200, "[]");
    });
    srv.on_ws_text("/feed", [this](net::WsSession& s, std::string_view t) {
      srv.record("md", std::string(t));
      if (json_str(t, "type") != "subscribe") return;
      s.send_text(
          R"({"type":"subscriptions","channels":[{"name":"level2_50","product_ids":["BTC-USD"]},{"name":"matches","product_ids":["BTC-USD"]}]})");
      if (t.find("level2_batch") != std::string_view::npos) s.send_text(kSnapshot);
    });
    srv.on_ws_text("/user", [this](net::WsSession& s, std::string_view t) {
      srv.record("user", std::string(t));
      if (json_str(t, "type") != "subscribe") return;
      if (!ws_signed(t)) {
        ++ws_bad;
        s.send_text(
            R"({"type":"error","message":"Authentication Failed","reason":"invalid signature"})");
        return;
      }
      user = &s;
      if (t.find(R"("name":"balance")") != std::string_view::npos) {
        srv.record("balance_sub", std::string(t));
        return;
      }
      s.send_text(
          R"({"type":"subscriptions","channels":[{"name":"user","product_ids":["BTC-USD"],"account_ids":null},{"name":"heartbeat","product_ids":["BTC-USD"],"account_ids":null}]})");
    });
    srv.start();
  }
  // The server thread reads the members above: stop it before they go.
  ~Harness() { srv.stop(); }

  VenueSection section(bool with_keys = true) const {
    VenueSection s;
    s.name = "fake-coinbase";
    s.kind = "coinbase_exchange";
    s.ws_url = srv.ws_base() + "/feed";
    s.rest_url = srv.http_base();
    s.testnet = true;
    if (with_keys) {
      s.api_key = kKey;
      s.api_secret = kSecret;
      s.api_passphrase = kPass;
    }
    s.supports_replace = false;
    s.extra["ws_private_url"] = srv.ws_base() + "/user";
    return s;
  }
  std::vector<std::string> rest() { return srv.frames("rest"); }
  // Runs on the server thread: the user channel as the venue would push it.
  void on_server(const std::function<void(Harness&)>& fn) {
    srv.run_on_server([this, &fn] { fn(*this); });
  }
};

struct Live {
  InstrumentTable instruments;
  RecordingSink md{8U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<CoinbaseExchangeVenue> venue;
  Collected oc;
  Collected mc;

  explicit Live(const VenueSection& section,
                const std::function<void(Live&)>& before_connect = {},
                bool dry_run = false) {
    Instrument i = make_instrument("BTC-USD", 1, "XBT", "USDC");
    i.tick = Price::from_int(1);
    REQUIRE(instruments.add(i));
    venue = std::make_unique<CoinbaseExchangeVenue>(kVenue, make_coinbase_config(section, dry_run));
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kBtc};
    venue->subscribe(ids);
    if (before_connect) before_connect(*this);
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
  // The start-up sweep is the connector's first reconciliation; the user channel (where the
  // orders' events come from) is subscribed by then.
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
  void wait(Pred pred, int timeout_ms = 5000) {
    REQUIRE(pump_until(
        reactor,
        [&] {
          oc.take(orders);
          return pred();
        },
        timeout_ms));
  }
};

OutNewOrderMsg new_order(std::uint32_t seq = 1) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, kBtc, kVenue);
  n.cl_ord_id = make_cl_ord_id(1, seq);
  n.side = Side::Sell;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("60000.1").value();
  n.qty = Qty::from_decimal("0.3").value();
  return n;
}

OutCancelMsg cancel_order(std::uint32_t seq = 1) {
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, kBtc, kVenue);
  c.cl_ord_id = make_cl_ord_id(1, seq);
  return c;
}

}  // namespace

TEST_CASE("coinbase.venue: config mapping checks keys, hosts and options") {
  VenueSection s;
  s.name = "cb";
  s.kind = "coinbase_exchange";
  s.ws_url = "wss://ws-feed-public.sandbox.exchange.coinbase.com";
  s.rest_url = "https://api-public.sandbox.exchange.coinbase.com";
  s.api_key = "k";
  s.api_secret = kSecret;
  s.api_passphrase = "p";
  CoinbaseVenueConfig c = make_coinbase_config(s, false);
  CHECK(c.sandbox);
  CHECK(c.ws_private_url == c.ws_url);
  CHECK(c.depth_channel == DepthChannel::Level2Batch);
  CHECK(c.stp == Stp::Dc);
  s.extra["depth_channel"] = "level2";
  s.extra["stp"] = "cn";
  s.extra["cancel_all_rounds"] = "50";
  c = make_coinbase_config(s, false);
  CHECK(c.depth_channel == DepthChannel::Level2);
  CHECK(c.stp == Stp::Cn);
  CHECK(c.cancel_all_rounds == 10);
  s.extra["stp"] = "none";
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_config(s, false)), std::invalid_argument);
  s.extra.erase("stp");
  s.extra["depth_channel"] = "full";
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_config(s, false)), std::invalid_argument);
  s.extra.erase("depth_channel");
  // The sandbox host with testnet = false, or production with testnet = true: refused.
  s.testnet = false;
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_config(s, false)), std::invalid_argument);
  s.ws_url = "wss://ws-feed.exchange.coinbase.com";
  s.rest_url = "https://api.exchange.coinbase.com";
  CHECK_FALSE(make_coinbase_config(s, false).sandbox);
  s.testnet = true;
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_config(s, false)), std::invalid_argument);
  s.testnet = false;
  // No replace for REST orders.
  s.supports_replace = true;
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_config(s, false)), std::invalid_argument);
  s.supports_replace = false;
  // A secret that is not base64, and a key without its passphrase.
  s.api_secret = "not base64!";
  try {
    static_cast<void>(make_coinbase_config(s, false));
    FAIL("a malformed secret accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).rfind("venues.cb.api_secret:", 0) == 0);
  }
  s.api_secret = kSecret;
  s.api_passphrase.clear();
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_config(s, false)), std::invalid_argument);
  // A dry run needs no keys; level2 needs a signature, so it falls back to level2_batch.
  VenueSection dry;
  dry.name = "cb";
  dry.ws_url = "wss://ws-feed.exchange.coinbase.com";
  dry.rest_url = "https://api.exchange.coinbase.com";
  dry.testnet = false;
  dry.extra["depth_channel"] = "level2";
  CHECK(make_coinbase_config(dry, true).depth_channel == DepthChannel::Level2Batch);
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_config(dry, false)), std::invalid_argument);
}

TEST_CASE("coinbase.venue: reference data sets tick, lot, minimum notional and currencies") {
  Harness h;
  InstrumentTable instruments;
  Instrument i = make_instrument("BTC-USD", 1, "XBT", "USDC");
  i.tick = Price::from_int(1);
  REQUIRE(instruments.add(i));
  CoinbaseExchangeVenue venue(kVenue, make_coinbase_config(h.section(), false));
  REQUIRE(venue.load_reference_data(instruments));
  const Instrument& in = instruments.get(kBtc);
  CHECK(in.tick == Price::from_decimal("0.01").value());
  CHECK(in.lot == Qty::from_decimal("0.00000001").value());
  CHECK(in.min_qty == in.lot);
  CHECK(in.min_notional == Notional::from_int(1));
  CHECK(in.base.view() == "BTC");
  CHECK(in.quote.view() == "USD");
  CHECK(in.enabled());
  CHECK(h.no_agent.load() == 0);  // the venue refuses a request without User-Agent

  // A product in cancel-only mode is disabled.
  Harness h2;
  h2.product.replace(h2.product.find(R"("cancel_only":false)"), 19, R"("cancel_only":true)");
  InstrumentTable i2;
  REQUIRE(i2.add(i));
  CoinbaseExchangeVenue v2(kVenue, make_coinbase_config(h2.section(), false));
  REQUIRE(v2.load_reference_data(i2));
  CHECK_FALSE(i2.get(kBtc).enabled());
}

TEST_CASE("coinbase.venue: the start-up sweep reads every page and replays fills first") {
  Harness h;
  // Page 1: 1000 orders of a product not traded here; page 2: an order of an earlier session.
  std::string page1 = "[";
  for (int k = 0; k < 1000; ++k) {
    if (k != 0) page1 += ',';
    page1 += order_row(order_id(500 + k), "d50ec974-76a2-454b-66f1-35b1ea8c0000", "ETH-USD");
  }
  page1 += ']';
  h.open_orders = page1;
  h.paged = true;
  h.open_orders_page2 = "[" +
                        order_row(kOldOrder,
                                  std::string(encode_client_oid(make_cl_ord_id(0, 7)).view()),
                                  "BTC-USD",
                                  "0.1") +
                        "]";
  {
    Live l(h.section());
    l.wait_for_sweep();
    const auto begins = l.reconcile(ReconcileMsg::Kind::Begin);
    REQUIRE(begins.size() == 1);
    // A sweep: an empty watermark, so it says nothing about this session's own orders.
    CHECK((begins[0]->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK_FALSE(begins[0]->sent_watermark.valid());
    CHECK((begins[0]->flags & ReconcileMsg::kExecutionsExact) != 0);
    const auto oo = l.reconcile(ReconcileMsg::Kind::OpenOrder);
    REQUIRE(oo.size() == 1);
    CHECK(oo[0]->cl_ord_id == make_cl_ord_id(0, 7));
    CHECK(oo[0]->venue_order_id.view() == kOldOrder);
    CHECK(oo[0]->state == OrderState::PartiallyFilled);
    CHECK(oo[0]->cum_qty == Qty::from_decimal("0.1").value());
    CHECK(oo[0]->orig_qty == Qty::from_decimal("0.3").value());
    CHECK(l.reconcile(ReconcileMsg::Kind::Position).empty());  // spot: no positions
    const auto rest = h.rest();
    REQUIRE(rest.size() >= 4);
    CHECK(rest[0] == "products");
    CHECK(rest[1].rfind("fills?product_id=BTC-USD&start_date=", 0) == 0);
    CHECK(rest[2] == "orders?status=open&status=pending&status=active&limit=1000");
    CHECK(rest[3] == "orders?status=open&status=pending&status=active&limit=1000&after=page2");
    CHECK(h.signed_bad.load() == 0);
    CHECK(h.ws_bad.load() == 0);
    CHECK(h.no_agent.load() == 0);
    const auto sub = h.srv.frames("user");
    REQUIRE(sub.size() == 1);
    CHECK(sub[0].find(R"("channels":["user","heartbeat"])") != std::string::npos);
    // The user channel's later events for the old order name it.
    REQUIRE(l.venue->private_parser()->find(kOldOrder) != nullptr);
    CHECK(l.venue->private_parser()->find(kOldOrder)->cl == make_cl_ord_id(0, 7));
  }
}

TEST_CASE("coinbase.venue: order round trip over REST with events from the user channel") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    const OutNewOrderMsg n = new_order();
    l.push(n.hdr);
    l.wait([&] {
      return l.oc.count(EventType::OrderAck) >= 2 && l.oc.count(EventType::OrderFill) == 1;
    });
    const auto bodies = h.srv.frames("new");
    REQUIRE(bodies.size() == 1);
    CHECK(
        bodies[0] ==
        R"({"client_oid":"666d0000-0000-4000-8000-000100000001","product_id":"BTC-USD","side":"sell","type":"limit","price":"60000.1","size":"0.3","time_in_force":"GTC","post_only":true,"stp":"dc"})");
    for (const OrderAckMsg* a : l.all<OrderAckMsg>(EventType::OrderAck)) {
      CHECK(a->cl_ord_id == n.cl_ord_id);
      CHECK(a->venue_order_id.view() == order_id(1));
    }
    const auto* f = l.oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(f->cl_ord_id == n.cl_ord_id);
    CHECK(f->exec_id.view() == "5001");
    CHECK(f->qty == Qty::from_decimal("0.1").value());
    CHECK(f->leaves_qty == Qty::from_decimal("0.2").value());
    CHECK(f->side == Side::Sell);
    CHECK(f->liquidity == Liquidity::Maker);
    CHECK(f->fee == Notional::from_decimal("24.00004").value());  // 60000.1 x 0.1 x 0.004
    CHECK(f->fee_asset == FeeAsset::Quote);
    CHECK(l.venue->shadow_count() == 1);

    const OutCancelMsg c = cancel_order();
    l.push(c.hdr);
    l.wait([&] { return l.oc.count(EventType::OrderCancelAck) == 1; });
    CHECK(h.srv.frames("cancel")[0] ==
          "/orders/client:666d0000-0000-4000-8000-000100000001?product_id=BTC-USD");
    const auto* ca = l.oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck);
    CHECK(ca->cl_ord_id == n.cl_ord_id);
    CHECK(ca->cum_qty == Qty::from_decimal("0.1").value());
    CHECK(l.venue->shadow_count() == 0);
    CHECK(h.signed_bad.load() == 0);
    CHECK_FALSE(l.venue->fatal());
  }
}

TEST_CASE("coinbase.venue: rejects of a new order and of a cancel, and an IOC remainder") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    // An HTTP 400 with a message.
    {
      const std::lock_guard lock(h.mu);
      h.new_status = 400;
    }
    l.push(new_order(1).hdr);
    l.wait([&] { return l.oc.count(EventType::OrderReject) == 1; });
    const auto* rj = l.oc.last<OrderRejectMsg>(EventType::OrderReject);
    CHECK(rj->cl_ord_id == make_cl_ord_id(1, 1));
    CHECK(rj->reason == RejectReason::InsufficientBalance);
    CHECK(rj->venue_code == 400);
    CHECK(rj->hdr.instrument == kBtc);
    // A post-only order that would take: 200 with status rejected.
    {
      const std::lock_guard lock(h.mu);
      h.new_status = 200;
      h.new_reply =
          R"({"id":"d50ec984-77a8-460a-b958-000000000077","price":"60000.1","size":"0.3","product_id":"BTC-USD","side":"sell","type":"limit","post_only":true,"status":"rejected","reject_reason":"post only","settled":true})";
    }
    l.push(new_order(2).hdr);
    l.wait([&] { return l.oc.count(EventType::OrderReject) == 2; });
    CHECK(l.oc.last<OrderRejectMsg>(EventType::OrderReject)->reason ==
          RejectReason::PostOnlyWouldCross);
    CHECK(l.venue->shadow_count() == 0);
    // A cancel the venue does not know.
    {
      const std::lock_guard lock(h.mu);
      h.new_reply.clear();
      h.cancel_status = 404;
    }
    l.push(cancel_order(3).hdr);
    l.wait([&] { return l.oc.count(EventType::OrderCancelReject) == 1; });
    CHECK(l.oc.last<OrderCancelRejectMsg>(EventType::OrderCancelReject)->reason ==
          RejectReason::VenueUnknownOrder);
    // An IOC whose remainder the venue cancels for its time in force: expired, not cancelled.
    {
      const std::lock_guard lock(h.mu);
      h.cancel_status = 200;
    }
    OutNewOrderMsg ioc = new_order(4);
    ioc.type = OrderType::Limit;
    ioc.tif = TimeInForce::Ioc;
    l.push(ioc.hdr);
    l.wait([&] { return l.oc.count(EventType::OrderFill) == 1; });
    h.on_server([](Harness& hs) {
      hs.push(hs.done_push(hs.orders[oid_of(4)], "canceled", "101:Time In Force"));
    });
    l.wait([&] { return l.oc.count(EventType::OrderExpired) == 1; });
    CHECK(l.oc.last<OrderExpiredMsg>(EventType::OrderExpired)->cum_qty ==
          Qty::from_decimal("0.1").value());
    CHECK(h.srv.frames("new").back().find(R"("time_in_force":"IOC")") != std::string::npos);
    CHECK_FALSE(l.venue->fatal());
  }
}

TEST_CASE("coinbase.venue: fills missed while the user channel was down are booked once") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.oc.all.clear();
    BookedPosition book;
    const OutNewOrderMsg n = new_order();
    book.submit(n.cl_ord_id, kBtc, kVenue, n.side, n.price, n.qty);
    l.push(n.hdr);
    l.wait([&] { return l.oc.count(EventType::OrderFill) == 1; });  // trade 5001, streamed
    // The user channel drops; while it is down the order fills again (trade 5002), and a fill of
    // an earlier session's order and of another client's order are in the history too.
    const long long now_ms = l.venue->venue_time_ms();
    {
      const std::lock_guard lock(h.mu);
      h.fills = "[" + fill_row(5003, kForeignOrder, now_ms) + "," +
                fill_row(5002, order_id(1), now_ms) + "," + fill_row(5001, order_id(1), now_ms) +
                "," + fill_row(4001, kOldOrder, now_ms) + "]";
    }
    const std::size_t ends = l.ends();
    h.srv.close_sessions("/user");
    // The connector cancels over REST when the channel goes, then reconciles when it is back.
    l.wait([&] { return !h.srv.frames("cancel_all").empty(); });
    CHECK(h.srv.frames("cancel_all")[0] == "product_id=BTC-USD");
    REQUIRE(pump_until(
        l.reactor, [&] { return l.ends() > ends && l.venue->order_channel_live(); }, 10'000));
    book.drain(l.oc);
    const auto fills = l.all<OrderFillMsg>(EventType::OrderFill);
    // Streamed 5001, then replayed 4001, 5001, 5002 and 5003.
    REQUIRE(fills.size() == 5);
    std::map<std::string, const OrderFillMsg*> replayed;
    for (const OrderFillMsg* f : fills) {
      if ((f->flags & OrderFillMsg::kReplayed) != 0) replayed[std::string(f->exec_id.view())] = f;
    }
    REQUIRE(replayed.size() == 4);
    CHECK(replayed["5002"]->cl_ord_id == n.cl_ord_id);
    CHECK(replayed["5002"]->fee == Notional::from_decimal("24.00004").value());
    CHECK(replayed["5002"]->liquidity == Liquidity::Maker);
    // The earlier session's order is named by looking it up; the other client's is not ours.
    CHECK(replayed["4001"]->cl_ord_id == make_cl_ord_id(0, 7));
    CHECK_FALSE(replayed["5003"]->cl_ord_id.valid());
    const auto rest = h.rest();
    CHECK(std::count(rest.begin(), rest.end(), "lookup") == 2);
    // The account's position: 0.1 streamed and 0.1 missed on this order, 0.1 each on the other
    // two, all sold; 5001 not booked twice.
    CHECK(book.position == -Qty::from_decimal("0.4").value());
    CHECK((l.reconcile(ReconcileMsg::Kind::Begin).back()->flags & ReconcileMsg::kExecutionsExact) !=
          0);
    // The replayed fills come before the snapshot.
    std::size_t last_fill = 0;
    std::size_t last_begin = 0;
    for (std::size_t k = 0; k < l.oc.all.size(); ++k) {
      if (RecordingSink::type_of(l.oc.all[k]) == EventType::OrderFill) last_fill = k;
      if (RecordingSink::type_of(l.oc.all[k]) == EventType::Reconcile &&
          RecordingSink::as<ReconcileMsg>(l.oc.all[k]).kind == ReconcileMsg::Kind::Begin)
        last_begin = k;
    }
    CHECK(last_fill < last_begin);
    // Another reconciliation forwards nothing new.
    const std::size_t before = l.all<OrderFillMsg>(EventType::OrderFill).size();
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() > ends + 1; }));
    CHECK(l.all<OrderFillMsg>(EventType::OrderFill).size() == before);
    CHECK(h.signed_bad.load() == 0);
  }
}

TEST_CASE("coinbase.venue: ControlCommand::Reconcile from the engine triggers the snapshot") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    const std::size_t pending = h.rest().size();
    ControlMsg m{};
    init_header(m, EventType::Control, InstrumentId::invalid(), kVenue);
    m.command = ControlCommand::Reconcile;
    l.push(m.hdr);
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() == 2; }));
    const auto rest = h.rest();
    REQUIRE(rest.size() >= pending + 2);
    CHECK(rest[pending].rfind("fills?", 0) == 0);
    CHECK(rest[pending + 1].rfind("orders?", 0) == 0);
    CHECK((l.reconcile(ReconcileMsg::Kind::Begin)[1]->flags & ReconcileMsg::kSentWatermark) != 0);
  }
}

TEST_CASE("coinbase.venue: the kill-path cancel-all waits out a rate limit") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    // 429, then one order cancelled, then nothing left: three requests.
    {
      const std::lock_guard lock(h.mu);
      h.cancel_all_status = {429, 200};
    }
    CHECK(l.venue->cancel_all());
    auto ca = h.srv.frames("cancel_all");
    REQUIRE(ca.size() == 3);
    for (const std::string& q : ca) CHECK(q == "product_id=BTC-USD");
    // A limit that never lifts: given up after the retries, reported as failed.
    {
      const std::lock_guard lock(h.mu);
      h.cancel_all_status = {429, 429, 429, 429, 429, 429};
    }
    CHECK_FALSE(l.venue->cancel_all());
    CHECK(h.srv.frames("cancel_all").size() == 3 + 4);
    CHECK(h.signed_bad.load() == 0);
  }
}

TEST_CASE("coinbase.venue: a lost trade on the feed resubscribes the book for a new snapshot") {
  Harness h;
  {
    Live l(h.section(false), {}, true);
    REQUIRE(pump_until(l.reactor, [&] {
      return l.venue->md_feed() != nullptr && l.venue->md_feed()->synced_count() == 1;
    }));
    const auto subscribe = h.srv.frames("md");
    REQUIRE(subscribe.size() == 1);
    CHECK(
        subscribe[0] ==
        R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["level2_batch","matches","heartbeat"]})");
    auto m = [](int id) {
      return R"({"type":"match","trade_id":)" + std::to_string(id) +
             R"(,"maker_order_id":"a","taker_order_id":"b","side":"buy","size":"1","price":"60000.10","product_id":"BTC-USD","sequence":1,"time":"2026-09-30T01:41:51Z"})";
    };
    h.srv.send_to("/feed", m(100));
    h.srv.send_to("/feed", m(102));
    REQUIRE(pump_until(l.reactor, [&] {
      for (const std::string& f : h.srv.frames("md")) {
        if (f.find(R"("type":"unsubscribe")") != std::string::npos) return true;
      }
      return false;
    }));
    REQUIRE(pump_until(l.reactor, [&] { return l.venue->md_feed()->synced_count() == 1; }));
    CHECK(l.venue->md_feed()->resync_count() == 1);
    const auto frames = h.srv.frames("md");
    CHECK(frames[1] ==
          R"({"type":"unsubscribe","product_ids":["BTC-USD"],"channels":["level2_batch"]})");
    CHECK(frames[2] ==
          R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["level2_batch"]})");
    l.mc.take(l.md);
    CHECK(l.mc.count(EventType::BookSnapshot) == 2);
    CHECK(l.mc.count(EventType::Trade) == 2);
  }
}

TEST_CASE("coinbase.venue: a dry run opens market data only and refuses orders") {
  Harness h;
  {
    Live l(h.section(false), {}, true);
    REQUIRE(pump_until(l.reactor, [&] { return l.venue->md_feed()->synced_count() == 1; }));
    CHECK_FALSE(l.venue->caps().user_stream);
    l.push(new_order().hdr);
    l.wait([&] { return l.oc.count(EventType::OrderReject) == 1; });
    CHECK(l.oc.last<OrderRejectMsg>(EventType::OrderReject)->reason == RejectReason::VenueKilled);
    CHECK(h.srv.open_count("/user") == 0);
    CHECK(h.srv.frames("new").empty());
    CHECK(l.venue->cancel_all());
    CHECK(h.srv.frames("cancel_all").empty());
  }
}

namespace {

constexpr const char* kUsdAccount = "7fd0abc0-e5ad-4cbb-8d54-f2b3f43364da";
constexpr const char* kBtcAccount = "d50ec984-77a8-460a-b958-66f114b0de9b";
constexpr const char* kEthAccount = "d50ec984-77a8-460a-b958-66f114b0de9a";

// apiAccount rows as GET /accounts documents them (16 decimals).
std::string accounts_reply(const char* btc_available, const char* btc_hold) {
  auto row = [](const char* id, const char* cur, const char* avail, const char* hold) {
    return R"({"id":")" + std::string(id) + R"(","currency":")" + cur +
           R"(","balance":"0","hold":")" + hold + R"(","available":")" + avail +
           R"(","profile_id":"8058d771-2d88-4f0f-ab6e-299c153d4308","trading_enabled":true})";
  };
  return "[" + row(kUsdAccount, "USD", "1000.5000000000000000", "20.0000000000000000") + "," +
         row(kBtcAccount, "BTC", btc_available, btc_hold) + "," +
         row(kEthAccount, "ETH", "3.0000000000000000", "0.0000000000000000") + "]";
}

std::string balance_push(const char* account, const char* currency, const char* available) {
  return R"({"type":"balance","account_id":")" + std::string(account) + R"(","currency":")" +
         currency + R"(","holds":"0.1","available":")" + available +
         R"(","updated":"2026-09-30T01:41:51.250Z","timestamp":"2026-09-30T01:41:51.300Z"})";
}

// The BalanceMsg snapshots (kSnapshot) the connector sent, each as its rows.
std::vector<std::vector<const BalanceMsg*>> balance_snapshots(const Collected& c) {
  std::vector<std::vector<const BalanceMsg*>> out(1);
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) != EventType::Balance) continue;
    const auto& b = RecordingSink::as<BalanceMsg>(m);
    if ((b.flags & BalanceMsg::kSnapshot) == 0) continue;
    out.back().push_back(&b);
    if ((b.flags & BalanceMsg::kSnapshotEnd) != 0) out.emplace_back();
  }
  out.pop_back();  // the one not ended yet
  return out;
}

std::vector<const BalanceMsg*> balance_updates(const Collected& c) {
  std::vector<const BalanceMsg*> out;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) != EventType::Balance) continue;
    const auto& b = RecordingSink::as<BalanceMsg>(m);
    if ((b.flags & BalanceMsg::kSnapshot) == 0) out.push_back(&b);
  }
  return out;
}

}  // namespace

TEST_CASE("coinbase.venue: the start-up balance snapshot, then the balance channel") {
  Harness h;
  h.accounts = accounts_reply("0.2500000000000001", "0.3000000000000000");
  {
    Live l(h.section());
    // The fake's /time is a fixed instant: the clock offset measured again at connect moves the
    // venue clock back by the time spent since load_reference_data (a few ms).
    const std::int64_t before_ms = l.venue->venue_time_ms() - 1000;
    l.wait([&] { return !balance_snapshots(l.oc).empty() && l.ends() >= 1; });
    const std::int64_t after_ms = l.venue->venue_time_ms();
    const auto snaps = balance_snapshots(l.oc);
    REQUIRE(snaps.size() == 1);
    const auto& rows = snaps[0];
    REQUIRE(rows.size() == 2);  // ETH: no instrument names it
    CHECK(rows[0]->asset.view() == "USD");
    CHECK(rows[0]->free == Notional::from_decimal("1000.5").value());
    CHECK(rows[0]->locked == Notional::from_int(20));
    CHECK(rows[1]->asset.view() == "BTC");
    CHECK(rows[1]->free == Notional::from_decimal("0.25").value());
    CHECK(rows[1]->locked == Notional::from_decimal("0.3").value());
    CHECK(rows[1]->total == Notional::from_decimal("0.55").value());
    for (const BalanceMsg* b : rows) {
      CHECK((b->flags & BalanceMsg::kSnapshot) != 0);
      CHECK(b->hdr.exch_ts.ns >= before_ms * 1'000'000);
      CHECK(b->hdr.exch_ts.ns <= after_ms * 1'000'000);
    }
    CHECK((rows[0]->flags & BalanceMsg::kSnapshotEnd) == 0);
    CHECK((rows[1]->flags & BalanceMsg::kSnapshotEnd) != 0);
    CHECK(h.srv.frames("accounts") == std::vector<std::string>{"/accounts"});

    // The balance channel for the tracked assets' accounts, signed, on the user connection.
    l.wait([&] { return !h.srv.frames("balance_sub").empty(); });
    const std::string sub = h.srv.frames("balance_sub")[0];
    CHECK(sub.rfind(
              std::string(R"({"type":"subscribe","channels":[{"name":"balance","account_ids":[")") +
                  kUsdAccount + R"(",")" + kBtcAccount + R"("]}],"signature":")",
              0) == 0);
    CHECK(ws_signed(sub));
    // Its updates go out as they come, for the tracked assets only, at the venue's `updated`.
    h.on_server([](Harness& hs) {
      hs.push(balance_push(kEthAccount, "ETH", "2"));
      hs.push(balance_push(kBtcAccount, "BTC", "0.4500000000000000"));
    });
    l.wait([&] { return balance_updates(l.oc).size() == 1; });
    l.pump();
    const auto updates = balance_updates(l.oc);
    REQUIRE(updates.size() == 1);
    CHECK(updates[0]->asset.view() == "BTC");
    CHECK(updates[0]->free == Notional::from_decimal("0.45").value());
    CHECK(updates[0]->locked == Notional::from_decimal("0.1").value());
    CHECK(updates[0]->flags == 0);
    CHECK(updates[0]->hdr.exch_ts.ns == parse_time_ns("2026-09-30T01:41:51.250Z"));

    // A new user connection subscribes the channel again once it is live.
    h.on_server([](Harness& hs) { hs.user = nullptr; });
    h.srv.close_sessions("/user");
    l.wait([&] { return h.srv.frames("balance_sub").size() == 2; }, 10'000);
    CHECK(h.srv.frames("balance_sub")[1].find(kBtcAccount) != std::string::npos);
    CHECK(h.signed_bad.load() == 0);
    CHECK_FALSE(l.venue->fatal());
  }
}

TEST_CASE("coinbase.venue: a fill asks for the balances again") {
  Harness h;
  h.accounts = accounts_reply("0.5000000000000000", "0.0000000000000000");
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.wait([&] { return balance_snapshots(l.oc).size() == 1; });
    {
      const std::lock_guard lock(h.mu);
      h.accounts = accounts_reply("0.1000000000000000", "0.3000000000000000");
    }
    // The order is matched for 0.1 at once.
    l.push(new_order().hdr);
    l.wait([&] {
      return l.oc.count(EventType::OrderFill) == 1 && balance_snapshots(l.oc).size() == 2;
    });
    const auto snaps = balance_snapshots(l.oc);
    REQUIRE(snaps[1].size() == 2);
    CHECK(snaps[1][1]->asset.view() == "BTC");
    CHECK(snaps[1][1]->free == Notional::from_decimal("0.1").value());
    CHECK(snaps[1][1]->locked == Notional::from_decimal("0.3").value());
    CHECK(h.srv.frames("accounts").size() == 2);
  }
}

TEST_CASE("coinbase.venue: a failed balance fetch does not hold up the order snapshot") {
  Harness h;
  h.accounts_status = 500;
  h.accounts = accounts_reply("0.5000000000000000", "0.0000000000000000");
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.wait([&] { return !h.srv.frames("accounts").empty(); });
    const std::size_t ends = l.ends();
    l.venue->request_open_orders();
    l.wait([&] { return l.ends() > ends; });
    CHECK(l.oc.count(EventType::Balance) == 0);
    CHECK(h.srv.frames("balance_sub").empty());  // no account ids yet
    // Asked again after ReconcileDriver::kRetryNs, now answered.
    {
      const std::lock_guard lock(h.mu);
      h.accounts_status = 200;
    }
    l.wait([&] { return balance_snapshots(l.oc).size() == 1; }, 10'000);
    CHECK(balance_snapshots(l.oc)[0].size() == 2);
    CHECK_FALSE(l.venue->fatal());
  }
}
