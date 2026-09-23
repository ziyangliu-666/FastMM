// DeribitVenue against a scripted in-process fake Deribit (REST + two WebSocket connections):
// reference data, public subscribe with snapshot/change sync and heartbeat test_request, private
// public/auth + set_heartbeat + enable_cancel_on_disconnect + private/subscribe, private/buy ->
// ack, private/edit with the user.orders update arriving before the edit response, user.trades
// fill mapped to the edited order, private/cancel -> user.orders cancelled, a change_id gap ->
// public/unsubscribe + public/subscribe -> new snapshot, get_open_orders_by_currency
// reconciliation, private disconnect -> REST cancel_all_by_instrument + re-authentication +
// reconciliation, 13009 on an order -> re-authentication, the blocking kill switch, invalid
// credentials -> fatal, and the local matching-engine credit limit.
//
// The fake serves the private connection on its own path so the two sessions are easy to tell
// apart; the real venue uses one URL for both (DeribitVenueConfig::ws_private_url defaults to
// ws_url).
#include "fastmm/venues/deribit/deribit_venue.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/time.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/registry.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

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
        s.send_text(open_orders);
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
                                                               "user.trades.future.BTC.raw"});
    venue.connect(reactor);

    Collected mdc;
    Collected oc;
    REQUIRE(pump_until(reactor, [&] {
      mdc.take(md);
      oc.take(orders);
      return venue.md_feed()->synced_count() == 2 && mdc.count(EventType::OptionTicker) == 1 &&
             count_state(oc, ConnState::Live) >= 1 && h.md_test_replies.load() >= 1 &&
             h.private_test_replies.load() >= 1;
    }));
    CHECK(venue.authenticated());
    const std::string auth = find_frame(h.srv, kPrivatePath, "public/auth");
    CHECK(
        auth ==
        R"({"jsonrpc":"2.0","id":1,"method":"public/auth","params":{"grant_type":"client_credentials","client_id":"fake-client","client_secret":"fake-secret"}})");
    const std::string sub = find_frame(h.srv, kPrivatePath, "private/subscribe");
    CHECK(
        sub.find(
            R"("channels":["user.orders.option.BTC.raw","user.trades.option.BTC.raw","user.orders.future.BTC.raw","user.trades.future.BTC.raw"])") !=
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

    venue.request_open_orders();
    REQUIRE(pump_until(reactor, [&] {
      oc.take(orders);
      return oc.count(EventType::Reconcile) == 4;  // Begin, option order, perpetual order, End
    }));
    const auto* rec = oc.last<ReconcileMsg>(EventType::Reconcile);
    CHECK(rec->kind == ReconcileMsg::Kind::End);
    // Begin carries the last order id the venue sent before it asked for the snapshot.
    const auto* begin = oc.first_if<ReconcileMsg>(EventType::Reconcile, [](const ReconcileMsg& m) {
      return m.kind == ReconcileMsg::Kind::Begin;
    });
    REQUIRE(begin != nullptr);
    CHECK((begin->flags & ReconcileMsg::kSentWatermark) != 0);
    CHECK(begin->sent_watermark == rp.cl_ord_id);

    // Private channel drop: REST cancel_all_by_instrument per instrument, reconnect, auth again,
    // reconcile again.
    const int auths_before = h.auths.load();
    h.srv.close_sessions(kPrivatePath);
    REQUIRE(pump_until(
        reactor,
        [&] {
          oc.take(orders);
          return h.cancel_all_ok.load() >= 2 && h.auths.load() > auths_before &&
                 oc.count(EventType::Reconcile) == 8 &&
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
