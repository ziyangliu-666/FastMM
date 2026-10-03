// GateUsdtVenue: config mapping, the registry entry, and the connector against a scripted
// in-process fake Gate (REST + one WebSocket URL serving the public, private and trade
// connections): reference data from the contract table, the account check, obu snapshot and
// increments, the login on both private connections and the signed subscriptions, order_place
// -> ack, a usertrades fill completed from the shadow, order_cancel -> orders finished/cancelled,
// the open-order and position reconciliation, the fill replay and the blocking cancel_all.
#include "fake_venue_util.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/gate/gate_usdt_venue.hpp"
#include "fastmm/venues/registry.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gate;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kKey = "fake-key";
constexpr const char* kSecret = "fake-secret";
constexpr VenueId kVenue{3};
const InstrumentId kNvda{0};

// {"time":T,"channel":C,"event":"api","payload":{"api_key":..,"signature":S,"timestamp":"T",..}}
bool login_ok(std::string_view t) {
  const std::string ts = json_str(t, "timestamp");
  const std::string sig = json_str(t, "signature");
  if (ts.empty() || sig.empty() || json_str(t, "api_key") != kKey) return false;
  return sig == net::hmac_sha512_hex(kSecret, "api\nfutures.login\n\n" + ts).view();
}

// A subscription's auth block: SIGN = HMAC(secret, "channel=C&event=subscribe&time=T").
bool subscribe_ok(std::string_view t) {
  const std::string channel = json_str(t, "channel");
  const std::string time = json_int(t, "time");
  const std::string sign = json_str(t, "SIGN");
  if (json_str(t, "KEY") != kKey) return false;
  return sign ==
         net::hmac_sha512_hex(kSecret, "channel=" + channel + "&event=subscribe&time=" + time)
             .view();
}

// SIGN == HMAC(secret, METHOD\npath\nquery\nsha512(body)\nTimestamp)
bool rest_signed(const net::HttpRequest& r) {
  const std::string pre =
      std::string(r.method) + "\n" + std::string(r.path) + "\n" + std::string(r.query) + "\n" +
      std::string(net::sha512_hex(r.body).view()) + "\n" + std::string(r.header("Timestamp"));
  return r.header("KEY") == kKey && r.header("SIGN") == net::hmac_sha512_hex(kSecret, pre).view();
}

std::string api_reply(std::string_view req_id,
                      std::string_view channel,
                      int status,
                      std::string_view data) {
  return "{\"request_id\":\"" + std::string(req_id) +
         "\",\"header\":{\"response_time\":\"1791009980000\"," + "\"status\":\"" +
         std::to_string(status) + "\",\"channel\":\"" + std::string(channel) +
         "\",\"event\":\"api\",\"x_gate_ratelimit_requests_remain\":99,\"x_gate_ratelimit_limit\":"
         "100}," +
         "\"data\":" + std::string(data) + "}";
}

std::string order_json(std::string_view text,
                       std::string_view status,
                       std::string_view finish_as,
                       const char* size,
                       const char* left) {
  return std::string(
             R"({"id":74046514,"user":110284739,"create_time":1791009980.101,"finish_time":0,"finish_as":")") +
         std::string(finish_as) + R"(","status":")" + std::string(status) +
         R"(","contract":"NVDA_USDT","size":")" + size + R"(","left":")" + left +
         R"(","price":"234.1","tif":"poc","fill_price":"0","text":")" + std::string(text) +
         R"(","tkfr":"0.0005","mkfr":"0.0002","amend_text":"-"})";
}

std::string orders_push(std::string_view text,
                        std::string_view status,
                        std::string_view finish_as,
                        const char* size,
                        const char* left) {
  return R"({"channel":"futures.orders","event":"update","time":1791009990,"time_ms":1791009990123,"result":[)" +
         order_json(text, status, finish_as, size, left) + "]}";
}

struct Harness {
  FakeVenueServer srv;
  std::string contract_nvda = fastmm::test::fixture("gate/rest_contract_nvda.json");
  std::string order_book = fastmm::test::fixture("gate/rest_order_book.json");
  std::string accounts = fastmm::test::fixture("gate/rest_accounts.json");
  std::string obu_snapshot = fastmm::test::fixture("gate/obu_snapshot.json");
  std::string obu_delta = fastmm::test::fixture("gate/obu_delta.json");
  std::string open_orders = "[]";
  std::string positions = "[]";
  std::mutex mu;
  std::vector<std::string> trades_pages{"[]"};
  std::atomic<int> cancel_all_calls{0};
  std::atomic<int> cancel_all_429{0};
  std::atomic<int> open_orders_calls{0};
  std::atomic<int> positions_calls{0};
  std::atomic<int> accounts_calls{0};
  std::atomic<int> trades_calls{0};
  std::atomic<int> logins{0};
  std::atomic<int> bad_logins{0};
  std::atomic<int> subscriptions{0};
  std::atomic<int> bad_subscriptions{0};
  std::atomic<int> places{0};
  std::atomic<int> cancels{0};
  net::WsSession* private_session = nullptr;  // server thread only
  net::WsSession* md_session = nullptr;

  ~Harness() { srv.stop(); }
  Harness() {
    srv.route("GET", "/api/v4/futures/usdt/contracts/NVDA_USDT", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, contract_nvda);
    });
    srv.route("GET", "/api/v4/futures/usdt/order_book", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, order_book);
    });
    srv.route("GET", "/api/v4/futures/usdt/accounts", [this](const net::HttpRequest& r) {
      ++accounts_calls;
      if (!rest_signed(r))
        return net::HttpServerResponse::json(401,
                                             R"({"label":"INVALID_SIGNATURE","message":"bad"})");
      return net::HttpServerResponse::json(200, accounts);
    });
    srv.route("GET", "/api/v4/futures/usdt/orders", [this](const net::HttpRequest& r) {
      ++open_orders_calls;
      srv.record("open_orders", std::string(r.query));
      if (!rest_signed(r))
        return net::HttpServerResponse::json(401,
                                             R"({"label":"INVALID_SIGNATURE","message":"bad"})");
      return net::HttpServerResponse::json(200, open_orders);
    });
    srv.route("DELETE", "/api/v4/futures/usdt/orders", [this](const net::HttpRequest& r) {
      ++cancel_all_calls;
      srv.record("cancel_all", std::string(r.query));
      if (!rest_signed(r))
        return net::HttpServerResponse::json(401,
                                             R"({"label":"INVALID_SIGNATURE","message":"bad"})");
      if (cancel_all_429.load() > 0) {
        --cancel_all_429;
        return net::HttpServerResponse::json(429,
                                             R"({"label":"TOO_MANY_REQUESTS","message":"slow"})");
      }
      return net::HttpServerResponse::json(200, "[]");
    });
    srv.route("GET", "/api/v4/futures/usdt/positions", [this](const net::HttpRequest& r) {
      ++positions_calls;
      if (!rest_signed(r))
        return net::HttpServerResponse::json(401,
                                             R"({"label":"INVALID_SIGNATURE","message":"bad"})");
      return net::HttpServerResponse::json(200, positions);
    });
    srv.route("GET", "/api/v4/futures/usdt/my_trades_timerange", [this](const net::HttpRequest& r) {
      ++trades_calls;
      srv.record("my_trades", std::string(r.query));
      if (!rest_signed(r))
        return net::HttpServerResponse::json(401,
                                             R"({"label":"INVALID_SIGNATURE","message":"bad"})");
      std::lock_guard<std::mutex> lock(mu);
      std::string page = trades_pages.back();
      if (trades_pages.size() > 1) {
        page = trades_pages.front();
        trades_pages.erase(trades_pages.begin());
      }
      return net::HttpServerResponse::json(200, page);
    });
    srv.on_ws_open("/v4/ws/usdt", [](net::WsSession&) {});
    srv.on_ws_text("/v4/ws/usdt", [this](net::WsSession& s, std::string_view t) {
      const std::string channel = json_str(t, "channel");
      const std::string event = json_str(t, "event");
      if (channel == "futures.ping") {
        s.send_text(
            R"({"time":1,"time_ms":1000,"channel":"futures.pong","event":"","result":null})");
        return;
      }
      if (channel == "futures.login") {
        const std::string req_id = json_str(t, "req_id");
        if (login_ok(t)) {
          ++logins;
          if (req_id == "login-p") private_session = &s;
          s.send_text(api_reply(
              req_id, channel, 200, R"({"result":{"api_key":"fake-key","uid":"110284739"}})"));
        } else {
          ++bad_logins;
          s.send_text(
              api_reply(req_id,
                        channel,
                        401,
                        R"({"errs":{"label":"INVALID_KEY","message":"Invalid key provided"}})"));
        }
        return;
      }
      if (event == "subscribe") {
        srv.record("subscribe", std::string(t));
        const bool is_private = t.find("\"auth\"") != std::string_view::npos;
        if (is_private) {
          if (subscribe_ok(t)) {
            ++subscriptions;
          } else {
            ++bad_subscriptions;
            s.send_text(
                R"({"time":1,"time_ms":1000,"channel":")" + channel +
                R"(","event":"subscribe","error":{"code":4,"message":"authentication fail"},"result":{"status":"fail"}})");
            return;
          }
        } else {
          md_session = &s;
        }
        s.send_text(R"({"time":1,"time_ms":1000,"channel":")" + channel +
                    R"(","event":"subscribe","result":{"status":"success"}})");
        if (channel == "futures.obu") {
          s.send_text(obu_snapshot);
          s.send_text(obu_delta);
        }
        return;
      }
      if (channel == "futures.order_place") {
        ++places;
        srv.record("order_place", std::string(t));
        const std::string req_id = json_str(t, "req_id");
        const std::string text = json_str(t, "text");
        s.send_text(api_reply(
            req_id, channel, 200, "{\"result\":" + order_json(text, "open", "", "3", "3") + "}"));
        return;
      }
      if (channel == "futures.order_cancel") {
        ++cancels;
        srv.record("order_cancel", std::string(t));
        const std::string req_id = json_str(t, "req_id");
        s.send_text(api_reply(
            req_id,
            channel,
            200,
            "{\"result\":" + order_json("t-fm000000000001", "finished", "cancelled", "3", "2") +
                "}"));
        if (private_session != nullptr)
          private_session->send_text(
              orders_push("t-fm000000000001", "finished", "cancelled", "3", "2"));
        return;
      }
    });
    srv.start();
  }

  VenueSection section() const {
    VenueSection s;
    s.name = "gate_usdt";
    s.kind = "gate_usdt";
    s.ws_url = srv.ws_base() + "/v4/ws/usdt";
    s.rest_url = srv.http_base();
    s.api_key = kKey;
    s.api_secret = kSecret;
    return s;
  }
};

}  // namespace

TEST_CASE("gate.venue: config mapping and defaults") {
  VenueSection s;
  s.name = "gate";
  s.extra["book_level"] = "400";
  s.extra["order_api"] = "rest";
  s.extra["dead_mans_switch_s"] = "30";
  s.extra["settle"] = "USDT";
  const GateUsdtVenueConfig c = make_gate_usdt_config(s, true);
  CHECK(c.ws_url == "wss://fx-ws.gateio.ws/v4/ws/usdt");
  CHECK(c.rest_url == "https://fx-api.gateio.ws");
  CHECK(c.settle == "usdt");
  CHECK(c.book_level == 400);
  CHECK_FALSE(c.ws_order_api);
  CHECK(c.dead_mans_switch_s == 30);
  CHECK(c.dry_run);
  CHECK(c.orders_per_second == 50);
  s.extra["book_level"] = "20";
  CHECK_THROWS_AS(make_gate_usdt_config(s, true), std::invalid_argument);
  s.extra["book_level"] = "50";
  s.extra["settle"] = "eur";
  CHECK_THROWS_AS(make_gate_usdt_config(s, true), std::invalid_argument);
}

TEST_CASE("gate.venue: the registry resolves gate_usdt and its aliases") {
  VenueRegistry r;
  register_builtin_venues(r);
  const VenueEntry* e = r.find("gate_usdt");
  REQUIRE(e != nullptr);
  CHECK(r.find("gate") == e);
  CHECK(r.find("gate_futures") == e);
  CHECK(e->caps.executions);
  CHECK(e->caps.positions);
  CHECK(e->caps.replace);
  VenueSection s;
  s.name = "g";
  s.kind = "gate";
  VenueFactoryOptions opts;
  opts.dry_run = true;
  const std::unique_ptr<Venue> v = make_venue(VenueId{3}, s, opts, r);
  REQUIRE(v);
  CHECK(v->name() == "g");
  CHECK_FALSE(v->caps().user_stream);
  Config cfg;
  s.extra["no_such_key"] = "1";
  cfg.venues.push_back(s);
  CHECK_THROWS_WITH_AS(validate_venues(cfg, r), doctest::Contains("no_such_key"), ConfigError);
}

TEST_CASE("gate.venue: scripted fake exchange end to end") {
  Harness h;
  InstrumentTable instruments;
  Instrument inst = make_instrument("NVDA_USDT", 3, "NVDA", "USDT");
  inst.asset_class = AssetClass::Perpetual;
  REQUIRE(instruments.add(inst));
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  const Seqlocked<TscCalibration> tsc(calibrate_tsc(milliseconds(10)));
  {
    GateUsdtVenueConfig cfg = make_gate_usdt_config(h.section(), false);
    GateUsdtVenue venue(kVenue, cfg);
    venue.set_tsc_calibration_source(&tsc);
    REQUIRE(venue.load_reference_data(instruments));
    CHECK(instruments.get(kNvda).tick == Price::from_decimal("0.01").value());
    CHECK(instruments.get(kNvda).lot == Qty::from_int(1));
    CHECK(instruments.get(kNvda).contract_multiplier.is_positive());
    CHECK(instruments.get(kNvda).base.view() == "NVDA");
    CHECK(instruments.get(kNvda).quote.view() == "USDT");
    CHECK(instruments.get(kNvda).asset_class == AssetClass::Perpetual);
    CHECK(venue.user_id() == "110284739");
    CHECK(h.accounts_calls.load() == 1);
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kNvda};
    venue.subscribe(ids);
    venue.connect(reactor);
    auto run_until = [&](auto pred, int ms = 3000) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (!pred() && std::chrono::steady_clock::now() < deadline) {
        reactor.run_once(5);
        venue.on_timer(net::Reactor::now_ns());
      }
      return pred();
    };
    // Books: the obu snapshot and its increment reach the engine; both private connections log
    // in and the four channels are subscribed with valid signatures.
    REQUIRE(run_until([&] {
      return venue.status().books_synced == 1 && h.logins.load() == 2 &&
             h.subscriptions.load() == 4;
    }));
    CHECK(h.bad_logins.load() == 0);
    CHECK(h.bad_subscriptions.load() == 0);
    REQUIRE(run_until([&] { return venue.order_channel_live(); }));
    {
      bool snapshot = false;
      bool delta = false;
      for (const auto& m : md.drain()) {
        if (RecordingSink::type_of(m) == EventType::BookSnapshot) snapshot = true;
        if (RecordingSink::type_of(m) == EventType::BookDelta) delta = true;
      }
      CHECK(snapshot);
      CHECK(delta);
    }
    // The start-up sweep: the fill replay, then the open orders per contract and the positions.
    // (accounts_calls: the start-up check, then the balance leg after the snapshot.)
    REQUIRE(run_until([&] {
      return h.open_orders_calls.load() >= 1 && h.positions_calls.load() >= 1 &&
             h.trades_calls.load() >= 1 && h.accounts_calls.load() >= 2;
    }));
    reactor.run_once(20);
    REQUIRE(h.srv.frames("open_orders").size() >= 1);
    CHECK(h.srv.frames("open_orders")[0] == "status=open&contract=NVDA_USDT&limit=100");
    {
      bool begin = false;
      bool position = false;
      bool end = false;
      bool balance = false;
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) == EventType::Reconcile) {
          const auto& r = RecordingSink::as<ReconcileMsg>(m);
          if (r.kind == ReconcileMsg::Kind::Begin) begin = true;
          if (r.kind == ReconcileMsg::Kind::Position) position = true;
          if (r.kind == ReconcileMsg::Kind::End) end = true;
        }
        if (RecordingSink::type_of(m) == EventType::Balance) balance = true;
      }
      CHECK(begin);
      CHECK(position);
      CHECK(end);
      CHECK(balance);
    }
    // A post-only bid: order_place -> ack from the reply; the orders push repeats "open" and is
    // not a second ack.
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kNvda, kVenue);
    n.cl_ord_id = ClientOrderId{1};
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.tif = TimeInForce::Gtc;
    n.price = Price::from_decimal("234.1").value();
    n.qty = Qty::from_int(3);
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.places.load() == 1; }));
    REQUIRE(h.srv.frames("order_place").size() == 1);
    CHECK(
        h.srv.frames("order_place")[0].find(
            R"("req_param":{"contract":"NVDA_USDT","size":3,"price":"234.1","tif":"poc","text":"t-fm000000000001"})") !=
        std::string::npos);
    h.srv.send_to("/v4/ws/usdt", orders_push("t-fm000000000001", "open", "", "3", "3"));
    std::size_t ack_count = 0;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) == EventType::OrderAck) ++ack_count;
      }
      return ack_count >= 1;
    }));
    reactor.run_once(50);
    venue.on_timer(net::Reactor::now_ns());
    for (const auto& m : orders.drain()) CHECK(RecordingSink::type_of(m) != EventType::OrderAck);
    CHECK(venue.shadow_count() == 1);
    // A usertrades fill: the shadow supplies cum and leaves.
    h.srv.send_to(
        "/v4/ws/usdt",
        R"({"time":1791009985,"time_ms":1791009985321,"channel":"futures.usertrades","event":"update","result":[{"id":"3335259","create_time":1791009985,"create_time_ms":1791009985300,"contract":"NVDA_USDT","order_id":"74046514","size":"1","price":"234.1","role":"maker","text":"t-fm000000000001","fee":0.04682,"point_fee":0}]})");
    bool filled = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderFill) continue;
        const auto& f = RecordingSink::as<OrderFillMsg>(m);
        CHECK(f.cl_ord_id == ClientOrderId{1});
        CHECK(f.qty == Qty::from_int(1));
        CHECK(f.cum_qty == Qty::from_int(1));
        CHECK(f.leaves_qty == Qty::from_int(2));
        CHECK(f.fee == Notional::from_decimal("0.04682").value());
        CHECK(f.liquidity == Liquidity::Maker);
        filled = true;
      }
      return filled;
    }));
    // Cancel: by the venue's id now that the reply named it; the orders push finishes it.
    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kNvda, kVenue);
    c.cl_ord_id = ClientOrderId{1};
    REQUIRE(outbound.try_push(&c, c.hdr.len));
    venue.on_wake();
    REQUIRE(run_until([&] { return h.cancels.load() == 1; }));
    CHECK(h.srv.frames("order_cancel")[0].find(R"("req_param":{"order_id":"74046514"})") !=
          std::string::npos);
    bool cancelled = false;
    REQUIRE(run_until([&] {
      for (const auto& m : orders.drain()) {
        if (RecordingSink::type_of(m) != EventType::OrderCancelAck) continue;
        const auto& a = RecordingSink::as<OrderCancelAckMsg>(m);
        CHECK(a.cl_ord_id == ClientOrderId{1});
        CHECK(a.cum_qty == Qty::from_int(1));
        cancelled = true;
      }
      return cancelled;
    }));
    CHECK(venue.shadow_count() == 0);
    // The blocking kill switch: one DELETE /orders?contract=NVDA_USDT, a 429 waited out.
    h.cancel_all_429 = 1;
    CHECK(venue.cancel_all());
    CHECK(h.cancel_all_calls.load() == 2);
    CHECK(h.srv.frames("cancel_all").back() == "contract=NVDA_USDT");
    venue.disconnect();
  }
}
