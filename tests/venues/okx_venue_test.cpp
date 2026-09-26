// OkxVenue against a scripted fake OKX v5: config mapping, reference data in contracts, the
// account-mode check at start-up, the start-up sweep, order entry over the WebSocket, amends, the
// fill and funding replays, cancel-all-after, the retry of a failed replay, an engine-requested
// reconciliation and a book gap. Wire formats as in the okx_* unit tests (OKX v5 docs read
// 2026-09-26); only the public market data has met the real venue.
#include "fastmm/venues/okx/okx_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/net/crypto.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::okx;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kKey = "fake-key";
constexpr const char* kSecret = "fake-secret";
constexpr const char* kPass = "fake-pass";
constexpr VenueId kVenue{1};
const InstrumentId kBtc{0};
constexpr long long kT = 1789299703000;  // 2026-09-13, older than both fills and bills windows

std::string b64_hmac(std::string_view data) {
  std::array<std::uint8_t, net::kSha256Size> mac{};
  REQUIRE(net::hmac_sha256(kSecret, data, mac));
  return net::base64_encode(std::span<const std::uint8_t>(mac));
}

bool login_ok(std::string_view t) {
  const std::string ts = json_str(t, "timestamp");
  return json_str(t, "apiKey") == kKey && json_str(t, "passphrase") == kPass &&
         json_str(t, "sign") == b64_hmac(ts + "GET/users/self/verify");
}

bool rest_signed(const net::HttpRequest& r) {
  const std::string pre = std::string(r.header("OK-ACCESS-TIMESTAMP")) + std::string(r.method) +
                          std::string(r.target) + std::string(r.body);
  return r.header("OK-ACCESS-KEY") == kKey && r.header("OK-ACCESS-PASSPHRASE") == kPass &&
         r.header("OK-ACCESS-SIGN") == b64_hmac(pre);
}

std::string ok_data(const std::string& rows) {
  return R"({"code":"0","msg":"","data":[)" + rows + "]}";
}

// An orders-channel push for BTC-USDT-SWAP.
std::string order_push(const char* cl,
                       const char* state,
                       const char* acc,
                       const char* fill_sz,
                       const char* trade_id,
                       const char* req_id = "",
                       const char* amend_result = "",
                       const char* cancel_source = "") {
  return std::string(
             R"({"arg":{"channel":"orders","instType":"SWAP","uid":"77"},"data":[{"instType":"SWAP","instId":"BTC-USDT-SWAP","ordId":"312","clOrdId":")") +
         cl +
         R"(","px":"60000.1","sz":"3","ordType":"post_only","side":"sell","posSide":"net","tdMode":"cross","accFillSz":")" +
         acc + R"(","state":")" + state + R"(","fillPx":"60000.1","tradeId":")" + trade_id +
         R"(","fillSz":")" + fill_sz +
         R"(","fillTime":"1789299703453","fillFee":"0.006","fillFeeCcy":"USDT","execType":"M","uTime":"1789299703470","reqId":")" +
         req_id + R"(","amendResult":")" + amend_result + R"(","cancelSource":")" + cancel_source +
         R"(","code":"0","msg":""}]})";
}

std::string fill_row(const char* trade_id, const char* cl, long long ts) {
  return std::string(R"({"instType":"SWAP","instId":"BTC-USDT-SWAP","tradeId":")") + trade_id +
         R"(","ordId":"312","clOrdId":")" + cl + R"(","billId":"b)" + trade_id +
         R"(","subType":"1","tag":"","fillPx":"60000.1","fillSz":"2","side":"sell","posSide":"net","execType":"M","feeCcy":"USDT","fee":"0.012","ts":")" +
         std::to_string(ts + 7) + R"(","fillTime":")" + std::to_string(ts) + R"("})";
}

std::string bill_row(const char* inst, const char* id, const char* chg, long long ts) {
  return std::string(R"({"billId":")") + id + R"(","instId":")" + inst +
         R"(","ccy":"USDT","balChg":")" + chg + R"(","bal":"100","type":"8","subType":")" +
         (chg[0] == '-' ? "173" : "174") + R"(","ts":")" + std::to_string(ts) + R"(","pnl":")" +
         chg + R"(","fee":"0"})";
}

const std::string kSnapshot =
    R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":[{"asks":[["60000.2","5","0","1"]],"bids":[["60000.1","3","0","1"]],"ts":"1789299703000","checksum":0,"prevSeqId":-1,"seqId":100}]})";

struct Harness {
  FakeVenueServer srv;
  std::mutex mu;  // guards the replies below, which tests change while the server runs
  std::string instruments = fastmm::test::fixture("okx/instruments_btc_usdt_swap.json");
  std::string server_time = fastmm::test::fixture("okx/server_time.json");
  std::string account_config =
      ok_data(R"({"acctLv":"2","posMode":"net_mode","uid":"77","perm":"read_only,trade"})");
  std::string open_orders = ok_data("");
  std::string positions = ok_data("");
  std::string fills = ok_data("");
  std::string bills = ok_data("");
  std::string cancel_after_reply =
      R"({"code":"0","msg":"","data":[{"triggerTime":"1","tag":"","ts":"1"}]})";
  int fills_failures = 0;  // the next fills queries answer 50011
  std::atomic<int> signed_ok{0};
  std::atomic<int> signed_bad{0};
  std::atomic<int> login_failures{0};
  // "order" answers as an IOC of 3 that took 2 in two trades and ended, the push that ends it
  // (accFillSz 2) ahead of the two that carry the fills.
  std::atomic<bool> ioc_end_first{false};
  net::WsSession* private_session = nullptr;  // server thread only
  // Answer the private login only once an order has arrived on the order connection, so that
  // the start-up sweep is asked for after an order went out (server thread only).
  bool hold_private_login = false;
  net::WsSession* held_login = nullptr;

  void count(const net::HttpRequest& r) { ++(rest_signed(r) ? signed_ok : signed_bad); }
  std::string reply(const std::string& s) {
    const std::lock_guard lock(mu);
    return s;
  }

  Harness() {
    srv.route("GET", "/api/v5/public/time", [this](const net::HttpRequest& r) {
      srv.record("sim_header", std::string(r.header("x-simulated-trading")));
      return net::HttpServerResponse::json(200, reply(server_time));
    });
    srv.route("GET", "/api/v5/public/instruments", [this](const net::HttpRequest& r) {
      srv.record("instruments", std::string(r.query));
      srv.record("sim_header", std::string(r.header("x-simulated-trading")));
      return net::HttpServerResponse::json(200, reply(instruments));
    });
    srv.route("GET", "/api/v5/account/config", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "config");
      return net::HttpServerResponse::json(200, reply(account_config));
    });
    srv.route("GET", "/api/v5/trade/orders-pending", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "orders-pending?" + std::string(r.query));
      return net::HttpServerResponse::json(200, reply(open_orders));
    });
    srv.route("GET", "/api/v5/account/positions", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest", "positions?" + std::string(r.query));
      return net::HttpServerResponse::json(200, reply(positions));
    });
    auto fills_route = [this](const net::HttpRequest& r) {
      count(r);
      srv.record("rest",
                 std::string(r.path.substr(r.path.rfind('/') + 1)) + "?" + std::string(r.query));
      srv.record("fills", std::string(r.query));
      const std::lock_guard lock(mu);
      if (fills_failures > 0) {
        --fills_failures;
        return net::HttpServerResponse::json(
            200,
            R"({"code":"50011","msg":"Rate limit reached. Please refer to API documentation and throttle requests accordingly.","data":[]})");
      }
      return net::HttpServerResponse::json(200, fills);
    };
    srv.route("GET", "/api/v5/trade/fills", fills_route);
    srv.route("GET", "/api/v5/trade/fills-history", fills_route);
    auto bills_route = [this](const net::HttpRequest& r) {
      count(r);
      srv.record("bills", std::string(r.path) + "?" + std::string(r.query));
      return net::HttpServerResponse::json(200, reply(bills));
    };
    srv.route("GET", "/api/v5/account/bills", bills_route);
    srv.route("GET", "/api/v5/account/bills-archive", bills_route);
    srv.route("POST", "/api/v5/trade/cancel-all-after", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("cancel_after", std::string(r.body));
      return net::HttpServerResponse::json(200, reply(cancel_after_reply));
    });
    srv.route("POST", "/api/v5/trade/cancel-batch-orders", [this](const net::HttpRequest& r) {
      count(r);
      srv.record("cancel_batch", std::string(r.body));
      return net::HttpServerResponse::json(
          200, ok_data(R"({"ordId":"312","clOrdId":"","sCode":"0","sMsg":""})"));
    });
    srv.on_ws_text("/ws/v5/public", [this](net::WsSession& s, std::string_view t) {
      if (t == "ping") {
        s.send_text("pong");
        return;
      }
      srv.record("md", std::string(t));
      if (json_str(t, "op") == "login") {
        s.send_text(
            login_ok(t)
                ? R"({"event":"login","code":"0","msg":"","connId":"m1"})"
                : R"({"event":"error","code":"60009","msg":"Login failed.","connId":"m1"})");
        return;
      }
      if (json_str(t, "op") != "subscribe") return;
      s.send_text(
          R"({"id":"md","event":"subscribe","arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"connId":"m1"})");
      if (t.find(R"("channel":"books")") != std::string_view::npos) s.send_text(kSnapshot);
    });
    srv.on_ws_text("/ws/v5/private", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      if (op == "login") {
        const bool ok = login_ok(t);
        if (!ok) ++login_failures;
        if (ok && hold_private_login && srv.frames("trade").empty()) {
          held_login = &s;
          return;
        }
        s.send_text(ok ? R"({"event":"login","code":"0","msg":"","connId":"p1"})"
                       : R"({"event":"error","code":"60009","msg":"Login failed.","connId":"p1"})");
      } else if (op == "subscribe") {
        private_session = &s;
        srv.record("private_subscribe", std::string(t));
        for (const char* ch : {"orders", "positions"}) {
          s.send_text(std::string(R"({"id":"private","event":"subscribe","arg":{"channel":")") +
                      ch + R"(","instType":"SWAP"},"connId":"p1"})");
        }
      }
    });
    srv.on_ws_text("/ws/v5/trade", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      if (op == "login") {
        s.send_text(
            login_ok(t)
                ? R"({"event":"login","code":"0","msg":"","connId":"t1"})"
                : R"({"event":"error","code":"60009","msg":"Login failed.","connId":"t1"})");
        return;
      }
      if (op != "order" && op != "amend-order" && op != "cancel-order") return;
      srv.record("trade", std::string(t));
      if (held_login != nullptr) {
        held_login->send_text(R"({"event":"login","code":"0","msg":"","connId":"p1"})");
        held_login = nullptr;
      }
      const std::string id = json_str(t, "id");
      const std::string req = json_str(t, "reqId");
      s.send_text(
          R"({"id":")" + id + R"(","op":")" + op +
          R"(","data":[{"clOrdId":"fm000100000001","ordId":"312","tag":"","reqId":")" + req +
          R"(","ts":"1789299700444","sCode":"0","sMsg":""}],"code":"0","msg":"","inTime":"1","outTime":"2"})");
      if (private_session == nullptr) return;
      if (op == "order" && ioc_end_first.load()) {
        private_session->send_text(
            order_push("fm000100000001", "canceled", "2", "0", "", "", "", "14"));
        private_session->send_text(
            order_push("fm000100000001", "partially_filled", "1", "1", "4463701411"));
        private_session->send_text(
            order_push("fm000100000001", "partially_filled", "2", "1", "4463701412"));
      } else if (op == "order") {
        private_session->send_text(order_push("fm000100000001", "live", "0", "0", ""));
        private_session->send_text(
            order_push("fm000100000001", "partially_filled", "1", "1", "4463701411"));
      } else if (op == "amend-order") {
        private_session->send_text(
            order_push("fm000100000001", "partially_filled", "1", "0", "", req.c_str(), "0"));
      } else {
        private_session->send_text(
            order_push("fm000100000001", "canceled", "1", "0", "", "", "", "1"));
      }
    });
    srv.start();
  }
  // The server thread reads the members above: stop it before they go.
  ~Harness() { srv.stop(); }

  VenueSection section(bool with_keys = true) const {
    VenueSection s;
    s.name = "fake-okx";
    s.kind = "okx";
    s.ws_url = srv.ws_base() + "/ws/v5/public";
    s.ws_api_url = srv.ws_base() + "/ws/v5/trade";
    s.rest_url = srv.http_base();
    s.testnet = true;  // demo trading: the x-simulated-trading header
    if (with_keys) {
      s.api_key = kKey;
      s.api_secret = kSecret;
      s.api_passphrase = kPass;
    }
    s.supports_replace = true;
    s.extra["dead_mans_switch_s"] = "0";  // on in dead_mans_switch tests only
    return s;
  }
  std::vector<std::string> rest() { return srv.frames("rest"); }
};

// BTC-USDT-SWAP configured as a spot instrument with a USDC quote and a tick of 1: reference data
// has to make it a USDT-settled perpetual in contracts of 0.01 BTC.
Instrument configured_swap() {
  Instrument i = make_instrument("BTC-USDT-SWAP", 1, "", "USDC");
  i.tick = Price::from_int(1);
  return i;
}

struct Live {
  InstrumentTable instruments;
  RecordingSink md{8U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<OkxVenue> venue;
  Collected oc;
  Collected mc;

  Live(const VenueSection& section, const std::function<void(Live&)>& before_connect = {}) {
    REQUIRE(instruments.add(configured_swap()));
    venue = std::make_unique<OkxVenue>(kVenue, make_okx_config(section, false));
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
  // The start-up sweep is the connector's own first reconciliation: wait for its End, and for
  // the order connection, which logs in on its own and may come up later.
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
};

OutNewOrderMsg new_order() {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, kBtc, kVenue);
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Sell;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("60000.1").value();
  n.qty = Qty::from_int(3);
  return n;
}

}  // namespace

TEST_CASE("okx.venue: config mapping derives the URLs and checks keys and environment") {
  VenueSection s;
  s.name = "okx";
  s.kind = "okx";
  s.ws_url = "wss://wspap.okx.com:8443/ws/v5/public";
  s.rest_url = "https://www.okx.com";
  s.api_key = "k";
  s.api_secret = "s";
  s.api_passphrase = "p";
  OkxVenueConfig c = make_okx_config(s, false);
  CHECK(c.ws_private_url == "wss://wspap.okx.com:8443/ws/v5/private");
  CHECK(c.ws_trade_url == c.ws_private_url);  // order operations go to the private endpoint
  CHECK(c.simulated);
  CHECK(c.td_mode == TdMode::Cross);
  CHECK(c.depth_channel == OkxDepthChannel::Books);
  CHECK(c.dead_mans_switch_s == 60);
  CHECK(c.credentials.usable());
  s.extra["td_mode"] = "isolated";
  s.extra["depth_channel"] = "books-l2-tbt";
  s.extra["dead_mans_switch_s"] = "5";
  c = make_okx_config(s, false);
  CHECK(c.td_mode == TdMode::Isolated);
  CHECK(c.depth_channel == OkxDepthChannel::BooksL2Tbt);
  CHECK(c.dead_mans_switch_s == 10);  // clamped to 10..120
  s.extra["dead_mans_switch_s"] = "0";
  CHECK(make_okx_config(s, false).dead_mans_switch_s == 0);
  s.extra["td_mode"] = "margin";
  CHECK_THROWS_AS(static_cast<void>(make_okx_config(s, false)), std::invalid_argument);
  s.extra.erase("td_mode");
  s.extra["depth_channel"] = "books5";
  CHECK_THROWS_AS(static_cast<void>(make_okx_config(s, false)), std::invalid_argument);
  s.extra.erase("depth_channel");
  // Demo host and production flag, or the reverse: refused.
  s.testnet = false;
  CHECK_THROWS_AS(static_cast<void>(make_okx_config(s, false)), std::invalid_argument);
  s.ws_url = "wss://ws.okx.com:8443/ws/v5/public";
  CHECK_FALSE(make_okx_config(s, false).simulated);
  s.testnet = true;
  CHECK_THROWS_AS(static_cast<void>(make_okx_config(s, false)), std::invalid_argument);
  s.ws_url = "wss://wspap.okx.com:8443/ws/v5/public";
  // A key without its passphrase logs in nowhere; a dry run needs none.
  s.api_passphrase.clear();
  try {
    static_cast<void>(make_okx_config(s, false));
    FAIL("no passphrase accepted");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).rfind("venues.okx.api_passphrase:", 0) == 0);
  }
  VenueSection dry;
  dry.name = "okx";
  dry.ws_url = "wss://wspap.okx.com:8443/ws/v5/public";
  dry.extra["depth_channel"] = "books50-l2-tbt";
  c = make_okx_config(dry, true);
  CHECK(c.depth_channel == OkxDepthChannel::Books);  // no login without keys
  CHECK_THROWS_AS(static_cast<void>(make_okx_config(dry, false)), std::invalid_argument);
}

TEST_CASE("okx.venue: reference data makes a USDT-settled perpetual counted in contracts") {
  Harness h;
  InstrumentTable instruments;
  REQUIRE(instruments.add(configured_swap()));
  OkxVenue venue(kVenue, make_okx_config(h.section(), false));
  REQUIRE(venue.load_reference_data(instruments));
  const Instrument& in = instruments.get(kBtc);
  CHECK(in.asset_class == AssetClass::Perpetual);
  CHECK(in.contract_multiplier == Qty::from_decimal("0.01").value());  // ctVal * ctMult
  CHECK_FALSE(in.inverse());
  CHECK((in.flags & Instrument::kReduceOnlySupported) != 0);
  CHECK(in.enabled());
  CHECK(in.tick == Price::from_decimal("0.1").value());
  CHECK(in.lot == Qty::from_decimal("0.01").value());
  CHECK(in.min_qty == Qty::from_decimal("0.01").value());
  CHECK(in.max_qty == Qty::from_int(100000000));
  CHECK(in.min_notional.is_zero());
  CHECK(in.base.view() == "BTC");
  CHECK(in.quote.view() == "USDT");  // settleCcy, not the configured USDC
  CHECK(in.settlement_ccy() == "USDT");
  // 2 contracts at 60000 are 0.02 BTC: 1200 USDT, and a tick is worth 0.001 USDT a contract.
  CHECK(in.notional(Price::from_int(60000), Qty::from_int(2)) == Notional::from_int(1200));
  CHECK(in.pnl_per_tick() == Notional::from_decimal("0.001").value());
  CHECK(venue.inst_id_code(kBtc) == 10459);
  const auto q = h.srv.frames("instruments");
  REQUIRE(q.size() == 1);
  CHECK(q[0] == "instType=SWAP&instId=BTC-USDT-SWAP");
  // Demo trading: every request says so, public ones too (instIdCode differs on demo).
  for (const std::string& v : h.srv.frames("sim_header")) CHECK(v == "1");
  // With keys the account mode is read, signed.
  CHECK(h.rest() == std::vector<std::string>{"config"});
  CHECK(h.signed_ok.load() == 1);
  CHECK(h.signed_bad.load() == 0);
  CHECK_FALSE(venue.refused_account_settings());

  // An inverse swap is refused.
  Harness h2;
  h2.instruments = fastmm::test::fixture("okx/instruments_btc_usd_swap.json");
  const std::string usd = "\"instId\":\"BTC-USD-SWAP\"";
  h2.instruments.replace(h2.instruments.find(usd), usd.size(), "\"instId\":\"BTC-USDT-SWAP\"");
  InstrumentTable i2;
  REQUIRE(i2.add(configured_swap()));
  OkxVenue v2(kVenue, make_okx_config(h2.section(), false));
  const auto r2 = v2.load_reference_data(i2);
  REQUIRE_FALSE(r2);
  CHECK(r2.error().find("inverse") != std::string::npos);
}

TEST_CASE("okx.venue: long/short position mode and the spot account mode are refused") {
  Harness h;
  h.account_config = ok_data(R"({"acctLv":"2","posMode":"long_short_mode","uid":"77"})");
  InstrumentTable instruments;
  REQUIRE(instruments.add(configured_swap()));
  OkxVenue venue(kVenue, make_okx_config(h.section(), false));
  auto r = venue.load_reference_data(instruments);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("long_short_mode") != std::string::npos);
  CHECK(venue.refused_account_settings());  // fastmm-live exits 3, not 4

  h.account_config = ok_data(R"({"acctLv":"1","posMode":"net_mode","uid":"77"})");
  OkxVenue spot(kVenue, make_okx_config(h.section(), false));
  InstrumentTable i2;
  REQUIRE(i2.add(configured_swap()));
  r = spot.load_reference_data(i2);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("spot mode") != std::string::npos);
  CHECK(spot.refused_account_settings());

  // A key the venue refuses is refused too; a venue that cannot be reached is not.
  h.account_config = R"({"code":"50113","msg":"Invalid Sign","data":[]})";
  OkxVenue bad_key(kVenue, make_okx_config(h.section(), false));
  InstrumentTable i3;
  REQUIRE(i3.add(configured_swap()));
  r = bad_key.load_reference_data(i3);
  REQUIRE_FALSE(r);
  CHECK(r.error().find("50113") != std::string::npos);
  CHECK(bad_key.refused_account_settings());
  h.account_config = "<html>502 Bad Gateway</html>";
  OkxVenue down(kVenue, make_okx_config(h.section(), false));
  InstrumentTable i4;
  REQUIRE(i4.add(configured_swap()));
  REQUIRE_FALSE(down.load_reference_data(i4));
  CHECK_FALSE(down.refused_account_settings());
}

TEST_CASE("okx.venue: the start-up sweep reports open orders and the position") {
  Harness h;
  h.open_orders = ok_data(
      R"({"instId":"BTC-USDT-SWAP","ordId":"311","clOrdId":"fm000000000007","px":"59990","sz":"2","side":"buy","state":"partially_filled","accFillSz":"0.5","ordType":"post_only"},{"instId":"ETH-USDT-SWAP","ordId":"999","clOrdId":"x","px":"3000","sz":"1","side":"buy","state":"live","accFillSz":"0"})");
  h.positions = ok_data(
      R"({"instId":"BTC-USDT-SWAP","posSide":"net","pos":"-3.5","avgPx":"60123.4","mgnMode":"cross","uTime":"1"})");
  h.hold_private_login = true;
  {
    // Queued before connect: it goes out as soon as the order connection is up, and the fake
    // server answers the private login only after that, so the sweep follows a sent order.
    Live l(h.section(), [](Live& live) {
      const OutNewOrderMsg n = new_order();
      REQUIRE(live.outbound.try_push(&n, n.hdr.len));
    });
    l.wait_for_sweep();
    REQUIRE(h.srv.frames("trade").size() == 1);
    const auto begins = l.reconcile(ReconcileMsg::Kind::Begin);
    REQUIRE(begins.size() == 1);
    // The sweep says nothing about this session's own orders: an empty watermark, although one
    // order had gone out (an order in flight must not be taken for cancelled).
    CHECK((begins[0]->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK_FALSE(begins[0]->sent_watermark.valid());
    CHECK((begins[0]->flags & ReconcileMsg::kExecutionsExact) != 0);
    const auto oo = l.reconcile(ReconcileMsg::Kind::OpenOrder);
    REQUIRE(oo.size() == 1);  // the ETH order is not on a subscribed instrument
    CHECK(oo[0]->cl_ord_id == decode_cl_ord_id("fm000000000007").value());
    CHECK(oo[0]->venue_order_id.view() == "311");
    CHECK(oo[0]->side == Side::Buy);
    CHECK(oo[0]->state == OrderState::PartiallyFilled);
    CHECK(oo[0]->price == Price::from_int(59990));
    CHECK(oo[0]->orig_qty == Qty::from_int(2));
    CHECK(oo[0]->cum_qty == Qty::from_decimal("0.5").value());
    const auto pos = l.reconcile(ReconcileMsg::Kind::Position);
    REQUIRE(pos.size() == 1);
    CHECK(pos[0]->hdr.instrument == kBtc);
    CHECK(pos[0]->position_qty == Qty::from_decimal("-3.5").value());  // contracts
    CHECK(pos[0]->avg_px == Price::from_decimal("60123.4").value());
    // The fills first, then the orders, then the positions; all signed.
    const auto rest = h.rest();
    REQUIRE(rest.size() >= 4);
    CHECK(rest[0] == "config");
    CHECK(rest[1].rfind("fills?instType=SWAP&begin=", 0) == 0);
    CHECK(rest[2] == "orders-pending?instType=SWAP&limit=100");
    CHECK(rest[3] == "positions?instType=SWAP");
    CHECK(h.signed_bad.load() == 0);
    CHECK(h.login_failures.load() == 0);
    const auto subs = h.srv.frames("private_subscribe");
    REQUIRE(subs.size() == 1);
    CHECK(subs[0].find(R"({"channel":"orders","instType":"SWAP"})") != std::string::npos);

    // A flat account lists nothing: the next reconciliation reports zero.
    {
      const std::lock_guard lock(h.mu);
      h.positions = ok_data("");
    }
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() == 2; }));
    CHECK(l.reconcile(ReconcileMsg::Kind::Position)[1]->position_qty.is_zero());
  }
}

TEST_CASE("okx.venue: order round trip over the WebSocket, then the blocking cancel-all") {
  Harness h;
  h.open_orders = ok_data(
      R"({"instId":"BTC-USDT-SWAP","ordId":"312","clOrdId":"fm000100000001","px":"60000.1","sz":"3","side":"sell","state":"live","accFillSz":"0"})");
  {
    Live l(h.section());
    l.wait_for_sweep();
    const OutNewOrderMsg n = new_order();
    l.push(n.hdr);
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderAck) >= 2 && l.oc.count(EventType::OrderFill) == 1;
    }));
    const auto frames = h.srv.frames("trade");
    REQUIRE(frames.size() == 1);
    CHECK(
        frames[0] ==
        R"({"id":"nfm000100000001","op":"order","args":[{"instIdCode":10459,"tdMode":"cross","clOrdId":"fm000100000001","side":"sell","ordType":"post_only","px":"60000.1","sz":"3"}]})");
    const auto* ack =
        l.oc.first_if<OrderAckMsg>(EventType::OrderAck, [](const OrderAckMsg&) { return true; });
    CHECK(ack->cl_ord_id == n.cl_ord_id);
    CHECK(ack->venue_order_id.view() == "312");
    const auto* f = l.oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(f->cl_ord_id == n.cl_ord_id);
    CHECK(f->exec_id.view() == "4463701411");
    CHECK(f->qty == Qty::from_int(1));
    CHECK(f->leaves_qty == Qty::from_int(2));
    CHECK(f->fee == Notional::from_decimal("-0.006").value());  // a maker rebate
    CHECK(f->fee_asset == FeeAsset::Quote);
    CHECK(f->liquidity == Liquidity::Maker);

    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kBtc, kVenue);
    c.cl_ord_id = n.cl_ord_id;
    c.venue_order_id.assign("312");
    l.push(c.hdr);
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderCancelAck) == 1;
    }));
    CHECK(l.oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck)->cum_qty == Qty::from_int(1));
    CHECK(
        h.srv.frames("trade")[1] ==
        R"({"id":"cfm000100000001","op":"cancel-order","args":[{"instIdCode":10459,"ordId":"312"}]})");

    CHECK(l.venue->cancel_all());
    const auto cb = h.srv.frames("cancel_batch");
    REQUIRE(cb.size() == 1);
    CHECK(cb[0] == R"([{"instId":"BTC-USDT-SWAP","ordId":"312"}])");
    CHECK(h.signed_bad.load() == 0);
    CHECK_FALSE(l.venue->fatal());
  }
}

// An orders push that ends an IOC (cancelSource 14) with an accFillSz ahead of the pushes that
// carry its fills: the end books the quantity, the fills name it and are not booked again.
TEST_CASE("okx.venue: an IOC reported ended before its fills is booked once") {
  Harness h;
  h.ioc_end_first = true;
  {
    Live l(h.section());
    l.wait_for_sweep();
    l.oc.all.clear();
    BookedPosition book;
    OutNewOrderMsg n = new_order();
    n.type = OrderType::Limit;
    n.tif = TimeInForce::Ioc;
    book.submit(n.cl_ord_id, kBtc, kVenue, n.side, n.price, n.qty);
    l.push(n.hdr);
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderExpired) == 1 && l.oc.count(EventType::OrderFill) == 2;
    }));
    CHECK(l.oc.last<OrderExpiredMsg>(EventType::OrderExpired)->cum_qty == Qty::from_int(2));
    book.drain(l.oc);
    CHECK(book.position == -Qty::from_int(2));
    CHECK(book.oms.stats().corrected_fills == 2);
    CHECK(book.oms.open_count() == 0);
  }
}

TEST_CASE("okx.venue: an amend is acknowledged from the orders channel under its reqId") {
  Harness h;
  {
    Live l(h.section());
    l.wait_for_sweep();
    const OutNewOrderMsg n = new_order();
    l.push(n.hdr);
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderFill) == 1;
    }));
    const std::size_t acks = l.oc.count(EventType::OrderAck);
    OutReplaceMsg r{};
    init_header(r, EventType::OutReplace, kBtc, kVenue);
    r.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
    r.orig_cl_ord_id = n.cl_ord_id;
    r.venue_order_id.assign("312");
    r.price = Price::from_decimal("60001").value();
    r.qty = Qty::from_int(4);
    l.push(r.hdr);
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderAck) == acks + 1;
    }));
    CHECK(
        h.srv.frames("trade")[1] ==
        R"({"id":"rfm000100000002","op":"amend-order","args":[{"instIdCode":10459,"ordId":"312","reqId":"fm000100000002","newSz":"4","newPx":"60001"}]})");
    const auto* ack = l.oc.last<OrderAckMsg>(EventType::OrderAck);
    CHECK(ack->cl_ord_id == r.cl_ord_id);
    CHECK(ack->flags == OrderAckMsg::kAmendedInPlace);
    // Later events for the venue's clOrdId (the original id) are the new id's.
    h.srv.send_to("/ws/v5/private",
                  order_push("fm000100000001", "canceled", "1", "0", "", "", "", "1"));
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderCancelAck) == 1;
    }));
    CHECK(l.oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck)->cl_ord_id == r.cl_ord_id);
  }
}

TEST_CASE(
    "okx.venue: a fill the private stream missed is booked from the fills before the snapshot") {
  Harness h;
  h.fills = ok_data(fill_row("5550001", "fm000100000001", kT));
  {
    Live l(h.section(), [](Live& live) { live.venue->resume_executions(kT - 1'000, {}); });
    l.wait_for_sweep();
    const auto fills = l.all<OrderFillMsg>(EventType::OrderFill);
    REQUIRE(fills.size() == 1);
    const OrderFillMsg& f = *fills[0];
    CHECK((f.flags & OrderFillMsg::kReplayed) != 0);
    CHECK(f.exec_id.view() == "5550001");
    CHECK(f.cl_ord_id == decode_cl_ord_id("fm000100000001").value());
    CHECK(f.side == Side::Sell);
    CHECK(f.qty == Qty::from_int(2));
    CHECK(f.fee == Notional::from_decimal("-0.012").value());
    CHECK(f.fee_asset == FeeAsset::Quote);
    CHECK(f.liquidity == Liquidity::Maker);
    CHECK(f.hdr.exch_ts == Timestamp{kT * 1'000'000});  // fillTime
    // Booked before the snapshot, whose Begin says the replay was complete.
    std::size_t fill_at = 0;
    std::size_t begin_at = 0;
    for (std::size_t i = 0; i < l.oc.all.size(); ++i) {
      const auto t = RecordingSink::type_of(l.oc.all[i]);
      if (t == EventType::OrderFill) fill_at = i;
      if (t == EventType::Reconcile && begin_at == 0) begin_at = i;
    }
    CHECK(fill_at < begin_at);
    CHECK((l.reconcile(ReconcileMsg::Kind::Begin)[0]->flags & ReconcileMsg::kExecutionsExact) != 0);
    // A start 13 days back reads fills-history (fills covers 3 days), from one ms before it.
    const auto q = h.srv.frames("fills");
    REQUIRE(q.size() == 1);
    CHECK(q[0] == "instType=SWAP&begin=" + std::to_string(kT - 1'001) + "&limit=100");
    CHECK(h.rest()[1].rfind("fills-history?", 0) == 0);
    // The next reconciliation replays from the newest fill: nothing is forwarded twice.
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() == 2; }));
    CHECK(l.all<OrderFillMsg>(EventType::OrderFill).size() == 1);
    CHECK(h.srv.frames("fills")[1] ==
          "instType=SWAP&begin=" + std::to_string(kT + 6) + "&limit=100");
  }
}

TEST_CASE("okx.venue: funding from the bills is booked once") {
  Harness h;
  // Paid before this session connected, within its replay: the start-up sweep books it. A bill
  // on an instrument not traded here is not forwarded.
  h.bills = ok_data(bill_row("BTC-USDT-SWAP", "fund-1", "-0.25", kT) + "," +
                    bill_row("ETH-USDT-SWAP", "fund-eth", "-0.1", kT));
  {
    Live l(h.section(), [](Live& live) { live.venue->resume_executions(kT - 1'000, {}); });
    l.wait_for_sweep();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Funding) >= 1;
    }));
    auto got = l.all<FundingMsg>(EventType::Funding);
    REQUIRE(got.size() == 1);
    CHECK(got[0]->hdr.instrument == kBtc);
    CHECK(got[0]->amount == Notional::from_decimal("-0.25").value());  // paid
    CHECK(got[0]->asset.view() == "USDT");
    CHECK(got[0]->funding_id.view() == "fund-1");
    CHECK(got[0]->hdr.exch_ts == Timestamp{kT * 1'000'000});
    CHECK((got[0]->flags & FundingMsg::kReplayed) != 0);
    CHECK(l.oc.count(EventType::OrderFill) == 0);
    const auto b = h.srv.frames("bills");
    REQUIRE_FALSE(b.empty());
    // 13 days back: the archive (bills covers 7 days), funding fee only.
    CHECK(b[0] == "/api/v5/account/bills-archive?instType=SWAP&type=8&begin=" +
                      std::to_string(kT - 1'001) + "&limit=100");

    // A balance_and_position push says funding was paid: the bills are read a second later, and
    // the new payment is booked; the old one is not forwarded again.
    {
      const std::lock_guard lock(h.mu);
      h.bills = ok_data(bill_row("BTC-USDT-SWAP", "fund-2", "0.5", kT + 28'800'000) + "," +
                        bill_row("BTC-USDT-SWAP", "fund-1", "-0.25", kT));
    }
    const std::size_t queries = h.srv.frames("bills").size();
    h.srv.send_to(
        "/ws/v5/private",
        R"({"arg":{"channel":"balance_and_position","uid":"77"},"data":[{"pTime":"1","eventType":"funding_fee","balData":[],"posData":[],"trades":[]}]})");
    l.pump();
    l.venue->on_timer(net::Reactor::now_ns() + 2'000'000'000);
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Funding) == 2;
    }));
    CHECK(h.srv.frames("bills").size() == queries + 1);
    got = l.all<FundingMsg>(EventType::Funding);
    CHECK(got[1]->funding_id.view() == "fund-2");
    CHECK(got[1]->amount == Notional::from_decimal("0.5").value());
    // A reconciliation reads them again: nothing new.
    l.venue->request_open_orders();
    REQUIRE(pump_until(l.reactor, [&] { return l.ends() == 2; }));
    l.pump();
    CHECK(l.all<FundingMsg>(EventType::Funding).size() == 2);
  }
  // A restart skips the payments its store holds.
  Harness h2;
  h2.bills = ok_data(bill_row("BTC-USDT-SWAP", "fund-1", "-0.25", kT) + "," +
                     bill_row("BTC-USDT-SWAP", "fund-3", "-0.7", kT + 5));
  {
    Live l(h2.section(), [](Live& live) {
      live.venue->resume_executions(kT - 1'000, {std::string(kFundingIdPrefix) + "fund-1"});
    });
    l.wait_for_sweep();
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::Funding) >= 1;
    }));
    l.pump();
    const auto got = l.all<FundingMsg>(EventType::Funding);
    REQUIRE(got.size() == 1);
    CHECK(got[0]->funding_id.view() == "fund-3");
  }
}

TEST_CASE("okx.venue: cancel-all-after is armed, refreshed and stopped") {
  Harness h;
  VenueSection s = h.section();
  s.extra["dead_mans_switch_s"] = "30";
  {
    Live l(s);
    // Armed from the housekeeping timer once connected.
    REQUIRE(pump_until(l.reactor, [&] { return h.srv.frames("cancel_after").size() == 1; }));
    CHECK(h.srv.frames("cancel_after")[0] == R"({"timeOut":"30"})");
    const std::int64_t armed = net::Reactor::now_ns();
    l.pump();
    // Not again before a third of the window (unless this machine is so slow that the
    // housekeeping timer got there by itself)...
    l.venue->on_timer(net::Reactor::now_ns() + 5'000'000'000);
    l.pump();
    if (net::Reactor::now_ns() - armed < 5'000'000'000)
      CHECK(h.srv.frames("cancel_after").size() == 1);
    // ...then refreshed.
    l.venue->on_timer(net::Reactor::now_ns() + 11'000'000'000);
    REQUIRE(pump_until(l.reactor, [&] { return h.srv.frames("cancel_after").size() >= 2; }));
    CHECK(h.srv.frames("cancel_after")[1] == R"({"timeOut":"30"})");
    CHECK(h.signed_bad.load() == 0);
    CHECK_FALSE(l.venue->fatal());
    // A clean shutdown stops the countdown, before disconnect() returns.
    l.venue->disconnect();
    CHECK(h.srv.frames("cancel_after").back() == R"({"timeOut":"0"})");
  }
  const auto ca = h.srv.frames("cancel_after");
  REQUIRE(ca.size() >= 3);
  CHECK(ca.back() == R"({"timeOut":"0"})");

  // A countdown the venue stops confirming lapses: the venue has cancelled everything, and the
  // connector kills the venue rather than quote again.
  Harness h2;
  VenueSection s2 = h2.section();
  s2.extra["dead_mans_switch_s"] = "10";
  {
    Live l(s2);
    REQUIRE(pump_until(l.reactor, [&] { return h2.srv.frames("cancel_after").size() == 1; }));
    l.pump();
    {
      const std::lock_guard lock(h2.mu);
      h2.cancel_after_reply =
          R"({"code":"50001","msg":"Service temporarily unavailable","data":[]})";
    }
    l.venue->on_timer(net::Reactor::now_ns() + 4'000'000'000);
    REQUIRE(pump_until(l.reactor, [&] { return h2.srv.frames("cancel_after").size() >= 2; }));
    l.pump();
    CHECK_FALSE(l.venue->fatal());
    l.venue->on_timer(net::Reactor::now_ns() + 11'000'000'000);
    CHECK(l.venue->fatal());
    l.oc.take(l.orders);
    const ControlMsg* kill = l.oc.last<ControlMsg>(EventType::Control);
    REQUIRE(kill != nullptr);
    CHECK(kill->command == ControlCommand::TripVenueKill);
    CHECK(kill->arg == static_cast<std::uint64_t>(KillReason::DeadMansSwitchLost));
  }
}

TEST_CASE("okx.venue: a failed fills query is retried from the timer") {
  Harness h;
  h.fills_failures = 1;
  h.fills = ok_data(fill_row("5550002", "fm000100000001", kT));
  {
    Live l(h.section(), [](Live& live) { live.venue->resume_executions(kT - 1'000, {}); });
    l.wait_for_sweep();
    const std::int64_t swept = net::Reactor::now_ns();
    // The sweep went ahead without the replay and says so.
    CHECK((l.reconcile(ReconcileMsg::Kind::Begin)[0]->flags & ReconcileMsg::kExecutionsExact) == 0);
    CHECK(l.oc.count(EventType::OrderFill) == 0);
    // Not before the retry interval (5 s after the failure; the housekeeping timer would retry
    // by itself once that much real time has passed, as it may on a loaded machine)...
    l.venue->on_timer(net::Reactor::now_ns() + 1'000'000'000);
    l.pump();
    if (net::Reactor::now_ns() - swept < 3'000'000'000) CHECK(h.srv.frames("fills").size() == 1);
    // ...then from the same watermark, and the fill is booked.
    l.venue->on_timer(net::Reactor::now_ns() + 6'000'000'000);
    REQUIRE(pump_until(l.reactor, [&] {
      l.oc.take(l.orders);
      return l.oc.count(EventType::OrderFill) == 1;
    }));
    const auto q = h.srv.frames("fills");
    REQUIRE(q.size() >= 2);
    CHECK(q[1] == q[0]);  // the watermark did not move past the failure
    CHECK(l.all<OrderFillMsg>(EventType::OrderFill)[0]->exec_id.view() == "5550002");
    CHECK(l.ends() == 1);  // a retry replays fills only
  }
}

TEST_CASE("okx.venue: ControlCommand::Reconcile from the engine triggers the snapshot") {
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
    REQUIRE(rest.size() >= pending + 3);
    CHECK(rest[pending].rfind("fills?", 0) == 0);
    CHECK(rest[pending + 1].rfind("orders-pending?", 0) == 0);
    // An engine-requested snapshot carries the sent watermark (nothing sent yet: empty).
    CHECK((l.reconcile(ReconcileMsg::Kind::Begin)[1]->flags & ReconcileMsg::kSentWatermark) != 0);
  }
}

TEST_CASE("okx.venue: a seqId gap resubscribes the depth channel for a new snapshot") {
  Harness h;
  {
    Live l(h.section(false));
    REQUIRE(pump_until(l.reactor, [&] {
      return l.venue->md_feed() != nullptr && l.venue->md_feed()->synced_count() == 1;
    }));
    // 100 -> 105 is a gap (prevSeqId 104).
    h.srv.send_to(
        "/ws/v5/public",
        R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":[{"asks":[],"bids":[["60000","1","0","1"]],"ts":"1789299703100","checksum":0,"prevSeqId":104,"seqId":105}]})");
    REQUIRE(pump_until(l.reactor, [&] {
      for (const std::string& f : h.srv.frames("md")) {
        if (f.find(R"("op":"unsubscribe")") != std::string::npos) return true;
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
  }
}

TEST_CASE("okx.venue: a tbt depth channel logs the market-data connection in first") {
  Harness h;
  VenueSection s = h.section();
  s.extra["depth_channel"] = "books50-l2-tbt";
  {
    Live l(s);
    REQUIRE(pump_until(l.reactor, [&] { return h.srv.frames("md").size() >= 2; }));
    const auto f = h.srv.frames("md");
    CHECK(json_str(f[0], "op") == "login");
    CHECK(login_ok(f[0]));
    CHECK(json_str(f[1], "op") == "subscribe");
    CHECK(f[1].find(R"({"channel":"books50-l2-tbt","instId":"BTC-USDT-SWAP"})") !=
          std::string::npos);
  }
  // Without it (books), no login on the public connection.
  Harness h2;
  {
    Live l(h2.section());
    REQUIRE(pump_until(l.reactor, [&] { return !h2.srv.frames("md").empty(); }));
    CHECK(json_str(h2.srv.frames("md")[0], "op") == "subscribe");
  }
}

TEST_CASE("okx.venue: the positions channel corrects the engine only when it differs") {
  Harness h;
  h.positions =
      ok_data(R"({"instId":"BTC-USDT-SWAP","posSide":"net","pos":"-3","avgPx":"60000.1"})");
  {
    Live l(h.section());
    l.wait_for_sweep();  // tracked = venue = -3
    const std::size_t before = l.oc.count(EventType::PositionUpdate);
    auto position = [](const char* pos) {
      return std::string(
                 R"({"arg":{"channel":"positions","instType":"SWAP","uid":"77"},"eventType":"event_update","data":[{"instId":"BTC-USDT-SWAP","posSide":"net","pos":")") +
             pos + R"(","avgPx":"60000.1","uTime":"1"}]})";
    };
    h.srv.send_to("/ws/v5/private", position("-3"));
    l.pump();
    l.venue->on_timer(net::Reactor::now_ns() + 2'000'000'000);
    l.oc.take(l.orders);
    CHECK(l.oc.count(EventType::PositionUpdate) == before);
    // A liquidation or another client moved it: the engine takes the venue's.
    h.srv.send_to("/ws/v5/private", position("1.5"));
    l.pump();
    l.venue->on_timer(net::Reactor::now_ns() + 2'000'000'000);
    l.oc.take(l.orders);
    REQUIRE(l.oc.count(EventType::PositionUpdate) == before + 1);
    CHECK(l.oc.last<PositionUpdateMsg>(EventType::PositionUpdate)->qty ==
          Qty::from_decimal("1.5").value());
    // A long/short-mode position during the session stops new orders.
    h.srv.send_to(
        "/ws/v5/private",
        R"({"arg":{"channel":"positions","instType":"SWAP","uid":"77"},"eventType":"event_update","data":[{"instId":"BTC-USDT-SWAP","posSide":"long","pos":"1","avgPx":"60000.1","uTime":"1"}]})");
    REQUIRE(pump_until(l.reactor, [&] { return l.venue->fatal(); }));
  }
}
