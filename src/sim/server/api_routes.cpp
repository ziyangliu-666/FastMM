// Request routing of the simulated exchange: REST routes (/api/v3/*) with weight accounting and
// rate-limit headers, and the WebSocket API method table (/ws-api/v3).
#include "server_impl.hpp"

#include <algorithm>

namespace fastmm::sim::server {

using Impl = SimExchangeServer::Impl;

namespace {

bool is_order_endpoint(RestEndpoint ep) noexcept {
  switch (ep) {
    case RestEndpoint::NewOrder:
    case RestEndpoint::TestOrder:
    case RestEndpoint::CancelOrder:
    case RestEndpoint::CancelReplace:
    case RestEndpoint::Amend:
    case RestEndpoint::CancelAll:
      return true;
    default:
      return false;
  }
}

// rest-api.md "Order book" weight by limit.
std::uint32_t depth_weight(const ParamList& p) {
  const auto limit = parse_int(p.get("limit")).value_or(100);
  if (limit <= 100) return 5;
  if (limit <= 500) return 25;
  if (limit <= 1000) return 50;
  return 250;
}

struct RouteDef {
  const char* method;
  const char* path;
  RestEndpoint ep;
};

constexpr RouteDef kRoutes[] = {
    {"GET", "/api/v3/ping", RestEndpoint::Ping},
    {"GET", "/api/v3/time", RestEndpoint::Time},
    {"GET", "/api/v3/exchangeInfo", RestEndpoint::ExchangeInfo},
    {"GET", "/api/v3/depth", RestEndpoint::Depth},
    {"GET", "/api/v3/ticker/bookTicker", RestEndpoint::BookTicker},
    {"GET", "/api/v3/account", RestEndpoint::AccountInfo},
    {"POST", "/api/v3/order", RestEndpoint::NewOrder},
    {"POST", "/api/v3/order/test", RestEndpoint::TestOrder},
    {"DELETE", "/api/v3/order", RestEndpoint::CancelOrder},
    {"GET", "/api/v3/order", RestEndpoint::QueryOrder},
    {"POST", "/api/v3/order/cancelReplace", RestEndpoint::CancelReplace},
    {"PUT", "/api/v3/order/amend/keepPriority", RestEndpoint::Amend},
    {"GET", "/api/v3/openOrders", RestEndpoint::OpenOrders},
    {"GET", "/api/v3/myTrades", RestEndpoint::MyTrades},
    {"DELETE", "/api/v3/openOrders", RestEndpoint::CancelAll},
    {"POST", "/api/v3/userDataStream", RestEndpoint::ListenKeyCreate},
    {"PUT", "/api/v3/userDataStream", RestEndpoint::ListenKeyKeepalive},
    {"DELETE", "/api/v3/userDataStream", RestEndpoint::ListenKeyClose},
};

constexpr std::string_view kSignedWsMethods[] = {
    "userDataStream.subscribe.signature",
    "order.place",
    "order.test",
    "order.cancel",
    "order.cancelReplace",
    "order.amend.keepPriority",
    "order.status",
    "openOrders.status",
    "openOrders.cancelAll",
    "account.status",
};

}  // namespace

template <class Stream>
void Impl::install_routes(net::HttpServer<Stream>& srv) {
  for (const RouteDef& r : kRoutes) {
    srv.route(r.method, r.path, [this, ep = r.ep](const net::HttpRequest& req) {
      return handle_rest(req, ep);
    });
  }
  srv.set_default_handler([](const net::HttpRequest&) {
    std::string body;
    append_error(body, -1000, "Unknown endpoint.");
    return net::HttpServerResponse::json(404, std::move(body));
  });
}

template void Impl::install_routes<net::PlainStream>(net::HttpServer<net::PlainStream>&);
template void Impl::install_routes<TlsServerStream>(net::HttpServer<TlsServerStream>&);

// ---- REST ---------------------------------------------------------------------------------------

std::uint32_t Impl::rest_weight(RestEndpoint ep, const ParamList& p) const {
  switch (ep) {
    case RestEndpoint::ExchangeInfo:
    case RestEndpoint::AccountInfo:
      return 20;
    case RestEndpoint::Depth:
      return depth_weight(p);
    case RestEndpoint::BookTicker:
    case RestEndpoint::ListenKeyCreate:
    case RestEndpoint::ListenKeyKeepalive:
    case RestEndpoint::ListenKeyClose:
      return 2;
    case RestEndpoint::QueryOrder:
    case RestEndpoint::Amend:
      return 4;
    case RestEndpoint::OpenOrders:
      return p.has("symbol") ? 6 : 80;
    case RestEndpoint::MyTrades:
      return p.has("orderId") ? 5 : 20;  // rest-api.md "Account trade list"
    default:
      return 1;
  }
}

std::string Impl::rate_limits_json(std::int64_t now_ms) {
  Account& a = accounts_.front();
  const RateLimitView limits[] = {
      {"REQUEST_WEIGHT",
       "MINUTE",
       1,
       cfg_.weight_limit_per_minute,
       static_cast<std::int64_t>(weight_1m_.count(now_ms))},
      {"ORDERS",
       "SECOND",
       10,
       cfg_.orders_limit_per_10s,
       static_cast<std::int64_t>(a.orders_10s.count(now_ms))},
      {"ORDERS",
       "DAY",
       1,
       cfg_.orders_limit_per_day,
       static_cast<std::int64_t>(a.orders_1d.count(now_ms))},
  };
  std::string out;
  append_rate_limits(out, limits);
  return out;
}

OpResult Impl::rate_limit_error(std::int64_t now_ms, bool ws_api) {
  ++stats_.rate_limited;
  const std::int64_t wait_ms = weight_1m_.ms_until_reset(now_ms);
  const std::string msg = "Too much request weight used; current limit is " +
                          std::to_string(cfg_.weight_limit_per_minute) +
                          " request weight per 1 MINUTE. Please use WebSocket Streams for live "
                          "updates to avoid polling the API.";
  OpResult r;
  if (ws_api) {
    std::string data;
    JsonObjectWriter w(data);
    w.num("serverTime", now_ms).num("retryAfter", now_ms + wait_ms);
    w.close();
    r = OpResult::error_data(429, -1003, msg, data);
  } else {
    r = OpResult::error(429, -1003, msg);
  }
  r.retry_after_s = std::max<std::int64_t>(1, (wait_ms + 999) / 1000);
  return r;
}

net::HttpServerResponse Impl::handle_rest(const net::HttpRequest& req, RestEndpoint ep) {
  ++stats_.rest_requests;
  if (faults_.rest_unresponsive) {
    ++stats_.unanswered_rest;
    return net::HttpServerResponse::none();
  }
  const std::int64_t now_ms = server_ms();
  ParamList params;
  parse_query(req.query, params);
  if (!req.body.empty()) parse_query(req.body, params);
  begin_request(0);
  OpResult r;
  if (auto banned = injected_stop(now_ms)) {
    r = *banned;
  } else if (faults_.rate_limit_next > 0) {
    --faults_.rate_limit_next;
    r = rate_limit_error(now_ms, false);
  } else if (weight_1m_.add(rest_weight(ep, params), now_ms) > cfg_.weight_limit_per_minute) {
    r = rate_limit_error(now_ms, false);
  } else {
    r = dispatch_rest(req, ep, params, now_ms);
  }
  end_request();
  net::HttpServerResponse resp = net::HttpServerResponse::json(r.status, std::move(r.body));
  resp.headers.emplace_back("X-MBX-USED-WEIGHT-1M", std::to_string(weight_1m_.count(now_ms)));
  if (is_order_endpoint(ep)) {
    Account& a = accounts_.front();
    resp.headers.emplace_back("X-MBX-ORDER-COUNT-10S", std::to_string(a.orders_10s.count(now_ms)));
    resp.headers.emplace_back("X-MBX-ORDER-COUNT-1D", std::to_string(a.orders_1d.count(now_ms)));
  }
  if ((r.status == 429 || r.status == 418) && r.retry_after_s > 0)
    resp.headers.emplace_back("Retry-After", std::to_string(r.retry_after_s));
  return resp;
}

OpResult Impl::dispatch_rest(const net::HttpRequest& req,
                             RestEndpoint ep,
                             const ParamList& params,
                             std::int64_t now_ms) {
  switch (ep) {
    case RestEndpoint::Ping:
      return OpResult::ok("{}");
    case RestEndpoint::Time: {
      ++stats_.time_requests;
      std::string body;
      JsonObjectWriter w(body);
      w.num("serverTime", now_ms);
      w.close();
      return OpResult::ok(std::move(body));
    }
    case RestEndpoint::ExchangeInfo:
      return op_exchange_info(params);
    case RestEndpoint::Depth:
      return op_depth(params);
    case RestEndpoint::BookTicker:
      return op_book_ticker(params);
    case RestEndpoint::ListenKeyCreate:
    case RestEndpoint::ListenKeyKeepalive:
    case RestEndpoint::ListenKeyClose:
      return op_listen_key(ep, req, params);
    default:
      break;
  }
  std::string signature;
  const std::string payload = rest_signature_payload(req.query, req.body, signature);
  Account* acct = nullptr;
  if (auto err = authenticate(req.header("X-MBX-APIKEY"), params, payload, signature, now_ms, acct))
    return *err;
  switch (ep) {
    case RestEndpoint::AccountInfo:
      return op_account(*acct);
    case RestEndpoint::NewOrder:
      return op_place(*acct, params, false);
    case RestEndpoint::TestOrder:
      return op_place(*acct, params, true);
    case RestEndpoint::CancelOrder:
      return op_cancel(*acct, params);
    case RestEndpoint::QueryOrder:
      return op_query_order(*acct, params);
    case RestEndpoint::CancelReplace:
      return op_cancel_replace(*acct, params);
    case RestEndpoint::Amend:
      return op_amend(*acct, params);
    case RestEndpoint::OpenOrders:
      return op_open_orders(*acct, params);
    case RestEndpoint::MyTrades:
      return op_my_trades(*acct, params);
    case RestEndpoint::CancelAll:
      return op_cancel_all(*acct, params);
    default:
      return OpResult::error(404, -1000, "Unknown endpoint.");
  }
}

// ---- WebSocket API
// --------------------------------------------------------------------------------

std::uint32_t Impl::ws_api_weight(std::string_view method, const ParamList& p) const {
  if (method == "exchangeInfo" || method == "account.status") return 20;
  if (method == "depth") return depth_weight(p);
  if (method == "openOrders.status") return p.has("symbol") ? 6 : 80;
  if (method == "userDataStream.subscribe.signature" || method == "order.status" ||
      method == "order.amend.keepPriority")
    return method == "userDataStream.subscribe.signature" ? 2 : 4;
  return 1;
}

void Impl::handle_ws_api(net::WsSession& s, std::string_view text) {
  ++stats_.ws_api_requests;
  const SessionState* st = session_state(&s);
  if (st == nullptr) return;
  const std::uint64_t token = st->token;
  const std::int64_t now_ms = server_ms();
  WsApiRequest req;
  OpResult r;
  std::uint32_t delay = 0;
  begin_request(0);
  if (!parse_ws_api_request(text, req)) {
    r = OpResult::error(400, -1100, "Malformed request: expected {\"id\",\"method\",\"params\"}.");
  } else {
    const bool order_method =
        req.method.starts_with("order.") || req.method == "openOrders.cancelAll";
    if (order_method) delay = faults_.delay_ack_ms;
    request_delay_ms_ = delay;
    if (auto banned = injected_stop(now_ms)) {
      r = *banned;
    } else if (faults_.rate_limit_next > 0) {
      --faults_.rate_limit_next;
      r = rate_limit_error(now_ms, true);
    } else if (weight_1m_.add(ws_api_weight(req.method, req.params), now_ms) >
               cfg_.weight_limit_per_minute) {
      r = rate_limit_error(now_ms, true);
    } else {
      r = dispatch_ws_api(s, req, now_ms);
    }
  }
  // The reply is dropped after the request has been carried out: the venue acted and the client
  // never hears the outcome.
  bool swallow = false;
  if (faults_.swallow_ws_api_next > 0) {
    --faults_.swallow_ws_api_next;
    ++stats_.responses_swallowed;
    swallow = true;
  }
  if (!swallow) {
    std::string out;
    append_ws_api_response(
        out, req.id_json, r.status, r.is_error, r.body, rate_limits_json(now_ms));
    send_to_session(&s, token, std::move(out), delay);
  }
  end_request();  // user events follow the response (same delay)
}

OpResult Impl::dispatch_ws_api(net::WsSession& s, const WsApiRequest& req, std::int64_t now_ms) {
  const std::string_view m = req.method;
  const ParamList& p = req.params;
  if (m == "ping") return OpResult::ok("{}");
  if (m == "time") {
    ++stats_.time_requests;
    std::string body;
    JsonObjectWriter w(body);
    w.num("serverTime", now_ms);
    w.close();
    return OpResult::ok(std::move(body));
  }
  if (m == "exchangeInfo") return op_exchange_info(p);
  if (m == "depth") return op_depth(p);
  if (m == "ticker.book") return op_book_ticker(p);
  if (m == "session.logon" || m == "session.status" || m == "session.logout")
    return op_session(s, m, p, now_ms);
  if (m == "userDataStream.subscribe") {
    SessionState* st = session_state(&s);
    if (st == nullptr || st->logon_account == nullptr)
      return OpResult::error(401, -1002, "You are not authorized to execute this request.");
    const std::int64_t id = st->next_subscription_id++;
    st->user_subs.push_back(SessionState::UserSub{id, st->logon_account->id});
    std::string body;
    JsonObjectWriter w(body);
    w.num("subscriptionId", id);
    w.close();
    return OpResult::ok(std::move(body));
  }
  if (m == "userDataStream.unsubscribe") {
    SessionState* st = session_state(&s);
    if (st == nullptr) return OpResult::ok("{}");
    if (const auto id = parse_int(p.get("subscriptionId"))) {
      std::erase_if(st->user_subs, [&](const SessionState::UserSub& u) { return u.id == *id; });
    } else {
      st->user_subs.clear();
    }
    return OpResult::ok("{}");
  }
  if (std::find(std::begin(kSignedWsMethods), std::end(kSignedWsMethods), m) ==
      std::end(kSignedWsMethods))
    return OpResult::error(400, -1020, "This operation is not supported.");

  Account* acct = nullptr;
  const SessionState* logon = session_state(&s);
  if (logon != nullptr && logon->logon_account != nullptr && !p.has("apiKey") &&
      !p.has("signature")) {
    // web-socket-api "session.logon": after logon apiKey and signature may be omitted; the
    // timestamp is still required.
    if (auto err = check_timing(p, now_ms)) return *err;
    acct = logon->logon_account;
  } else if (auto err = authenticate(
                 p.get("apiKey"), p, p.sorted_payload(), p.get("signature"), now_ms, acct)) {
    return *err;
  }
  if (m == "userDataStream.subscribe.signature") {
    SessionState* st = session_state(&s);
    if (st == nullptr) return OpResult::error(500, -1000, "Session closed.");
    const std::int64_t id = st->next_subscription_id++;
    st->user_subs.push_back(SessionState::UserSub{id, acct->id});
    std::string body;
    JsonObjectWriter w(body);
    w.num("subscriptionId", id);
    w.close();
    return OpResult::ok(std::move(body));
  }
  if (m == "order.place") return op_place(*acct, p, false);
  if (m == "order.test") return op_place(*acct, p, true);
  if (m == "order.cancel") return op_cancel(*acct, p);
  if (m == "order.cancelReplace") return op_cancel_replace(*acct, p);
  if (m == "order.amend.keepPriority") return op_amend(*acct, p);
  if (m == "order.status") return op_query_order(*acct, p);
  if (m == "openOrders.status") return op_open_orders(*acct, p);
  if (m == "openOrders.cancelAll") return op_cancel_all(*acct, p);
  return op_account(*acct);  // account.status
}

// session.logon / session.status / session.logout (web-socket-api "Session Authentication").
OpResult Impl::op_session(net::WsSession& s,
                          std::string_view method,
                          const ParamList& p,
                          std::int64_t now_ms) {
  SessionState* st = session_state(&s);
  if (st == nullptr) return OpResult::error(500, -1000, "Session closed.");
  if (method == "session.logon") {
    Account* acct = nullptr;
    if (auto err =
            authenticate(p.get("apiKey"), p, p.sorted_payload(), p.get("signature"), now_ms, acct))
      return *err;
    if (acct->ed25519 == nullptr)
      return OpResult::error(
          400, -4056, "HMAC_SHA256 API key is not supported. Only Ed25519 keys are supported.");
    st->logon_account = acct;
    st->authorized_since_ms = now_ms;
    ++stats_.session_logons;
  } else if (method == "session.logout") {
    st->logon_account = nullptr;
    st->authorized_since_ms = 0;
  }
  std::string body;
  JsonObjectWriter w(body);
  if (st->logon_account != nullptr) {
    w.str("apiKey", st->logon_account->api_key).num("authorizedSince", st->authorized_since_ms);
  } else {
    w.null("apiKey").null("authorizedSince");
  }
  w.num("connectedSince", st->connected_since_ms)
      .boolean("returnRateLimits", true)
      .num("serverTime", now_ms)
      .boolean("userDataStream", !st->user_subs.empty());
  w.close();
  return OpResult::ok(std::move(body));
}

}  // namespace fastmm::sim::server
