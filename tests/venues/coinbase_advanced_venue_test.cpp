// CoinbaseAdvancedVenue against a scripted fake Coinbase Advanced Trade: config mapping, reference
// data, ES256 JWTs on every private request and on the user subscription (verified by the fake
// with the key's public half), the start-up sweep, order entry over REST with states from the user
// channel and executions read per order, a cancel that waits for the venue's order id, rejects,
// executions the venue lists late, the fill replay after the user channel dropped (no fill booked
// twice), the kill-path cancel-all through a rate limit, a sequence gap on the feed and a dry run.
// Formats as in coinbase_advanced_test.cpp; the public feed, public REST and the read-only private
// calls have met the real venue (tests/venues/live_coinbase_test.cpp).
#include "fake_venue_util.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/coinbase/advanced_venue.hpp"

#include <algorithm>
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

constexpr const char* kKeyName = "organizations/org-1/apiKeys/key-1";
constexpr const char* kPrivatePem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIMmvqdhFunUWa1whV2ex1pNOUMPbNuibEnuKYisSD2choAoGCCqGSM49\n"
    "AwEHoUQDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7Yfps5mliLmDyn7Z5A/4QCLi8\n"
    "maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n"
    "-----END EC PRIVATE KEY-----\n";
constexpr const char* kPublicPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7\n"
    "Yfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n"
    "-----END PUBLIC KEY-----\n";
constexpr VenueId kVenue{1};
const InstrumentId kBtc{0};
constexpr const char* kPrefix = "/api/v3/brokerage";
// An order of an earlier session, and one placed on the website.
constexpr const char* kOldOrder = "aaaaaaaa-2222-4333-8444-000000000007";
constexpr const char* kWebOrder = "bbbbbbbb-2222-4333-8444-000000000009";

std::string order_id(int n) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "11111111-2222-4333-8444-%012d", n);
  return buf;
}

std::string unb64url(std::string_view s) {
  std::string out(s.size() + 3, '\0');
  const std::size_t n = net::base64url_decode(
      s, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(out.data()), out.size()));
  if (n == SIZE_MAX) return {};
  out.resize(n);
  return out;
}

// A CDP JWT with a valid ES256 signature, this key name, and `uri` (empty: none may be present).
bool jwt_ok(std::string_view jwt, const std::string& uri) {
  const std::size_t d1 = jwt.find('.');
  const std::size_t d2 = jwt.rfind('.');
  if (d1 == std::string_view::npos || d2 <= d1) return false;
  const std::string header = unb64url(jwt.substr(0, d1));
  const std::string payload = unb64url(jwt.substr(d1 + 1, d2 - d1 - 1));
  const std::string sig = unb64url(jwt.substr(d2 + 1));
  static const net::EcdsaP256Key pub = net::EcdsaP256Key::from_public_pem(kPublicPem);
  if (!pub.verify(jwt.substr(0, d2),
                  std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(sig.data()),
                                                sig.size())))
    return false;
  if (json_str(header, "alg") != "ES256" || json_str(header, "kid") != kKeyName ||
      json_str(header, "nonce").size() != 32)
    return false;
  if (json_str(payload, "sub") != kKeyName || json_str(payload, "iss") != "cdp") return false;
  const long long nbf = std::stoll(json_int(payload, "nbf"));
  const long long exp = std::stoll(json_int(payload, "exp"));
  if (exp - nbf != 120) return false;
  return json_str(payload, "uri") == uri;
}

bool rest_signed(const net::HttpRequest& r) {
  const std::string_view auth = r.header("Authorization");
  if (auth.substr(0, 7) != "Bearer ") return false;
  const std::string uri = std::string(r.method) + " 127.0.0.1" + std::string(r.path);
  return jwt_ok(auth.substr(7), uri);
}

std::string open_row(const std::string& id,
                     const std::string& client,
                     const char* product,
                     const char* filled = "0") {
  return R"({"order_id":")" + id + R"(","product_id":")" + product +
         R"(","side":"SELL","client_order_id":")" + client +
         R"(","status":"OPEN","order_configuration":{"limit_limit_gtc":{"base_size":"0.3","limit_price":"60000.1","post_only":true}},"filled_size":")" +
         filled + R"("})";
}

std::string fill_row(const std::string& trade,
                     const std::string& order,
                     long long ts_ms,
                     const char* size = "0.1") {
  return R"({"entry_id":"e)" + trade + R"(","trade_id":")" + trade + R"(","order_id":")" + order +
         R"(","trade_time":")" + std::string(format_time_ms(ts_ms).view()) +
         R"(","trade_type":"FILL","price":"60000.1","size":")" + size +
         R"(","commission":"24.0000400000000000","product_id":"BTC-USD","sequence_timestamp":")" +
         std::string(format_time_ms(ts_ms).view()) +
         R"(","liquidity_indicator":"MAKER","size_in_quote":false,"user_id":"u","side":"SELL","retail_portfolio_id":"p"})";
}

std::string snapshot(std::uint64_t seq) {
  return R"({"channel":"l2_data","client_id":"","timestamp":"2026-09-30T01:41:25Z","sequence_num":)" +
         std::to_string(seq) +
         R"(,"events":[{"type":"snapshot","product_id":"BTC-USD","updates":[{"side":"bid","event_time":"2026-09-30T01:41:24Z","price_level":"60000.1","new_quantity":"3"},{"side":"offer","event_time":"2026-09-30T01:41:24Z","price_level":"60000.2","new_quantity":"5"}]}]})";
}

struct FakeOrder {
  std::string id;
  std::string client;
  std::string size;
  std::string cum = "0";
  std::vector<std::string> fills;  // rows the fills endpoint lists
  bool ended = false;
};

struct Harness {
  FakeVenueServer srv;
  std::mutex mu;  // guards the settings below, which tests change while the server runs
  std::string product = fastmm::test::fixture("coinbase/advanced_product_btc_usd.json");
  std::string open_page1 = R"({"orders":[],"has_next":false,"cursor":""})";
  std::string open_page2 = R"({"orders":[],"has_next":false,"cursor":""})";
  std::string account_fills = R"({"fills":[],"cursor":""})";
  std::string create_failure;      // non-empty: POST orders fails with this reason
  bool fill_on_new = true;         // a new order is matched for 0.1 at once
  int fills_hidden = 0;            // the next per-order fills queries list nothing
  std::vector<int> cancel_status;  // the next batch_cancel answers (429), then 200
  bool unknown_cancel = false;
  std::atomic<int> signed_ok{0};
  std::atomic<int> signed_bad{0};
  std::atomic<int> ws_bad{0};
  // Server thread only.
  net::WsSession* user = nullptr;
  net::WsSession* feed = nullptr;
  std::uint64_t user_seq = 0;
  std::uint64_t md_seq = 0;
  std::map<std::string, FakeOrder> orders;  // by order id
  int next_order = 1;
  int next_trade = 5001;

  void count(const net::HttpRequest& r) { ++(rest_signed(r) ? signed_ok : signed_bad); }
  std::string reply(const std::string& s) {
    const std::lock_guard lock(mu);
    return s;
  }
  void push_order(const FakeOrder& o,
                  const char* status,
                  const char* tif = "GOOD_UNTIL_CANCELLED") {
    if (user == nullptr) return;
    user->send_text(
        R"({"channel":"user","client_id":"","timestamp":"2026-09-30T02:37:14Z","sequence_num":)" +
        std::to_string(++user_seq) +
        R"(,"events":[{"type":"update","orders":[{"avg_price":"60000.1","cancel_reason":"","client_order_id":")" +
        o.client + R"(","cumulative_quantity":")" + o.cum +
        R"(","leaves_quantity":"0","limit_price":"60000.1","order_id":")" + o.id +
        R"(","order_side":"SELL","order_type":"LIMIT","post_only":"true","product_id":"BTC-USD","reject_reason":"","status":")" +
        status + R"(","time_in_force":")" + tif + R"(","total_fees":"0"}],"positions":{}}]})");
  }
  // A fill of `o` for 0.1 now: listed by the fills endpoint, shown on the user channel.
  void fill(FakeOrder& o) {
    o.cum = o.cum == "0" ? "0.1" : "0.2";
    const int trade = next_trade++;
    o.fills.push_back(fill_row(std::to_string(trade), o.id, wall_now().ns / 1'000'000));
    push_order(o, "OPEN");
  }
  FakeOrder* by_client(const std::string& client) {
    for (auto& [id, o] : orders) {
      if (o.client == client) return &o;
    }
    return nullptr;
  }

  Harness() {
    const std::string p = kPrefix;
    srv.route("GET", p + "/time", [](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200,
                                           fastmm::test::fixture("coinbase/advanced_time.json"));
    });
    srv.route("GET", p + "/market/products/BTC-USD", [this](const net::HttpRequest& r) {
      srv.record("rest", "product");
      srv.record("agent", std::string(r.header("User-Agent")));
      return net::HttpServerResponse::json(200, reply(product));
    });
    srv.route("GET", p + "/orders/historical/batch", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "open?" + std::string(r.query));
      const std::lock_guard lock(mu);
      return net::HttpServerResponse::json(
          200, r.query.find("cursor=page2") != std::string_view::npos ? open_page2 : open_page1);
    });
    srv.route("GET", p + "/orders/historical/fills", [this](const net::HttpRequest& r) {
      count(r);
      const std::string q(r.query);
      srv.record("rest", "fills?" + q);
      const std::size_t at = q.find("order_ids=");
      if (at == std::string::npos) return net::HttpServerResponse::json(200, reply(account_fills));
      {
        const std::lock_guard lock(mu);
        if (fills_hidden > 0) {
          --fills_hidden;
          return net::HttpServerResponse::json(200, R"({"fills":[],"cursor":""})");
        }
      }
      const std::string id = q.substr(at + 10, 36);
      std::string rows;
      if (auto it = orders.find(id); it != orders.end()) {
        // Newest first, as the venue sorts by default.
        for (auto f = it->second.fills.rbegin(); f != it->second.fills.rend(); ++f)
          rows += (rows.empty() ? "" : ",") + *f;
      }
      return net::HttpServerResponse::json(200, R"({"fills":[)" + rows + R"(],"cursor":""})");
    });
    for (const char* known : {kOldOrder, kWebOrder}) {
      const std::string id = known;
      srv.route("GET", p + "/orders/historical/" + id, [this, id](const net::HttpRequest& r) {
        count(r);
        srv.record("rest", "lookup");
        const std::string client = id == kOldOrder ? "fm000000000007" : "web-1";
        return net::HttpServerResponse::json(
            200, R"({"order":)" + open_row(id, client, "BTC-USD") + "}");
      });
    }
    srv.route("POST", p + "/orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("new", std::string(r.body));
      {
        const std::lock_guard lock(mu);
        if (!create_failure.empty())
          return net::HttpServerResponse::json(
              200,
              R"({"success":false,"error_response":{"error":"UNKNOWN_FAILURE_REASON","message":"refused","new_order_failure_reason":")" +
                  create_failure + R"("}})");
      }
      FakeOrder o;
      o.id = order_id(next_order++);
      o.client = json_str(r.body, "client_order_id");
      o.size = json_str(r.body, "base_size");
      FakeOrder& stored = orders[o.id] = o;
      push_order(stored, "OPEN");
      const bool fill_now = [this] {
        const std::lock_guard lock(mu);
        return fill_on_new;
      }();
      if (fill_now) fill(stored);
      return net::HttpServerResponse::json(
          200,
          R"({"success":true,"success_response":{"order_id":")" + stored.id +
              R"(","product_id":"BTC-USD","side":"SELL","client_order_id":")" + stored.client +
              R"("},"order_configuration":{}})");
    });
    srv.route("POST", p + "/orders/batch_cancel", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("cancel", std::string(r.body));
      {
        const std::lock_guard lock(mu);
        if (!cancel_status.empty()) {
          const int st = cancel_status.front();
          cancel_status.erase(cancel_status.begin());
          if (st == 429)
            return net::HttpServerResponse::json(
                429, R"({"error":"RATE_LIMIT_EXCEEDED","code":8,"message":"Too many requests"})");
        }
      }
      std::string results;
      std::string body(r.body);
      std::size_t pos = body.find('[');
      while ((pos = body.find('"', pos)) != std::string::npos) {
        const std::size_t end = body.find('"', pos + 1);
        const std::string id = body.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        auto it = orders.find(id);
        const bool known = it != orders.end() && !it->second.ended && !unknown_cancel;
        if (known) {
          it->second.ended = true;
          push_order(it->second, "CANCELLED");
        }
        results += (results.empty() ? "" : ",") + std::string(R"({"success":)") +
                   (known ? "true" : "false") + R"(,"failure_reason":")" +
                   (known ? "UNKNOWN_CANCEL_FAILURE_REASON" : "UNKNOWN_CANCEL_ORDER") +
                   R"(","order_id":")" + id + "\"}";
      }
      return net::HttpServerResponse::json(200, R"({"results":[)" + results + "]}");
    });
    srv.on_ws_text("/feed", [this](net::WsSession& s, std::string_view t) {
      srv.record("md", std::string(t));
      feed = &s;
      if (json_str(t, "type") != "subscribe" || json_str(t, "channel") != "level2") return;
      s.send_text(snapshot(md_seq++));
    });
    srv.on_ws_text("/user", [this](net::WsSession& s, std::string_view t) {
      srv.record("user", std::string(t));
      if (json_str(t, "type") != "subscribe" || json_str(t, "channel") != "user") return;
      if (!jwt_ok(json_str(t, "jwt"), {})) {
        ++ws_bad;
        s.send_text(R"({"type":"error","message":"authentication failure"})");
        return;
      }
      user = &s;
      user_seq = 0;
      s.send_text(
          R"({"channel":"user","client_id":"","timestamp":"2026-09-30T02:37:14Z","sequence_num":0,"events":[{"type":"snapshot","orders":[],"positions":{}}]})");
      s.send_text(
          R"({"channel":"subscriptions","client_id":"","timestamp":"2026-09-30T02:37:14Z","sequence_num":)" +
          std::to_string(++user_seq) +
          R"(,"events":[{"subscriptions":{"user":["00000000-0000-0000-0000-000000000000"]}}]})");
    });
    srv.start();
  }
  ~Harness() { srv.stop(); }

  VenueSection section(bool with_keys = true) const {
    VenueSection s;
    s.name = "fake-coinbase";
    s.kind = "coinbase_advanced";
    s.ws_url = srv.ws_base() + "/feed";
    s.rest_url = srv.http_base();
    s.testnet = false;
    if (with_keys) {
      s.api_key = kKeyName;
      s.api_secret = kPrivatePem;
    }
    s.extra["ws_private_url"] = srv.ws_base() + "/user";
    return s;
  }
  std::vector<std::string> rest() { return srv.frames("rest"); }
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
  std::unique_ptr<CoinbaseAdvancedVenue> venue;
  Collected oc;
  Collected mc;

  explicit Live(const VenueSection& section, bool dry_run = false) {
    Instrument i = make_instrument("BTC-USD", 1, "XBT", "USDC");
    i.tick = Price::from_int(1);
    REQUIRE(instruments.add(i));
    venue = std::make_unique<CoinbaseAdvancedVenue>(
        kVenue, make_coinbase_advanced_config(section, dry_run));
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
  void wait_for_sweep() {
    REQUIRE(
        pump_until(reactor, [&] { return ends() >= 1 && venue->order_channel_live(); }, 10'000));
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
  void pump_for(int iterations) {
    for (int i = 0; i < iterations; ++i) reactor.run_once(5);
    oc.take(orders);
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

TEST_CASE("coinbase_advanced.venue: config mapping checks the key, the hosts and options") {
  VenueSection s;
  s.name = "cb";
  s.kind = "coinbase_advanced";
  s.ws_url = "wss://advanced-trade-ws.coinbase.com";
  s.rest_url = "https://api.coinbase.com";
  s.testnet = false;
  s.api_key = kKeyName;
  s.api_secret = kPrivatePem;
  AdvancedVenueConfig c = make_coinbase_advanced_config(s, false);
  CHECK(c.ws_private_url == "wss://advanced-trade-ws-user.coinbase.com");
  CHECK(CdpJwtSigner(c.credentials).usable());
  s.extra["cancel_batch"] = "500";
  CHECK(make_coinbase_advanced_config(s, false).cancel_batch == 100);
  s.extra.erase("cancel_batch");
  // There is no test environment: testnet = true with the production host is refused.
  s.testnet = true;
  try {
    static_cast<void>(make_coinbase_advanced_config(s, false));
    FAIL("testnet with the production host accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).rfind("venues.cb.testnet:", 0) == 0);
  }
  s.testnet = false;
  s.supports_replace = true;
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_advanced_config(s, false)),
                  std::invalid_argument);
  s.supports_replace = false;
  // An Ed25519 key, or a key name that is not a CDP one.
  s.api_secret = fastmm::test::fixture("binance/ed25519-test-private.pem");
  try {
    static_cast<void>(make_coinbase_advanced_config(s, false));
    FAIL("an Ed25519 key accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).rfind("venues.cb.api_secret:", 0) == 0);
  }
  s.api_secret = kPrivatePem;
  s.api_key = "some-key";
  CHECK_THROWS_AS(static_cast<void>(make_coinbase_advanced_config(s, false)),
                  std::invalid_argument);
  // A dry run takes no key at all.
  VenueSection dry;
  dry.name = "cb";
  dry.ws_url = s.ws_url;
  dry.rest_url = s.rest_url;
  dry.testnet = false;
  CHECK_FALSE(CdpJwtSigner(make_coinbase_advanced_config(dry, true).credentials).usable());
}

TEST_CASE("coinbase_advanced.venue: reference data, then the start-up sweep with signed requests") {
  Harness h;
  // Page 1: an order of a product not traded here; page 2: an earlier session's order.
  h.open_page1 = R"({"orders":[)" + open_row(order_id(900), "fm000000000003", "ETH-USD") +
                 R"(],"has_next":true,"cursor":"page2"})";
  h.open_page2 = R"({"orders":[)" + open_row(kOldOrder, "fm000000000007", "BTC-USD", "0.1") +
                 R"(],"has_next":false,"cursor":""})";
  {
    Live l(h.section());
    const Instrument& in = l.instruments.get(kBtc);
    CHECK(in.tick == Price::from_decimal("0.01").value());
    CHECK(in.lot == Qty::from_decimal("0.00000001").value());
    CHECK(in.min_notional == Notional::from_int(1));
    CHECK(in.base.view() == "BTC");
    CHECK(in.quote.view() == "USD");
    l.wait_for_sweep();
    const auto begins = l.reconcile(ReconcileMsg::Kind::Begin);
    REQUIRE(begins.size() == 1);
    CHECK_FALSE(begins[0]->sent_watermark.valid());  // a sweep
    CHECK((begins[0]->flags & ReconcileMsg::kExecutionsExact) != 0);
    const auto oo = l.reconcile(ReconcileMsg::Kind::OpenOrder);
    REQUIRE(oo.size() == 1);
    CHECK(oo[0]->cl_ord_id == make_cl_ord_id(0, 7));
    CHECK(oo[0]->venue_order_id.view() == kOldOrder);
    CHECK(oo[0]->state == OrderState::PartiallyFilled);
    CHECK(oo[0]->orig_qty == Qty::from_decimal("0.3").value());
    CHECK(oo[0]->price == Price::from_decimal("60000.1").value());
    const auto rest = h.rest();
    REQUIRE(rest.size() >= 4);
    CHECK(rest[0] == "product");
    CHECK(rest[1].rfind("fills?product_ids=BTC-USD&limit=100&start_sequence_timestamp=", 0) == 0);
    CHECK(rest[2] == "open?order_status=OPEN&product_ids=BTC-USD&limit=250");
    CHECK(rest[3] == "open?order_status=OPEN&product_ids=BTC-USD&limit=250&cursor=page2");
    CHECK(h.signed_ok.load() >= 3);
    CHECK(h.signed_bad.load() == 0);
    CHECK(h.ws_bad.load() == 0);
    CHECK(h.srv.frames("agent")[0] == "fastmm");
    const auto sub = h.srv.frames("user");
    REQUIRE(sub.size() == 2);
    CHECK(sub[0].find(R"("product_ids":["BTC-USD"],"channel":"user","jwt":")") !=
          std::string::npos);
    CHECK(sub[1] == R"({"type":"subscribe","channel":"heartbeats"})");
  }
}

TEST_CASE("coinbase_advanced.venue: order round trip, executions read per order, cancel") {
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
        R"({"client_order_id":"fm000100000001","product_id":"BTC-USD","side":"SELL","order_configuration":{"limit_limit_gtc":{"base_size":"0.3","limit_price":"60000.1","post_only":true}}})");
    for (const OrderAckMsg* a : l.all<OrderAckMsg>(EventType::OrderAck)) {
      CHECK(a->cl_ord_id == n.cl_ord_id);
      CHECK(a->venue_order_id.view() == order_id(1));
    }
    // The user channel said 0.1 filled; the execution was read with its trade id.
    const auto* f = l.oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(f->cl_ord_id == n.cl_ord_id);
    CHECK(f->exec_id.view() == "5001");
    CHECK((f->flags & OrderFillMsg::kReplayed) == 0);
    CHECK(f->qty == Qty::from_decimal("0.1").value());
    CHECK(f->cum_qty == Qty::from_decimal("0.1").value());
    CHECK(f->leaves_qty == Qty::from_decimal("0.2").value());
    CHECK(f->fee == Notional::from_decimal("24.00004").value());
    CHECK(f->liquidity == Liquidity::Maker);
    CHECK(f->side == Side::Sell);
    const auto rest = h.rest();
    CHECK(std::count(rest.begin(), rest.end(), "fills?order_ids=" + order_id(1) + "&limit=100") ==
          1);
    // A second fill: the order's executions are read again, and only the new one goes out.
    h.on_server([](Harness& hs) { hs.fill(hs.orders[order_id(1)]); });
    l.wait([&] { return l.oc.count(EventType::OrderFill) == 2; });
    l.pump_for(50);
    CHECK(l.oc.count(EventType::OrderFill) == 2);
    const auto* f2 = l.oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(f2->exec_id.view() == "5002");
    CHECK(f2->cum_qty == Qty::from_decimal("0.2").value());
    CHECK(f2->leaves_qty == Qty::from_decimal("0.1").value());

    l.push(cancel_order().hdr);
    l.wait([&] { return l.oc.count(EventType::OrderCancelAck) == 1; });
    CHECK(h.srv.frames("cancel")[0] == R"({"order_ids":[")" + order_id(1) + R"("]})");
    const auto* ca = l.oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck);
    CHECK(ca->cl_ord_id == n.cl_ord_id);
    CHECK(ca->cum_qty == Qty::from_decimal("0.2").value());
    CHECK(l.venue->shadow_count() == 0);
    CHECK(h.signed_bad.load() == 0);
    CHECK_FALSE(l.venue->fatal());
  }
}

TEST_CASE("coinbase_advanced.venue: a cancel sent before the order id is known waits for it") {
  Harness h;
  h.fill_on_new = false;
  {
    Live l(h.section());
    l.wait_for_sweep();
    // New and cancel in one drain: the cancel names the order id the reply brings.
    const OutNewOrderMsg n = new_order(2);
    const OutCancelMsg c = cancel_order(2);
    REQUIRE(l.outbound.try_push(&n, n.hdr.len));
    REQUIRE(l.outbound.try_push(&c, c.hdr.len));
    l.venue->on_wake();
    l.wait([&] { return l.oc.count(EventType::OrderCancelAck) == 1; });
    const auto cancels = h.srv.frames("cancel");
    REQUIRE(cancels.size() == 1);
    CHECK(cancels[0] == R"({"order_ids":[")" + order_id(1) + R"("]})");
    CHECK(l.oc.count(EventType::OrderCancelReject) == 0);
  }
}

TEST_CASE("coinbase_advanced.venue: rejects, a cancel the venue does not know, late executions") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    {
      const std::lock_guard lock(h.mu);
      h.create_failure = "INSUFFICIENT_FUND";
    }
    l.push(new_order(1).hdr);
    l.wait([&] { return l.oc.count(EventType::OrderReject) == 1; });
    const auto* rj = l.oc.last<OrderRejectMsg>(EventType::OrderReject);
    CHECK(rj->cl_ord_id == make_cl_ord_id(1, 1));
    CHECK(rj->reason == RejectReason::InsufficientBalance);
    CHECK(rj->hdr.instrument == kBtc);
    {
      const std::lock_guard lock(h.mu);
      h.create_failure = "INVALID_LIMIT_PRICE_POST_ONLY";
    }
    l.push(new_order(2).hdr);
    l.wait([&] { return l.oc.count(EventType::OrderReject) == 2; });
    CHECK(l.oc.last<OrderRejectMsg>(EventType::OrderReject)->reason ==
          RejectReason::PostOnlyWouldCross);
    CHECK(l.venue->shadow_count() == 0);
    // Executions listed late: the first read finds none, the retry finds the fill.
    {
      const std::lock_guard lock(h.mu);
      h.create_failure.clear();
      h.fills_hidden = 1;
    }
    l.push(new_order(3).hdr);
    l.wait([&] {
      const auto rest = h.rest();
      return std::count(
                 rest.begin(), rest.end(), "fills?order_ids=" + order_id(1) + "&limit=100") >= 1;
    });
    l.venue->on_timer(net::Reactor::now_ns() + 1'000'000'000);
    l.wait([&] { return l.oc.count(EventType::OrderFill) == 1; });
    CHECK(l.oc.last<OrderFillMsg>(EventType::OrderFill)->exec_id.view() == "5001");
    // A cancel of an order the venue no longer knows.
    {
      const std::lock_guard lock(h.mu);
      h.unknown_cancel = true;
    }
    l.push(cancel_order(3).hdr);
    l.wait([&] { return l.oc.count(EventType::OrderCancelReject) == 1; });
    CHECK(l.oc.last<OrderCancelRejectMsg>(EventType::OrderCancelReject)->reason ==
          RejectReason::VenueUnknownOrder);
    CHECK_FALSE(l.venue->fatal());
  }
}

TEST_CASE("coinbase_advanced.venue: fills made while the user channel was down are booked once") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.oc.all.clear();
    BookedPosition book;
    const OutNewOrderMsg n = new_order();
    book.submit(n.cl_ord_id, kBtc, kVenue, n.side, n.price, n.qty);
    l.push(n.hdr);
    l.wait([&] { return l.oc.count(EventType::OrderFill) == 1; });  // trade 5001, from the stream
    // The user channel drops. Meanwhile the order fills again (5002), and the account's history
    // also holds a fill of an earlier session's order and one of an order placed on the website.
    const long long now_ms = l.venue->venue_time_ms();
    h.on_server([&](Harness& hs) {
      FakeOrder& o = hs.orders[order_id(1)];
      o.cum = "0.2";
      o.fills.push_back(fill_row("5002", o.id, now_ms));
      const std::lock_guard lock(hs.mu);
      hs.account_fills = R"({"fills":[)" + fill_row("5003", kWebOrder, now_ms) + "," + o.fills[1] +
                         "," + o.fills[0] + "," + fill_row("4001", kOldOrder, now_ms) +
                         R"(],"cursor":""})";
      hs.open_page1 = R"({"orders":[)" + open_row(o.id, o.client, "BTC-USD", "0.2") +
                      R"(],"has_next":false,"cursor":""})";
    });
    const std::size_t ends = l.ends();
    h.on_server([](Harness& hs) { hs.user = nullptr; });  // the session is about to go
    h.srv.close_sessions("/user");
    // The connector cancels over REST when the channel goes (the order is listed, then cancelled),
    // and reconciles when it is back.
    l.wait([&] { return !h.srv.frames("cancel").empty(); });
    CHECK(h.srv.frames("cancel")[0] == R"({"order_ids":[")" + order_id(1) + R"("]})");
    REQUIRE(pump_until(
        l.reactor, [&] { return l.ends() > ends && l.venue->order_channel_live(); }, 10'000));
    l.oc.take(l.orders);
    book.drain(l.oc);
    std::map<std::string, const OrderFillMsg*> replayed;
    for (const OrderFillMsg* f : l.all<OrderFillMsg>(EventType::OrderFill)) {
      if ((f->flags & OrderFillMsg::kReplayed) != 0) replayed[std::string(f->exec_id.view())] = f;
    }
    REQUIRE(replayed.size() == 4);
    CHECK(replayed["5002"]->cl_ord_id == n.cl_ord_id);
    CHECK(replayed["4001"]->cl_ord_id == make_cl_ord_id(0, 7));  // named by a lookup
    CHECK_FALSE(replayed["5003"]->cl_ord_id.valid());            // not FastMM's
    // 5001 twice (stream, replay) and once each for the others: the account sold 0.4, not 0.5.
    CHECK(book.position == -Qty::from_decimal("0.4").value());
    CHECK((l.reconcile(ReconcileMsg::Kind::Begin).back()->flags & ReconcileMsg::kExecutionsExact) !=
          0);
    CHECK(h.signed_bad.load() == 0);
  }
}

TEST_CASE("coinbase_advanced.venue: the kill-path cancel-all lists, cancels and waits out a 429") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    {
      const std::lock_guard lock(h.mu);
      h.fill_on_new = false;
    }
    l.push(new_order(1).hdr);
    l.push(new_order(2).hdr);
    l.wait([&] { return l.oc.count(EventType::OrderAck) >= 4; });
    h.on_server([&](Harness& hs) {
      const std::lock_guard lock(hs.mu);
      hs.open_page1 = R"({"orders":[)" + open_row(order_id(1), "fm000100000001", "BTC-USD") + "," +
                      open_row(order_id(2), "fm000100000002", "BTC-USD") +
                      R"(],"has_next":false,"cursor":""})";
      hs.cancel_status = {429};
    });
    // Round 1: listed, batch_cancel refused once (429) then accepted; round 2: listed again (the
    // fake still lists them), cancelled again as unknown; the list never empties, so it fails.
    CHECK_FALSE(l.venue->cancel_all());
    const auto cancels = h.srv.frames("cancel");
    REQUIRE(cancels.size() >= 2);
    CHECK(cancels[0] == R"({"order_ids":[")" + order_id(1) + R"(",")" + order_id(2) + R"("]})");
    CHECK(cancels[1] == cancels[0]);
    // Once the venue lists nothing: done.
    h.on_server([&](Harness& hs) {
      const std::lock_guard lock(hs.mu);
      hs.open_page1 = R"({"orders":[],"has_next":false,"cursor":""})";
    });
    CHECK(l.venue->cancel_all());
    CHECK(h.signed_bad.load() == 0);
  }
}

TEST_CASE("coinbase_advanced.venue: a sequence gap on the feed resubscribes the book") {
  Harness h;
  {
    Live l(h.section(false), true);
    // The book synced and all three subscriptions reached the server (they are recorded there).
    REQUIRE(pump_until(l.reactor, [&] {
      return l.venue->md_feed()->synced_count() == 1 && h.srv.frames("md").size() == 3;
    }));
    const auto md = h.srv.frames("md");
    REQUIRE(md.size() == 3);
    CHECK(md[0] == R"({"type":"subscribe","product_ids":["BTC-USD"],"channel":"level2"})");
    CHECK(md[2] == R"({"type":"subscribe","channel":"heartbeats"})");
    // sequence_num 0 was the snapshot; 5 skips four, and the connection goes on from there.
    h.on_server([](Harness& hs) {
      hs.md_seq = 6;
      hs.feed->send_text(
          R"({"channel":"heartbeats","client_id":"","timestamp":"2026-09-30T02:37:15Z","sequence_num":5,"events":[{"current_time":"x","heartbeat_counter":1}]})");
    });
    REQUIRE(pump_until(l.reactor, [&] {
      for (const std::string& f : h.srv.frames("md")) {
        if (f.find(R"("type":"unsubscribe")") != std::string::npos) return true;
      }
      return false;
    }));
    REQUIRE(pump_until(l.reactor, [&] { return l.venue->md_feed()->synced_count() == 1; }));
    CHECK(l.venue->md_feed()->resync_count() == 1);
    CHECK(l.venue->md_feed()->stats().sequence_gaps == 1);
    // The dry run: no user connection, orders refused, cancel-all a no-op.
    CHECK_FALSE(l.venue->caps().user_stream);
    CHECK(h.srv.open_count("/user") == 0);
    l.push(new_order().hdr);
    l.wait([&] { return l.oc.count(EventType::OrderReject) == 1; });
    CHECK(l.oc.last<OrderRejectMsg>(EventType::OrderReject)->reason == RejectReason::VenueKilled);
    CHECK(l.venue->cancel_all());
    CHECK(h.srv.frames("new").empty());
  }
}
