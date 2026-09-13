// SimExchangeServer lifecycle, WebSocket sessions, market-data fan-out, fault injection and
// the public (thread-safe) API.
#include "server_impl.hpp"

#include "fastmm/core/log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace fastmm::sim::server {

namespace {

constexpr std::int64_t kHousekeepingNs = 1'000'000'000;

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

MarketGeneratorParams generator_params(const SimServerConfig& c, const SimSymbolConfig& s) {
  MarketGeneratorParams p = c.generator;
  p.start_mid = s.start_mid;
  p.tick = s.tick;
  p.lot = s.lot;
  return p;
}

}  // namespace

using Impl = SimExchangeServer::Impl;

// ---- construction ---------------------------------------------------------------------------

Impl::Impl(SimServerConfig c)
    : cfg_(std::move(c)), fees_(FeeModel::from_bps(cfg_.maker_bps, cfg_.taker_bps)) {
  if (cfg_.symbols.empty()) throw std::invalid_argument("SimExchangeServer: no symbols configured");
  if (cfg_.symbols.size() > 64) throw std::invalid_argument("SimExchangeServer: too many symbols");
  if (cfg_.api_key.empty() || cfg_.api_secret.empty())
    throw std::invalid_argument("SimExchangeServer: api_key/api_secret must not be empty");
  if (cfg_.depth_update_ms == 0 || cfg_.driver_tick_ms == 0)
    throw std::invalid_argument("SimExchangeServer: depth_update_ms/driver_tick_ms must be > 0");
  for (const SimSymbolConfig& s : cfg_.symbols) {
    if (s.symbol.empty() || s.base_asset.empty() || s.quote_asset.empty() ||
        !s.tick.is_positive() || !s.lot.is_positive() || !s.start_mid.is_positive())
      throw std::invalid_argument("SimExchangeServer: symbol '" + s.symbol +
                                  "' needs base/quote assets and tick, lot, start_mid > 0");
    SymbolRuntime rt;
    rt.cfg = s;
    rt.lower = to_lower(s.symbol);
    rt.flushed_bids.resize(kMaxSimLevels);
    rt.flushed_asks.resize(kMaxSimLevels);
    symbols_.push_back(std::move(rt));
  }
  start_mono_ns_ = net::Reactor::now_ns();
  start_epoch_ns_ = cfg_.start_time_ms > 0 ? cfg_.start_time_ms * kNsPerMs : wall_now().ns;
  clock_offset_ms_ = cfg_.clock_offset_ms;
  faults_.delay_ack_ms = cfg_.faults.delay_ack_ms;
  faults_.reject_next = cfg_.faults.reject_next_orders;
  faults_.rate_limit_next = cfg_.faults.rate_limit_next;

  Account acct;
  acct.id = kStrategyAccount;
  acct.api_key = cfg_.api_key;
  acct.api_secret = cfg_.api_secret;
  if (cfg_.balances.empty()) {
    for (const SymbolRuntime& s : symbols_) {
      acct.balance(s.cfg.base_asset).free = Qty::from_int(100);
      acct.balance(s.cfg.quote_asset).free = Qty::from_int(10'000'000);
    }
  } else {
    for (const SimBalanceConfig& b : cfg_.balances) acct.balance(b.asset).free = b.amount;
  }
  accounts_.push_back(std::move(acct));

  const Timestamp now = sim_now();
  me_ = std::make_unique<MatchingEngine>(symbols_.size(), this);
  // exchangeInfo advertises EXPIRE_MAKER as the default self-trade prevention mode.
  me_->set_stp(kStrategyAccount, StpMode::CancelMaker);
  MdAggregatorConfig ac;
  ac.interval = milliseconds(static_cast<std::int64_t>(cfg_.depth_update_ms));
  ac.book_ticker = true;
  agg_ = std::make_unique<MdAggregator>(symbols_.size(), *me_, ac, now);
  for (std::size_t i = 0; i < symbols_.size(); ++i) {
    auto gen = std::make_unique<MarketGenerator>(generator_params(cfg_, symbols_[i].cfg),
                                                 cfg_.seed + 0x9E37'79B9'7F4A'7C15ULL * i,
                                                 InstrumentId{static_cast<std::uint32_t>(i)},
                                                 now);
    if (cfg_.generator_enabled && cfg_.seed_levels > 0) gen->seed_book(*me_, cfg_.seed_levels, now);
    generators_.push_back(std::move(gen));
  }
  // MdAggregator's first flush per instrument is an (unpublished) snapshot that would swallow
  // every update id since construction: a client that took its REST snapshot in that window
  // would see the first depthUpdate start past lastUpdateId + 1. Take that snapshot now, so
  // every published flush is a contiguous delta.
  for (std::size_t i = 0; i < symbols_.size(); ++i) {
    agg_->emit_snapshot(
        InstrumentId{static_cast<std::uint32_t>(i)},
        now,
        [](void*, EventHeader&, Timestamp) {},
        nullptr);
    capture_flushed_book(static_cast<std::uint32_t>(i));
  }
  level_buf_.resize(10'000);
}

Impl::~Impl() {
  stop_thread();
}

net::HttpServerConfig Impl::http_config() const {
  net::HttpServerConfig c;
  c.recv_capacity = std::size_t{256} * 1024;
  c.send_capacity = std::size_t{2} * 1024 * 1024;  // depth?limit=5000 is ~250 KB
  c.max_body_bytes = std::size_t{64} * 1024;
  c.ws.recv_capacity = std::size_t{256} * 1024;
  c.ws.send_capacity = std::size_t{4} * 1024 * 1024;
  return c;
}

bool Impl::listen() {
  if (listen_mono_ns_ != 0) return true;
  if (cfg_.port >= 0) {
    const auto addr = net::SockAddr::from_ip(cfg_.bind_host, static_cast<std::uint16_t>(cfg_.port));
    if (!addr) {
      last_error_ = "bad bind address '" + cfg_.bind_host + "'";
      return false;
    }
    plain_ = std::make_unique<net::HttpServer<net::PlainStream>>(
        reactor_, [](net::TcpSocket&& s) { return net::PlainStream(std::move(s)); }, http_config());
    install_routes(*plain_);
    plain_->set_ws_handler(this);
    if (!plain_->listen(*addr)) {
      const int err = errno;
      last_error_ = "cannot listen on " + cfg_.bind_host + ":" + std::to_string(cfg_.port) + ": " +
                    std::strerror(err);
      plain_.reset();
      return false;
    }
    port_ = plain_->port();
  }
  if (cfg_.tls_port >= 0) {
    const auto addr =
        net::SockAddr::from_ip(cfg_.bind_host, static_cast<std::uint16_t>(cfg_.tls_port));
    if (!addr) {
      last_error_ = "bad bind address '" + cfg_.bind_host + "'";
      return false;
    }
    try {
      tls_ctx_ =
          std::make_unique<net::TlsContext>(net::TlsContext::server(cfg_.tls_cert, cfg_.tls_key));
    } catch (const std::exception& e) {
      last_error_ =
          "TLS certificate/key (" + cfg_.tls_cert + ", " + cfg_.tls_key + "): " + e.what();
      return false;
    }
    net::TlsContext* ctx = tls_ctx_.get();
    tls_ = std::make_unique<net::HttpServer<TlsServerStream>>(
        reactor_,
        [ctx](net::TcpSocket&& s) { return TlsServerStream(*ctx, net::PlainStream(std::move(s))); },
        http_config());
    install_routes(*tls_);
    tls_->set_ws_handler(this);
    if (!tls_->listen(*addr)) {
      const int err = errno;
      last_error_ = "cannot listen on " + cfg_.bind_host + ":" + std::to_string(cfg_.tls_port) +
                    ": " + std::strerror(err);
      tls_.reset();
      return false;
    }
    tls_port_ = tls_->port();
  }
  listen_mono_ns_ = net::Reactor::now_ns();
  reactor_.add_timer_after(static_cast<std::int64_t>(cfg_.driver_tick_ms) * kNsPerMs,
                           [this] { on_driver(); });
  reactor_.add_timer_after(kHousekeepingNs, [this] { on_housekeeping(); });
  if (cfg_.ping_interval_ms > 0)
    reactor_.add_timer_after(static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs,
                             [this] { on_ping_timer(); });
  return true;
}

void Impl::start_thread() {
  if (running_.load(std::memory_order_acquire)) return;
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread([this] { reactor_.run(stop_); });
  thread_id_ = thread_.get_id();
  running_.store(true, std::memory_order_release);
}

void Impl::stop_thread() {
  if (!running_.load(std::memory_order_acquire)) return;
  stop_.store(true, std::memory_order_release);
  reactor_.wake();
  thread_.join();
  running_.store(false, std::memory_order_release);
  thread_id_ = std::thread::id{};
  reactor_.run_once(0);  // tasks posted while the thread was stopping
}

// ---- statistics -----------------------------------------------------------------------------

SimServerStats Impl::snapshot_stats() {
  SimServerStats s = stats_;
  s.uptime_ms = uptime_ms();
  s.http_connections =
      (plain_ ? plain_->http_connections() : 0U) + (tls_ ? tls_->http_connections() : 0U);
  s.md_sessions = 0;
  s.api_sessions = 0;
  s.user_subscriptions = 0;
  for (const auto& [ptr, st] : sessions_) {
    switch (st.kind) {
      case SessionKind::MarketData:
        ++s.md_sessions;
        break;
      case SessionKind::WsApi:
        ++s.api_sessions;
        s.user_subscriptions += st.user_subs.size();
        break;
      case SessionKind::ListenKey:
        ++s.user_subscriptions;
        break;
    }
  }
  s.open_orders = orders_.open_count(kStrategyAccount);
  const MatchingEngine::TopOfBook top = me_->top_of_book(InstrumentId{0});
  s.best_bid = top.bid;
  s.best_ask = top.ask;
  if (top.bid.price.is_positive() && top.ask.price.is_positive()) {
    const Price mid =
        Price::from_raw(top.bid.price.raw + (top.ask.price.raw - top.bid.price.raw) / 2);
    s.pnl = s.cash_flow + mul(mid, s.position);
  }
  s.last_update_id = me_->update_id(InstrumentId{0});
  return s;
}

void Impl::mark() {
  stats_.min_open_orders_since_mark = orders_.open_count(kStrategyAccount);
  stats_.orders_since_mark = 0;
  stats_.cancels_since_mark = 0;
  stats_.fills_since_mark = 0;
  stats_.cancel_all_since_mark = 0;
  stats_.open_orders_queries_since_mark = 0;
  stats_.md_sessions_opened_since_mark = 0;
  stats_.api_sessions_opened_since_mark = 0;
}

// ---- timers ---------------------------------------------------------------------------------

void Impl::on_driver() {
  reactor_.add_timer_after(static_cast<std::int64_t>(cfg_.driver_tick_ms) * kNsPerMs,
                           [this] { on_driver(); });
  const Timestamp now = sim_now();
  begin_request(0);
  if (cfg_.generator_enabled) {
    for (auto& g : generators_) {
      for (int guard = 0; guard < 100'000 && g->next_ts() <= now; ++guard) {
        g->step(*me_);
        ++stats_.generator_actions;
      }
    }
  }
  if (agg_->next_flush_ts() <= now) agg_->flush(now, &Impl::md_trampoline, this);
  end_request();
  run_scheduled_faults();
}

void Impl::on_housekeeping() {
  reactor_.add_timer_after(kHousekeepingNs, [this] { on_housekeeping(); });
  const std::int64_t now_ms = server_ms();
  std::vector<std::string> expired;
  expired.reserve(listen_keys_.size());
  for (const auto& [key, lk] : listen_keys_) {
    if (lk.expires_ms <= now_ms) expired.push_back(key);
  }
  for (const std::string& key : expired) {
    std::string event;
    append_listen_key_expired(event, now_ms, key);
    std::vector<std::pair<net::WsSession*, std::uint64_t>> targets;
    targets.reserve(sessions_.size());
    for (const auto& [ptr, st] : sessions_) {
      if (st.kind == SessionKind::ListenKey && st.listen_key == key)
        targets.emplace_back(ptr, st.token);
    }
    for (const auto& [ptr, token] : targets) {
      deliver(ptr, token, event);
      close_session(ptr);
    }
    listen_keys_.erase(key);
  }
}

void Impl::on_ping_timer() {
  reactor_.add_timer_after(static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs,
                           [this] { on_ping_timer(); });
  const std::int64_t now = net::Reactor::now_ns();
  const std::int64_t timeout_ns = static_cast<std::int64_t>(cfg_.pong_timeout_ms) * kNsPerMs;
  std::vector<std::pair<net::WsSession*, bool>> targets;
  targets.reserve(sessions_.size());
  for (const auto& [ptr, st] : sessions_)
    targets.emplace_back(ptr, cfg_.pong_timeout_ms > 0 && now - st.last_rx_ns > timeout_ns);
  for (const auto& [ptr, timed_out] : targets) {
    if (session_state(ptr) == nullptr) continue;
    if (timed_out) {
      FASTMM_LOG_WARN("sim-exchange: closing WebSocket session without pong for {} ms",
                      cfg_.pong_timeout_ms);
      close_session(ptr);
    } else {
      ptr->send_ping();
    }
  }
}

void Impl::run_scheduled_faults() {
  const FaultConfig& f = cfg_.faults;
  const std::int64_t up = uptime_ms();
  auto due = [up](std::uint32_t after, bool& done) {
    if (after == 0 || done || up < static_cast<std::int64_t>(after)) return false;
    done = true;
    return true;
  };
  if (due(f.drop_md_after_ms, faults_.drop_md_done)) {
    FASTMM_LOG_WARN("sim-exchange fault: dropping market-data connections");
    drop_market_data();
  }
  if (due(f.drop_ws_api_after_ms, faults_.drop_api_done)) {
    FASTMM_LOG_WARN("sim-exchange fault: dropping order-entry WS API connections");
    drop_ws_api(false);
  }
  if (due(f.skip_depth_update_after_ms, faults_.skip_depth_done)) {
    FASTMM_LOG_WARN("sim-exchange fault: skipping the next depthUpdate");
    faults_.skip_depth = true;
  }
  if (due(f.timestamp_error_after_ms, faults_.timestamp_done)) {
    FASTMM_LOG_WARN("sim-exchange fault: next signed request answers -1021");
    faults_.timestamp_once = true;
  }
  if (due(f.rest_unresponsive_after_ms, faults_.rest_start_done)) {
    FASTMM_LOG_WARN("sim-exchange fault: REST stops answering");
    faults_.rest_unresponsive = true;
  }
  if (faults_.rest_start_done && !faults_.rest_end_done && f.rest_unresponsive_for_ms > 0 &&
      up >= static_cast<std::int64_t>(f.rest_unresponsive_after_ms) +
                static_cast<std::int64_t>(f.rest_unresponsive_for_ms)) {
    faults_.rest_end_done = true;
    faults_.rest_unresponsive = false;
    FASTMM_LOG_WARN("sim-exchange fault: REST answers again");
  }
}

// ---- WebSocket sessions -----------------------------------------------------------------------

SessionState* Impl::session_state(net::WsSession* s) noexcept {
  const auto it = sessions_.find(s);
  return it == sessions_.end() ? nullptr : &it->second;
}

bool Impl::parse_stream_name(std::string_view name, StreamSub& out) const {
  const std::size_t at = name.find('@');
  if (at == std::string_view::npos || at == 0) return false;
  const std::string sym = to_lower(name.substr(0, at));
  const std::string_view suffix = name.substr(at);
  std::optional<std::uint32_t> idx;
  for (std::size_t i = 0; i < symbols_.size(); ++i) {
    if (symbols_[i].lower == sym) idx = static_cast<std::uint32_t>(i);
  }
  if (!idx) return false;
  if (suffix == "@depth" || suffix == "@depth@100ms" || suffix == "@depth@1000ms") {
    out.kind = StreamKind::Depth;
  } else if (suffix == "@bookTicker") {
    out.kind = StreamKind::Ticker;
  } else if (suffix == "@trade") {
    out.kind = StreamKind::Trade;
  } else {
    return false;
  }
  out.symbol = *idx;
  out.name = std::string(name);
  return true;
}

bool Impl::accept_upgrade(std::string_view path, std::string_view) {
  if (path == "/ws-api/v3" || path == "/stream" || path == "/ws") return true;
  if (!path.starts_with("/ws/")) return false;
  std::string_view rest = path.substr(4);
  if (listen_keys_.find(rest) != listen_keys_.end()) return true;
  if (rest.empty()) return false;
  while (!rest.empty()) {
    const std::size_t slash = rest.find('/');
    StreamSub sub;
    if (!parse_stream_name(slash == std::string_view::npos ? rest : rest.substr(0, slash), sub))
      return false;
    if (slash == std::string_view::npos) break;
    rest.remove_prefix(slash + 1);
  }
  return true;
}

void Impl::on_open(net::WsSession& s) {
  SessionState st;
  st.token = next_token_++;
  st.last_rx_ns = net::Reactor::now_ns();
  const std::string_view path = s.path();
  if (path == "/ws-api/v3") {
    st.kind = SessionKind::WsApi;
    ++stats_.api_sessions_opened;
    ++stats_.api_sessions_opened_since_mark;
  } else if (path.starts_with("/ws/") && listen_keys_.find(path.substr(4)) != listen_keys_.end()) {
    st.kind = SessionKind::ListenKey;
    st.listen_key = std::string(path.substr(4));
    st.listen_account = listen_keys_.find(path.substr(4))->second.account;
  } else {
    st.kind = SessionKind::MarketData;
    st.combined = path == "/stream";
    ++stats_.md_sessions_opened;
    ++stats_.md_sessions_opened_since_mark;
    std::string list;
    if (path == "/stream") {
      list = percent_decode(net::query_param(s.query(), "streams"));
    } else if (path.starts_with("/ws/")) {
      list = std::string(path.substr(4));
    }
    std::string_view rest = list;
    while (!rest.empty()) {
      const std::size_t slash = rest.find('/');
      StreamSub sub;
      if (parse_stream_name(slash == std::string_view::npos ? rest : rest.substr(0, slash), sub))
        st.streams.push_back(std::move(sub));
      if (slash == std::string_view::npos) break;
      rest.remove_prefix(slash + 1);
    }
  }
  sessions_.insert_or_assign(&s, std::move(st));
}

void Impl::on_text(net::WsSession& s, std::string_view text) {
  SessionState* st = session_state(&s);
  if (st == nullptr) return;
  st->last_rx_ns = net::Reactor::now_ns();
  switch (st->kind) {
    case SessionKind::WsApi:
      handle_ws_api(s, text);
      break;
    case SessionKind::MarketData:
      handle_stream_control(s, text);
      break;
    case SessionKind::ListenKey:
      break;
  }
}

void Impl::on_pong(net::WsSession& s, std::span<const std::byte>) {
  if (SessionState* st = session_state(&s)) st->last_rx_ns = net::Reactor::now_ns();
}

void Impl::on_close(net::WsSession& s, std::uint16_t, std::string_view) {
  sessions_.erase(&s);
}

void Impl::on_error(net::WsSession& s, net::NetError, std::string_view) {
  sessions_.erase(&s);
}

void Impl::handle_stream_control(net::WsSession& s, std::string_view text) {
  SessionState* st = session_state(&s);
  if (st == nullptr) return;
  const std::uint64_t token = st->token;
  StreamControlRequest req;
  std::string reply;
  if (!parse_stream_control(text, req)) {
    reply =
        R"({"error":{"code":2,"msg":"Invalid request: malformed JSON or missing method"},"id":null})";
  } else if (req.method == "SUBSCRIBE" || req.method == "UNSUBSCRIBE") {
    std::vector<StreamSub> subs;
    bool valid = true;
    for (const std::string& name : req.params) {
      StreamSub sub;
      if (!parse_stream_name(name, sub)) {
        valid = false;
        break;
      }
      subs.push_back(std::move(sub));
    }
    if (!valid) {
      reply = R"({"error":{"code":2,"msg":"Invalid request: unknown stream name"},"id":)" +
              req.id_json + "}";
    } else {
      for (StreamSub& sub : subs) {
        auto& v = st->streams;
        const auto it = std::find_if(
            v.begin(), v.end(), [&](const StreamSub& x) { return x.name == sub.name; });
        if (req.method == "SUBSCRIBE") {
          if (it == v.end()) v.push_back(std::move(sub));
        } else if (it != v.end()) {
          v.erase(it);
        }
      }
      reply = R"({"result":null,"id":)" + req.id_json + "}";
    }
  } else if (req.method == "LIST_SUBSCRIPTIONS") {
    reply = R"({"result":[)";
    bool first = true;
    for (const StreamSub& sub : st->streams) {
      if (!first) reply.push_back(',');
      first = false;
      append_json_string(reply, sub.name);
    }
    reply += R"(],"id":)" + req.id_json + "}";
  } else {
    reply =
        R"({"error":{"code":1,"msg":"Invalid request: unknown method"},"id":)" + req.id_json + "}";
  }
  deliver(&s, token, reply);
}

void Impl::deliver(net::WsSession* s, std::uint64_t token, std::string_view text) {
  const SessionState* st = session_state(s);
  if (st == nullptr || st->token != token || !s->is_open()) return;
  s->send_text(text);  // a failed send closes the session (on_error erases it)
}

void Impl::send_to_session(net::WsSession* s,
                           std::uint64_t token,
                           std::string text,
                           std::uint32_t delay_ms) {
  if (delay_ms == 0) {
    deliver(s, token, text);
    return;
  }
  reactor_.add_timer_after(static_cast<std::int64_t>(delay_ms) * kNsPerMs,
                           [this, s, token, t = std::move(text)] { deliver(s, token, t); });
}

void Impl::close_session(net::WsSession* s) {
  if (session_state(s) == nullptr) return;
  sessions_.erase(s);
  s->close_abrupt();  // no handler callback; HttpServer reaps the object on the next loop
}

// ---- faults -----------------------------------------------------------------------------------

void Impl::drop_market_data() {
  std::vector<net::WsSession*> targets;
  targets.reserve(sessions_.size());
  for (const auto& [ptr, st] : sessions_) {
    if (st.kind == SessionKind::MarketData) targets.push_back(ptr);
  }
  for (net::WsSession* s : targets) {
    close_session(s);
    ++stats_.md_connections_dropped;
  }
}

void Impl::drop_ws_api(bool include_user_streams) {
  std::vector<net::WsSession*> targets;
  targets.reserve(sessions_.size());
  for (const auto& [ptr, st] : sessions_) {
    if (st.kind == SessionKind::WsApi && (include_user_streams || st.user_subs.empty()))
      targets.push_back(ptr);
  }
  for (net::WsSession* s : targets) {
    close_session(s);
    ++stats_.api_connections_dropped;
  }
}

void Impl::expire_all_listen_keys() {
  const std::int64_t now_ms = server_ms();
  std::vector<std::string> keys;
  keys.reserve(listen_keys_.size());
  for (const auto& [key, lk] : listen_keys_) keys.push_back(key);
  for (const std::string& key : keys) {
    std::string event;
    append_listen_key_expired(event, now_ms, key);
    std::vector<std::pair<net::WsSession*, std::uint64_t>> targets;
    targets.reserve(sessions_.size());
    for (const auto& [ptr, st] : sessions_) {
      if (st.kind == SessionKind::ListenKey && st.listen_key == key)
        targets.emplace_back(ptr, st.token);
    }
    for (const auto& [ptr, token] : targets) {
      deliver(ptr, token, event);
      close_session(ptr);
    }
  }
  listen_keys_.clear();
}

// ---- market data ------------------------------------------------------------------------------

void Impl::md_trampoline(void* ctx, EventHeader& h, Timestamp) noexcept {
  static_cast<Impl*>(ctx)->on_md_event(h);
}

void Impl::on_md_event(EventHeader& h) {
  const std::uint32_t sym = h.instrument.value;
  if (sym >= symbols_.size()) return;
  const std::int64_t ms = server_ms();
  if (h.type == EventType::BookDelta && (h.flags & EventHeader::kSnapshot) == 0) {
    const auto& d = *reinterpret_cast<const BookDeltaMsg*>(&h);
    capture_flushed_book(sym);  // the engine book is exactly at d.last_update_id here
    if (faults_.skip_depth) {
      faults_.skip_depth = false;
      ++stats_.depth_updates_skipped;
      return;
    }
    md_scratch_.clear();
    append_depth_update(md_scratch_,
                        ms,
                        symbols_[sym].cfg.symbol,
                        d.first_update_id,
                        d.last_update_id,
                        d.bids(),
                        d.asks());
    ++stats_.depth_updates;
    publish_md(sym, StreamKind::Depth, md_scratch_);
  } else if (h.type == EventType::BookTicker) {
    const auto& t = *reinterpret_cast<const BookTickerMsg*>(&h);
    md_scratch_.clear();
    append_book_ticker(md_scratch_,
                       t.hdr.venue_seq,
                       symbols_[sym].cfg.symbol,
                       Level{t.bid_px, t.bid_qty},
                       Level{t.ask_px, t.ask_qty});
    ++stats_.book_tickers;
    publish_md(sym, StreamKind::Ticker, md_scratch_);
  }
  // BookSnapshot (first flush / aggregation overflow) has no Binance stream equivalent: a
  // connector that missed changes detects the U gap and re-snapshots over REST.
}

void Impl::capture_flushed_book(std::uint32_t symbol) {
  SymbolRuntime& rt = symbols_[symbol];
  const InstrumentId inst{symbol};
  rt.flushed_nb = me_->l2_snapshot(inst, Side::Buy, rt.flushed_bids.data(), rt.flushed_bids.size());
  rt.flushed_na =
      me_->l2_snapshot(inst, Side::Sell, rt.flushed_asks.data(), rt.flushed_asks.size());
  rt.flushed_update_id = me_->update_id(inst);
}

void Impl::publish_md(std::uint32_t symbol, StreamKind kind, std::string_view data) {
  struct Target {
    net::WsSession* session;
    std::uint64_t token;
    bool combined;
    std::string name;
  };
  std::vector<Target> targets;
  targets.reserve(sessions_.size());
  for (const auto& [ptr, st] : sessions_) {
    if (st.kind != SessionKind::MarketData) continue;
    for (const StreamSub& sub : st.streams) {
      if (sub.symbol == symbol && sub.kind == kind)
        targets.push_back(Target{ptr, st.token, st.combined, sub.name});
    }
  }
  for (const Target& t : targets) {
    if (t.combined) {
      md_wrapped_.clear();
      append_stream_message(md_wrapped_, t.name, data);
      deliver(t.session, t.token, md_wrapped_);
    } else {
      deliver(t.session, t.token, data);
    }
  }
}

// ---- public API ---------------------------------------------------------------------------------

SimExchangeServer::SimExchangeServer(SimServerConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

SimExchangeServer::~SimExchangeServer() {
  if (impl_) impl_->stop_thread();
}

bool SimExchangeServer::listen() {
  return impl_->listen();
}
const std::string& SimExchangeServer::last_error() const noexcept {
  return impl_->last_error_;
}
std::uint16_t SimExchangeServer::port() const noexcept {
  return impl_->port_;
}
std::uint16_t SimExchangeServer::tls_port() const noexcept {
  return impl_->tls_port_;
}
const SimServerConfig& SimExchangeServer::config() const noexcept {
  return impl_->cfg_;
}
int SimExchangeServer::poll(int max_wait_ms) {
  return impl_->reactor_.run_once(max_wait_ms);
}
void SimExchangeServer::start() {
  impl_->start_thread();
}
void SimExchangeServer::stop() {
  impl_->stop_thread();
}
bool SimExchangeServer::running() const noexcept {
  return impl_->running_.load(std::memory_order_acquire);
}
SimServerStats SimExchangeServer::stats() const {
  Impl& impl = *impl_;
  return impl.call([&impl] { return impl.snapshot_stats(); });
}
std::int64_t SimExchangeServer::server_time_ms() const {
  Impl& impl = *impl_;
  return impl.call([&impl] { return impl.server_ms(); });
}
void SimExchangeServer::mark() {
  impl_->call([this] { impl_->mark(); });
}
void SimExchangeServer::drop_market_data_connections() {
  impl_->call([this] { impl_->drop_market_data(); });
}
void SimExchangeServer::drop_ws_api_connections(bool include_user_streams) {
  impl_->call([this, include_user_streams] { impl_->drop_ws_api(include_user_streams); });
}
void SimExchangeServer::skip_next_depth_update() {
  impl_->call([this] { impl_->faults_.skip_depth = true; });
}
void SimExchangeServer::set_ack_delay_ms(std::uint32_t ms) {
  impl_->call([this, ms] { impl_->faults_.delay_ack_ms = ms; });
}
void SimExchangeServer::reject_next_orders(std::uint32_t count) {
  impl_->call([this, count] { impl_->faults_.reject_next = count; });
}
void SimExchangeServer::fail_next_timestamp() {
  impl_->call([this] { impl_->faults_.timestamp_once = true; });
}
void SimExchangeServer::set_rest_unresponsive(bool unresponsive) {
  impl_->call([this, unresponsive] { impl_->faults_.rest_unresponsive = unresponsive; });
}
void SimExchangeServer::rate_limit_next_requests(std::uint32_t count) {
  impl_->call([this, count] { impl_->faults_.rate_limit_next = count; });
}
void SimExchangeServer::set_clock_offset_ms(std::int64_t offset_ms) {
  impl_->call([this, offset_ms] { impl_->clock_offset_ms_ = offset_ms; });
}
void SimExchangeServer::expire_listen_keys() {
  impl_->call([this] { impl_->expire_all_listen_keys(); });
}

}  // namespace fastmm::sim::server
