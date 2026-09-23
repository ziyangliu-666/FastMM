// Trading side of the simulated exchange: authentication, order entry / cancel / cancel-replace
// / amend, open orders, balances, and the MatchingSink callbacks that turn matching-engine
// events into executionReport / outboundAccountPosition user-data events.
#include "server_impl.hpp"

#include <algorithm>

namespace fastmm::sim::server {

using Impl = SimExchangeServer::Impl;

namespace {

constexpr std::string_view kUnknownOrder = "Unknown order sent.";
constexpr std::string_view kInsufficientBalance =
    "Account has insufficient balance for requested action.";

std::string mandatory(std::string_view name) {
  return "Mandatory parameter '" + std::string(name) +
         "' was not sent, was empty/null, or malformed.";
}

std::string illegal_decimal(std::string_view name) {
  return "Illegal characters found in parameter '" + std::string(name) +
         "'; legal range is '^([0-9]{1,20})(\\.[0-9]{1,20})?$'.";
}

Qty as_qty(Notional n) noexcept {
  return Qty::from_raw(n.raw);
}

bool valid_client_id(std::string_view s) noexcept {
  if (s.empty() || s.size() > 36) return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == ':' || c == '/' || c == '_' || c == '-';
  });
}

OrderType core_type(BinanceOrderType t) noexcept {
  switch (t) {
    case BinanceOrderType::LimitMaker:
      return OrderType::PostOnly;
    case BinanceOrderType::Market:
      return OrderType::Market;
    case BinanceOrderType::Limit:
      return OrderType::Limit;
  }
  return OrderType::Limit;
}

// Parses a decimal parameter; error OpResult on bad syntax / precision.
template <class Tag>
std::optional<OpResult> parse_decimal_param(std::string_view name,
                                            std::string_view text,
                                            Fixed<Tag>& out) {
  if (text.empty()) return OpResult::error(400, -1102, mandatory(name));
  if (!is_decimal_syntax(text)) return OpResult::error(400, -1100, illegal_decimal(name));
  const auto v = Fixed<Tag>::from_decimal(text);
  if (!v)
    return OpResult::error(
        400, -1111, "Parameter '" + std::string(name) + "' has too much precision.");
  out = *v;
  return std::nullopt;
}

}  // namespace

// ---- request context ----------------------------------------------------------------------------

void Impl::begin_request(std::uint32_t event_delay_ms) {
  in_request_ = true;
  request_delay_ms_ = event_delay_ms;
}

void Impl::end_request() {
  queue_balance_updates();
  in_request_ = false;
  const std::uint32_t delay = request_delay_ms_;
  request_delay_ms_ = 0;
  if (!pending_events_.empty()) {
    const auto events = std::move(pending_events_);
    pending_events_.clear();
    for (const auto& [acct, event] : events) publish_user_event(acct, event, delay);
  }
  orders_.erase_terminal();
  refresh_order_stats();
}

void Impl::emit_user_event(AccountId account, std::string event_json) {
  if (in_request_) {
    pending_events_.emplace_back(account, std::move(event_json));
    return;
  }
  publish_user_event(account, event_json, 0);
}

void Impl::publish_user_event(AccountId account,
                              std::string_view event_json,
                              std::uint32_t delay_ms) {
  if (faults_.user_stream_muted) {
    ++stats_.user_events_dropped;
    return;  // as on a real venue, nothing replays an event nobody was listening for
  }
  std::uint32_t copies = 1;
  if (faults_.duplicate_user_events_next > 0) {
    --faults_.duplicate_user_events_next;
    ++stats_.user_events_duplicated;
    copies = 2;
  }
  struct Target {
    net::WsSession* session;
    std::uint64_t token;
    std::int64_t subscription_id;  // < 0: raw listenKey stream
  };
  std::vector<Target> targets;
  targets.reserve(sessions_.size());
  for (const auto& [ptr, st] : sessions_) {
    if (st.kind == SessionKind::WsApi) {
      for (const SessionState::UserSub& sub : st.user_subs) {
        if (sub.account == account) targets.push_back(Target{ptr, st.token, sub.id});
      }
    } else if (st.kind == SessionKind::ListenKey && st.listen_account == account) {
      targets.push_back(Target{ptr, st.token, -1});
    }
  }
  for (const Target& t : targets) {
    std::string text;
    if (t.subscription_id < 0) {
      text.assign(event_json);
    } else {
      append_user_event(text, t.subscription_id, event_json);
    }
    for (std::uint32_t i = 1; i < copies; ++i) send_to_session(t.session, t.token, text, delay_ms);
    send_to_session(t.session, t.token, std::move(text), delay_ms);
  }
}

void Impl::queue_balance_updates() {
  for (Account& a : accounts_) {
    if (!a.balance_dirty) continue;
    a.balance_dirty = false;
    std::vector<BalanceView> views;
    views.reserve(a.balances.size());
    for (const auto& [asset, b] : a.balances) views.push_back(BalanceView{asset, b.free, b.locked});
    const std::int64_t ms = server_ms();
    std::string event;
    append_account_position(event, ms, ms, views);
    emit_user_event(a.id, std::move(event));
  }
}

void Impl::refresh_order_stats() {
  const std::uint64_t open = orders_.open_count(kStrategyAccount);
  stats_.open_orders = open;
  stats_.max_open_orders = std::max(stats_.max_open_orders, open);
  stats_.min_open_orders_since_mark = std::min(stats_.min_open_orders_since_mark, open);
}

Account* Impl::find_account(std::string_view api_key) noexcept {
  for (Account& a : accounts_) {
    if (a.api_key == api_key) return &a;
  }
  return nullptr;
}

Account& Impl::account(AccountId id) noexcept {
  for (Account& a : accounts_) {
    if (a.id == id) return a;
  }
  return accounts_.front();
}

std::optional<std::uint32_t> Impl::find_symbol(std::string_view symbol) const noexcept {
  for (std::size_t i = 0; i < symbols_.size(); ++i) {
    const std::string& s = symbols_[i].cfg.symbol;
    if (s.size() != symbol.size()) continue;
    bool same = true;
    for (std::size_t k = 0; k < s.size() && same; ++k) {
      char c = symbol[k];
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      same = c == s[k];
    }
    if (same) return static_cast<std::uint32_t>(i);
  }
  return std::nullopt;
}

OrderView Impl::view_of(const OrderRecord& r) const {
  OrderView v;
  v.symbol = symbols_[r.symbol].cfg.symbol;
  v.order_id = r.order_id;
  v.client_order_id = r.client_order_id;
  v.side = r.side;
  v.type = r.type;
  v.tif = r.tif;
  v.price = r.price;
  v.orig_qty = r.orig_qty;
  v.executed_qty = r.executed;
  v.cum_quote = r.cum_quote;
  v.status = r.status;
  v.time_ms = r.time_ms;
  v.update_ms = r.update_ms;
  return v;
}

std::string Impl::next_client_id(std::string_view prefix) {
  std::string id(prefix);
  std::string n = std::to_string(next_generated_id_++);
  if (n.size() < 12) id.append(12 - n.size(), '0');
  id += n;
  return id;
}

void Impl::emit_exec(const OrderRecord& r,
                     ExecType x,
                     std::string_view client_id_override,
                     std::string_view orig_client_id,
                     const CapturedFill* fill,
                     bool maker) {
  ExecReportView v;
  v.order = view_of(r);
  if (!client_id_override.empty()) v.order.client_order_id = client_id_override;
  v.exec_type = x;
  v.orig_client_order_id = orig_client_id;
  v.event_ms = server_ms();
  v.transact_ms = v.event_ms;
  v.execution_id = next_execution_id_++;
  v.working = !r.terminal();
  if (fill != nullptr) {
    v.last_qty = fill->qty;
    v.last_price = fill->price;
    v.commission = fill->commission;
    v.commission_asset = symbols_[r.symbol].cfg.quote_asset;
    v.trade_id = static_cast<std::int64_t>(fill->trade_id);
    v.maker = maker;
  }
  std::string json;
  append_execution_report(json, v);
  emit_user_event(r.account, std::move(json));
}

void Impl::release_lock(OrderRecord& r) {
  if (!r.locked.is_positive()) {
    r.locked = Qty{};
    return;
  }
  const SymbolRuntime& sym = symbols_[r.symbol];
  Account& acct = account(r.account);
  Balance& b = acct.balance(r.locks_quote ? sym.cfg.quote_asset : sym.cfg.base_asset);
  b.locked -= r.locked;
  b.free += r.locked;
  r.locked = Qty{};
  acct.balance_dirty = true;
}

// ---- MatchingSink
// ---------------------------------------------------------------------------------

void Impl::on_book_change(InstrumentId id, Side side, Price px, Qty qty, std::uint64_t uid) {
  if (agg_) agg_->on_book_change(id, side, px, qty, uid);
}

void Impl::on_trade(
    InstrumentId id, Price px, Qty qty, Side aggressor, std::uint64_t trade_id, Timestamp) {
  ++stats_.trades;
  if (id.value >= symbols_.size()) return;
  const std::int64_t ms = server_ms();
  std::string data;
  append_trade(
      data, ms, symbols_[id.value].cfg.symbol, trade_id, px, qty, ms, aggressor == Side::Sell);
  publish_md(id.value, StreamKind::Trade, data);
}

void Impl::on_ack(const SimOrder& o, Timestamp) {
  if (o.account == kGeneratorAccount || amend_in_progress_) return;
  OrderRecord* r = orders_.by_internal(o.cl_ord_id);
  if (r == nullptr) return;
  r->acked = true;
  emit_exec(*r, ExecType::New, {}, {});
}

void Impl::on_reject(const NewOrder& n, RejectReason, Timestamp) {
  if (n.account == kGeneratorAccount || amend_in_progress_) return;
  OrderRecord* r = orders_.by_internal(n.cl_ord_id);
  if (r == nullptr || !r->acked || r->terminal()) return;  // pre-ack: the request reports it
  r->status = BinanceOrderStatus::Rejected;
  r->update_ms = server_ms();
  release_lock(*r);
  ++stats_.orders_rejected;
  emit_exec(*r, ExecType::Rejected, {}, {});
}

void Impl::on_cancel(const SimOrder& o, CancelReason why, Timestamp) {
  if (o.account == kGeneratorAccount || amend_in_progress_) return;
  OrderRecord* r = orders_.by_internal(o.cl_ord_id);
  if (r == nullptr || r->terminal()) return;
  r->update_ms = server_ms();
  ExecType x = ExecType::Canceled;
  switch (why) {
    case CancelReason::Requested:
    case CancelReason::Replaced:
      r->status = BinanceOrderStatus::Canceled;
      x = ExecType::Canceled;
      break;
    case CancelReason::Ioc:
    case CancelReason::Fok:
    case CancelReason::NoLiquidity:
      r->status = BinanceOrderStatus::Expired;
      x = ExecType::Expired;
      break;
    case CancelReason::Stp:
      r->status = BinanceOrderStatus::ExpiredInMatch;
      x = ExecType::TradePrevention;
      break;
  }
  release_lock(*r);
  if (x == ExecType::Canceled) {
    // CANCELED reports carry the cancel request's id in c and the cancelled order's id in C.
    const std::string cancel_id =
        cancel_client_id_.empty() ? next_client_id("x-sim-cxl-") : cancel_client_id_;
    emit_exec(*r, x, cancel_id, r->client_order_id);
  } else {
    emit_exec(*r, x, {}, {});
  }
}

void Impl::on_fill(const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp) {
  const std::uint64_t trade_id = ++trade_seq_;  // == the engine's trade id for this match
  if (maker.account != kGeneratorAccount) apply_fill(maker, px, qty, true, trade_id);
  if (taker.account != kGeneratorAccount) apply_fill(taker, px, qty, false, trade_id);
}

void Impl::apply_fill(const SimOrder& o, Price px, Qty qty, bool maker, std::uint64_t trade_id) {
  OrderRecord* r = orders_.by_internal(o.cl_ord_id);
  if (r == nullptr) return;
  const SymbolRuntime& sym = symbols_[r->symbol];
  Account& acct = account(r->account);
  const Notional notional = mul(px, qty);
  const Notional fee = fees_.fee(notional, maker ? Liquidity::Maker : Liquidity::Taker);
  Balance& base = acct.balance(sym.cfg.base_asset);
  Balance& quote = acct.balance(sym.cfg.quote_asset);
  if (r->side == Side::Buy) {
    if (r->locks_quote && r->locked.is_positive()) {
      const Qty release = min(as_qty(mul(r->price, qty)), r->locked);
      r->locked -= release;
      quote.locked -= release;
      quote.free += release;
    }
    quote.free -= as_qty(notional) + as_qty(fee);
    base.free += qty;
  } else {
    if (!r->locks_quote && r->locked.is_positive()) {
      const Qty from_locked = min(qty, r->locked);
      r->locked -= from_locked;
      base.locked -= from_locked;
      base.free -= qty - from_locked;
    } else {
      base.free -= qty;
    }
    quote.free += as_qty(notional) - as_qty(fee);
  }
  acct.balance_dirty = true;
  r->executed += qty;
  r->cum_quote += notional;
  r->update_ms = server_ms();
  r->status =
      r->executed >= r->orig_qty ? BinanceOrderStatus::Filled : BinanceOrderStatus::PartiallyFilled;
  if (r->status == BinanceOrderStatus::Filled) release_lock(*r);
  ++stats_.fills;
  ++stats_.fills_since_mark;
  stats_.fees += fee;
  if (r->symbol == 0) {
    stats_.cash_flow += r->side == Side::Buy ? -(notional + fee) : notional - fee;
    stats_.position += r->side == Side::Buy ? qty : -qty;
    stats_.max_abs_position = max(stats_.max_abs_position, stats_.position.abs());
  }
  trades_.push_back(TradeRecord{static_cast<std::int64_t>(trade_id),
                                r->order_id,
                                r->symbol,
                                r->account,
                                px,
                                qty,
                                fee,
                                r->update_ms,
                                r->side == Side::Buy,
                                maker});
  const CapturedFill f{px, qty, fee, trade_id};
  if (capture_order_id_ == r->order_id) captured_fills_.push_back(f);
  emit_exec(*r, ExecType::Trade, {}, {}, &f, maker);
}

// Crosses one named resting order with a counter-order from the generator account. Everything
// ahead of it in price-time is swept first (a limit IOC at the order's own price for the sum of
// the leaves ahead plus what is wanted), so the order named is the one that trades.
Qty Impl::force_fill(std::string_view client_order_id, Qty want) {
  OrderRecord* rec = nullptr;
  for (std::uint32_t s = 0; s < symbols_.size() && rec == nullptr; ++s)
    rec = orders_.by_client_id(kStrategyAccount, s, client_order_id);
  if (rec == nullptr || rec->terminal()) return Qty{};
  const Qty qty = want.is_positive() && want < rec->leaves() ? want : rec->leaves();
  if (!qty.is_positive()) return Qty{};

  const InstrumentId instrument{rec->symbol};
  const std::int64_t order_id = rec->order_id;
  const ClientOrderId internal = rec->internal;
  const Side side = rec->side;
  const Price price = rec->price;
  const Qty executed_before = rec->executed;
  Qty ahead{};
  bool found = false;
  me_->for_each_resting(instrument, side, [&](const SimOrder& o) {
    if (found) return;
    if (o.cl_ord_id == internal && o.account == kStrategyAccount) {
      found = true;
      return;
    }
    ahead += o.leaves();
  });
  if (!found) return Qty{};  // acknowledged but not resting (still in flight, or an IOC)

  NewOrder n;
  n.account = kGeneratorAccount;
  // Well above any id a MarketGenerator issues on the same account.
  n.cl_ord_id = ClientOrderId{(1ULL << 40U) + next_fault_order_++};
  n.instrument = instrument;
  n.side = side == Side::Buy ? Side::Sell : Side::Buy;
  n.type = OrderType::Limit;
  n.tif = TimeInForce::Ioc;
  n.price = price;
  n.qty = ahead + qty;
  begin_request(0);
  static_cast<void>(me_->submit(n, sim_now()));
  const OrderRecord* after = orders_.by_order_id(order_id);
  const Qty filled = after == nullptr ? Qty{} : after->executed - executed_before;
  end_request();
  return filled;
}

// ---- authentication
// -------------------------------------------------------------------------------

std::optional<OpResult> Impl::authenticate(std::string_view api_key,
                                           const ParamList& params,
                                           std::string_view payload,
                                           std::string_view signature,
                                           std::int64_t now_ms,
                                           Account*& out) {
  out = nullptr;
  constexpr std::string_view kBadKey = "Invalid API-key, IP, or permissions for action.";
  if (faults_.auth_fail_next > 0) {
    --faults_.auth_fail_next;
    ++stats_.key_errors;
    return OpResult::error(401, -2015, kBadKey);
  }
  if (api_key.empty()) return OpResult::error(401, -2014, "API-key format invalid.");
  Account* a = find_account(api_key);
  if (a == nullptr) {
    ++stats_.key_errors;
    return OpResult::error(401, -2015, kBadKey);
  }
  if (params.find("timestamp") == nullptr)
    return OpResult::error(400, -1102, mandatory("timestamp"));
  if (signature.empty()) return OpResult::error(400, -1102, mandatory("signature"));
  const bool sig_ok = a->ed25519 != nullptr
                          ? a->ed25519->verify_base64(payload, signature)
                          : verify_hmac_signature(a->api_secret, payload, signature);
  if (!sig_ok) {
    ++stats_.signature_errors;
    return OpResult::error(400, -1022, "Signature for this request is not valid.");
  }
  if (auto err = check_timing(params, now_ms)) return err;
  out = a;
  return std::nullopt;
}

std::optional<OpResult> Impl::check_timing(const ParamList& params, std::int64_t now_ms) {
  const auto ts = parse_int(params.get("timestamp"));
  if (!ts) return OpResult::error(400, -1102, mandatory("timestamp"));
  std::int64_t recv_window = 5000;
  if (params.find("recvWindow") != nullptr) {
    const auto rw = parse_int(params.get("recvWindow"));
    if (!rw || *rw <= 0) return OpResult::error(400, -1102, mandatory("recvWindow"));
    if (*rw > static_cast<std::int64_t>(cfg_.max_recv_window_ms))
      return OpResult::error(
          400,
          -1131,
          "recvWindow must be less than " + std::to_string(cfg_.max_recv_window_ms) + ".");
    recv_window = *rw;
  }
  if (faults_.timestamp_once) {
    faults_.timestamp_once = false;
    ++stats_.timestamp_errors;
    return OpResult::error(400, -1021, "Timestamp for this request is outside of the recvWindow.");
  }
  // rest-api.md "Timing security": timestamp < serverTime + 1000 and serverTime - timestamp <=
  // recvWindow.
  if (*ts >= now_ms + 1000) {
    ++stats_.timestamp_errors;
    return OpResult::error(
        400, -1021, "Timestamp for this request was 1000ms ahead of the server's time.");
  }
  if (now_ms - *ts > recv_window) {
    ++stats_.timestamp_errors;
    return OpResult::error(400, -1021, "Timestamp for this request is outside of the recvWindow.");
  }
  return std::nullopt;
}

// ---- new orders
// -----------------------------------------------------------------------------------

std::optional<OpResult> Impl::parse_new_order(const ParamList& p, NewOrderSpec& out) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  out.symbol = *idx;

  const std::string_view side = p.get("side");
  if (side.empty()) return OpResult::error(400, -1102, mandatory("side"));
  if (side == "BUY") {
    out.side = Side::Buy;
  } else if (side == "SELL") {
    out.side = Side::Sell;
  } else {
    return OpResult::error(
        400, -1100, "Illegal characters found in parameter 'side'; legal range is 'BUY, SELL'.");
  }

  const std::string_view type = p.get("type");
  if (type.empty()) return OpResult::error(400, -1102, mandatory("type"));
  if (type == "LIMIT") {
    out.type = BinanceOrderType::Limit;
  } else if (type == "LIMIT_MAKER") {
    out.type = BinanceOrderType::LimitMaker;
  } else if (type == "MARKET") {
    out.type = BinanceOrderType::Market;
  } else if (type == "STOP_LOSS" || type == "STOP_LOSS_LIMIT" || type == "TAKE_PROFIT" ||
             type == "TAKE_PROFIT_LIMIT") {
    return OpResult::error(400, -1014, "Unsupported order combination.");
  } else {
    return OpResult::error(400, -1116, "Invalid orderType.");
  }

  const std::string_view tif = p.get("timeInForce");
  if (out.type == BinanceOrderType::Limit) {
    if (tif.empty()) return OpResult::error(400, -1102, mandatory("timeInForce"));
    if (tif == "GTC") {
      out.tif = TimeInForce::Gtc;
    } else if (tif == "IOC") {
      out.tif = TimeInForce::Ioc;
    } else if (tif == "FOK") {
      out.tif = TimeInForce::Fok;
    } else {
      return OpResult::error(400, -1115, "Invalid timeInForce.");
    }
  } else if (!tif.empty()) {
    return OpResult::error(400, -1106, "Parameter 'timeInForce' sent when not required.");
  } else {
    out.tif = TimeInForce::Gtc;
  }

  if (p.has("quoteOrderQty") || p.has("icebergQty") || p.has("stopPrice") || p.has("trailingDelta"))
    return OpResult::error(400, -1014, "Unsupported order combination.");
  if (auto err = parse_decimal_param("quantity", p.get("quantity"), out.qty)) return err;
  if (out.type == BinanceOrderType::Market) {
    if (p.has("price"))
      return OpResult::error(400, -1106, "Parameter 'price' sent when not required.");
    out.price = Price{};
  } else if (auto err = parse_decimal_param("price", p.get("price"), out.price)) {
    return err;
  }

  const std::string_view cid = p.get("newClientOrderId");
  if (cid.empty()) {
    out.client_order_id = next_client_id("x-sim-");
  } else if (!valid_client_id(cid)) {
    return OpResult::error(400,
                           -1100,
                           "Illegal characters found in parameter 'newClientOrderId'; legal range "
                           "is '^[\\.A-Z\\:/a-z0-9_-]{1,36}$'.");
  } else {
    out.client_order_id = std::string(cid);
  }

  const std::string_view resp = p.get("newOrderRespType");
  if (resp.empty()) {
    out.resp = out.type == BinanceOrderType::LimitMaker ? RespType::Ack : RespType::Full;
  } else if (resp == "ACK") {
    out.resp = RespType::Ack;
  } else if (resp == "RESULT") {
    out.resp = RespType::Result;
  } else if (resp == "FULL") {
    out.resp = RespType::Full;
  } else {
    return OpResult::error(400,
                           -1100,
                           "Illegal characters found in parameter 'newOrderRespType'; legal range "
                           "is 'ACK, RESULT, FULL'.");
  }
  return std::nullopt;
}

OpResult Impl::submit_new_order(Account& a, const NewOrderSpec& spec, bool test_only) {
  const std::int64_t now_ms = server_ms();
  const SimSymbolConfig& f = symbols_[spec.symbol].cfg;
  auto reject = [this](int status, int code, std::string_view msg) {
    ++stats_.orders_rejected;
    return OpResult::error(status, code, msg);
  };
  if (spec.type != BinanceOrderType::Market &&
      (!spec.price.is_positive() || !on_tick(spec.price, f.tick)))
    return reject(400, -1013, "Filter failure: PRICE_FILTER");
  if (!spec.qty.is_positive() || !on_lot(spec.qty, f.lot) || spec.qty < f.min_qty ||
      (f.max_qty.is_positive() && spec.qty > f.max_qty))
    return reject(400, -1013, "Filter failure: LOT_SIZE");
  const MatchingEngine::TopOfBook top = me_->top_of_book(InstrumentId{spec.symbol});
  Price ref = spec.price;
  if (spec.type == BinanceOrderType::Market)
    ref = spec.side == Side::Buy ? top.ask.price : top.bid.price;
  if (ref.is_positive()) {
    const Notional n = mul(ref, spec.qty);
    if (n < f.min_notional || (f.max_notional.is_positive() && n > f.max_notional))
      return reject(400, -1013, "Filter failure: NOTIONAL");
  }
  if (const OrderRecord* dup = orders_.by_client_id(a.id, spec.symbol, spec.client_order_id);
      dup != nullptr && !dup->terminal())
    return reject(400, -2010, "Duplicate order sent.");
  if (orders_.open_orders(a.id, spec.symbol).size() >= cfg_.max_open_orders_per_symbol)
    return reject(400, -2010, "Filter failure: MAX_NUM_ORDERS");
  if (faults_.reject_next > 0) {
    --faults_.reject_next;
    return reject(400, -2010, "Order rejected by the simulator (fault injection).");
  }

  Balance& base = a.balance(f.base_asset);
  Balance& quote = a.balance(f.quote_asset);
  Qty lock{};
  bool locks_quote = false;
  if (spec.side == Side::Buy) {
    const Qty need = ref.is_positive() ? as_qty(mul(ref, spec.qty)) : Qty{};
    if (quote.free < need) return reject(400, -2010, kInsufficientBalance);
    if (spec.type != BinanceOrderType::Market) {
      lock = need;
      locks_quote = true;
    }
  } else {
    if (base.free < spec.qty) return reject(400, -2010, kInsufficientBalance);
    if (spec.type != BinanceOrderType::Market) lock = spec.qty;
  }
  if (test_only) return OpResult::ok("{}");
  if (a.orders_10s.count(now_ms) + 1 > cfg_.orders_limit_per_10s) {
    OpResult r = reject(429,
                        -1015,
                        "Too many new orders; current limit is " +
                            std::to_string(cfg_.orders_limit_per_10s) + " orders per 10 SECOND.");
    r.retry_after_s = (a.orders_10s.ms_until_reset(now_ms) + 999) / 1000;
    ++stats_.rate_limited;
    return r;
  }
  if (a.orders_1d.count(now_ms) + 1 > cfg_.orders_limit_per_day) {
    OpResult r = reject(429,
                        -1015,
                        "Too many new orders; current limit is " +
                            std::to_string(cfg_.orders_limit_per_day) + " orders per 1 DAY.");
    r.retry_after_s = (a.orders_1d.ms_until_reset(now_ms) + 999) / 1000;
    ++stats_.rate_limited;
    return r;
  }
  static_cast<void>(a.orders_10s.add(1, now_ms));
  static_cast<void>(a.orders_1d.add(1, now_ms));

  OrderRecord rec;
  rec.order_id = next_order_id_++;
  rec.internal = ClientOrderId{next_internal_++};
  rec.symbol = spec.symbol;
  rec.account = a.id;
  rec.client_order_id = spec.client_order_id;
  rec.side = spec.side;
  rec.type = spec.type;
  rec.tif = spec.tif;
  rec.price = spec.price;
  rec.orig_qty = spec.qty;
  rec.time_ms = now_ms;
  rec.update_ms = now_ms;
  rec.locked = lock;
  rec.locks_quote = locks_quote;
  const std::int64_t order_id = rec.order_id;
  const ClientOrderId internal = rec.internal;
  orders_.insert(std::move(rec));
  if (lock.is_positive()) {
    Balance& b = locks_quote ? quote : base;
    b.free -= lock;
    b.locked += lock;
    a.balance_dirty = true;
  }

  NewOrder n;
  n.account = a.id;
  n.cl_ord_id = internal;
  n.instrument = InstrumentId{spec.symbol};
  n.side = spec.side;
  n.type = core_type(spec.type);
  n.tif = spec.type == BinanceOrderType::Market ? TimeInForce::Ioc : spec.tif;
  n.price = spec.price;
  n.qty = spec.qty;
  capture_order_id_ = order_id;
  captured_fills_.clear();
  const SubmitResult res = me_->submit(n, sim_now());
  capture_order_id_ = 0;
  OrderRecord* r = orders_.by_order_id(order_id);
  if (r == nullptr)
    return reject(500, -1000, "An unknown error occurred while processing the request.");
  if (!res.accepted() && !r->acked) {
    release_lock(*r);
    r->status = BinanceOrderStatus::Rejected;
    if (res.reason == RejectReason::PostOnlyWouldCross)
      return reject(400, -2010, "Order would immediately match and take.");
    return reject(400, -2010, "Order rejected: " + std::string(to_string(res.reason)) + ".");
  }
  ++stats_.orders_accepted;
  ++stats_.orders_since_mark;
  if (!seen_client_ids_.insert(spec.client_order_id).second) ++stats_.duplicate_client_order_ids;
  stats_.max_order_qty = max(stats_.max_order_qty, spec.qty);

  std::string body;
  const OrderView v = view_of(*r);
  switch (spec.resp) {
    case RespType::Ack:
      append_ack_result(body, v, now_ms);
      break;
    case RespType::Result:
      append_result_result(body, v, now_ms, {}, false);
      break;
    case RespType::Full: {
      std::vector<FillView> fills;
      fills.reserve(captured_fills_.size());
      for (const CapturedFill& c : captured_fills_)
        fills.push_back(FillView{c.price, c.qty, c.commission, f.quote_asset, c.trade_id});
      append_result_result(body, v, now_ms, fills, true);
      break;
    }
  }
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_place(Account& a, const ParamList& p, bool test_only) {
  NewOrderSpec spec;
  if (auto err = parse_new_order(p, spec)) {
    ++stats_.orders_rejected;
    return *err;
  }
  return submit_new_order(a, spec, test_only);
}

// ---- cancels
// --------------------------------------------------------------------------------------

CancelOutcome Impl::cancel_order(Account& a,
                                 std::uint32_t symbol,
                                 std::string_view order_id,
                                 std::string_view orig_client_id,
                                 std::string_view new_client_id) {
  CancelOutcome out;
  OrderRecord* r = nullptr;
  if (!order_id.empty()) {
    const auto id = parse_int(order_id);
    if (!id) {
      out.code = -1102;
      out.msg = mandatory("orderId");
      return out;
    }
    r = orders_.by_order_id(*id);
    if (r != nullptr && !orig_client_id.empty() && r->client_order_id != orig_client_id)
      r = nullptr;
  } else if (!orig_client_id.empty()) {
    r = orders_.by_client_id(a.id, symbol, orig_client_id);
  }
  if (r == nullptr || r->account != a.id || r->symbol != symbol || r->terminal()) {
    ++stats_.cancel_rejects;
    return out;
  }
  if (!new_client_id.empty() && !valid_client_id(new_client_id)) {
    out.code = -1100;
    out.msg = "Illegal characters found in parameter 'newClientOrderId'.";
    return out;
  }
  const std::string cancel_id =
      new_client_id.empty() ? next_client_id("x-sim-cxl-") : std::string(new_client_id);
  cancel_client_id_ = cancel_id;
  const bool ok = me_->cancel(a.id, r->internal, sim_now());
  cancel_client_id_.clear();
  if (!ok) {
    ++stats_.cancel_rejects;
    return out;
  }
  ++stats_.cancels;
  ++stats_.cancels_since_mark;
  out.ok = true;
  out.status = 200;
  out.code = 0;
  out.msg.clear();
  append_cancel_result(out.result, view_of(*r), cancel_id, server_ms());
  return out;
}

OpResult Impl::op_cancel(Account& a, const ParamList& p) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  const std::string_view order_id = p.get("orderId");
  const std::string_view orig = p.get("origClientOrderId");
  if (order_id.empty() && orig.empty())
    return OpResult::error(
        400,
        -1102,
        "Param 'origClientOrderId' or 'orderId' must be sent, but both were empty/null!");
  CancelOutcome c = cancel_order(a, *idx, order_id, orig, p.get("newClientOrderId"));
  if (!c.ok) return OpResult::error(c.status, c.code, c.msg);
  return OpResult::ok(std::move(c.result));
}

OpResult Impl::op_cancel_replace(Account& a, const ParamList& p) {
  const std::string_view mode = p.get("cancelReplaceMode");
  if (mode.empty()) return OpResult::error(400, -1102, mandatory("cancelReplaceMode"));
  const bool stop_on_failure = mode == "STOP_ON_FAILURE";
  if (!stop_on_failure && mode != "ALLOW_FAILURE")
    return OpResult::error(400,
                           -1100,
                           "Illegal characters found in parameter 'cancelReplaceMode'; legal "
                           "range is 'STOP_ON_FAILURE, ALLOW_FAILURE'.");
  NewOrderSpec spec;
  if (auto err = parse_new_order(p, spec)) return *err;
  const std::string_view cancel_id = p.get("cancelOrderId");
  const std::string_view cancel_orig = p.get("cancelOrigClientOrderId");
  if (cancel_id.empty() && cancel_orig.empty())
    return OpResult::error(400,
                           -1102,
                           "Param 'cancelOrigClientOrderId' or 'cancelOrderId' must be sent, but "
                           "both were empty/null!");

  CancelOutcome c =
      cancel_order(a, spec.symbol, cancel_id, cancel_orig, p.get("cancelNewClientOrderId"));
  std::string cancel_json;
  if (c.ok) {
    cancel_json = std::move(c.result);
  } else {
    append_error(cancel_json, c.code, c.msg);
  }
  auto data = [&](std::string_view new_result, std::string_view new_json) {
    std::string d;
    JsonObjectWriter w(d);
    w.str("cancelResult", c.ok ? "SUCCESS" : "FAILURE")
        .str("newOrderResult", new_result)
        .raw("cancelResponse", cancel_json)
        .raw("newOrderResponse", new_json);
    w.close();
    return d;
  };
  if (!c.ok && stop_on_failure) {
    return OpResult::error_data(
        400, -2022, "Order cancel-replace failed.", data("NOT_ATTEMPTED", "null"));
  }
  const OpResult n = submit_new_order(a, spec, false);
  if (c.ok && !n.is_error) {
    ++stats_.replaces;
    return OpResult::ok(data("SUCCESS", n.body));
  }
  if (c.ok || !n.is_error) {
    return OpResult::error_data(409,
                                -2021,
                                "Order cancel-replace partially failed.",
                                data(n.is_error ? "FAILURE" : "SUCCESS", n.body));
  }
  return OpResult::error_data(400, -2022, "Order cancel-replace failed.", data("FAILURE", n.body));
}

OpResult Impl::op_amend(Account& a, const ParamList& p) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  const std::string_view order_id = p.get("orderId");
  const std::string_view orig = p.get("origClientOrderId");
  if (order_id.empty() && orig.empty())
    return OpResult::error(
        400,
        -1102,
        "Param 'origClientOrderId' or 'orderId' must be sent, but both were empty/null!");
  Qty new_qty{};
  if (auto err = parse_decimal_param("newQty", p.get("newQty"), new_qty)) return *err;
  OrderRecord* r = nullptr;
  if (!order_id.empty()) {
    if (const auto id = parse_int(order_id)) r = orders_.by_order_id(*id);
  } else {
    r = orders_.by_client_id(a.id, *idx, orig);
  }
  if (r == nullptr || r->account != a.id || r->symbol != *idx || r->terminal())
    return OpResult::error(400, -2013, "Order does not exist.");
  const SimSymbolConfig& f = symbols_[r->symbol].cfg;
  if (!new_qty.is_positive() || !on_lot(new_qty, f.lot) || new_qty < f.min_qty ||
      new_qty <= r->executed)
    return OpResult::error(400, -1013, "Filter failure: LOT_SIZE");
  if (new_qty >= r->orig_qty)
    return OpResult::error(400, -2038, "Order amend (quantity increase) is not supported.");
  std::string new_client(p.get("newClientOrderId"));
  if (new_client.empty()) {
    new_client = r->client_order_id;
  } else if (!valid_client_id(new_client)) {
    return OpResult::error(400, -1100, "Illegal characters found in parameter 'newClientOrderId'.");
  } else if (new_client != r->client_order_id &&
             orders_.by_client_id(a.id, r->symbol, new_client) != nullptr) {
    return OpResult::error(400, -2010, "Duplicate order sent.");
  }
  const Qty new_leaves = new_qty - r->executed;
  const ClientOrderId new_internal{next_internal_++};
  amend_in_progress_ = true;
  const SubmitResult res =
      me_->replace(a.id, r->internal, new_internal, r->price, new_leaves, sim_now());
  amend_in_progress_ = false;
  if (!res.accepted()) return OpResult::error(400, -2010, "Order amend rejected.");
  const std::string orig_client = r->client_order_id;
  orders_.rekey_internal(*r, new_internal);
  if (new_client != orig_client) orders_.rekey_client(*r, new_client);
  r->orig_qty = new_qty;
  r->update_ms = server_ms();
  if (r->locked.is_positive()) {
    const Qty target = r->locks_quote ? as_qty(mul(r->price, new_leaves)) : new_leaves;
    if (target < r->locked) {
      const Qty release = r->locked - target;
      Balance& b = a.balance(r->locks_quote ? f.quote_asset : f.base_asset);
      b.locked -= release;
      b.free += release;
      r->locked = target;
      a.balance_dirty = true;
    }
  }
  ++stats_.amends;
  emit_exec(*r, ExecType::Replaced, {}, orig_client);
  std::string body;
  append_amend_result(body, view_of(*r), orig_client, server_ms(), next_execution_id_++);
  return OpResult::ok(std::move(body));
}

// ---- queries
// ----------------------------------------------------------------------------------------

OpResult Impl::op_query_order(Account& a, const ParamList& p) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  OrderRecord* r = nullptr;
  if (const auto id = parse_int(p.get("orderId"))) {
    r = orders_.by_order_id(*id);
  } else if (p.has("origClientOrderId")) {
    r = orders_.by_client_id(a.id, *idx, p.get("origClientOrderId"));
  } else {
    return OpResult::error(
        400,
        -1102,
        "Param 'origClientOrderId' or 'orderId' must be sent, but both were empty/null!");
  }
  if (r == nullptr || r->account != a.id || r->symbol != *idx)
    return OpResult::error(400, -2013, "Order does not exist.");
  std::string body;
  append_order_object(body, view_of(*r));
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_open_orders(Account& a, const ParamList& p) {
  std::int64_t symbol = -1;
  if (p.has("symbol")) {
    const auto idx = find_symbol(p.get("symbol"));
    if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
    symbol = *idx;
  }
  ++stats_.open_orders_queries;
  ++stats_.open_orders_queries_since_mark;
  std::string body = "[";
  bool first = true;
  for (const OrderRecord* r : orders_.open_orders(a.id, symbol)) {
    if (!first) body.push_back(',');
    first = false;
    append_order_object(body, view_of(*r));
  }
  body.push_back(']');
  return OpResult::ok(std::move(body));
}

// GET /api/v3/myTrades: the account's executions on one symbol, oldest trade id first. `symbol` is
// mandatory and `fromId` (trades from that id on) cannot be combined with a time range, which is
// what rest-api.md's list of legal parameter combinations says. `limit` defaults to 500, caps at
// 1000. Records live for the whole run, so a trade of an order the venue has long forgotten is
// still reported - that is what makes it a recovery path rather than another view of the open
// orders.
OpResult Impl::op_my_trades(Account& a, const ParamList& p) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  const std::int64_t from_id = parse_int(p.get("fromId")).value_or(0);
  const std::int64_t start_ms = parse_int(p.get("startTime")).value_or(0);
  const std::int64_t end_ms = parse_int(p.get("endTime")).value_or(0);
  if (from_id > 0 && (start_ms > 0 || end_ms > 0))
    return OpResult::error(400, -1128, "Combination of optional parameters invalid.");
  if (start_ms > 0 && end_ms > 0 && end_ms - start_ms > 24LL * 3600 * 1000)
    return OpResult::error(400, -1127, "More than 24 hours between startTime and endTime.");
  std::int64_t limit = parse_int(p.get("limit")).value_or(500);
  if (limit <= 0 || limit > 1000) limit = 1000;
  ++stats_.my_trades_queries;
  ++stats_.my_trades_queries_since_mark;
  std::string body = "[";
  bool first = true;
  std::int64_t n = 0;
  for (const TradeRecord& t : trades_) {
    if (t.account != a.id || t.symbol != *idx) continue;
    if (from_id > 0 && t.id < from_id) continue;
    if (start_ms > 0 && t.time_ms < start_ms) continue;
    if (end_ms > 0 && t.time_ms > end_ms) continue;
    if (n++ >= limit) break;
    if (!first) body.push_back(',');
    first = false;
    JsonObjectWriter w(body);
    w.str("symbol", symbols_[t.symbol].cfg.symbol)
        .num("id", t.id)
        .num("orderId", t.order_id)
        .num("orderListId", -1)
        .dec("price", t.price)
        .dec("qty", t.qty)
        .dec("quoteQty", mul(t.price, t.qty))
        .dec("commission", t.commission)
        .str("commissionAsset", symbols_[t.symbol].cfg.quote_asset)
        .num("time", t.time_ms)
        .boolean("isBuyer", t.is_buyer)
        .boolean("isMaker", t.is_maker)
        .boolean("isBestMatch", true);
    w.close();
  }
  body.push_back(']');
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_cancel_all(Account& a, const ParamList& p) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  ++stats_.cancel_all_requests;
  ++stats_.cancel_all_since_mark;
  const std::vector<OrderRecord*> open = orders_.open_orders(a.id, *idx);
  if (open.empty()) return OpResult::error(400, -2011, kUnknownOrder);
  std::string body = "[";
  bool first = true;
  for (const OrderRecord* r : open) {
    const std::string id = std::to_string(r->order_id);
    CancelOutcome c = cancel_order(a, *idx, id, {}, {});
    if (!c.ok) continue;
    if (!first) body.push_back(',');
    first = false;
    body += c.result;
  }
  body.push_back(']');
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_account(Account& a) {
  std::vector<BalanceView> views;
  views.reserve(a.balances.size());
  for (const auto& [asset, b] : a.balances) views.push_back(BalanceView{asset, b.free, b.locked});
  std::string body;
  append_account_info(body, server_ms(), fees_.maker_cbps() / 100, fees_.taker_cbps() / 100, views);
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_exchange_info(const ParamList& p) {
  std::vector<std::uint32_t> selected;
  selected.reserve(symbols_.size());
  if (p.has("symbol")) {
    const auto idx = find_symbol(p.get("symbol"));
    if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
    selected.push_back(*idx);
  } else if (p.has("symbols")) {
    for (const std::string& s : parse_symbol_list(p.get("symbols"))) {
      const auto idx = find_symbol(s);
      if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
      selected.push_back(*idx);
    }
  } else {
    for (std::size_t i = 0; i < symbols_.size(); ++i)
      selected.push_back(static_cast<std::uint32_t>(i));
  }
  std::vector<SymbolInfoView> views;
  views.reserve(selected.size());
  for (const std::uint32_t i : selected) {
    const SimSymbolConfig& s = symbols_[i].cfg;
    views.push_back(SymbolInfoView{s.symbol,
                                   s.base_asset,
                                   s.quote_asset,
                                   s.tick,
                                   s.lot,
                                   s.min_qty,
                                   s.max_qty,
                                   s.min_notional,
                                   s.max_notional});
  }
  const RateLimitView limits[] = {
      {"REQUEST_WEIGHT", "MINUTE", 1, cfg_.weight_limit_per_minute, -1},
      {"ORDERS", "SECOND", 10, cfg_.orders_limit_per_10s, -1},
      {"ORDERS", "DAY", 1, cfg_.orders_limit_per_day, -1},
      {"RAW_REQUESTS", "MINUTE", 5, 61'000, -1},
  };
  std::string body;
  append_exchange_info(body, server_ms(), limits, views);
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_depth(const ParamList& p) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  std::int64_t limit = 100;
  if (p.has("limit")) {
    const auto l = parse_int(p.get("limit"));
    if (!l || *l <= 0)
      return OpResult::error(400, -1100, "Illegal characters found in parameter 'limit'.");
    limit = std::min<std::int64_t>(*l, 5000);
  }
  ++stats_.depth_snapshots;
  const auto n = static_cast<std::size_t>(limit);
  std::string body;
  if (cfg_.snapshot_at_flush) {
    const SymbolRuntime& rt = symbols_[*idx];
    append_depth_snapshot(
        body,
        rt.flushed_update_id,
        std::span<const Level>(rt.flushed_bids.data(), std::min(n, rt.flushed_nb)),
        std::span<const Level>(rt.flushed_asks.data(), std::min(n, rt.flushed_na)));
    return OpResult::ok(std::move(body));
  }
  const InstrumentId inst{*idx};
  const std::size_t nb = me_->l2_snapshot(inst, Side::Buy, level_buf_.data(), n);
  const std::size_t na = me_->l2_snapshot(inst, Side::Sell, level_buf_.data() + nb, n);
  append_depth_snapshot(body,
                        me_->update_id(inst),
                        std::span<const Level>(level_buf_.data(), nb),
                        std::span<const Level>(level_buf_.data() + nb, na));
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_book_ticker(const ParamList& p) {
  const std::string_view symbol = p.get("symbol");
  if (symbol.empty()) return OpResult::error(400, -1102, mandatory("symbol"));
  const auto idx = find_symbol(symbol);
  if (!idx) return OpResult::error(400, -1121, "Invalid symbol.");
  const MatchingEngine::TopOfBook top = me_->top_of_book(InstrumentId{*idx});
  std::string body;
  append_rest_book_ticker(body, symbols_[*idx].cfg.symbol, top.bid, top.ask);
  return OpResult::ok(std::move(body));
}

OpResult Impl::op_listen_key(RestEndpoint ep, const net::HttpRequest& req, const ParamList& p) {
  const std::string_view api_key = req.header("X-MBX-APIKEY");
  if (api_key.empty()) return OpResult::error(401, -2014, "API-key format invalid.");
  Account* a = find_account(api_key);
  if (a == nullptr)
    return OpResult::error(401, -2015, "Invalid API-key, IP, or permissions for action.");
  const std::int64_t now_ms = server_ms();
  if (ep == RestEndpoint::ListenKeyCreate) {
    std::string key;
    for (const auto& [k, lk] : listen_keys_) {
      if (lk.account == a->id) key = k;
    }
    if (key.empty()) key = next_client_id("simlistenkey");
    listen_keys_[key] =
        ListenKey{a->id, now_ms + static_cast<std::int64_t>(cfg_.listen_key_validity_ms)};
    std::string body;
    JsonObjectWriter w(body);
    w.str("listenKey", key);
    w.close();
    return OpResult::ok(std::move(body));
  }
  const std::string_view key = p.get("listenKey");
  if (key.empty()) return OpResult::error(400, -1102, mandatory("listenKey"));
  const auto it = listen_keys_.find(key);
  if (it == listen_keys_.end())
    return OpResult::error(400, -1125, "This listenKey does not exist.");
  if (ep == RestEndpoint::ListenKeyKeepalive) {
    it->second.expires_ms = now_ms + static_cast<std::int64_t>(cfg_.listen_key_validity_ms);
    return OpResult::ok("{}");
  }
  const std::string closed = it->first;
  listen_keys_.erase(it);
  std::vector<net::WsSession*> targets;
  targets.reserve(sessions_.size());
  for (const auto& [ptr, st] : sessions_) {
    if (st.kind == SessionKind::ListenKey && st.listen_key == closed) targets.push_back(ptr);
  }
  for (net::WsSession* s : targets) close_session(s);
  return OpResult::ok("{}");
}

}  // namespace fastmm::sim::server
