#include "fastmm/venues/bybit/bybit_venue.hpp"

#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/bybit/bybit_rest_decoder.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fastmm::venues::bybit {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kClockResyncNs = 30LL * 60 * 1'000'000'000;
constexpr std::int64_t kDefaultCooldownNs = 10'000'000'000;

ConnState map_state(net::ConnState s) noexcept {
  switch (s) {
    case net::ConnState::Live:
      return ConnState::Live;
    case net::ConnState::Stale:
      return ConnState::Stale;
    case net::ConnState::Resolving:
    case net::ConnState::Connecting:
    case net::ConnState::TlsHandshake:
    case net::ConnState::WsHandshake:
    case net::ConnState::Authenticating:
    case net::ConnState::Subscribing:
      return ConnState::Connecting;
    case net::ConnState::Idle:
    case net::ConnState::Closing:
    case net::ConnState::Backoff:
      return ConnState::Disconnected;
  }
  return ConnState::Disconnected;
}

ChannelState channel_state(net::ConnState s) noexcept {
  switch (map_state(s)) {
    case ConnState::Live:
      return ChannelState::Live;
    case ConnState::Stale:
      return ChannelState::Stale;
    case ConnState::Connecting:
      return ChannelState::Connecting;
    default:
      return ChannelState::Down;
  }
}

std::int64_t header_int(const net::HttpResponse& r, std::string_view name) noexcept {
  const std::string_view v = r.header(name);
  if (v.empty()) return -1;
  const auto parsed = parse_int64(v);
  return parsed ? *parsed : -1;
}

// Replaces the path of a ws(s) URL: wss://host/v5/public/spot -> wss://host/v5/private.
std::string with_path(const std::string& url, std::string_view path) {
  const auto u = net::Url::parse(url);
  if (!u) return {};
  std::string s(u->scheme);
  s += "://";
  s += u->host;
  const bool default_port = (u->tls && u->port == 443) || (!u->tls && u->port == 80);
  if (!default_port) s += ":" + std::to_string(u->port);
  s += path;
  return s;
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

BybitVenue::BybitVenue(VenueId id, BybitVenueConfig cfg)
    : id_(id), cfg_(std::move(cfg)), signer_(cfg_.credentials), rate_(cfg_.rate_threshold) {
  rate_.add_weight_bucket(0, 1'000'000'000);  // limits come from X-Bapi-Limit headers
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  std::memset(scratch_, 0, sizeof scratch_);
}

BybitVenue::~BybitVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps BybitVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = cfg_.ws_order_api;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

std::int64_t BybitVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_.load(std::memory_order_relaxed);
}

net::ConnectionConfig BybitVenue::ws_config(const std::string& url,
                                            bool manual_auth,
                                            bool manual_subscribe) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  // Our own {"op":"ping"} every ping_interval_ms is answered, so the dead threshold must
  // exceed it for quiet private/trade channels.
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, cfg_.ping_interval_ms * 2 + 5000);
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = 0;  // no documented connection lifetime; auth is per session
  c.manual_auth = manual_auth;
  c.manual_subscribe = manual_subscribe;
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> BybitVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    {
      const HttpReply t = http.get("/v5/market/time");
      std::int64_t server_ms = 0;
      if (t.ok() && decode_server_time(t.body, server_ms).empty()) {
        const std::int64_t recv_ms = wall_now().ns / kNsPerMs;
        // The first request on a new connection includes TCP connect + TLS handshake, so
        // a send/receive midpoint overstates the skew by half the handshake. Using the
        // receive time biases the estimate only by the one-way network latency.
        clock_offset_ms_.store(server_ms - recv_ms);
        clock_sync_ns_ = now_ns();
      }
    }
    for (Instrument* inst : mine) {
      // GET /v5/market/instruments-info?category=spot&symbol=SYM (market/instrument page).
      const HttpReply reply = http.get("/v5/market/instruments-info?category=spot&symbol=" +
                                       std::string(inst->symbol.view()));
      if (!reply.ok()) {
        const std::string why =
            reply.error.empty() ? fmt::format("HTTP {} {}", reply.status, reply.body.substr(0, 200))
                                : reply.error;
        if (!cfg_.allow_offline_reference_data)
          return fail(fmt::format("{}: instruments-info failed: {}", cfg_.name, why));
        FASTMM_LOG_WARN(
            "{}: instruments-info failed ({}); keeping configured tick/lot", cfg_.name, why);
        continue;
      }
      std::vector<InstrumentInfo> infos;
      if (const std::string err = decode_instruments(reply.body, infos); !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
      const InstrumentInfo* f = nullptr;
      for (const InstrumentInfo& i : infos) {
        if (iequals_symbol(i.symbol, inst->symbol.view())) f = &i;
      }
      if (f == nullptr)
        return fail(
            fmt::format("{}: symbol {} not in instruments-info", cfg_.name, inst->symbol.view()));
      if (!f->tick.is_positive() || !f->base_precision.is_positive())
        return fail(fmt::format(
            "{}: {} has invalid tickSize/basePrecision", cfg_.name, inst->symbol.view()));
      if (inst->tick != f->tick || inst->lot != f->base_precision) {
        FASTMM_LOG_WARN(
            "{}: {} tick/lot from instruments-info override config ({} / {} -> {} / {})",
            cfg_.name,
            inst->symbol.view(),
            inst->tick,
            inst->lot,
            f->tick,
            f->base_precision);
      }
      inst->tick = f->tick;
      inst->lot = f->base_precision;
      inst->min_qty = f->min_qty.is_positive() ? f->min_qty : f->base_precision;
      inst->max_qty = f->max_qty;
      inst->min_notional = f->min_amount;
      inst->max_notional = f->max_amount;
      if (f->status != "Trading") {
        FASTMM_LOG_ERROR("{}: {} status is {} (not Trading): disabled",
                         cfg_.name,
                         inst->symbol.view(),
                         f->status);
        inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kEnabled);
      }
    }
  } catch (const std::exception& e) {
    if (!cfg_.allow_offline_reference_data)
      return fail(
          fmt::format("{}: reference data failed: {}", cfg_.name, std::string_view(e.what())));
  }
  const std::int64_t off = clock_offset_ms_.load();
  stats_.clock_offset_ms = off;
  if (off > 1000 || off < -1000)
    FASTMM_LOG_WARN("{}: clock offset to venue is {} ms", cfg_.name, off);
  FASTMM_LOG_INFO("{}: reference data loaded for {} symbols", cfg_.name, mine.size());
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void BybitVenue::attach(const SymbolTable& symbols,
                        const InstrumentTable& instruments,
                        EventSink& md_sink,
                        EventSink& order_sink,
                        MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ =
      std::make_unique<BybitMdFeed>(symbols,
                                    id_,
                                    md_sink,
                                    ResubscribeRequester{&BybitVenue::resubscribe_requester, this},
                                    cfg_.depth);
  private_parser_ = std::make_unique<BybitPrivateParser>(symbols, instruments, id_);
  encoder_ = std::make_unique<BybitOrderEncoder>(signer_, symbols, cfg_.recv_window_ms);
  decoder_ = std::make_unique<BybitResponseDecoder>();
}

void BybitVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id);
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (connected_ && md_conn_.opened()) {
    md_conn_.close();
    open_md();
  }
}

void BybitVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("BybitVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    if (!cfg_.dry_run) {
      raw_private_.open(cfg_.record_raw_dir, cfg_.name, "private");
      raw_trade_.open(cfg_.record_raw_dir, cfg_.name, "trade");
    }
  }
  open_rest();
  request_server_time();
  open_md();
  if (!cfg_.dry_run && signer_.usable()) {
    open_private();
    if (cfg_.ws_order_api) open_trade();
  }
  last_ping_ns_ = now_ns();
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, private={}, ws_orders={})",
                  cfg_.name,
                  cfg_.dry_run,
                  !cfg_.dry_run && signer_.usable(),
                  cfg_.ws_order_api);
}

void BybitVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  md_conn_.close();
  private_conn_.close();
  trade_conn_.close();
  if (rest_) rest_->reset();
  md_feed_->on_disconnected();
  raw_md_.flush();
  raw_private_.flush();
  raw_trade_.flush();
  publish_status();
}

void BybitVenue::open_rest() {
  RestChannelConfig rc;
  rc.base_url = cfg_.rest_url;
  rc.ca_file = cfg_.ca_file;
  rc.insecure_tls = cfg_.insecure_tls;
  rc.timeout_ms = cfg_.http_timeout_ms;
  rest_ = std::make_unique<RestChannel>(*reactor_, rc);
}

void BybitVenue::open_md() {
  md_conn_.open(*reactor_, ws_config(cfg_.ws_public_url, false, false), md_handler_);
  md_conn_.connect();
}

void BybitVenue::open_private() {
  private_conn_.open(*reactor_, ws_config(cfg_.ws_private_url, true, true), private_handler_);
  private_conn_.connect();
}

void BybitVenue::open_trade() {
  trade_conn_.open(*reactor_, ws_config(cfg_.ws_trade_url, true, false), trade_handler_);
  trade_conn_.connect();
}

void BybitVenue::send_auth(ConnectionSlot<PrivateHandler>* priv,
                           ConnectionSlot<TradeHandler>* trade) {
  const std::size_t n =
      encoder_->encode_ws_auth(venue_time_ms() + cfg_.auth_expires_ms, request_buf_);
  const std::string_view frame(request_buf_, n);
  const bool ok = n > 0 && (priv != nullptr ? priv->send_text(frame) : trade->send_text(frame));
  if (!ok) FASTMM_LOG_ERROR("{}: could not send the auth request", cfg_.name);
}

// ---- market data ------------------------------------------------------------------------------

void BybitVenue::on_md_state(net::ConnState s) {
  const ConnState mapped = map_state(s);
  stats_.md = channel_state(s);
  if (s == net::ConnState::Backoff) ++stats_.reconnects;
  if (mapped == md_state_) return;
  const ConnState prev = md_state_;
  md_state_ = mapped;
  switch (mapped) {
    case ConnState::Live:
      emit_connection_state(*md_sink_, id_, 0, ConnState::Live);
      FASTMM_LOG_INFO("{}: md channel -> Live", cfg_.name);
      break;
    case ConnState::Stale:
      // The engine clears books on any non-Live md state: resubscribe for fresh snapshots.
      emit_connection_state(*md_sink_, id_, 0, ConnState::Stale);
      for (InstrumentId id : subscribed_) {
        if (BybitBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
      }
      break;
    case ConnState::Disconnected:
    case ConnState::Connecting:
      if (prev == ConnState::Live || prev == ConnState::Stale) {
        md_feed_->on_disconnected();
        emit_connection_state(*md_sink_, id_, 0, ConnState::Disconnected);
        FASTMM_LOG_WARN("{}: md channel lost", cfg_.name);
      }
      break;
    default:
      break;
  }
}

void BybitVenue::on_md_open() {
  for (const std::string& p : md_feed_->subscription_payloads()) {
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void BybitVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const std::uint64_t sub_errors = md_feed_->stats().subscribe_errors;
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  if (md_feed_->stats().subscribe_errors != sub_errors)
    FASTMM_LOG_ERROR("{}: market-data subscribe rejected: {}", cfg_.name, t.substr(0, 200));
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  }
}

void BybitVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;  // the reconnect subscribes everything again
  for (const std::string& p : md_feed_->resubscribe_payloads(id))
    static_cast<void>(md_conn_.send_text(p));
  FASTMM_LOG_INFO(
      "{}: resubscribing {} for a fresh snapshot", cfg_.name, symbols_->venue_symbol(id));
}

// ---- private stream -------------------------------------------------------------------------

void BybitVenue::on_private_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating) send_auth(&private_conn_, nullptr);
  const ConnState mapped = map_state(s);
  stats_.user = channel_state(s);
  if (mapped == private_state_) return;
  const ConnState prev = private_state_;
  private_state_ = mapped;
  if (mapped == ConnState::Live) {
    // Stale is not reported for these channels, so neither is the return from it.
    if (prev != ConnState::Stale) emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
    FASTMM_LOG_INFO("{}: private channel -> Live", cfg_.name);
    // 6.7: reconcile after a reconnect, not when a quiet channel returns from Stale.
    if (private_was_live_ && prev != ConnState::Stale) request_open_orders();
    private_was_live_ = true;
  } else if ((mapped == ConnState::Disconnected || mapped == ConnState::Connecting) &&
             (prev == ConnState::Live || prev == ConnState::Stale)) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: private channel lost", cfg_.name);
  }
}

void BybitVenue::on_private_open() {
  constexpr std::string_view kTopics[] = {"order", "execution", "wallet"};
  const std::size_t n = BybitOrderEncoder::encode_subscribe("private", kTopics, request_buf_);
  if (n == 0 || !private_conn_.send_text(std::string_view(request_buf_, n)))
    FASTMM_LOG_ERROR("{}: could not subscribe the private topics", cfg_.name);
}

ClientOrderId BybitVenue::current_id(ClientOrderId link) const noexcept {
  if (const ClientOrderId* cur = aliases_.find(link)) return *cur;
  return link;
}

void BybitVenue::forget_order(ClientOrderId id) noexcept {
  if (const OrderShadow* s = shadows_.find(id)) {
    if (s->link_id.valid() && s->link_id != id) aliases_.erase(s->link_id);
  }
  shadows_.erase(id);
}

void BybitVenue::on_private_text(std::string_view t, std::int64_t ts) {
  if (raw_private_.enabled()) raw_private_.record(ts, t);
  const Cycles t0 = rdtscp();
  const MdDecodeResult r = private_parser_->decode(t, wall_now(), t0, scratch_);
  if (r.status == ParseStatus::Ok) {
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      switch (h->type) {
        case EventType::PositionUpdate:
          if (!cfg_.position_from_wallet) continue;
          break;
        case EventType::OrderAck: {
          auto* m = reinterpret_cast<OrderAckMsg*>(h);
          m->cl_ord_id = current_id(m->cl_ord_id);
          break;
        }
        case EventType::OrderReject: {
          auto* m = reinterpret_cast<OrderRejectMsg*>(h);
          m->cl_ord_id = current_id(m->cl_ord_id);
          forget_order(m->cl_ord_id);
          break;
        }
        case EventType::OrderCancelAck: {
          auto* m = reinterpret_cast<OrderCancelAckMsg*>(h);
          m->cl_ord_id = current_id(m->cl_ord_id);
          forget_order(m->cl_ord_id);
          break;
        }
        case EventType::OrderExpired: {
          auto* m = reinterpret_cast<OrderExpiredMsg*>(h);
          m->cl_ord_id = current_id(m->cl_ord_id);
          forget_order(m->cl_ord_id);
          break;
        }
        case EventType::OrderFill: {
          auto* m = reinterpret_cast<OrderFillMsg*>(h);
          m->cl_ord_id = current_id(m->cl_ord_id);
          if (m->leaves_qty.raw <= 0) forget_order(m->cl_ord_id);
          break;
        }
        default:
          break;
      }
      static_cast<void>(order_sink_->push(*h));
      ++stats_.order_events;
    }
    return;
  }
  switch (r.control) {
    case ControlOp::Auth:
      if (r.control_success) {
        FASTMM_LOG_INFO("{}: private stream authenticated", cfg_.name);
        private_conn_.auth_done();
      } else {
        FASTMM_LOG_ERROR("{}: private auth failed: {}", cfg_.name, r.ret_msg);
        apply_action(VenueAction::Fatal, 0, r.ret_msg, 0);
      }
      return;
    case ControlOp::Subscribe:
      if (r.control_success) {
        private_conn_.subscribe_done();
      } else {
        FASTMM_LOG_ERROR("{}: private subscribe failed: {}", cfg_.name, r.ret_msg);
      }
      return;
    default:
      break;
  }
  if (r.status == ParseStatus::Malformed) FASTMM_LOG_WARN("{}: malformed private frame", cfg_.name);
}

// ---- trade channel --------------------------------------------------------------------------

void BybitVenue::on_trade_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating) send_auth(nullptr, &trade_conn_);
  const ConnState mapped = map_state(s);
  stats_.order = channel_state(s);
  if (mapped == trade_state_) return;
  const ConnState prev = trade_state_;
  trade_state_ = mapped;
  if (mapped == ConnState::Live) {
    // 6.7: orders were cancelled over REST while the trade channel was down; reconcile on a real
    // reconnect (not the first connect, not a return from Stale).
    const bool reconnected = trade_was_live_ && prev != ConnState::Stale;
    trade_was_live_ = true;
    // Stale is not reported for these channels, so neither is the return from it.
    if (prev != ConnState::Stale) emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
    FASTMM_LOG_INFO("{}: trade channel -> Live", cfg_.name);
    drain_outbound();
    if (reconnected && !cfg_.dry_run) request_open_orders();
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: trade channel lost", cfg_.name);
    // disconnect() clears connected_ before closing the channels: a requested shutdown already
    // runs the synchronous cancel_all(), and an async request would only be aborted.
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void BybitVenue::on_trade_text(std::string_view t, std::int64_t ts) {
  if (raw_trade_.enabled()) raw_trade_.record(ts, t);
  TradeResponse r;
  const ParseStatus st = decoder_->decode_ws(t, r);
  if (st == ParseStatus::Malformed) {
    FASTMM_LOG_WARN("{}: malformed trade frame", cfg_.name);
    return;
  }
  if (st != ParseStatus::Ok) return;
  if (r.op == "auth") {
    if (r.success) {
      FASTMM_LOG_INFO("{}: trade stream authenticated", cfg_.name);
      trade_conn_.auth_done();
    } else {
      FASTMM_LOG_ERROR("{}: trade auth failed: {} {}", cfg_.name, r.ret_code, r.ret_msg);
      apply_action(map_error(r.ret_code, r.ret_msg).action == VenueAction::None
                       ? VenueAction::Fatal
                       : map_error(r.ret_code, r.ret_msg).action,
                   r.ret_code,
                   r.ret_msg,
                   0);
    }
    return;
  }
  if (r.is_op_ack || r.op == "ping" || r.op == "pong") return;
  if (const auto req = parse_request_id(r.req_id)) {
    handle_order_response(req->first, req->second, r);
    return;
  }
  if (!r.success)
    FASTMM_LOG_WARN("{}: trade error for '{}': {} {}", cfg_.name, r.req_id, r.ret_code, r.ret_msg);
}

void BybitVenue::handle_order_response(RequestKind kind, ClientOrderId id, const TradeResponse& r) {
  rate_.on_remaining(r.limit, r.limit_status, now_ns());
  const OrderShadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  const ErrorMapping m = map_error(r.ret_code, r.ret_msg);
  switch (kind) {
    case RequestKind::New:
      if (r.success) {
        if (cfg_.emit_ack_from_response) emit_order_ack(*order_sink_, id_, inst, id, r.order_id);
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, r.ret_code, r.ret_msg);
        forget_order(id);
        apply_action(m.action, r.ret_code, r.ret_msg, r.limit_reset_ms);
      }
      ++stats_.order_events;
      return;
    case RequestKind::Cancel:
      if (r.success) {
        // With the private stream up the `order` topic reports the cancel with cumExecQty;
        // otherwise the response is all we will get.
        if (!private_conn_.is_live()) {
          emit_cancel_ack(*order_sink_, id_, inst, id, r.order_id, Qty{});
          forget_order(id);
          ++stats_.order_events;
        }
      } else {
        emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, r.ret_code, r.ret_msg);
        apply_action(m.action, r.ret_code, r.ret_msg, r.limit_reset_ms);
        ++stats_.order_events;
      }
      return;
    case RequestKind::Replace:
      if (r.success) {
        if (shadow != nullptr) {
          const ClientOrderId link = shadow->link_id;
          const ClientOrderId orig = shadow->replaces;
          if (link.valid() && link != id) aliases_.assign(link, id);
          if (orig.valid() && orig != id) shadows_.erase(orig);
        }
        emit_order_ack(*order_sink_, id_, inst, id, r.order_id);
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, r.ret_code, r.ret_msg);
        shadows_.erase(id);
        apply_action(m.action, r.ret_code, r.ret_msg, r.limit_reset_ms);
      }
      ++stats_.order_events;
      return;
    case RequestKind::Other:
      return;
  }
}

// ---- outbound ---------------------------------------------------------------------------------

void BybitVenue::on_wake() {
  drain_outbound();
}

void BybitVenue::drain_outbound() {
  if (outbound_ == nullptr) return;
  while (const std::byte* p = outbound_->try_peek()) {
    const auto* h = reinterpret_cast<const EventHeader*>(p);
    if (const auto cmd = OrderCommand::from(*h)) {
      sent_.note(*cmd);
      send_command(*cmd);
    }
    outbound_->release();
  }
}

void BybitVenue::send_command(const OrderCommand& cmd) {
  const std::int64_t now = now_ns();
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  auto refuse = [&](RejectReason reason, std::string_view why) {
    if (is_cancel) {
      emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
    } else {
      emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
    }
    ++stats_.order_events;
  };
  if (cfg_.dry_run) return refuse(RejectReason::VenueKilled, "dry-run: orders disabled");
  if (fatal_) return refuse(RejectReason::VenueKilled, "venue fatal");
  const OrderShadow* shadow = nullptr;
  if (cmd.kind == OrderCommandKind::New) {
    if (!rate_.can_send(1, now, true)) {
      ++stats_.rate_limit_cooldowns;
      return refuse(RejectReason::VenueRateLimit, "local rate limit");
    }
    shadows_.assign(cmd.cl_ord_id,
                    OrderShadow{cmd.instrument, cmd.side, cmd.type, cmd.tif, cmd.cl_ord_id, {}});
    shadow = shadows_.find(cmd.cl_ord_id);
  } else if (cmd.kind == OrderCommandKind::Replace) {
    const OrderShadow* orig = shadows_.find(cmd.orig_cl_ord_id);
    if (orig == nullptr) return refuse(RejectReason::UnknownOrder, "amend: original unknown");
    if (!rate_.can_send(1, now, true)) {
      ++stats_.rate_limit_cooldowns;
      return refuse(RejectReason::VenueRateLimit, "local rate limit");
    }
    OrderShadow copy = *orig;
    copy.replaces = cmd.orig_cl_ord_id;
    shadows_.assign(cmd.cl_ord_id, copy);
    shadow = shadows_.find(cmd.cl_ord_id);
  } else {
    shadow = shadows_.find(cmd.cl_ord_id);
  }
  if (cfg_.ws_order_api && trade_conn_.is_live()) {
    const Cycles before_encode = rdtscp();
    const std::size_t n = encoder_->encode_ws(cmd, shadow, venue_time_ms(), request_buf_);
    const Cycles after_encode = rdtscp();
    if (n > 0 && trade_conn_.send_text(std::string_view(request_buf_, n))) {
      wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
      rate_.on_sent(1, now, !is_cancel);
      switch (cmd.kind) {
        case OrderCommandKind::New:
          ++stats_.orders_sent;
          break;
        case OrderCommandKind::Cancel:
          ++stats_.cancels_sent;
          break;
        case OrderCommandKind::Replace:
          ++stats_.replaces_sent;
          break;
      }
      return;
    }
    ++stats_.order_send_failures;
  }
  send_command_rest(cmd, shadow);
}

void BybitVenue::send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow) {
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  RestRequest rr;
  const Cycles before_encode = rdtscp();
  if (rest_ == nullptr || rest_hard_stopped_ || !encoder_->encode_rest(cmd, shadow, rr)) {
    if (is_cancel) {
      emit_cancel_reject(*order_sink_,
                         id_,
                         cmd.instrument,
                         cmd.cl_ord_id,
                         RejectReason::VenueReject,
                         0,
                         "no order channel");
    } else {
      emit_order_reject(*order_sink_,
                        id_,
                        cmd.instrument,
                        cmd.cl_ord_id,
                        RejectReason::VenueReject,
                        0,
                        "no order channel");
      shadows_.erase(cmd.cl_ord_id);
    }
    ++stats_.order_send_failures;
    return;
  }
  const std::string headers = encoder_->rest_headers(rr, venue_time_ms());
  const Cycles after_encode = rdtscp();
  OrderCommand copy = cmd;
  copy.venue_order_id = nullptr;
  copy.header = nullptr;
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      rr.method, rr.target(), headers, rr.body, [this, alive, copy](const net::HttpResponse& r) {
        if (alive.expired()) return;
        handle_rest_order_response(copy, r);
      });
  if (!queued) {
    if (is_cancel) {
      emit_cancel_reject(*order_sink_,
                         id_,
                         cmd.instrument,
                         cmd.cl_ord_id,
                         RejectReason::TransportFull,
                         0,
                         "rest queue full");
    } else {
      emit_order_reject(*order_sink_,
                        id_,
                        cmd.instrument,
                        cmd.cl_ord_id,
                        RejectReason::TransportFull,
                        0,
                        "rest queue full");
    }
    ++stats_.order_send_failures;
    return;
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  rate_.on_sent(1, now_ns(), rr.is_order);
  switch (cmd.kind) {
    case OrderCommandKind::New:
      ++stats_.orders_sent;
      break;
    case OrderCommandKind::Cancel:
      ++stats_.cancels_sent;
      break;
    case OrderCommandKind::Replace:
      ++stats_.replaces_sent;
      break;
  }
}

void BybitVenue::handle_rest_order_response(const OrderCommand& cmd, const net::HttpResponse& r) {
  ++stats_.rest_requests;
  note_rate_headers(r);
  const RequestKind kind = cmd.kind == OrderCommandKind::New      ? RequestKind::New
                           : cmd.kind == OrderCommandKind::Cancel ? RequestKind::Cancel
                                                                  : RequestKind::Replace;
  TradeResponse tr;
  if (r.error != net::NetError::None) {
    ++stats_.rest_errors;
    tr.ret_code = -1;
    tr.ret_msg = net::to_string(r.error);
    handle_order_response(kind, cmd.cl_ord_id, tr);
    request_open_orders();  // send status unknown: reconcile rather than guess
    return;
  }
  const PaddedJson padded(r.body);
  RestResponse rest;
  if (decoder_->decode_rest(padded.view(), rest) != ParseStatus::Ok) {
    ++stats_.rest_errors;
    const ErrorMapping hm = map_http_status(r.status);
    tr.ret_code = r.status;
    tr.ret_msg = "unparseable REST reply";
    handle_order_response(kind, cmd.cl_ord_id, tr);
    apply_action(hm.action, r.status, tr.ret_msg, 0);
    return;
  }
  tr.ret_code = rest.ret_code;
  tr.success = rest.ret_code == 0 && r.status == 200;
  tr.ret_msg = rest.ret_msg;
  tr.order_id = rest.order_id;
  tr.order_link_id = rest.order_link_id;
  tr.limit = header_int(r, "X-Bapi-Limit");
  tr.limit_status = header_int(r, "X-Bapi-Limit-Status");
  tr.limit_reset_ms = header_int(r, "X-Bapi-Limit-Reset-Timestamp");
  handle_order_response(kind, cmd.cl_ord_id, tr);
}

void BybitVenue::note_rate_headers(const net::HttpResponse& r) {
  // guide "Rate Limit": X-Bapi-Limit, X-Bapi-Limit-Status (remaining),
  // X-Bapi-Limit-Reset-Timestamp.
  rate_.on_remaining(header_int(r, "X-Bapi-Limit"), header_int(r, "X-Bapi-Limit-Status"), now_ns());
  if (r.status == 403) {
    // IP limit (600 requests / 5 s per IP): "403, access too frequent", lifted after about
    // 10 minutes (https://bybit-exchange.github.io/docs/v5/rate-limit).
    rate_.cooldown(600'000'000'000, now_ns());
    ++stats_.rate_limit_cooldowns;
    FASTMM_LOG_ERROR("{}: HTTP 403 (IP rate limit): REST paused for 10 minutes", cfg_.name);
  }
}

// First HardStop / Fatal error: the engine trips this venue's kill switch (quotes pulled, new
// orders refused by risk); the other venues keep trading.
void BybitVenue::trip_venue_kill(KillReason reason) {
  if (venue_kill_sent_ || order_sink_ == nullptr) return;
  venue_kill_sent_ = true;
  FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
  emit_venue_kill(*order_sink_, id_, reason);
}

void BybitVenue::apply_action(VenueAction action,
                              int code,
                              std::string_view msg,
                              std::int64_t reset_epoch_ms) {
  switch (action) {
    case VenueAction::None:
      break;
    case VenueAction::Backoff:
      rate_.cooldown(1'000'000'000, now_ns());
      break;
    case VenueAction::RateLimit: {
      std::int64_t wait = kDefaultCooldownNs;
      if (reset_epoch_ms > 0)
        wait = std::max<std::int64_t>(0, reset_epoch_ms - venue_time_ms()) * kNsPerMs;
      rate_.cooldown(wait, now_ns());
      ++stats_.rate_limit_cooldowns;
      FASTMM_LOG_WARN(
          "{}: rate limited ({} {}); cooling down {} ms", cfg_.name, code, msg, wait / kNsPerMs);
      break;
    }
    case VenueAction::ResyncClock:
      clock_resync_wanted_ = true;
      request_server_time();
      break;
    case VenueAction::Reconcile:
      request_open_orders();
      break;
    case VenueAction::DisableInstrument:
      FASTMM_LOG_ERROR("{}: venue rejected a precision/filter rule ({} {}); check tick/lot config",
                       cfg_.name,
                       code,
                       msg);
      break;
    case VenueAction::HardStop:
      rate_.hard_stop();
      rest_hard_stopped_ = true;
      FASTMM_LOG_ERROR("{}: REST hard stop ({} {})", cfg_.name, code, msg);
      trip_venue_kill(KillReason::VenueHardStop);
      break;
    case VenueAction::Fatal:
      fatal_ = true;
      FASTMM_LOG_ERROR("{}: fatal venue error ({} {}); order entry disabled", cfg_.name, code, msg);
      trip_venue_kill(KillReason::VenueFatal);
      break;
  }
}

// ---- control requests -----------------------------------------------------------------------

void BybitVenue::emit_reconcile(std::string_view json, ClientOrderId sent_watermark) {
  ReconcileMsg begin{};
  init_header(begin, EventType::Reconcile, InstrumentId::invalid(), id_);
  begin.kind = ReconcileMsg::Kind::Begin;
  SentWatermark::stamp(begin, sent_watermark);
  begin.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(begin.hdr));
  std::size_t count = 0;
  const PaddedJson padded(json);
  const ParseStatus st = decoder_->decode_open_orders(padded.view(), [&](const OpenOrderRecord& o) {
    const InstrumentId inst = symbols_->find(id_, o.symbol);
    if (!inst.valid()) return;
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, inst, id_);
    m.kind = ReconcileMsg::Kind::OpenOrder;
    m.side = o.side == "Sell" ? Side::Sell : Side::Buy;
    m.state = o.status == "PartiallyFilled" ? OrderState::PartiallyFilled : OrderState::Live;
    if (const auto cl = decode_cl_ord_id(o.order_link_id)) m.cl_ord_id = current_id(*cl);
    m.venue_order_id.assign(o.order_id);
    if (const auto p = parse_price(o.price)) m.price = *p;
    if (const auto q = parse_qty(o.qty)) m.orig_qty = *q;
    if (!o.cum_exec_qty.empty()) {
      if (const auto q = parse_qty(o.cum_exec_qty)) m.cum_qty = *q;
    }
    m.hdr.recv_ts = wall_now();
    static_cast<void>(order_sink_->push(m.hdr));
    ++count;
  });
  ReconcileMsg end{};
  init_header(end, EventType::Reconcile, InstrumentId::invalid(), id_);
  end.kind = ReconcileMsg::Kind::End;
  end.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(end.hdr));
  if (st != ParseStatus::Ok) {
    FASTMM_LOG_WARN("{}: open orders reply could not be parsed", cfg_.name);
  } else {
    FASTMM_LOG_INFO("{}: reconciled {} open orders", cfg_.name, count);
  }
}

void BybitVenue::request_open_orders() {
  if (cfg_.dry_run || !connected_ || !signer_.usable() || rest_ == nullptr || rest_hard_stopped_)
    return;
  RestRequest rr;
  if (!encoder_->encode_rest_open_orders({}, rr)) return;
  const std::string headers = encoder_->rest_headers(rr, venue_time_ms());
  std::weak_ptr<int> alive = alive_;
  static_cast<void>(
      rest_->request("GET",
                     rr.target(),
                     headers,
                     {},
                     [this, alive, watermark = sent_.value()](const net::HttpResponse& r) {
                       if (alive.expired()) return;
                       ++stats_.rest_requests;
                       note_rate_headers(r);
                       if (!r.ok()) {
                         ++stats_.rest_errors;
                         FASTMM_LOG_WARN("{}: GET order/realtime failed: status={} err={}",
                                         cfg_.name,
                                         r.status,
                                         net::to_string(r.error));
                         return;
                       }
                       emit_reconcile(r.body, watermark);
                     }));
}

void BybitVenue::request_server_time() {
  if (rest_ == nullptr || time_request_pending_ || rest_hard_stopped_) return;
  time_request_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const bool queued =
      rest_->request("GET", "/v5/market/time", {}, {}, [this, alive](const net::HttpResponse& r) {
        if (alive.expired()) return;
        time_request_pending_ = false;
        ++stats_.rest_requests;
        if (!r.ok()) {
          ++stats_.rest_errors;
          return;
        }
        std::int64_t server_ms = 0;
        if (!decode_server_time(r.body, server_ms).empty()) return;
        const std::int64_t local_recv_ms = wall_now().ns / kNsPerMs;
        const std::int64_t offset = server_ms - local_recv_ms;  // see load_reference_data()
        clock_offset_ms_.store(offset);
        clock_sync_ns_ = now_ns();
        clock_resync_wanted_ = false;
        stats_.clock_offset_ms = offset;
        if (offset > 1000 || offset < -1000)
          FASTMM_LOG_WARN("{}: clock offset to venue is {} ms (recv_window {} ms)",
                          cfg_.name,
                          offset,
                          cfg_.recv_window_ms);
      });
  if (!queued) time_request_pending_ = false;
}

void BybitVenue::cancel_all_async() {
  if (rest_ == nullptr || !signer_.usable() || rest_hard_stopped_) return;
  for (InstrumentId id : subscribed_) {
    RestRequest rr;
    if (!encoder_->encode_rest_cancel_all(symbols_->venue_symbol(id), rr)) continue;
    const std::string headers = encoder_->rest_headers(rr, venue_time_ms());
    std::weak_ptr<int> alive = alive_;
    static_cast<void>(rest_->request(
        "POST", rr.target(), headers, rr.body, [this, alive, id](const net::HttpResponse& r) {
          if (alive.expired() || r.error == net::NetError::Canceled) return;
          ++stats_.rest_requests;
          note_rate_headers(r);
          int code = -1;
          std::string msg;
          if (!r.ok() || !decode_envelope(r.body, code, msg) || code != 0) {
            ++stats_.rest_errors;
            FASTMM_LOG_ERROR("{}: cancel-all for {} failed: status={} retCode={} {}",
                             cfg_.name,
                             symbols_->venue_symbol(id),
                             r.status,
                             code,
                             msg);
          }
        }));
  }
}

bool BybitVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr || encoder_ == nullptr) return true;
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  bool all_ok = true;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    for (InstrumentId id : subscribed_) {
      RestRequest rr;
      if (!encoder_->encode_rest_cancel_all(symbols_->venue_symbol(id), rr)) {
        all_ok = false;
        continue;
      }
      const std::string headers = encoder_->rest_headers(rr, venue_time_ms());
      const HttpReply reply = http.request("POST", rr.target(), headers, rr.body);
      int code = -1;
      std::string msg;
      if (!reply.ok() || !decode_envelope(reply.body, code, msg) || code != 0) {
        all_ok = false;
        FASTMM_LOG_ERROR("{}: kill-switch cancel-all for {} failed: status={} retCode={} {}",
                         cfg_.name,
                         symbols_->venue_symbol(id),
                         reply.status,
                         code,
                         reply.error.empty() ? msg : reply.error);
      } else {
        FASTMM_LOG_INFO(
            "{}: kill-switch cancel-all for {} ok", cfg_.name, symbols_->venue_symbol(id));
      }
    }
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR(
        "{}: kill-switch cancel-all failed: {}", cfg_.name, std::string_view(e.what()));
    return false;
  }
  return all_ok;
}

// ---- housekeeping -----------------------------------------------------------------------------

void BybitVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (now - last_ping_ns_ >= static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs) {
    last_ping_ns_ = now;
    const std::size_t n = BybitOrderEncoder::encode_ping("ping", request_buf_);
    const std::string_view ping(request_buf_, n);
    if (md_conn_.is_live()) static_cast<void>(md_conn_.send_text(ping));
    if (private_conn_.is_live()) static_cast<void>(private_conn_.send_text(ping));
    if (trade_conn_.is_live()) static_cast<void>(trade_conn_.send_text(ping));
  }
  if (clock_resync_wanted_ || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  publish_status();
  raw_md_.flush();
  raw_private_.flush();
  raw_trade_.flush();
  if (reactor_ != nullptr && housekeeping_timer_ == net::kInvalidTimer) {
    std::weak_ptr<int> alive = alive_;
    housekeeping_timer_ = reactor_->add_timer_after(kHousekeepingNs, [this, alive] {
      if (alive.expired()) return;
      housekeeping_timer_ = net::kInvalidTimer;
      on_timer(now_ns());
    });
  }
}

void BybitVenue::publish_status() noexcept {
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_.load(std::memory_order_relaxed);
  wire_.summarize(
      tsc_calibration(), stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
}

VenueStatus BybitVenue::status() const noexcept {
  VenueStatus s;
  for (int i = 0; i < 100; ++i) {
    if (published_.try_load(s)) return s;
  }
  return s;
}

// ---- config ---------------------------------------------------------------------------------

BybitVenueConfig make_bybit_config(const VenueSection& v, bool dry_run) {
  BybitVenueConfig c;
  c.name = v.name;
  c.ws_public_url = v.ws_url;
  c.ws_trade_url = v.ws_api_url;
  c.rest_url = v.rest_url;
  c.recv_window_ms = v.recv_window_ms > 0 ? v.recv_window_ms : kDefaultRecvWindowMs;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.supports_replace = v.supports_replace;
  c.dry_run = dry_run;
  c.credentials.api_key = v.api_key;
  c.credentials.secret.value = v.api_secret;
  auto extra = [&](const char* key) -> std::string {
    const auto it = v.extra.find(key);
    return it == v.extra.end() ? std::string{} : it->second;
  };
  auto extra_bool = [&](const char* key, bool def) {
    const std::string s = extra(key);
    if (s.empty()) return def;
    return s == "true" || s == "1" || s == "yes";
  };
  auto extra_int = [&](const char* key, std::int64_t def) {
    const std::string s = extra(key);
    if (s.empty()) return def;
    const auto n = parse_int64(s);
    return n ? *n : def;
  };
  c.ws_private_url = extra("ws_private_url");
  if (c.ws_private_url.empty()) c.ws_private_url = with_path(c.ws_public_url, "/v5/private");
  if (c.ws_trade_url.empty()) c.ws_trade_url = with_path(c.ws_public_url, "/v5/trade");
  if (extra("order_api") == "rest") c.ws_order_api = false;
  c.depth = static_cast<int>(std::clamp<std::int64_t>(extra_int("depth", 50), 1, 1000));
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, extra_int("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, extra_int("dead_ms", c.dead_ms)));
  c.ping_interval_ms = static_cast<std::uint32_t>(
      std::max<std::int64_t>(1000, extra_int("ping_interval_ms", c.ping_interval_ms)));
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, extra_int("orders_per_second", c.orders_per_second)));
  c.position_from_wallet = extra_bool("position_from_wallet", false);
  c.allow_offline_reference_data = extra_bool("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = extra_bool("cancel_on_order_channel_loss", true);
  c.emit_ack_from_response = extra_bool("emit_ack_from_response", true);
  return c;
}

}  // namespace fastmm::venues::bybit
