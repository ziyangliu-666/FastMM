// DeribitVenue against a scripted in-process fake Deribit (REST + two WebSocket connections):
// reference data, public subscribe with snapshot/change sync and heartbeat test_request, private
// public/auth + set_heartbeat + enable_cancel_on_disconnect + private/subscribe, private/buy ->
// ack, private/edit with the user.orders update arriving before the edit response, user.trades
// fill mapped to the edited order, private/cancel -> user.orders cancelled, a change_id gap ->
// public/unsubscribe + public/subscribe -> new snapshot, get_open_orders_by_currency
// reconciliation, private disconnect -> REST cancel_all_by_instrument + re-authentication +
// reconciliation, 13009 on an order -> re-authentication, the blocking kill switch, invalid
// credentials -> fatal, the local matching-engine credit limit, and the execution replay
// (get_user_trades_by_currency_and_time) before each open-order snapshot.
//
// The fake serves the private connection on its own path so the two sessions are easy to tell
// apart; the real venue uses one URL for both (DeribitVenueConfig::ws_private_url defaults to
// ws_url).
#include "fastmm/venues/deribit/deribit_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/registry.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::deribit;
using namespace fastmm::venues::test;

namespace {

constexpr const char* kClientId = "fake-client";
constexpr const char* kSecret = "fake-secret";
constexpr VenueId kVenue{3};
const std::string kMdPath = "/ws/api/v2";
const std::string kPrivatePath = "/ws/api/v2/private";
const InstrumentId kCall{0};
const InstrumentId kPerp{1};

std::string rpc_ok(std::string_view id_json, std::string_view result_json) {
  return std::string(R"({"jsonrpc":"2.0","id":)") + std::string(id_json) + R"(,"result":)" +
         std::string(result_json) + R"(,"usIn":1,"usOut":2,"usDiff":1,"testnet":true})";
}

// The raw JSON of "id": an integer or a quoted string.
std::string id_json(std::string_view t) {
  const std::size_t p = t.find("\"id\":");
  if (p == std::string_view::npos) return "null";
  std::size_t s = p + 5;
  std::size_t e = s;
  if (t[s] == '"') {
    e = t.find('"', s + 1) + 1;
  } else {
    while (e < t.size() && ((t[e] >= '0' && t[e] <= '9') || t[e] == '-')) ++e;
  }
  return std::string(t.substr(s, e - s));
}

std::string rpc_error(std::string_view id_json, int code, std::string_view message) {
  return std::string(R"({"jsonrpc":"2.0","id":)") + std::string(id_json) +
         R"(,"error":{"message":")" + std::string(message) + R"(","code":)" + std::to_string(code) +
         R"(},"usIn":1,"usOut":2,"usDiff":1,"testnet":true})";
}

// One row of private/get_user_trades_by_currency_and_time, for the option order the tests place.
std::string trade_row(std::string_view trade_id,
                      std::int64_t ts,
                      std::string_view price,
                      std::string_view amount,
                      std::string_view fee) {
  return std::string(R"({"trade_seq":1,"trade_id":")") + std::string(trade_id) +
         R"(","timestamp":)" + std::to_string(ts) +
         R"(,"tick_direction":0,"state":"filled","price":)" + std::string(price) +
         R"(,"order_type":"limit","order_id":"42710123456","mark_price":0.0061,"liquidity":"M",)"
         R"("label":"fm000100000001","instrument_name":"BTC-15SEP26-77000-C",)"
         R"("index_price":76950.1,"fee_currency":"BTC","fee":)" +
         std::string(fee) + R"(,"direction":"buy","amount":)" + std::string(amount) +
         R"(,"contracts":)" + std::string(amount) + "}";
}

std::string trades_page(const std::vector<std::string>& rows, bool has_more) {
  std::string out = R"({"trades":[)";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) out += ",";
    out += rows[i];
  }
  return out + R"(],"has_more":)" + (has_more ? "true" : "false") + "}";
}

std::string channels_json(std::string_view t) {
  const std::size_t p = t.find("\"channels\":[");
  if (p == std::string_view::npos) return "[]";
  const std::size_t s = p + 11;
  return std::string(t.substr(s, t.find(']', s) - s + 1));
}

std::string basic_auth(const char* id, const char* secret) {
  const std::string raw = std::string(id) + ":" + secret;
  std::string b64(net::base64_encoded_size(raw.size()), '\0');
  b64.resize(net::base64_encode(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()),
      b64));
  return "Basic " + b64;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
  for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
    s.replace(p, from.size(), to);
  return s;
}

struct Harness {
  FakeVenueServer srv;
  std::string options = fastmm::test::fixture("deribit/get_instruments_option.json");
  std::string futures = fastmm::test::fixture("deribit/get_instruments_future.json");
  std::string server_time = fastmm::test::fixture("deribit/get_time.json");
  std::string opt_snapshot = fastmm::test::fixture("deribit/book_option_snapshot.json");
  std::string opt_change = fastmm::test::fixture("deribit/book_option_change.json");
  std::string opt_ticker = fastmm::test::fixture("deribit/ticker_option.json");
  std::string perp_snapshot = fastmm::test::fixture("deribit/book_perp_snapshot.json");
  std::string perp_change_1 = fastmm::test::fixture("deribit/book_perp_change_1.json");
  std::string perp_change_2 = fastmm::test::fixture("deribit/book_perp_change_2.json");
  std::string test_request = fastmm::test::fixture("deribit/heartbeat_test_request.json");
  std::string auth_ok = fastmm::test::fixture("deribit/auth_ok.json");
  std::string invalid_credentials = fastmm::test::fixture("deribit/rpc_invalid_credentials.json");
  std::string buy_ok = fastmm::test::fixture("deribit/buy_ok.json");
  std::string edit_ok = fastmm::test::fixture("deribit/edit_ok.json");
  std::string cancel_ok = fastmm::test::fixture("deribit/cancel_ok.json");
  std::string orders_open = fastmm::test::fixture("deribit/user_orders_open.json");
  std::string orders_cancelled = fastmm::test::fixture("deribit/user_orders_cancelled.json");
  std::string trades = fastmm::test::fixture("deribit/user_trades.json");
  std::string open_orders = fastmm::test::fixture("deribit/open_orders.json");
  std::string unauthorized = fastmm::test::fixture("deribit/rpc_unauthorized.json");
  // private/get_account_summaries: the documented example, with the envelope's usIn/usOut.
  std::string summaries = fastmm::test::fixture("deribit/account_summaries.json");
  std::atomic<int> summaries_requests{0};
  std::atomic<bool> summaries_fail{false};  // answer get_account_summaries with an error

  std::atomic<int> auths{0};
  std::atomic<int> reauths{0};
  std::atomic<int> md_test_replies{0};
  std::atomic<int> private_test_replies{0};
  std::atomic<int> perp_snapshots{0};
  std::atomic<int> unsubscribes{0};
  std::atomic<int> open_orders_requests{0};
  std::atomic<int> cancel_all_ok{0};
  std::atomic<int> cancel_all_bad{0};
  std::atomic<int> buys{0};
  std::atomic<bool> unauthorized_next_buy{false};
  bool option_book_sent = false;  // server thread only

  // get_user_trades_by_currency_and_time: `trades_reply(start_timestamp, historical)` gives the
  // result object, or an empty string for an error reply; unset, the history is empty.
  struct TradesQuery {
    std::int64_t start = 0;
    std::int64_t end = 0;
    bool historical = false;
  };
  std::mutex mu;
  std::function<std::string(std::int64_t, bool)> trades_reply;
  std::vector<TradesQuery> trades_queries;
  std::string open_orders_reply;       // result array; empty: the open_orders fixture
  std::string positions_reply = "[]";  // get_positions result array
  std::atomic<int> positions_requests{0};
  std::atomic<int> open_orders_failures{0};  // get_open_orders_by_currency answers an error
  // get_user_trades_by_currency_and_time is not answered; its request ids are kept here.
  std::atomic<bool> hold_trades{false};
  std::vector<std::string> held_ids;

  std::vector<std::string> held() {
    const std::lock_guard lock(mu);
    return held_ids;
  }

  void set_trades(std::function<std::string(std::int64_t, bool)> f) {
    const std::lock_guard lock(mu);
    trades_reply = std::move(f);
  }
  std::vector<TradesQuery> queries() {
    const std::lock_guard lock(mu);
    return trades_queries;
  }

  // The server thread reads this harness's members: stop it before they go.
  ~Harness() { srv.stop(); }
  Harness() {
    srv.route("GET", "/api/v2/public/get_time", [this](const net::HttpRequest&) {
      return net::HttpServerResponse::json(200, server_time);
    });
    srv.route("GET", "/api/v2/public/get_instruments", [this](const net::HttpRequest& r) {
      srv.record("instruments", std::string(r.query));
      return net::HttpServerResponse::json(
          200, r.query.find("kind=option") != std::string_view::npos ? options : futures);
    });
    srv.route("GET", "/api/v2/private/cancel_all_by_instrument", [this](const net::HttpRequest& r) {
      const bool ok = r.header("Authorization") == basic_auth(kClientId, kSecret);
      srv.record("cancel_all", std::string(r.query));
      ++(ok ? cancel_all_ok : cancel_all_bad);
      return net::HttpServerResponse::json(ok ? 200 : 401,
                                           ok ? rpc_ok("1", "1") : invalid_credentials);
    });
    srv.on_ws_text(kMdPath, [this](net::WsSession& s, std::string_view t) {
      const std::string method = json_str(t, "method");
      const std::string id = id_json(t);
      if (method == "public/set_heartbeat") {
        s.send_text(rpc_ok(id, R"("ok")"));
        s.send_text(test_request);  // the client must answer with public/test
      } else if (method == "public/test") {
        ++md_test_replies;
        s.send_text(rpc_ok(id, R"({"version":"1.2.26"})"));
      } else if (method == "public/unsubscribe") {
        ++unsubscribes;
        s.send_text(rpc_ok(id, channels_json(t)));
      } else if (method == "public/subscribe") {
        s.send_text(rpc_ok(id, channels_json(t)));
        if (t.find("book.BTC-15SEP26-77000-C.100ms") != std::string_view::npos &&
            !option_book_sent) {
          option_book_sent = true;
          s.send_text(opt_snapshot);
          s.send_text(opt_change);
          s.send_text(opt_ticker);
        }
        if (t.find("book.BTC-PERPETUAL.100ms") != std::string_view::npos) {
          s.send_text(perp_snapshot);
          if (++perp_snapshots == 1) {
            s.send_text(perp_change_1);
            s.send_text(perp_change_2);
          }
        }
      }
    });
    srv.on_ws_text(kPrivatePath, [this](net::WsSession& s, std::string_view t) {
      const std::string method = json_str(t, "method");
      const std::string id = id_json(t);
      if (method == "public/auth") {
        const bool ok = t.find(R"("grant_type":"client_credentials")") != std::string_view::npos &&
                        json_str(t, "client_id") == kClientId &&
                        json_str(t, "client_secret") == kSecret;
        ++auths;
        if (id == "7") ++reauths;
        s.send_text(ok ? replace_all(auth_ok, R"("id":1,)", R"("id":)" + id + ",")
                       : replace_all(invalid_credentials, R"("id":7,)", R"("id":)" + id + ","));
        if (ok) s.send_text(test_request);
      } else if (method == "public/set_heartbeat" ||
                 method == "private/enable_cancel_on_disconnect") {
        s.send_text(rpc_ok(id, R"("ok")"));
      } else if (method == "public/test") {
        ++private_test_replies;
        s.send_text(rpc_ok(id, R"({"version":"1.2.26"})"));
      } else if (method == "private/subscribe") {
        s.send_text(rpc_ok(id, channels_json(t)));
      } else if (method == "private/buy") {
        ++buys;
        if (unauthorized_next_buy.exchange(false)) {
          s.send_text(replace_all(unauthorized, R"("id":6,)", R"("id":)" + id + ","));
          return;
        }
        s.send_text(buy_ok);
        s.send_text(orders_open);
      } else if (method == "private/edit") {
        // The user.orders update for the (original) label arrives before the edit response.
        s.send_text(replace_all(orders_open, R"("replaced":false)", R"("replaced":true)"));
        s.send_text(edit_ok);
        s.send_text(trades);
      } else if (method == "private/cancel") {
        s.send_text(cancel_ok);
        s.send_text(orders_cancelled);
      } else if (method == "private/get_open_orders_by_currency") {
        ++open_orders_requests;
        if (open_orders_failures.load() > 0) {
          --open_orders_failures;
          s.send_text(rpc_error(id, 10028, "too_many_requests"));
          return;
        }
        const std::lock_guard lock(mu);
        s.send_text(open_orders_reply.empty() ? open_orders : rpc_ok(id, open_orders_reply));
      } else if (method == "private/get_account_summaries") {
        ++summaries_requests;
        s.send_text(summaries_fail.load()
                        ? rpc_error(id, 10028, "too_many_requests")
                        : replace_all(summaries,
                                      R"("id":2515,)",
                                      R"("id":)" + id +
                                          R"(,"usIn":1790730000000100,"usOut":1790730000000450,)"));
      } else if (method == "private/get_positions") {
        ++positions_requests;
        const std::lock_guard lock(mu);
        s.send_text(rpc_ok(id, positions_reply));
      } else if (method == "private/get_user_trades_by_currency_and_time") {
        const TradesQuery q{std::stoll(json_int(t, "start_timestamp")),
                            std::stoll(json_int(t, "end_timestamp")),
                            t.find(R"("historical":true)") != std::string_view::npos};
        std::string result = R"({"trades":[],"has_more":false})";
        {
          const std::lock_guard lock(mu);
          trades_queries.push_back(q);
          if (hold_trades.load()) {
            held_ids.push_back(id);
            return;
          }
          if (trades_reply) result = trades_reply(q.start, q.historical);
        }
        s.send_text(result.empty() ? rpc_error(id, 10028, "too_many_requests")
                                   : rpc_ok(id, result));
      }
    });
    srv.start();
  }

  VenueSection section(const char* secret = kSecret) const {
    VenueSection s;
    s.name = "fake-deribit";
    s.kind = "deribit";
    s.ws_url = srv.ws_base() + kMdPath;
    s.rest_url = srv.http_base() + "/api/v2";
    s.api_key = kClientId;
    s.api_secret = secret;
    s.supports_replace = true;
    s.extra["currencies"] = "BTC";
    return s;
  }
};

InstrumentTable make_instruments() {
  InstrumentTable t;
  REQUIRE(t.add(make_instrument("BTC-15SEP26-77000-C", kVenue.value, "BTC", "BTC")));
  REQUIRE(t.add(make_instrument("BTC-PERPETUAL", kVenue.value, "BTC", "USD")));
  return t;
}

std::size_t count_state(const Collected& c, ConnState state) {
  std::size_t n = 0;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) == EventType::ConnectionState &&
        RecordingSink::as<ConnectionStateMsg>(m).state == state)
      ++n;
  }
  return n;
}

std::string find_frame(FakeVenueServer& srv, const std::string& path, std::string_view needle) {
  std::string out;
  for (const auto& f : srv.frames(path)) {
    if (f.find(needle) != std::string::npos) out = f;
  }
  return out;
}

}  // namespace

TEST_CASE("deribit.venue: scripted fake exchange end to end") {
  Harness h;
  InstrumentTable instruments = make_instruments();
  RecordingSink md(8U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;
  {
    DeribitVenueConfig cfg = make_deribit_config(h.section(), false);
    cfg.ws_private_url = h.srv.ws_base() + kPrivatePath;
    DeribitVenue venue(kVenue, cfg);
    REQUIRE(venue.load_reference_data(instruments));
    const Instrument& call = instruments.get(kCall);
    CHECK(call.asset_class == AssetClass::Option);
    CHECK(call.strike == Price::from_int(77000));
    CHECK(call.lot == Qty::from_decimal("0.1").value());
    CHECK(call.tick == Price::from_decimal("0.0001").value());
    CHECK(venue.tick_schedule(kCall).tick_for(Price::from_decimal("0.01").value()) ==
          Price::from_decimal("0.0005").value());
    CHECK(instruments.get(kPerp).contract_multiplier == Qty::from_int(10));
    CHECK(instruments.get(kPerp).asset_class == AssetClass::Perpetual);
    const auto queries = h.srv.frames("instruments");
    REQUIRE(queries.size() == 2);
    CHECK(queries[0] == "currency=BTC&kind=option&expired=false");
    CHECK(queries[1] == "currency=BTC&kind=future&expired=false");
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kCall, kPerp};
    venue.subscribe(ids);
    CHECK(venue.private_channels() == std::vector<std::string>{"user.orders.option.BTC.raw",
                                                               "user.trades.option.BTC.raw",
                                                               "user.orders.future.BTC.raw",
                                                               "user.trades.future.BTC.raw",
                                                               "user.portfolio.BTC"});
    venue.connect(reactor);

    Collected mdc;
    Collected oc;
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      oc.take(orders);
      return venue.md_feed()->synced_count() == 2 && mdc.count(EventType::OptionTicker) == 1 &&
             mdc.count(EventType::BookDelta) >= 3 && count_state(oc, ConnState::Live) >= 1 &&
             h.md_test_replies.load() >= 1 && h.private_test_replies.load() >= 1;
    }));
    CHECK(venue.authenticated());
    const std::string auth = find_frame(h.srv, kPrivatePath, "public/auth");
    CHECK(
        auth ==
        R"({"jsonrpc":"2.0","id":1,"method":"public/auth","params":{"grant_type":"client_credentials","client_id":"fake-client","client_secret":"fake-secret"}})");
    const std::string sub = find_frame(h.srv, kPrivatePath, "private/subscribe");
    CHECK(
        sub.find(
            R"("channels":["user.orders.option.BTC.raw","user.trades.option.BTC.raw","user.orders.future.BTC.raw","user.trades.future.BTC.raw","user.portfolio.BTC"])") !=
        std::string::npos);
    CHECK(sub.find(R"("access_token":"1789345400000.1MbQ-J_4.CBP-OqOwFakeAccessToken")") !=
          std::string::npos);
    CHECK_FALSE(find_frame(h.srv, kPrivatePath, "private/enable_cancel_on_disconnect").empty());
    CHECK(find_frame(h.srv, kMdPath, "public/set_heartbeat") ==
          R"({"jsonrpc":"2.0","id":3,"method":"public/set_heartbeat","params":{"interval":10}})");
    CHECK(mdc.count(EventType::BookSnapshot) == 2);
    CHECK(mdc.count(EventType::BookDelta) == 3);
    const auto* ot = mdc.last<OptionTickerMsg>(EventType::OptionTicker);
    REQUIRE(ot != nullptr);
    CHECK(ot->mark_iv == doctest::Approx(0.312));

    // New order: response ack plus the user.orders update (a duplicate the OMS ignores).
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kCall, kVenue);
    n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("0.0065").value();
    n.qty = Qty::from_int(1);
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 2;
    }));
    CHECK(oc.last<OrderAckMsg>(EventType::OrderAck)->cl_ord_id == n.cl_ord_id);
    CHECK(oc.last<OrderAckMsg>(EventType::OrderAck)->venue_order_id.view() == "42710123456");
    const std::string buy = find_frame(h.srv, kPrivatePath, "private/buy");
    CHECK(buy.find(R"("label":"fm000100000001")") != std::string::npos);
    CHECK(buy.find(R"("contracts":1,)") != std::string::npos);
    CHECK(buy.find(R"("reject_post_only":true)") != std::string::npos);

    // Edit: the early user.orders update for the old label is not acked; the response acks the
    // new id and the fill for the old label maps to it with shadow cum/leaves.
    const std::size_t acks_before_edit = oc.count(EventType::OrderAck);
    OutReplaceMsg rp{};
    init_header(rp, EventType::OutReplace, kCall, kVenue);
    rp.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
    rp.orig_cl_ord_id = n.cl_ord_id;
    rp.venue_order_id.assign("42710123456");
    rp.price = Price::from_decimal("0.006").value();
    rp.qty = Qty::from_int(2);
    REQUIRE(outbound.try_push(&rp, rp.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderFill) == 1;
    }));
    CHECK(oc.count(EventType::OrderAck) == acks_before_edit + 1);
    CHECK(oc.last<OrderAckMsg>(EventType::OrderAck)->cl_ord_id == rp.cl_ord_id);
    const auto* fill = oc.last<OrderFillMsg>(EventType::OrderFill);
    CHECK(fill->cl_ord_id == rp.cl_ord_id);
    CHECK(fill->qty == Qty::from_decimal("0.5").value());
    CHECK(fill->cum_qty == Qty::from_decimal("0.5").value());
    CHECK(fill->leaves_qty == Qty::from_decimal("1.5").value());
    CHECK(fill->liquidity == Liquidity::Maker);
    CHECK(find_frame(h.srv, kPrivatePath, "private/edit")
              .find(R"("order_id":"42710123456","contracts":2,"price":0.006)") !=
          std::string::npos);

    OutCancelMsg c{};
    init_header(c, EventType::OutCancel, kCall, kVenue);
    c.cl_ord_id = rp.cl_ord_id;
    c.venue_order_id.assign("42710123456");
    REQUIRE(outbound.try_push(&c, c.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderCancelAck) == 1;
    }));
    const auto* cx = oc.last<OrderCancelAckMsg>(EventType::OrderCancelAck);
    CHECK(cx->cl_ord_id == rp.cl_ord_id);
    CHECK(cx->cum_qty == Qty::from_decimal("0.5").value());

    // change_id gap on the perpetual book -> Resyncing, unsubscribe + subscribe, new snapshot.
    h.srv.send_to(kMdPath, fastmm::test::fixture("deribit/book_perp_change_gap.json"));
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      return h.perp_snapshots.load() == 2 && venue.md_feed()->synced_count() == 2 &&
             mdc.count(EventType::BookSnapshot) == 3;
    }));
    CHECK(venue.md_feed()->resync_count() == 1);
    CHECK(h.unsubscribes.load() == 1);
    CHECK(count_state(mdc, ConnState::Resyncing) == 1);

    // The start-up sweep ran when the private channel came up, with an empty watermark: it says
    // nothing about orders of this session.
    const auto is_begin = [](const ReconcileMsg& m) { return m.kind == ReconcileMsg::Kind::Begin; };
    const auto* sweep = oc.first_if<ReconcileMsg>(EventType::Reconcile, is_begin);
    REQUIRE(sweep != nullptr);
    CHECK(sweep->sent_watermark == ClientOrderId{});
    const auto open_orders_reported = [&] {
      std::size_t count = 0;
      for (const auto& m : oc.all) {
        if (RecordingSink::type_of(m) == EventType::Reconcile &&
            RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::OpenOrder)
          ++count;
      }
      return count;
    };
    // What a session that died left resting is reported before this session asks for anything.
    CHECK(open_orders_reported() == 2);
    const std::size_t before = oc.count(EventType::Reconcile);
    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      // Begin, option order, perpetual order, a position per instrument, End
      return oc.count(EventType::Reconcile) == before + 6;
    }));
    const auto* rec = oc.last<ReconcileMsg>(EventType::Reconcile);
    CHECK(rec->kind == ReconcileMsg::Kind::End);
    // Begin carries the last order id the venue sent before it asked for the snapshot.
    const auto* begin = oc.last_if<ReconcileMsg>(EventType::Reconcile, is_begin);
    REQUIRE(begin != nullptr);
    CHECK((begin->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK(begin->sent_watermark == rp.cl_ord_id);

    // Private channel drop: REST cancel_all_by_instrument per instrument, reconnect, auth again,
    // reconcile again.
    const int auths_before = h.auths.load();
    const std::size_t reconciles_before = oc.count(EventType::Reconcile);
    h.srv.close_sessions(kPrivatePath);
    REQUIRE(pump_until(
        reactor,
        [&] {
          oc.take(orders);
          return h.cancel_all_ok.load() >= 2 && h.auths.load() > auths_before &&
                 oc.count(EventType::Reconcile) == reconciles_before + 6 &&
                 count_state(oc, ConnState::Disconnected) >= 1;
        },
        15'000));
    CHECK(h.cancel_all_bad.load() == 0);
    const auto cancels = h.srv.frames("cancel_all");
    CHECK(cancels[0] == "instrument_name=BTC-15SEP26-77000-C");
    CHECK(cancels[1] == "instrument_name=BTC-PERPETUAL");
    REQUIRE(pump_until(reactor, [&] { return venue.authenticated(); }));

    // 13009 on an order: reject + re-authentication with the client credentials.
    h.unauthorized_next_buy = true;
    OutNewOrderMsg n2 = n;
    n2.cl_ord_id = decode_cl_ord_id("fm000100000003").value();
    REQUIRE(outbound.try_push(&n2, n2.hdr.len));
    venue.on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderReject) == 1 && h.reauths.load() == 1;
    }));
    const auto* rej = oc.last<OrderRejectMsg>(EventType::OrderReject);
    CHECK(rej->cl_ord_id == n2.cl_ord_id);
    CHECK(rej->venue_code == 13009);
    CHECK(rej->reason == RejectReason::VenueReject);

    CHECK(venue.cancel_all());
    CHECK(h.cancel_all_ok.load() >= 4);
    venue.on_timer(net::Reactor::now_ns());
    const VenueStatus st = venue.status();
    CHECK(st.books_synced == 2);
    CHECK(st.orders_sent == 2);
    CHECK(st.replaces_sent == 1);
    CHECK(st.cancels_sent == 1);
    CHECK(st.resyncs == 1);
    CHECK_FALSE(venue.fatal());
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}

TEST_CASE("deribit.venue: invalid credentials are fatal and the credit limit refuses orders") {
  Harness h;
  RecordingSink md(1U << 20);
  RecordingSink orders(1U << 20, SinkPolicy::Spin);
  MsgRing outbound(1U << 16);
  net::Reactor reactor;
  SymbolTable symbols;

  SUBCASE("invalid credentials") {
    InstrumentTable instruments = make_instruments();
    DeribitVenueConfig cfg = make_deribit_config(h.section("wrong-secret"), false);
    cfg.ws_private_url = h.srv.ws_base() + kPrivatePath;
    DeribitVenue venue(kVenue, cfg);
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kCall};
    venue.subscribe(ids);
    venue.connect(reactor);
    REQUIRE(pump_until(reactor, [&] { return venue.fatal(); }));
    CHECK_FALSE(venue.authenticated());
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kCall, kVenue);
    n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("0.0065").value();
    n.qty = Qty::from_int(1);
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue.on_wake();
    Collected oc;
    oc.take(orders);
    REQUIRE(oc.count(EventType::OrderReject) == 1);
    CHECK(oc.last<OrderRejectMsg>(EventType::OrderReject)->reason == RejectReason::VenueKilled);
    CHECK(h.buys.load() == 0);
    // The failed authentication asked the engine, once, to kill this venue only.
    REQUIRE(oc.count(EventType::Control) == 1);
    const ControlMsg* kill = oc.last<ControlMsg>(EventType::Control);
    CHECK(kill->command == ControlCommand::TripVenueKill);
    CHECK(kill->hdr.venue == kVenue);
    CHECK(static_cast<KillReason>(kill->arg) == KillReason::VenueFatal);
    venue.disconnect();
    reactor.run_once(0);
  }
  SUBCASE("matching-engine credits") {
    InstrumentTable instruments = make_instruments();
    VenueSection s = h.section();
    s.extra["matching_engine_rate"] = "1";
    s.extra["matching_engine_burst"] = "1";
    DeribitVenueConfig cfg = make_deribit_config(s, false);
    cfg.ws_private_url = h.srv.ws_base() + kPrivatePath;
    DeribitVenue venue(kVenue, cfg);
    REQUIRE(venue.load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue.attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kCall};
    venue.subscribe(ids);
    venue.connect(reactor);
    Collected oc;
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return count_state(oc, ConnState::Live) >= 1;
    }));
    for (const char* id : {"fm000100000001", "fm000100000002"}) {
      OutNewOrderMsg n{};
      init_header(n, EventType::OutNewOrder, kCall, kVenue);
      n.cl_ord_id = decode_cl_ord_id(id).value();
      n.type = OrderType::PostOnly;
      n.price = Price::from_decimal("0.0065").value();
      n.qty = Qty::from_int(1);
      REQUIRE(outbound.try_push(&n, n.hdr.len));
    }
    venue.on_wake();
    oc.take(orders);
    REQUIRE(oc.count(EventType::OrderReject) == 1);
    const auto* rej = oc.last<OrderRejectMsg>(EventType::OrderReject);
    CHECK(rej->reason == RejectReason::VenueRateLimit);
    CHECK(rej->cl_ord_id == decode_cl_ord_id("fm000100000002").value());
    REQUIRE(pump_until(reactor, [&] { return h.buys.load() == 1; }));
    venue.disconnect();
    reactor.run_once(0);
  }
  h.srv.stop();
}

namespace {

// The engine's side of the order events, reduced to what the replay tests measure: the OMS and
// the position of the option (Engine::on_fill and after_oms_update, without synthetic fills).
struct Mirror {
  explicit Mirror(const InstrumentTable& i) : instruments(i) {}
  const InstrumentTable& instruments;
  Oms oms{1};
  PositionTracker positions;
  std::size_t cursor = 0;
  std::uint64_t exact = 0;

  void drain(const Collected& c) {
    for (; cursor < c.all.size(); ++cursor) {
      const auto& raw = c.all[cursor];
      switch (RecordingSink::type_of(raw)) {
        case EventType::OrderAck:
          static_cast<void>(oms.on_ack(RecordingSink::as<OrderAckMsg>(raw)));
          break;
        case EventType::OrderFill: {
          const auto& f = RecordingSink::as<OrderFillMsg>(raw);
          if (oms.on_fill(f).action != OmsAction::Duplicate)
            positions.on_fill(
                kCall, f.side, f.price, f.qty, f.fee, instruments.get(f.hdr.instrument));
          break;
        }
        case EventType::Reconcile: {
          const auto& m = RecordingSink::as<ReconcileMsg>(raw);
          if (m.kind == ReconcileMsg::Kind::Begin) {
            if ((m.flags & ReconcileMsg::kExecutionsExact) != 0) ++exact;
            oms.reconcile_begin(m.hdr.venue, std::optional<ClientOrderId>(m.sent_watermark));
          } else if (m.kind == ReconcileMsg::Kind::OpenOrder) {
            static_cast<void>(oms.reconcile_open_order(m));
          } else if (m.kind == ReconcileMsg::Kind::End) {
            oms.reconcile_end([](const OmsUpdate&) {}, m.hdr.venue);
          }
          break;
        }
        default:
          break;
      }
    }
  }
};

// A connected, authenticated DeribitVenue subscribed to the option, against the fake, whose
// open-order snapshots are empty.
struct ReplaySession {
  Harness h;
  InstrumentTable instruments = make_instruments();
  RecordingSink md{1U << 20};
  RecordingSink orders{1U << 20, SinkPolicy::Spin};
  MsgRing outbound{1U << 16};
  net::Reactor reactor;
  SymbolTable symbols;
  std::unique_ptr<DeribitVenue> venue;
  Collected oc;
  Collected startup;  // what came up to the start-up sweep's End

  explicit ReplaySession(const std::map<std::string, std::string>& extra = {}) {
    {
      const std::lock_guard lock(h.mu);
      h.open_orders_reply = "[]";
    }
    VenueSection section = h.section();
    for (const auto& [k, v] : extra) section.extra[k] = v;
    DeribitVenueConfig cfg = make_deribit_config(section, false);
    cfg.ws_private_url = h.srv.ws_base() + kPrivatePath;
    venue = std::make_unique<DeribitVenue>(kVenue, cfg);
    REQUIRE(venue->load_reference_data(instruments));
    REQUIRE(symbols.build(instruments));
    venue->attach(symbols, instruments, md.sink, orders.sink, &outbound);
    const InstrumentId ids[] = {kCall};
    venue->subscribe(ids);
    venue->connect(reactor);
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      Collected mdc;
      mdc.take(md);
      return venue->authenticated() && count_state(oc, ConnState::Live) >= 1;
    }));
    // The start-up sweep (an empty book here) finishes before a test asks for its own snapshot;
    // the tests count what comes after it.
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return count_kind(ReconcileMsg::Kind::End) >= 1;
    }));
    startup.all = std::move(oc.all);
    oc.all.clear();
    h.open_orders_requests = 0;
    const std::lock_guard lock(h.mu);
    h.trades_queries.clear();
  }
  ~ReplaySession() {
    venue->disconnect();
    reactor.run_once(0);
    h.srv.stop();
  }
  ReplaySession(const ReplaySession&) = delete;
  ReplaySession& operator=(const ReplaySession&) = delete;

  // Buys 1 contract of the option at 0.0065 as fm000100000001 and waits for its ack.
  OutNewOrderMsg buy(Mirror& m) {
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kCall, kVenue);
    n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("0.0065").value();
    n.qty = Qty::from_int(1);
    NewOrderRequest req;
    req.instrument = kCall;
    req.venue = kVenue;
    req.side = n.side;
    req.type = n.type;
    req.post_only = true;
    req.price = n.price;
    req.qty = n.qty;
    REQUIRE(m.oms.submit(req, n.cl_ord_id, wall_now()).has_value());
    REQUIRE(outbound.try_push(&n, n.hdr.len));
    venue->on_wake();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::OrderAck) >= 1;
    }));
    return n;
  }

  // Asks for a reconciliation and waits for its End; returns its Begin.
  const ReconcileMsg* reconcile() {
    const std::size_t ends = count_kind(ReconcileMsg::Kind::End);
    venue->request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return count_kind(ReconcileMsg::Kind::End) == ends + 1;
    }));
    return oc.last_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
      return m.kind == ReconcileMsg::Kind::Begin;
    });
  }

  [[nodiscard]] std::size_t count_kind(ReconcileMsg::Kind k) const {
    std::size_t n = 0;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) == EventType::Reconcile &&
          RecordingSink::as<ReconcileMsg>(m).kind == k)
        ++n;
    }
    return n;
  }

  [[nodiscard]] std::vector<std::string> replayed_ids() const {
    std::vector<std::string> out;
    for (const auto& m : oc.all) {
      if (RecordingSink::type_of(m) != EventType::OrderFill) continue;
      const auto& f = RecordingSink::as<OrderFillMsg>(m);
      if ((f.flags & OrderFillMsg::kReplayed) != 0) out.emplace_back(f.exec_id.view());
    }
    return out;
  }

  VenueStatus status() {
    venue->on_timer(net::Reactor::now_ns());
    return venue->status();
  }
};

}  // namespace

TEST_CASE("deribit.venue: a fill the private stream missed is booked at reconciliation") {
  ReplaySession s;
  Mirror m(s.instruments);
  const OutNewOrderMsg n = s.buy(m);
  m.drain(s.oc);
  // The order fills completely at 0.0055 and user.trades never says so; the open-order snapshot
  // no longer lists it. The trade history does.
  s.h.set_trades([](std::int64_t start, bool) {
    return trades_page({trade_row("T1", start + 5, "0.0055", "1.0", "0.0003")}, false);
  });
  const ReconcileMsg* begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK((begin->flags & ReconcileMsg::kExecutionsExact) != 0);
  m.drain(s.oc);

  REQUIRE(s.replayed_ids() == std::vector<std::string>{"T1"});
  const auto* fill = s.oc.last<OrderFillMsg>(EventType::OrderFill);
  CHECK(fill->cl_ord_id == n.cl_ord_id);
  CHECK(fill->venue_order_id.view() == "42710123456");
  CHECK(fill->side == Side::Buy);
  CHECK(fill->liquidity == Liquidity::Maker);
  CHECK(fill->fee_asset == FeeAsset::Quote);
  // Booked with its own price and fee before the snapshot: the order is filled, not unresolved.
  const Position& p = m.positions.get(kCall);
  CHECK(p.qty == Qty::from_int(1));
  CHECK(p.avg_px == Price::from_decimal("0.0055").value());
  CHECK(p.fees == Notional::from_decimal("0.0003").value());
  CHECK(m.oms.stats().filled == 1);
  CHECK(m.oms.stats().reconcile_unresolved == 0);
  CHECK(m.exact == 1);

  const auto q = s.h.queries();
  REQUIRE(q.size() == 1);
  CHECK_FALSE(q[0].historical);
  const std::string req =
      find_frame(s.h.srv, kPrivatePath, "private/get_user_trades_by_currency_and_time");
  CHECK(req.find(R"("currency":"BTC","kind":"any")") != std::string::npos);
  CHECK(req.find(R"("count":1000,"sorting":"asc","historical":false)") != std::string::npos);
  const VenueStatus st = s.status();
  CHECK(st.execution_queries == 2);  // the start-up sweep's and this one
  CHECK(st.executions_fetched == 1);
  CHECK(st.execution_query_errors == 0);

  // The next reconciliation starts no later than T1's timestamp (the watermark stays the settle
  // margin behind the replay before) and does not replay it again.
  s.h.set_trades([](std::int64_t start, bool) {
    return trades_page({trade_row("T1", start, "0.0055", "1.0", "0.0003")}, false);
  });
  static_cast<void>(s.reconcile());
  CHECK(s.replayed_ids().size() == 1);
  CHECK(s.h.queries().back().start <= q[0].start + 5);
}

TEST_CASE("deribit.venue: the execution replay follows has_more across pages") {
  ReplaySession s;
  std::int64_t first = 0;  // server thread only
  s.h.set_trades([&first](std::int64_t start, bool) {
    if (first == 0) first = start;
    if (start == first)
      return trades_page({trade_row("T1", start + 1, "0.0055", "0.3", "0.0001"),
                          trade_row("T2", start + 2, "0.0056", "0.3", "0.0001")},
                         true);
    // The next page starts at the last row's timestamp, so T2 comes back.
    return trades_page({trade_row("T2", start, "0.0056", "0.3", "0.0001"),
                        trade_row("T3", start + 1, "0.0057", "0.4", "0.0001")},
                       false);
  });
  const ReconcileMsg* begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK((begin->flags & ReconcileMsg::kExecutionsExact) != 0);
  CHECK(s.replayed_ids() == std::vector<std::string>{"T1", "T2", "T3"});
  const auto q = s.h.queries();
  REQUIRE(q.size() == 2);
  CHECK(q[1].start == q[0].start + 2);
  CHECK(q[1].end == q[0].end);
  // The snapshot was asked for only after the last page.
  CHECK(s.h.open_orders_requests.load() == 1);
  CHECK(s.status().executions_fetched == 3);
}

TEST_CASE(
    "deribit.venue: kExecutionsExact only after a complete replay and a failed one is retried") {
  ReplaySession s;
  int calls = 0;  // server thread only
  s.h.set_trades([&calls](std::int64_t start, bool) -> std::string {
    if (++calls == 1) return {};  // too_many_requests
    return trades_page({trade_row("T1", start + 1, "0.0055", "0.5", "0.0002")}, false);
  });
  const ReconcileMsg* begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK((begin->flags & ReconcileMsg::kExecutionsExact) == 0);
  CHECK(s.replayed_ids().empty());

  // Nobody asks again: the housekeeping timer does, and the snapshot is not repeated.
  REQUIRE(pump_until(
      s.reactor,
      [&] {
        s.oc.take(s.orders);
        return !s.replayed_ids().empty();
      },
      8000));
  CHECK(s.replayed_ids() == std::vector<std::string>{"T1"});
  CHECK(s.count_kind(ReconcileMsg::Kind::Begin) == 1);
  CHECK(s.h.open_orders_requests.load() == 1);
  const VenueStatus st = s.status();
  CHECK(st.execution_queries == 3);  // the start-up sweep's, the failed one, the retry
  CHECK(st.execution_query_errors == 1);
  CHECK(st.executions_fetched == 1);

  begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK((begin->flags & ReconcileMsg::kExecutionsExact) != 0);

  // A page that says there is more but cannot move past its start (1000 rows in one millisecond)
  // leaves the replay incomplete.
  s.h.set_trades([](std::int64_t start, bool) {
    return trades_page({trade_row("T9", start, "0.0055", "0.1", "0")}, true);
  });
  begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK((begin->flags & ReconcileMsg::kExecutionsExact) == 0);
}

TEST_CASE("deribit.venue: a replay reaching back past 24 h asks the history first") {
  constexpr std::int64_t kHour = 3'600'000;
  ReplaySession s;
  // The fake's clock (get_time fixture) is not ours: the offset moves the venue's time.
  const std::int64_t venue_now = wall_now().ns / 1'000'000 + s.venue->clock_offset_ms();
  s.venue->resume_executions(venue_now - 30 * kHour, {"T1"});
  s.h.set_trades([](std::int64_t start, bool historical) {
    if (historical)
      return trades_page({trade_row("T1", start + 1, "0.0055", "0.1", "0"),
                          trade_row("T2", start + 2, "0.0055", "0.1", "0")},
                         false);
    // The last 24 h, from where the history window ended.
    return trades_page({trade_row("T3", start + 1, "0.0055", "0.1", "0")}, false);
  });
  const ReconcileMsg* begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK((begin->flags & ReconcileMsg::kExecutionsExact) != 0);
  const auto q = s.h.queries();
  REQUIRE(q.size() == 2);
  CHECK(q[0].historical);
  CHECK(q[0].start == venue_now - 30 * kHour);
  CHECK(q[0].end <= venue_now - 22 * kHour);
  CHECK(q[0].end >= venue_now - 24 * kHour);
  CHECK_FALSE(q[1].historical);
  CHECK(q[1].start == q[0].end + 1);
  // T1 was booked by the session resumed from.
  CHECK(s.replayed_ids() == std::vector<std::string>{"T2", "T3"});
}

namespace {

// The engine side of EngineConfig::await_reconcile: no order and no quote until the venue's first
// reconciliation has ended.
struct NullTransport {
  bool send(const EventHeader&) noexcept { return true; }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept { return batch.size(); }
  bool supports_replace(VenueId) const noexcept { return false; }
};
struct NoStrategy {};
using GateEngine = Engine<NoStrategy, SimClock, NullTransport, InlineFeed>;

struct Gate {
  InlineFeed feed{1U << 20};
  InstrumentTable instruments = make_instruments();
  SimClock clock{Timestamp{1'000'000'000}};
  std::unique_ptr<GateEngine> engine;
  NullTransport transport;
  NoStrategy strategy;

  Gate() {
    EngineConfig cfg;
    cfg.await_reconcile = 1U << kVenue.value;
    engine = std::make_unique<GateEngine>(cfg, instruments, clock, transport, feed, strategy);
    engine->start();
  }
  // The connector's reconciliation messages, as the engine reads them from the order ring.
  void feed_reconcile(const Collected& c) {
    for (const auto& m : c.all) {
      if (RecordingSink::type_of(m) == EventType::Reconcile)
        REQUIRE(feed.push(RecordingSink::as<ReconcileMsg>(m).hdr));
    }
    static_cast<void>(engine->drain());
  }
};

}  // namespace

TEST_CASE("deribit.venue: a trade-history query never answered ends the replay incomplete") {
  // The query goes out on the private WebSocket, which stays up. Before, the replay waited for
  // its answer until the connection dropped, the snapshot waited for the replay, and after a
  // restart the engine (await_reconcile) sent no order for as long.
  ReplaySession s;
  s.h.hold_trades = true;
  const std::size_t ends = s.count_kind(ReconcileMsg::Kind::End);
  s.venue->request_open_orders();
  REQUIRE(pump_until(s.reactor, [&] { return s.h.held().size() == 1; }));
  static_cast<void>(pump_until(s.reactor, [] { return false; }, 300));
  s.oc.take(s.orders);
  CHECK(s.count_kind(ReconcileMsg::Kind::Begin) == 0);
  CHECK(s.h.open_orders_requests.load() == 0);  // the snapshot waits for the replay

  // The housekeeping timer past the query's deadline: the replay is incomplete and the snapshot
  // goes ahead without kExecutionsExact.
  s.venue->on_timer(net::Reactor::now_ns() + ReplaySchedulerBase::kQueryTimeoutNs + 1'000'000);
  REQUIRE(pump_until(s.reactor, [&] {
    s.oc.take(s.orders);
    return s.count_kind(ReconcileMsg::Kind::End) == ends + 1;
  }));
  const auto* begin = s.oc.last_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
    return m.kind == ReconcileMsg::Kind::Begin;
  });
  REQUIRE(begin != nullptr);
  CHECK((begin->flags & ReconcileMsg::kExecutionsExact) == 0);
  CHECK(s.h.open_orders_requests.load() == 1);

  // The engine's gate opens, the reconciliation counted as estimated.
  Gate g;
  CHECK(g.engine->awaiting_reconcile() == 1U << kVenue.value);
  g.feed_reconcile(s.oc);
  CHECK(g.engine->awaiting_reconcile() == 0);
  CHECK_FALSE(g.engine->reconciling());
  CHECK(g.engine->stats().estimated_reconciles == 1);
  CHECK(g.engine->stats().exact_reconciles == 0);

  // The replay is asked again kRetryNs later. The first query's answer, arriving now, is not the
  // retry's: only the retry's own answer is booked.
  REQUIRE(pump_until(s.reactor, [&] { return s.h.held().size() == 2; }, 9000));
  const std::vector<std::string> held = s.h.held();
  const std::int64_t ts = wall_now().ns / 1'000'000;
  s.h.srv.send_to(
      kPrivatePath,
      rpc_ok(held[0], trades_page({trade_row("T1", ts, "0.0055", "1.0", "0.0003")}, false)));
  s.h.srv.send_to(
      kPrivatePath,
      rpc_ok(held[1], trades_page({trade_row("T2", ts, "0.0056", "1.0", "0.0003")}, false)));
  REQUIRE(pump_until(s.reactor, [&] {
    s.oc.take(s.orders);
    return !s.replayed_ids().empty();
  }));
  CHECK(s.replayed_ids() == std::vector<std::string>{"T2"});
  CHECK(held[0] != held[1]);
  CHECK(s.count_kind(ReconcileMsg::Kind::Begin) == 1);  // the retry is not a reconciliation
}

// ---- the snapshot (ReconcileDriver) -----------------------------------------------------------

TEST_CASE("deribit.venue: a snapshot whose open orders fail is asked again") {
  // Before ReconcileDriver a failed get_open_orders_by_currency was logged and dropped: the next
  // snapshot came with the next reconnect or ControlCommand::Reconcile, however long that took.
  ReplaySession s;
  s.h.open_orders_failures = 1;
  const std::size_t ends = s.count_kind(ReconcileMsg::Kind::End);
  s.venue->request_open_orders();
  REQUIRE(pump_until(s.reactor, [&] { return s.h.open_orders_requests.load() == 1; }));
  static_cast<void>(pump_until(s.reactor, [] { return false; }, 300));
  s.oc.take(s.orders);
  CHECK(s.count_kind(ReconcileMsg::Kind::Begin) == 0);  // nothing of the failed one
  // Nobody asks again: the driver does, after its retry delay.
  REQUIRE(pump_until(
      s.reactor,
      [&] {
        s.oc.take(s.orders);
        return s.count_kind(ReconcileMsg::Kind::End) == ends + 1;
      },
      9000));
  CHECK(s.h.open_orders_requests.load() == 2);
  CHECK(s.count_kind(ReconcileMsg::Kind::Begin) == 1);
}

TEST_CASE("deribit.venue: the snapshot carries the position of every subscribed instrument") {
  // Nothing on the private stream reports a position: without this leg a delivery, a
  // liquidation or another client's trade never reached the engine.
  ReplaySession s;
  {
    const std::lock_guard lock(s.h.mu);
    s.h.positions_reply =
        R"([{"average_price":0.006,"delta":-0.3,"direction":"sell","floating_profit_loss":0.0001,"index_price":77000.0,"initial_margin":0.01,"instrument_name":"BTC-15SEP26-77000-C","kind":"option","maintenance_margin":0.005,"mark_price":0.0058,"open_orders_margin":0,"realized_profit_loss":0,"settlement_price":0.0059,"size":-0.5,"total_profit_loss":0.0001},)"
        R"({"average_price":70000.0,"direction":"buy","instrument_name":"BTC-PERPETUAL","kind":"future","size":100.0,"size_currency":0.0014}])";
  }
  const ReconcileMsg* begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK(s.h.positions_requests.load() >= 2);  // the start-up sweep's and this one
  CHECK(find_frame(s.h.srv, kPrivatePath, "private/get_positions").find(R"("currency":"BTC")") !=
        std::string::npos);
  std::vector<const ReconcileMsg*> rows;
  for (const auto& m : s.oc.all) {
    if (RecordingSink::type_of(m) == EventType::Reconcile &&
        RecordingSink::as<ReconcileMsg>(m).kind == ReconcileMsg::Kind::Position)
      rows.push_back(&RecordingSink::as<ReconcileMsg>(m));
  }
  // Subscribed: the option only. The perpetual's row is not this session's instrument.
  REQUIRE(rows.size() == 1);
  CHECK(rows[0]->hdr.instrument == kCall);
  CHECK(rows[0]->position_qty == Qty::from_decimal("-0.5").value());
  CHECK(rows[0]->avg_px == Price::from_decimal("0.006").value());

  // Flat is a row too: the venue lists nothing for the instrument.
  {
    const std::lock_guard lock(s.h.mu);
    s.h.positions_reply = "[]";
  }
  static_cast<void>(s.reconcile());
  const auto* flat = s.oc.last_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
    return m.kind == ReconcileMsg::Kind::Position;
  });
  REQUIRE(flat != nullptr);
  CHECK(flat != rows[0]);
  CHECK(flat->position_qty.is_zero());
}

TEST_CASE("deribit.venue: an order shadow whose terminal event was lost is dropped") {
  // Before ReconcileDriver only Binance Spot swept its shadows: an order that ended with nobody
  // told kept its entry in the 8192-slot table for the rest of the session.
  ReplaySession s;
  Mirror m(s.instruments);
  static_cast<void>(s.buy(m));
  CHECK(s.venue->shadow_count() == 1);
  // The order is gone at the venue (cancelled with the connection, say) and no event said so.
  const ReconcileMsg* begin = s.reconcile();
  REQUIRE(begin != nullptr);
  CHECK(begin->sent_watermark.valid());
  CHECK(s.venue->shadow_count() == 0);
  CHECK(s.status().shadows_swept == 1);
}

TEST_CASE("deribit.venue: with the order table full an order or edit is refused, not sent") {
  // Before, the order went out untracked: its reply carried no instrument, an edit of it was
  // refused as unknown, and its first user.trades fill came with no cumulative or remaining
  // quantity (the shadow counts them), so the engine took a partial fill for the whole order.
  ReplaySession s({{"matching_engine_rate", "1000000"}, {"matching_engine_burst", "1000000"}});
  constexpr std::size_t kRoom = kShadowSlots - kShadowSlots / 8;
  const auto id_of = [](std::size_t k) { return make_cl_ord_id(2, static_cast<std::uint32_t>(k)); };
  const auto push_new = [&](ClientOrderId id) {
    OutNewOrderMsg n{};
    init_header(n, EventType::OutNewOrder, kCall, kVenue);
    n.cl_ord_id = id;
    n.side = Side::Buy;
    n.type = OrderType::PostOnly;
    n.price = Price::from_decimal("0.0065").value();
    n.qty = Qty::from_int(1);
    while (!s.outbound.try_push(&n, n.hdr.len)) {
      s.venue->on_wake();
      s.reactor.run_once(1);
    }
  };
  const int buys = s.h.buys.load();
  for (std::size_t k = 1; k <= kRoom; ++k) push_new(id_of(k));
  s.venue->on_wake();
  REQUIRE(pump_until(
      s.reactor,
      [&] {
        s.oc.take(s.orders);
        return s.h.buys.load() == buys + static_cast<int>(kRoom);
      },
      60000));
  REQUIRE(s.venue->shadow_count() == kRoom);

  push_new(id_of(kRoom + 1));
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, kCall, kVenue);
  r.cl_ord_id = id_of(kRoom + 2);
  r.orig_cl_ord_id = id_of(1);
  r.venue_order_id.assign("ETH-349280");
  r.price = Price::from_decimal("0.007").value();
  r.qty = Qty::from_int(2);
  REQUIRE(s.outbound.try_push(&r, r.hdr.len));
  s.venue->on_wake();
  const auto refused = [&](ClientOrderId id) {
    return s.oc.first_if<OrderRejectMsg>(EventType::OrderReject, [&](const OrderRejectMsg& m) {
      return m.cl_ord_id == id && m.reason == RejectReason::OrderTableFull;
    });
  };
  REQUIRE(pump_until(s.reactor, [&] {
    s.oc.take(s.orders);
    return refused(id_of(kRoom + 1)) != nullptr && refused(id_of(kRoom + 2)) != nullptr;
  }));
  CHECK(refused(id_of(kRoom + 1))->hdr.instrument == kCall);
  CHECK(refused(id_of(kRoom + 2))->hdr.instrument == kCall);

  // The snapshot names none of them: swept, but for the last ones sent, which the fake never
  // answered (its replies name another order) and which still hold the watermark back. The next
  // order goes; the fake has counted every frame sent before it by the time it counts that one.
  static_cast<void>(s.reconcile());
  CHECK(s.venue->shadow_count() <= SentWatermark::kMaxInFlight);
  push_new(id_of(kRoom + 3));
  s.venue->on_wake();
  REQUIRE(
      pump_until(s.reactor, [&] { return s.h.buys.load() == buys + static_cast<int>(kRoom) + 1; }));
  CHECK(find_frame(s.h.srv, kPrivatePath, "private/edit").empty());
  CHECK(s.status().shadows_refused == 2);
}

namespace {

// The rows of the first complete balance snapshot in `a` then `b`; empty until one ended.
std::vector<const BalanceMsg*> first_balance_snapshot(const Collected& a, const Collected& b) {
  std::vector<const BalanceMsg*> rows;
  for (const Collected* c : {&a, &b}) {
    for (const auto& m : c->all) {
      if (RecordingSink::type_of(m) != EventType::Balance) continue;
      const auto& bm = RecordingSink::as<BalanceMsg>(m);
      if ((bm.flags & BalanceMsg::kSnapshot) == 0) continue;
      rows.push_back(&bm);
      if ((bm.flags & BalanceMsg::kSnapshotEnd) != 0) return rows;
    }
  }
  return {};
}

std::vector<const BalanceMsg*> balance_msgs(const Collected& c, bool snapshot) {
  std::vector<const BalanceMsg*> out;
  for (const auto& m : c.all) {
    if (RecordingSink::type_of(m) != EventType::Balance) continue;
    const auto& bm = RecordingSink::as<BalanceMsg>(m);
    if (((bm.flags & BalanceMsg::kSnapshot) != 0) == snapshot) out.push_back(&bm);
  }
  return out;
}

Notional notional(const char* s) {
  return Notional::from_decimal(s).value();
}

}  // namespace

TEST_CASE("deribit.venue: the start-up balance snapshot carries the tracked currencies") {
  // After the start-up sweep's End: private/get_account_summaries (the documented example: BTC and
  // ETH). The instruments name BTC and USD: the BTC row goes, ETH is nobody's, and USD is not a
  // Deribit currency. Stamped with the reply's usOut (1790730000000450 us, rounded up to ms).
  ReplaySession s;
  std::vector<const BalanceMsg*> rows;
  REQUIRE(pump_until(s.reactor, [&] {
    s.oc.take(s.orders);
    rows = first_balance_snapshot(s.startup, s.oc);
    return !rows.empty();
  }));
  CHECK(s.h.summaries_requests.load() == 1);
  const std::string req = find_frame(s.h.srv, kPrivatePath, "private/get_account_summaries");
  CHECK(req.find(R"("params":{"access_token":"1789345400000.1MbQ-J_4.CBP-OqOwFakeAccessToken"})") !=
        std::string::npos);
  REQUIRE(rows.size() == 1);
  const BalanceMsg& b = *rows[0];
  CHECK(b.asset.view() == "BTC");
  CHECK(b.flags == (BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd));
  CHECK(b.hdr.venue == kVenue);
  CHECK(b.hdr.exch_ts == Timestamp{1790730000001LL * 1'000'000});
  CHECK(b.free == notional("301.38059622"));
  CHECK(b.locked == notional("1.24669592"));
  CHECK(b.total == notional("302.60065765"));
  CHECK(b.equity == notional("302.61869214"));
  CHECK(b.maintenance == notional("0.8857841"));
}

TEST_CASE("deribit.venue: a user.portfolio notification updates the balance and the account") {
  ReplaySession s;
  CHECK(find_frame(s.h.srv, kPrivatePath, "private/subscribe").find("\"user.portfolio.BTC\"") !=
        std::string::npos);
  s.oc.all.clear();
  // ETH is no instrument's currency: dropped. BTC with cross collateral: the currency and the
  // account's USD row, stamped with the venue clock (the notification carries no time).
  s.h.srv.send_to(
      kPrivatePath,
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.portfolio.eth","data":{"currency":"ETH","balance":10,"equity":10,"available_funds":10,"initial_margin":0,"maintenance_margin":0,"cross_collateral_enabled":false}}})");
  s.h.srv.send_to(
      kPrivatePath,
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.portfolio.btc","data":{"currency":"BTC","balance":1.5,"equity":1.52,"available_funds":1.2,"initial_margin":0.3,"maintenance_margin":0.2,"margin_balance":1.5,"cross_collateral_enabled":true,"total_equity_usd":171000.5,"total_initial_margin_usd":30000,"total_maintenance_margin_usd":20000,"total_margin_balance_usd":170000.25}}})");
  std::vector<const BalanceMsg*> rows;
  REQUIRE(pump_until(s.reactor, [&] {
    s.oc.take(s.orders);
    rows = balance_msgs(s.oc, false);
    return rows.size() >= 2;
  }));
  REQUIRE(rows.size() == 2);
  CHECK(rows[0]->asset.view() == "BTC");
  CHECK(rows[0]->flags == 0);
  CHECK(rows[0]->free == notional("1.2"));
  CHECK(rows[0]->locked == notional("0.3"));
  CHECK(rows[0]->total == notional("1.5"));
  CHECK(rows[0]->equity == notional("1.52"));
  CHECK(rows[0]->maintenance == notional("0.2"));
  CHECK(rows[1]->asset.view() == "USD");
  CHECK(rows[1]->flags == BalanceMsg::kAccount);
  CHECK(rows[1]->free == notional("140000.25"));
  CHECK(rows[1]->locked == notional("30000"));
  CHECK(rows[1]->total == notional("170000.25"));
  CHECK(rows[1]->equity == notional("171000.5"));
  CHECK(rows[1]->maintenance == notional("20000"));
  const std::int64_t venue_now_ms = wall_now().ns / 1'000'000 + s.venue->clock_offset_ms();
  const std::int64_t stamped_ms = rows[0]->hdr.exch_ts.ns / 1'000'000;
  CHECK(stamped_ms <= venue_now_ms);
  CHECK(stamped_ms > venue_now_ms - 10'000);
  CHECK(rows[1]->hdr.exch_ts == rows[0]->hdr.exch_ts);
}

TEST_CASE("deribit.venue: a failed balance fetch does not hold up the order snapshot") {
  ReplaySession s;
  REQUIRE(pump_until(s.reactor, [&] {
    s.oc.take(s.orders);
    return !first_balance_snapshot(s.startup, s.oc).empty();
  }));
  s.oc.all.clear();
  s.h.summaries_fail = true;
  const int asked = s.h.summaries_requests.load();
  // The order snapshot ends as ever; the balance leg after it fails and sends nothing.
  REQUIRE(s.reconcile() != nullptr);
  REQUIRE(pump_until(s.reactor, [&] {
    s.oc.take(s.orders);
    return s.h.summaries_requests.load() == asked + 1;
  }));
  static_cast<void>(pump_until(s.reactor, [] { return false; }, 300));
  s.oc.take(s.orders);
  CHECK(s.oc.count(EventType::Balance) == 0);
  // Another reconciliation while the balances fail: its orders still come.
  REQUIRE(s.reconcile() != nullptr);
  CHECK(s.count_kind(ReconcileMsg::Kind::End) == 2);
  // The driver asks again after its retry delay; the venue answers now.
  s.h.summaries_fail = false;
  std::vector<const BalanceMsg*> rows;
  REQUIRE(pump_until(
      s.reactor,
      [&] {
        s.oc.take(s.orders);
        rows = first_balance_snapshot(Collected{}, s.oc);
        return !rows.empty();
      },
      9000));
  REQUIRE(rows.size() == 1);
  CHECK(rows[0]->asset.view() == "BTC");
  CHECK(rows[0]->flags == (BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd));
}
