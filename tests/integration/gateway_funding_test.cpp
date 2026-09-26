// fastmm-gateway on a Bybit linear perpetual (a scripted fake Bybit v5 in the test process): a
// funding payment on the private stream reaches the strategy that owns the instrument and the
// gateway's account, each books it once however often it is delivered, and the strategy's store
// holds one row for it. The gateway and the strategy are real child processes.
#include "../venues/fake_venue_util.hpp"
#include "process_util.hpp"

#include "fastmm/core/journal.hpp"
#include "fastmm/core/status_segment.hpp"
#include "fastmm/live/session.hpp"

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::integration;
using namespace fastmm::venues::test;

#if defined(FASTMM_LIVE_EXE) && defined(FASTMM_GATEWAY_EXE)

namespace {

constexpr const char* kKey = "fake-key";
constexpr const char* kSecret = "fake-secret";

std::string order_event(const std::string& link, const char* status) {
  return R"({"id":"o","topic":"order","creationTime":1789299700474,"data":[{"category":"linear","symbol":"BTCUSDT","orderId":"oid-)" +
         link + R"(","orderLinkId":")" + link +
         R"(","side":"Buy","positionIdx":0,"orderType":"Limit","price":"59000","qty":"0.001","timeInForce":"PostOnly","orderStatus":")" +
         status +
         R"(","leavesQty":"0.001","cumExecQty":"0","reduceOnly":false,"createdTime":"1789299700444","updatedTime":"1789299700457","rejectReason":"EC_NoError"}]})";
}

// Bybit's funding execution: execType Funding, execFee positive when the account paid.
std::string funding_frame(const char* id, const char* fee) {
  return std::string(
             R"({"topic":"execution","id":"f","creationTime":1789315200010,"data":[{"category":"linear","symbol":"BTCUSDT","execFee":")") +
         fee + R"(","execId":")" + id +
         R"(","execPrice":"60000","execQty":"0.2","execType":"Funding","orderId":"f-order","orderLinkId":"","orderType":"UNKNOWN","side":"Buy","execTime":"1789315200000","isMaker":false,"feeCurrency":""}]})";
}

constexpr const char* kEmptyList =
    R"({"retCode":0,"retMsg":"OK","result":{"list":[],"nextPageCursor":"","category":"linear"},"retExtInfo":{},"time":1789299704000})";

// A Bybit linear account with no position and nothing open, whose streams stay up.
struct FakeBybit {
  FakeVenueServer srv;
  std::string info = fastmm::test::fixture("bybit/linear_instruments_info.json");
  std::string time = fastmm::test::fixture("bybit/server_time.json");
  std::string mode = fastmm::test::fixture("bybit/linear_position_list.json");
  std::string snapshot = fastmm::test::fixture("bybit/orderbook50_snapshot.json");
  std::mutex mu;
  net::WsSession* private_session = nullptr;  // server thread only

  static std::string pong(std::string_view t) {
    return R"({"success":true,"ret_msg":"pong","conn_id":"c","req_id":")" + json_str(t, "req_id") +
           R"(","op":"pong"})";
  }

  FakeBybit() {
    const auto json = [](const std::string& body) {
      return [body](const net::HttpRequest&) { return net::HttpServerResponse::json(200, body); };
    };
    srv.route("GET", "/v5/market/time", json(time));
    srv.route("GET", "/v5/market/instruments-info", json(info));
    srv.route("GET", "/v5/position/list", [this](const net::HttpRequest& r) {
      srv.record("positions", std::string(r.query));
      const bool by_symbol = r.query.find("symbol=") != std::string_view::npos;
      return net::HttpServerResponse::json(200, by_symbol ? mode : kEmptyList);
    });
    srv.route("GET", "/v5/order/realtime", json(kEmptyList));
    srv.route("GET", "/v5/execution/list", [this](const net::HttpRequest& r) {
      srv.record("executions", std::string(r.query));
      return net::HttpServerResponse::json(200, kEmptyList);
    });
    srv.route("POST", "/v5/order/cancel-all", [](const net::HttpRequest&) {
      return net::HttpServerResponse::json(
          200,
          R"({"retCode":0,"retMsg":"OK","result":{"list":[],"success":"1"},"retExtInfo":{},"time":1789299704000})");
    });
    srv.on_ws_text("/v5/public/linear", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      if (op == "ping") {
        s.send_text(pong(t));
      } else if (op == "subscribe") {
        s.send_text(R"({"success":true,"ret_msg":"subscribe","conn_id":"c1","req_id":")" +
                    json_str(t, "req_id") + R"(","op":"subscribe"})");
        if (t.find("orderbook.50.BTCUSDT") != std::string_view::npos) s.send_text(snapshot);
      }
    });
    srv.on_ws_text("/v5/private", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      if (op == "ping") {
        s.send_text(pong(t));
      } else if (op == "auth") {
        s.send_text(R"({"success":true,"ret_msg":"","op":"auth","conn_id":"p1"})");
      } else if (op == "subscribe") {
        private_session = &s;
        srv.record("private_subscribe", std::string(t));
        s.send_text(R"({"success":true,"ret_msg":"","op":"subscribe","conn_id":"p1","req_id":")" +
                    json_str(t, "req_id") + R"("})");
      }
    });
    srv.on_ws_text("/v5/trade", [this](net::WsSession& s, std::string_view t) {
      const std::string op = json_str(t, "op");
      if (op == "ping") {
        s.send_text(R"({"op":"pong","retCode":0,"retMsg":"OK"})");
        return;
      }
      if (op == "auth") {
        s.send_text(R"({"retCode":0,"retMsg":"OK","op":"auth","connId":"t1"})");
        return;
      }
      const std::string link = json_str(t, "orderLinkId");
      s.send_text(R"({"reqId":")" + json_str(t, "reqId") + R"(","retCode":0,"retMsg":"OK","op":")" +
                  op + R"(","data":{"orderId":"oid-)" + link + R"(","orderLinkId":")" + link +
                  R"("},"retExtInfo":{},"header":{},"connId":"t1"})");
      if (private_session == nullptr) return;
      if (op == "order.create") private_session->send_text(order_event(link, "New"));
      if (op == "order.cancel") private_session->send_text(order_event(link, "Cancelled"));
    });
    srv.start();
  }
};

// One program's files: its configuration, log, status, and journal directory (store inside).
struct Files {
  std::string config;
  std::string journal_dir;
  std::string status;
  std::string log;
};

Files write_files(const FakeBybit& b, const std::string& stem) {
  Files f;
  f.config = tmp_path(stem + ".toml");
  f.journal_dir = tmp_path(stem + "-runs");
  f.status = tmp_path(stem + ".status");
  f.log = f.config + ".log";
  remove_all_of({f.config, f.journal_dir, f.status, f.log, f.config + ".gw", f.config + ".gw.log"});
  std::ofstream out(f.config, std::ios::trunc);
  REQUIRE(out.good());
  out << "[engine]\nname = \"" << stem << "\"\ncpu = -1\nspin_mode = \"adaptive\"\n"
      << "journal = true\njournal_dir = \"" << f.journal_dir << "\"\nepoch_file = \""
      << f.journal_dir << "/epoch\"\nkill_file = \"" << f.journal_dir << "/kill\"\n"
      << "post_only = true\nsupports_replace = false\n\n"
      << "[venues.bybit_linear]\nkind = \"bybit\"\ncategory = \"linear\"\n"
      << "ws_url = \"" << b.srv.ws_base() << "/v5/public/linear\"\n"
      << "ws_api_url = \"" << b.srv.ws_base() << "/v5/trade\"\n"
      << "ws_private_url = \"" << b.srv.ws_base() << "/v5/private\"\n"
      << "rest_url = \"" << b.srv.http_base() << "\"\n"
      << "api_key = \"" << kKey << "\"\napi_secret = \"" << kSecret << "\"\n"
      << "supports_replace = false\n\n"
      << "[[instruments]]\nvenue = \"bybit_linear\"\nsymbol = \"BTCUSDT\"\nbase = \"BTC\"\n"
      << "quote = \"USDT\"\nasset_class = \"perpetual\"\ntick = \"0.10\"\nlot = \"0.001\"\n"
      << "min_qty = \"0.001\"\nmax_qty = \"100\"\nmin_notional = \"5\"\nenabled = true\n\n"
      << "[strategy]\nname = \"basic_mm\"\n\n[strategy.params]\nhalf_spread_bps = 50.0\n"
      << "skew_bps_per_unit = 0.5\nquote_qty = 0.001\nmax_inventory = 0.003\n"
      << "requote_threshold_ticks = 30\npull_on_stale_ms = 60000\nlevels = 1\n\n"
      << "[risk]\nmax_order_qty = \"0.002\"\nmax_order_notional = \"300\"\n"
      << "max_position = \"0.003\"\nmax_open_orders = 4\nprice_collar_bps = 500\n"
      << "fat_finger_bps = 2000\nstale_md_ms = 60000\nmax_loss = \"1000\"\n"
      << "orders_per_sec = 5\nburst = 5\n\n[logging]\nlevel = \"info\"\n";
  out.close();
  return f;
}

std::optional<StatusSnapshot> read_status(const std::string& path) {
  StatusReader r;
  std::string err;
  if (!r.open(path, &err)) return std::nullopt;
  StatusSnapshot s;
  if (!r.read(s)) return std::nullopt;
  return s;
}

// The Funding events the strategy's journals hold, by venue id.
std::vector<std::string> journal_funding(const Files& f) {
  std::vector<std::string> ids;
  for (const auto& e : std::filesystem::directory_iterator(f.journal_dir)) {
    if (e.path().extension() != ".fmj") continue;
    JournalReader r;
    REQUIRE(r.open(e.path().string()).has_value());
    r.for_each([&](const EventHeader* h) {
      if (h->type == EventType::Funding)
        ids.emplace_back(msg_cast<FundingMsg>(h).funding_id.view());
    });
  }
  return ids;
}

}  // namespace

TEST_CASE("gateway funding: a payment reaches the owner and the account, each books it once") {
  FakeBybit bybit;
  const Files gw = write_files(bybit, "gw-funding-gw");
  const Files st = write_files(bybit, "gw-funding-a");
  const std::string socket = gw.config + ".gw";
  const std::string gw_log = gw.config + ".gw.log";
  const pid_t gateway = spawn_process(FASTMM_GATEWAY_EXE,
                                      {"--config",
                                       gw.config,
                                       "--socket",
                                       socket,
                                       "--log",
                                       gw_log,
                                       "--status",
                                       gw.status,
                                       "--duration",
                                       "120s"});
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        return std::filesystem::exists(socket) &&
                               !bybit.srv.frames("private_subscribe").empty();
                      },
                      20000),
                  "the gateway did not come up: " << fastmm::test::read_file(gw_log));
  const pid_t strategy = spawn_process(FASTMM_LIVE_EXE,
                                       {"--config",
                                        st.config,
                                        "--duration",
                                        "120s",
                                        "--status",
                                        st.status,
                                        "--log",
                                        st.log,
                                        "--gateway",
                                        socket,
                                        "--no-control"});
  // Attached: the gateway routes the instrument to it from here on.
  REQUIRE_MESSAGE(
      wait_until(
          [&] { return fastmm::test::read_file(gw_log).find(" attached: ") != std::string::npos; },
          20000),
      "the strategy did not attach: " << fastmm::test::read_file(st.log));

  // Funding paid (0.5 USDT), delivered twice: a stream that repeats itself, or the stream and a
  // replay of execution/list.
  bybit.srv.send_to("/v5/private", funding_frame("fund-1", "0.5"));
  bybit.srv.send_to("/v5/private", funding_frame("fund-1", "0.5"));
  const std::int64_t paid = Notional::from_decimal("-0.5").value().raw;
  // The account: its realized PnL on this venue is the payment, once.
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        const auto s = read_status(gw.status);
                        return s && s->gateway.venues[0].realized_raw == paid;
                      },
                      20000),
                  "the account did not book it: " << fastmm::test::read_file(gw_log));
  // The owner: its realized PnL too (published with its status about once a second).
  REQUIRE_MESSAGE(wait_until(
                      [&] {
                        const auto s = read_status(st.status);
                        return s && s->realized_pnl_raw == paid;
                      },
                      20000),
                  "the strategy did not book it: " << fastmm::test::read_file(st.log));
  // A second payment, after the first was booked everywhere.
  bybit.srv.send_to("/v5/private", funding_frame("fund-2", "-0.2"));
  const std::int64_t both = Notional::from_decimal("-0.3").value().raw;
  REQUIRE(wait_until(
      [&] {
        const auto a = read_status(gw.status);
        const auto s = read_status(st.status);
        return a && s && a->gateway.venues[0].realized_raw == both && s->realized_pnl_raw == both;
      },
      20000));

  REQUIRE(::kill(strategy, SIGTERM) == 0);
  CHECK(reap(strategy) == live::kExitOk);
  REQUIRE(::kill(gateway, SIGTERM) == 0);
  CHECK(reap(gateway) == live::kExitOk);

  // Both deliveries reached the owner; its engine booked one.
  std::vector<std::string> ids = journal_funding(st);
  std::sort(ids.begin(), ids.end());
  CHECK(ids == std::vector<std::string>{"fund-1", "fund-1", "fund-2"});
  store::register_builtin_backends();
  auto reader = store::StoreRegistry::instance().make_reader("sqlite");
  REQUIRE(reader != nullptr);
  store::BackendOptions opts;
  opts.engine_name = "gw-funding-a";
  opts.default_dir = st.journal_dir;
  opts.read_only = true;
  REQUIRE(reader->open(opts).has_value());
  auto rows = reader->funding(store::QueryFilter{});
  REQUIRE(rows.has_value());
  REQUIRE(rows->rows.size() == 2);
  CHECK(rows->rows[0][2] == "-0.5");
  CHECK(rows->rows[1][2] == "0.2");
}

#endif  // FASTMM_LIVE_EXE && FASTMM_GATEWAY_EXE
