#include "fastmm/venues/binance/binance_venue.hpp"

#include "fastmm/venues/binance/binance_rest_decoder.hpp"
#include "fastmm/venues/binance/binance_trade_history.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace fastmm::venues::binance {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kListenKeyKeepaliveNs = 30LL * 60 * 1'000'000'000;  // every 30 min
constexpr std::int64_t kClockResyncNs = 30LL * 60 * 1'000'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kDefaultCooldownNs = 10'000'000'000;
constexpr std::int64_t kLogonRetryNs = 2'000'000'000;
// rest-api.md "Account trade list": limit max 1000, and the window the venue will look back over
// is at most 24 hours. A replay that would need more than that cannot be complete.
constexpr int kMyTradesLimit = 1000;
constexpr std::int64_t kExecutionRetryNs = 5'000'000'000;  // between retries of a failed replay
constexpr std::int64_t kExecutionSweepNs = 60 * 1'000'000'000LL;  // a replay while all is well
// cancel_all() is the kill switch's only remedy, so a rate-limited refusal is retried rather than
// reported: bounded, because the caller is blocked on it.
constexpr int kCancelAllRateLimitRetries = 3;
constexpr int kCancelAllRetryMs = 400;
constexpr std::int64_t kMyTradesMaxLookbackMs = 24LL * 3600 * 1000;
// rest-api.md "Order book" weight by limit: 1-100:5, 101-500:25, 501-1000:50, 1001-5000:250.
std::uint32_t depth_weight(int limit) noexcept {
  if (limit <= 100) return 5;
  if (limit <= 500) return 25;
  if (limit <= 1000) return 50;
  return 250;
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

BinanceVenue::BinanceVenue(VenueId id, BinanceVenueConfig cfg)
    : id_(id), cfg_(std::move(cfg)), signer_(cfg_.credentials), rate_(cfg_.rate_threshold) {
  if (cfg_.user_stream == UserStreamMode::Auto)
    cfg_.user_stream = signer_.usable() ? UserStreamMode::WsApi : UserStreamMode::None;
  if (cfg_.dry_run) cfg_.user_stream = UserStreamMode::None;
  std::memset(scratch_, 0, sizeof scratch_);
}

BinanceVenue::~BinanceVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps BinanceVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = cfg_.ws_order_api;
  c.user_stream = cfg_.user_stream != UserStreamMode::None;
  return c;
}

std::int64_t BinanceVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_;
}

std::string BinanceVenue::api_headers() const {
  return signer_.usable() ? api_key_header(signer_.api_key()) : std::string{};
}

InstrumentId BinanceVenue::instrument_of(std::string_view symbol) const noexcept {
  return symbols_ != nullptr ? symbols_->find(id_, symbol) : InstrumentId::invalid();
}

net::ConnectionConfig BinanceVenue::ws_config(const std::string& url, bool manual_subscribe) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  // Binance sends a WebSocket ping every 20 s (web-socket-streams.md / web-socket-api.md
  // "General WSS information"); any dead threshold must exceed that or a quiet symbol would
  // reconnect in a loop. Pings count as receive activity in net::Connection.
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, 45'000);
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = cfg_.max_lifetime_ms;
  c.manual_subscribe = manual_subscribe;
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> BinanceVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  // GET /api/v3/exchangeInfo?symbols=["BTCUSDT","ETHUSDT"] (rest-api.md "Exchange
  // information"); the array is percent-encoded in the query.
  std::string list = "[";
  for (std::size_t i = 0; i < mine.size(); ++i) {
    if (i > 0) list += ',';
    list += '"';
    list += mine[i]->symbol.view();
    list += '"';
  }
  list += ']';
  char enc[2048];
  const std::size_t n = net::percent_encode(list, enc);
  if (n == 0) return fail(std::string("exchangeInfo: symbol list too long"));
  const std::string target = "/api/v3/exchangeInfo?symbols=" + std::string(enc, n);

  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  HttpReply reply;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    reply = http.get(target);
  } catch (const std::exception& e) {
    reply.error = e.what();
  }
  if (!reply.ok()) {
    const std::string why = reply.error.empty()
                                ? fmt::format("HTTP {} {}", reply.status, reply.body.substr(0, 200))
                                : reply.error;
    if (!cfg_.allow_offline_reference_data)
      return fail(fmt::format("{}: exchangeInfo failed: {}", cfg_.name, why));
    FASTMM_LOG_WARN("{}: exchangeInfo failed ({}); keeping configured tick/lot", cfg_.name, why);
    return {};
  }
  ExchangeInfo info;
  if (const std::string err = decode_exchange_info(reply.body, info); !err.empty())
    return fail(fmt::format("{}: {}", cfg_.name, err));
  for (Instrument* inst : mine) {
    const SymbolFilters* f = nullptr;
    for (const SymbolFilters& s : info.symbols) {
      if (iequals_symbol(s.symbol, inst->symbol.view())) f = &s;
    }
    if (f == nullptr)
      return fail(fmt::format("{}: symbol {} not in exchangeInfo", cfg_.name, inst->symbol.view()));
    if (!f->tick.is_positive() || !f->step.is_positive())
      return fail(fmt::format("{}: {} has invalid tick/step", cfg_.name, inst->symbol.view()));
    if (inst->tick != f->tick || inst->lot != f->step) {
      FASTMM_LOG_WARN("{}: {} tick/lot from exchangeInfo override config ({} / {} -> {} / {})",
                      cfg_.name,
                      inst->symbol.view(),
                      inst->tick,
                      inst->lot,
                      f->tick,
                      f->step);
    }
    inst->tick = f->tick;
    inst->lot = f->step;
    inst->min_qty = f->min_qty.is_positive() ? f->min_qty : f->step;
    inst->max_qty = f->max_qty;
    inst->min_notional = f->min_notional;
    inst->max_notional = f->max_notional;
    if (f->status != "TRADING") {
      FASTMM_LOG_ERROR(
          "{}: {} status is {} (not TRADING): disabled", cfg_.name, inst->symbol.view(), f->status);
      inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kEnabled);
    }
    if (!f->post_only_allowed)
      FASTMM_LOG_WARN(
          "{}: {} does not allow LIMIT_MAKER (post-only)", cfg_.name, inst->symbol.view());
  }
  for (const RateLimitRule& r : info.rate_limits) {
    if (r.type == "REQUEST_WEIGHT") {
      rate_.add_weight_bucket(static_cast<std::uint32_t>(r.limit), r.window_ns());
    } else if (r.type == "ORDERS") {
      rate_.add_order_bucket(static_cast<std::uint32_t>(r.limit), r.window_ns());
    }
  }
  if (info.server_time_ms != 0) {
    clock_offset_ms_ = info.server_time_ms - wall_now().ns / kNsPerMs;
    clock_sync_ns_ = now_ns();
    stats_.clock_offset_ms = clock_offset_ms_;
    if (clock_offset_ms_ > 1000 || clock_offset_ms_ < -1000)
      FASTMM_LOG_WARN("{}: clock offset to venue is {} ms", cfg_.name, clock_offset_ms_.load());
  }
  const std::string_view w = reply.header("x-mbx-used-weight-1m");
  if (!w.empty()) {
    if (const auto used = parse_int64(w)) rate_.on_headers(*used, -1, now_ns());
  }
  FASTMM_LOG_INFO("{}: reference data loaded for {} symbols ({} rate limit rules)",
                  cfg_.name,
                  mine.size(),
                  info.rate_limits.size());
  return {};
}

// GET /api/v3/account/commission for every instrument of this venue (weight 20 each), signed like
// every account request (HMAC or Ed25519). Fails when the connector has no credentials to sign
// with: the operator asked for the account's rates and would otherwise quote on the config's.
Result<std::vector<VenueFee>, std::string> BinanceVenue::account_fees(
    const InstrumentTable& instruments) {
  std::vector<VenueFee> out;
  if (!cfg_.fetch_fees) return out;
  if (!signer_.usable())
    return fail(fmt::format(
        "{}: fetch_fees needs api_key and a secret or Ed25519 key to sign the request", cfg_.name));
  SymbolTable symbols;
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    BinanceOrderEncoder enc(signer_, symbols, cfg_.recv_window_ms);
    for (const Instrument& inst : instruments) {
      if (inst.venue != id_) continue;
      RestRequest rr;
      if (!enc.encode_rest_commission(inst.symbol.view(), venue_time_ms(), rr))
        return fail(fmt::format(
            "{}: cannot sign account/commission for {}", cfg_.name, inst.symbol.view()));
      const HttpReply reply = http.request(
          "GET", std::string(rr.path) + "?" + std::string(rr.query.view()), api_headers());
      if (!reply.ok())
        return fail(fmt::format(
            "{}: account/commission for {} failed: {}",
            cfg_.name,
            inst.symbol.view(),
            reply.error.empty() ? fmt::format("HTTP {} {}", reply.status, reply.body.substr(0, 200))
                                : reply.error));
      CommissionRates c;
      if (const std::string err = decode_commission(reply.body, c); !err.empty())
        return fail(fmt::format("{}: {}: {}", cfg_.name, inst.symbol.view(), err));
      if (c.side_dependent)
        FASTMM_LOG_WARN("{}: {} charges buyers and sellers differently; the larger rate is used",
                        cfg_.name,
                        inst.symbol.view());
      out.push_back({inst.id, c.rates});
    }
  } catch (const std::exception& e) {
    return fail(fmt::format("{}: account/commission: {}", cfg_.name, std::string_view(e.what())));
  }
  return out;
}

// ---- wiring ---------------------------------------------------------------------------------

void BinanceVenue::attach(const SymbolTable& symbols,
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
      std::make_unique<BinanceMdFeed>(symbols,
                                      id_,
                                      md_sink,
                                      SnapshotRequester{&BinanceVenue::snapshot_requester, this},
                                      cfg_.min_snapshot_interval_ns,
                                      cfg_.md_format);
  user_parser_ = std::make_unique<BinanceUserParser>(symbols, instruments, id_);
  encoder_ = std::make_unique<BinanceOrderEncoder>(signer_, symbols, cfg_.recv_window_ms);
  ws_api_decoder_ = std::make_unique<BinanceWsApiDecoder>();
}

void BinanceVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id);
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (connected_ && md_conn_.opened()) {
    // Stream list lives in the URL: reopen the market-data connection.
    md_conn_.close();
    open_md();
  }
}

void BinanceVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("BinanceVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  // Nobody said where the execution replay should start, so it starts here: this session can only
  // have missed what happened after it connected, and replaying further back would book another
  // session's fills into a position that starts at zero.
  if (exec_since_ms_ <= 0) exec_since_ms_ = venue_time_ms();
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    raw_user_.open(cfg_.record_raw_dir, cfg_.name, "user");
    raw_order_.open(cfg_.record_raw_dir, cfg_.name, "order");
  }
  open_rest();
  request_server_time();
  open_md();
  if (!cfg_.dry_run) {
    if (cfg_.ws_order_api) open_order();
    if (cfg_.user_stream == UserStreamMode::WsApi) {
      open_user();
    } else if (cfg_.user_stream == UserStreamMode::ListenKey) {
      request_listen_key();
    }
  }
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, user_stream={}, ws_orders={})",
                  cfg_.name,
                  cfg_.dry_run,
                  static_cast<int>(cfg_.user_stream),
                  cfg_.ws_order_api);
}

void BinanceVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  md_conn_.close();
  user_conn_.close();
  order_conn_.close();
  if (rest_) rest_->reset();
  md_feed_->on_disconnected();
  raw_md_.flush();
  raw_user_.flush();
  raw_order_.flush();
  publish_status();
}

void BinanceVenue::open_rest() {
  RestChannelConfig rc;
  rc.base_url = cfg_.rest_url;
  rc.ca_file = cfg_.ca_file;
  rc.insecure_tls = cfg_.insecure_tls;
  rc.timeout_ms = cfg_.http_timeout_ms;
  rest_ = std::make_unique<RestChannel>(*reactor_, rc);
}

void BinanceVenue::open_md() {
  const bool sbe = cfg_.md_format == MdFormat::Sbe;
  const std::string& ws_url = sbe ? cfg_.sbe_ws_url : cfg_.ws_url;
  const auto base = net::Url::parse(ws_url);
  if (!base) throw std::invalid_argument("binance: bad market-data url " + ws_url);
  std::string path(base->path);
  if (path == "/" || path.empty()) path = "/stream";
  const std::string url = origin_of(*base) + md_feed_->stream_target(path);
  net::ConnectionConfig c = ws_config(url, /*manual_subscribe=*/false);
  // SBE streams authenticate the connection with the API key header only (no signature).
  if (sbe) c.extra_headers = api_key_header(signer_.api_key());
  md_conn_.open(*reactor_, c, md_handler_);
  md_conn_.connect();
}

void BinanceVenue::open_user() {
  std::string url;
  if (cfg_.user_stream == UserStreamMode::ListenKey) {
    const auto base = net::Url::parse(cfg_.ws_url);
    if (!base || listen_key_.empty()) return;
    url = origin_of(*base) + "/ws/" + listen_key_;
    user_conn_.open(*reactor_, ws_config(url, /*manual_subscribe=*/false), user_handler_);
  } else {
    url = cfg_.ws_api_url;
    user_conn_.open(*reactor_, ws_config(url, /*manual_subscribe=*/true), user_handler_);
  }
  user_subscribed_ = false;
  user_conn_.connect();
}

void BinanceVenue::open_order() {
  net::ConnectionConfig c = ws_config(cfg_.ws_api_url, /*manual_subscribe=*/false);
  // Ed25519 keys log on once (session.logon from on_connected_send_subscriptions) and send
  // unsigned requests after that; the channel goes Live on the logon reply.
  c.manual_subscribe = signer_.type() == KeyType::Ed25519;
  order_conn_.open(*reactor_, c, order_handler_);
  order_conn_.connect();
}

// ---- market data channel ------------------------------------------------------------------

void BinanceVenue::on_md_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.md = channel_state(s);
  if (s == net::ConnState::Backoff) ++stats_.reconnects;
  if (mapped == md_state_) return;
  const ConnState prev = md_state_;
  md_state_ = mapped;
  switch (mapped) {
    case ConnState::Live:
      // Books are restarted from on_md_open(); after a Stale episode the engine cleared its
      // books, so every syncer is already resyncing (see the Stale case).
      emit_connection_state(Channel::Md, ConnState::Live);
      break;
    case ConnState::Stale:
      // The engine clears the books on any non-Live state (channel 0): make the syncers
      // agree by forcing a fresh snapshot once data flows again.
      emit_connection_state(Channel::Md, ConnState::Stale);
      for (InstrumentId id : subscribed_) {
        if (BinanceDepthSync* sync = md_feed_->sync(id))
          sync->resync(SyncReason::Explicit, now_ns());
      }
      break;
    case ConnState::Disconnected:
      if (prev != ConnState::Connecting) {
        md_feed_->on_disconnected();
        emit_connection_state(Channel::Md, ConnState::Disconnected);
      }
      break;
    case ConnState::Connecting:
      if (prev == ConnState::Live || prev == ConnState::Stale) {
        md_feed_->on_disconnected();
        emit_connection_state(Channel::Md, ConnState::Disconnected);
      }
      break;
    default:
      break;
  }
}

void BinanceVenue::on_md_open() {
  md_feed_->on_connected();  // start every syncer -> REST snapshots
}

// A new consumer (a strategy attached to fastmm-gateway) needs whole books: every syncer asks for
// a snapshot again, which the engine receives after a Resyncing state. Nothing to do before the
// channel is live: its first snapshots are on the way.
void BinanceVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (BinanceDepthSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void BinanceVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  if (cfg_.md_format == MdFormat::Sbe) {
    // Only control replies / errors arrive as text on an SBE stream connection.
    FASTMM_LOG_WARN("{}: text frame on the SBE stream: {}", cfg_.name, t.substr(0, 200));
    return;
  }
  note_md_status(md_feed_->on_message(t, ts), ts);
}

void BinanceVenue::on_md_binary(std::span<const std::byte> b, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record_hex(ts, b);
  note_md_status(md_feed_->on_binary(b, ts), ts);
}

void BinanceVenue::note_md_status(ParseStatus st, std::int64_t ts) {
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  } else if (st == ParseStatus::Overflow) {
    ++stats_.md_dropped;
  }
}

void BinanceVenue::request_snapshot(InstrumentId id) {
  if (rest_ == nullptr || !connected_ || rest_hard_stopped_) {
    md_feed_->on_snapshot_failed(id, now_ns());
    return;
  }
  const std::uint32_t weight = depth_weight(cfg_.depth_limit);
  if (!rate_.can_send(weight, now_ns())) {
    ++stats_.rate_limit_cooldowns;
    md_feed_->on_snapshot_failed(id, now_ns());
    return;
  }
  const std::string target =
      fmt::format("/api/v3/depth?symbol={}&limit={}", symbols_->venue_symbol(id), cfg_.depth_limit);
  std::weak_ptr<int> alive = alive_;
  const bool queued =
      rest_->request("GET", target, {}, {}, [this, alive, id](const net::HttpResponse& r) {
        if (alive.expired()) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (r.ok()) {
          md_feed_->on_snapshot_body(id, r.body, now_ns());
          return;
        }
        ++stats_.rest_errors;
        if (r.error == net::NetError::None) {
          const ErrorMapping m = map_http_status(r.status);
          apply_action(
              m.action, r.status, r.body.substr(0, 120), header_int(r, "Retry-After") * 1000);
        }
        FASTMM_LOG_WARN("{}: depth snapshot for {} failed: status={} err={}",
                        cfg_.name,
                        symbols_->venue_symbol(id),
                        r.status,
                        net::to_string(r.error));
        md_feed_->on_snapshot_failed(id, now_ns());
      });
  if (!queued) {
    md_feed_->on_snapshot_failed(id, now_ns());
    return;
  }
  rate_.on_sent(weight, now_ns());
}

// ---- user data channel --------------------------------------------------------------------

void BinanceVenue::on_user_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.user = private_channel_state(s);
  if (mapped == user_state_) return;
  const ConnState prev = user_state_;
  user_state_ = mapped;
  if (mapped == ConnState::Live) {
    // A quiet stream goes Stale without an event, so coming back from Stale is not news either.
    if (prev != ConnState::Stale) emit_connection_state(Channel::User, ConnState::Live);
    // 6.7: after a user-stream reconnect the OMS view may be stale. The old condition relied on
    // stats_.reconnects, which only market-data backoffs increment, and fired on every
    // Stale -> Live flip of a quiet stream.
    if (user_was_live_ && prev != ConnState::Stale) request_open_orders();
    user_was_live_ = true;
  } else if (mapped == ConnState::Disconnected && prev != ConnState::Connecting) {
    user_subscribed_ = false;
    emit_connection_state(Channel::User, ConnState::Disconnected);
  }
}

void BinanceVenue::on_user_open() {
  if (cfg_.user_stream != UserStreamMode::WsApi) return;  // listenKey stream needs no request
  std::size_t n = 0;
  if (signer_.type() == KeyType::Ed25519) {
    n = encoder_->encode_ws_logon("logon-u", venue_time_ms(), request_buf_);
  } else {
    n = encoder_->encode_ws_user_stream_subscribe(
        "uds", venue_time_ms(), /*with_signature=*/true, request_buf_);
  }
  if (n == 0 || !user_conn_.send_text(std::string_view(request_buf_, n)))
    FASTMM_LOG_ERROR("{}: could not send the user-stream subscription request", cfg_.name);
}

void BinanceVenue::on_user_text(std::string_view t, std::int64_t ts) {
  if (raw_user_.enabled()) raw_user_.record(ts, t);
  const Cycles t0 = rdtscp();
  const UserDecodeResult r = user_parser_->decode(t, wall_now(), t0, scratch_);
  if (r.status == ParseStatus::Ok) {
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      off += h->len;
      if (h->type == EventType::PositionUpdate && !cfg_.position_from_balance) continue;
      switch (h->type) {
        case EventType::OrderReject:
        case EventType::OrderCancelAck:
        case EventType::OrderExpired:
          shadows_.erase(reinterpret_cast<const OrderAckMsg*>(h)->cl_ord_id);
          break;
        case EventType::OrderFill: {
          const auto* f = reinterpret_cast<const OrderFillMsg*>(h);
          if (f->leaves_qty.raw <= 0) shadows_.erase(f->cl_ord_id);
          break;
        }
        default:
          break;
      }
      sent_.answered(*h);
      static_cast<void>(order_sink_->push(*h));
      ++stats_.order_events;
    }
    return;
  }
  if (r.status == ParseStatus::Ignored) {
    WsApiResponse resp;
    if (ws_api_decoder_->decode(t, resp) == ParseStatus::Ok) handle_ws_api_response(resp, t);
    return;
  }
  if (r.status == ParseStatus::Malformed)
    FASTMM_LOG_WARN("{}: malformed user-stream frame", cfg_.name);
}

// ---- order channel --------------------------------------------------------------------------

void BinanceVenue::on_order_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.order = private_channel_state(s);
  if (mapped == order_state_) return;
  const ConnState prev = order_state_;
  order_state_ = mapped;
  // Requests in flight on a connection that is gone are never answered on it: they no longer
  // hold the snapshot watermark back (the reconnect's snapshot settles them).
  if (mapped != ConnState::Live && mapped != ConnState::Stale) sent_.connection_lost();
  if (mapped == ConnState::Live) {
    // 6.7: after the order channel comes back the OMS view may be stale (orders were cancelled
    // over REST while it was down). Not when a quiet channel merely returns from Stale, which
    // would query open orders on every idle period.
    const bool reconnected = order_was_live_ && prev != ConnState::Stale;
    // On the first connect, sweep for orders nobody owns: a session that died without cancelling
    // left its orders resting and nothing else would ever go looking for them. Their client order
    // ids belong to an earlier session epoch, so the engine does not recognise them and cancels
    // them. The empty watermark keeps that sweep from saying anything about our own orders.
    const bool first_connect = !order_was_live_;
    order_was_live_ = true;
    // Stale is not reported for a quiet order channel, so neither is the return from it.
    if (prev != ConnState::Stale) emit_connection_state(Channel::Order, ConnState::Live);
    drain_outbound();  // anything queued while the channel was down
    if (!cfg_.dry_run) {
      if (reconnected) {
        request_open_orders();
      } else if (first_connect) {
        request_open_orders(ClientOrderId{});
      }
    }
    return;
  }
  if (mapped == ConnState::Stale) return;  // quiet order channels are normal
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    session_logged_on_ = false;
    encoder_->set_session_authenticated(false);
    emit_connection_state(Channel::Order, ConnState::Disconnected);
    oo_watermarks_.clear();  // requests on the closed connection get no reply
    // 6.7 "order channel down": cancel everything through REST immediately.
    // disconnect() clears connected_ before closing the channels: a requested shutdown already
    // runs the synchronous cancel_all(), and an async request would only be aborted.
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void BinanceVenue::on_order_open() {
  if (signer_.type() == KeyType::Ed25519) {
    const std::size_t n = encoder_->encode_ws_logon("logon-o", venue_time_ms(), request_buf_);
    if (n == 0 || !order_conn_.send_text(std::string_view(request_buf_, n)))
      FASTMM_LOG_ERROR("{}: could not send session.logon", cfg_.name);
  }
}

void BinanceVenue::on_order_text(std::string_view t, std::int64_t ts) {
  if (raw_order_.enabled()) raw_order_.record(ts, t);
  WsApiResponse r;
  const ParseStatus st = ws_api_decoder_->decode(t, r);
  if (st == ParseStatus::Ok) {
    handle_ws_api_response(r, t);
  } else if (st == ParseStatus::Malformed) {
    FASTMM_LOG_WARN("{}: malformed WS API frame", cfg_.name);
  }
}

void BinanceVenue::handle_ws_api_response(const WsApiResponse& r, std::string_view raw) {
  rate_.on_headers(r.rate.used_weight, r.rate.order_count, now_ns());
  if (const auto req = parse_request_id(r.id)) {
    handle_order_response(req->first, req->second, r);
    return;
  }
  if (r.id == "logon-o" || r.id == "logon-u") {
    if (r.is_error) {
      const ErrorMapping m = map_error(r.code, r.msg);
      FASTMM_LOG_ERROR("{}: session.logon failed: {} {}", cfg_.name, r.code, r.msg);
      const VenueAction action = m.action == VenueAction::None ? VenueAction::Fatal : m.action;
      apply_action(action, r.code, r.msg, r.retry_after_ms);
      // Transient (timestamp, rate limit, server busy): log on again once the action had
      // time to work (clock resync, cooldown). Bad key / signature / permission is Fatal.
      if (action != VenueAction::Fatal && action != VenueAction::HardStop)
        schedule_logon_retry(r.id == "logon-o" ? Channel::Order : Channel::User);
      return;
    }
    if (r.id == "logon-o") {
      session_logged_on_ = true;
      encoder_->set_session_authenticated(true);
      order_conn_.subscribe_done();
    } else {
      const std::size_t n =
          encoder_->encode_ws_user_stream_subscribe("uds", venue_time_ms(), false, request_buf_);
      if (n == 0 || !user_conn_.send_text(std::string_view(request_buf_, n)))
        FASTMM_LOG_ERROR("{}: could not send userDataStream.subscribe", cfg_.name);
    }
    return;
  }
  if (r.id == "uds") {
    if (r.is_error) {
      FASTMM_LOG_ERROR("{}: userDataStream.subscribe failed: {} {}", cfg_.name, r.code, r.msg);
      const ErrorMapping m = map_error(r.code, r.msg);
      apply_action(m.action, r.code, r.msg, r.retry_after_ms);
      return;
    }
    user_subscribed_ = true;
    user_conn_.subscribe_done();
    FASTMM_LOG_INFO(
        "{}: user data stream subscribed (subscriptionId={})", cfg_.name, r.subscription_id);
    return;
  }
  if (r.id == "oo") {
    const ClientOrderId watermark =
        oo_watermarks_.empty() ? sent_.value(now_ns()) : oo_watermarks_.front();
    if (!oo_watermarks_.empty()) oo_watermarks_.erase(oo_watermarks_.begin());
    if (r.is_error) {
      FASTMM_LOG_WARN("{}: openOrders.status failed: {} {}", cfg_.name, r.code, r.msg);
      return;
    }
    emit_reconcile(raw, /*rest_array=*/false, watermark);
    return;
  }
  if (r.id == "ca") {
    if (r.is_error && r.code != -2011)
      FASTMM_LOG_WARN("{}: openOrders.cancelAll failed: {} {}", cfg_.name, r.code, r.msg);
    return;
  }
  if (r.id.empty() && r.is_error && r.status == 401) {
    // "Session revocation" (web-socket-api request-security): the logged-on key became invalid
    // (deleted, IP whitelist, permissions) and the server revoked the session with id null.
    // Requests would now need apiKey/signature, which an Ed25519 session no longer sends.
    FASTMM_LOG_ERROR("{}: WS API session revoked: {} {}", cfg_.name, r.code, r.msg);
    session_logged_on_ = false;
    encoder_->set_session_authenticated(false);
    const ErrorMapping m = map_error(r.code, r.msg);
    apply_action(m.action == VenueAction::None ? VenueAction::Fatal : m.action,
                 r.code,
                 r.msg,
                 r.retry_after_ms);
    return;
  }
  if (r.is_error)
    FASTMM_LOG_WARN("{}: WS API error for id '{}': {} {}", cfg_.name, r.id, r.code, r.msg);
}

void BinanceVenue::schedule_logon_retry(Channel ch) {
  if (reactor_ == nullptr) return;
  std::weak_ptr<int> alive = alive_;
  reactor_->add_timer_after(kLogonRetryNs, [this, alive, ch] {
    if (alive.expired() || !connected_ || fatal_) return;
    const bool order = ch == Channel::Order;
    if (order && session_logged_on_) return;
    const std::size_t n =
        encoder_->encode_ws_logon(order ? "logon-o" : "logon-u", venue_time_ms(), request_buf_);
    if (n == 0) return;
    const std::string_view frame(request_buf_, n);
    const bool sent = order ? order_conn_.send_text(frame) : user_conn_.send_text(frame);
    if (!sent) FASTMM_LOG_WARN("{}: session.logon retry not sent", cfg_.name);
  });
}

void BinanceVenue::handle_order_response(RequestKind kind,
                                         ClientOrderId id,
                                         const WsApiResponse& r) {
  // The venue answered a placement: the order no longer holds the snapshot watermark back.
  if (kind != RequestKind::Cancel) sent_.answered(id);
  const OrderShadow* shadow = shadows_.find(id);
  InstrumentId inst = shadow != nullptr ? shadow->instrument : instrument_of(r.symbol);
  if (r.status == 429 || r.status == 418) {
    const ErrorMapping hm = map_http_status(r.status);
    apply_action(hm.action, r.code, r.msg, r.retry_after_ms);
  }
  switch (kind) {
    case RequestKind::New:
      if (!r.is_error) {
        if (cfg_.emit_ack_from_response) emit_ack(inst, id, r.order_id);
        return;
      }
      {
        const ErrorMapping m = map_error(r.code, r.msg);
        emit_reject(inst, id, m.reason, r.code, r.msg);
        shadows_.erase(id);
        apply_action(m.action, r.code, r.msg, r.retry_after_ms);
      }
      return;
    case RequestKind::Cancel:
      if (!r.is_error) {
        ClientOrderId target = id;
        if (const auto orig = decode_cl_ord_id(r.orig_client_order_id)) target = *orig;
        emit_cancel_ack(inst, target, r.order_id, r.executed_qty);
        shadows_.erase(target);
        return;
      }
      {
        const ErrorMapping m = map_error(r.code, r.msg);
        emit_cancel_reject(inst, id, m.reason, r.code, r.msg);
        apply_action(m.action, r.code, r.msg, r.retry_after_ms);
      }
      return;
    case RequestKind::Amend: {
      // order.amend.keepPriority: the venue kept the order, its id and its place in the queue,
      // and only shrank the remaining quantity. There is no cancel leg and no new venue order,
      // so the ack says so and the engine keeps the fills booked against it.
      if (!r.is_error) {
        ClientOrderId orig{};
        if (const auto o = decode_cl_ord_id(r.orig_client_order_id)) orig = *o;
        if (orig.valid() && orig != id) shadows_.erase(orig);
        if (cfg_.emit_ack_from_response) emit_ack(inst, id, r.order_id, true);
        return;
      }
      // The amend was refused (-2038 quantity not below the current one, -2013 gone, a filter
      // failure). Nothing changed on the venue: the original is still resting under its old id,
      // so the replacement id is rejected and the OMS leaves the order working. The quote
      // manager requotes on the next reconcile; it does not retry as a cancelReplace here,
      // because by then the order may have traded.
      const ErrorMapping m = map_error(r.code, r.msg);
      emit_reject(inst, id, m.reason, r.code, r.msg);
      shadows_.erase(id);
      apply_action(m.action, r.code, r.msg, r.retry_after_ms);
      return;
    }
    case RequestKind::Replace: {
      ClientOrderId orig{};
      if (const auto o = decode_cl_ord_id(r.cancel_client_order_id)) orig = *o;
      if (!r.is_error) {
        if (orig.valid()) {
          emit_cancel_ack(inst, orig, r.cancel_order_id, r.cancel_executed_qty);
          shadows_.erase(orig);
        }
        if (cfg_.emit_ack_from_response) emit_ack(inst, id, r.order_id);
        return;
      }
      // STOP_ON_FAILURE: cancel failed -> nothing changed; cancel ok, new failed -> the
      // original is gone and the replacement was rejected (-2021).
      if (r.cancel_result == "SUCCESS" && orig.valid()) {
        emit_cancel_ack(inst, orig, r.cancel_order_id, r.cancel_executed_qty);
        shadows_.erase(orig);
      }
      const int code = r.new_order_code != 0 ? r.new_order_code : r.code;
      const std::string_view msg = !r.new_order_msg.empty() ? r.new_order_msg : r.msg;
      const ErrorMapping m = map_error(code, msg);
      emit_reject(inst, id, m.reason, code, msg);
      shadows_.erase(id);
      apply_action(m.action, code, msg, r.retry_after_ms);
      return;
    }
    case RequestKind::Other:
      return;
  }
}

// ---- outbound -------------------------------------------------------------------------------

void BinanceVenue::on_wake() {
  drain_outbound();
}

// The orders the ring holds go out as one write of all their WebSocket frames.
template <class Ring>
void BinanceVenue::write_orders(Ring& ring) {
  drain_outbound_coalesced(
      ring,
      wire_,
      [this] {
        order_conn_.cork();
        batch_.clear();
      },
      [this](const EventHeader& h) {
        if (const auto cmd = OrderCommand::from(h)) {
          sent_.note(*cmd, now_ns());
          send_command(*cmd);
        } else if (is_reconcile_request(h)) {
          request_open_orders();
        }
      },
      [this] {
        if (order_conn_.uncork()) return true;
        fail_batch();
        return false;
      });
}

void BinanceVenue::fail_batch() {
  for (const BatchedOrders::Entry& e : batch_.entries()) {
    ++stats_.order_send_failures;
    unsend(stats_, e.kind);
    if (e.kind == OrderCommandKind::Cancel) {
      emit_cancel_reject(
          e.instrument, e.cl_ord_id, RejectReason::TransportFull, 0, "order batch not written");
    } else {
      emit_reject(
          e.instrument, e.cl_ord_id, RejectReason::TransportFull, 0, "order batch not written");
      shadows_.erase(e.cl_ord_id);
    }
  }
  batch_.clear();
}

void BinanceVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void BinanceVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

void BinanceVenue::send_command(const OrderCommand& cmd) {
  const std::int64_t now = now_ns();
  if (cfg_.dry_run) {
    emit_reject(
        cmd.instrument, cmd.cl_ord_id, RejectReason::VenueKilled, 0, "dry-run: orders disabled");
    return;
  }
  // A venue-fatal error and a REST hard stop stop new orders, never cancels: the kill path's
  // whole remedy is to cancel, so a cancel goes out on whatever transport is still usable.
  if (fatal_ && cmd.kind != OrderCommandKind::Cancel) {
    emit_reject(cmd.instrument, cmd.cl_ord_id, RejectReason::VenueKilled, 0, "venue fatal");
    return;
  }
  const OrderShadow* shadow = nullptr;
  bool amend_in_place = false;
  // Weight and unfilled-order-count cost of this request, from the endpoint tables. An amend
  // costs IP weight 4 and nothing at all on the ORDERS bucket ("Unfilled Order Count: 0"),
  // where a place and a cancelReplace cost weight 1 and one order.
  std::uint32_t weight = 1;
  bool is_order = cmd.kind != OrderCommandKind::Cancel;
  if (cmd.kind == OrderCommandKind::New) {
    if (!rate_.can_send(weight, now, true)) {
      ++stats_.rate_limit_cooldowns;
      emit_reject(
          cmd.instrument, cmd.cl_ord_id, RejectReason::VenueRateLimit, 0, "local rate limit");
      return;
    }
    shadows_.assign(cmd.cl_ord_id,
                    OrderShadow{cmd.side, cmd.type, cmd.tif, cmd.instrument, cmd.price, cmd.qty});
  } else if (cmd.kind == OrderCommandKind::Replace) {
    shadow = shadows_.find(cmd.orig_cl_ord_id);
    if (shadow == nullptr) {
      emit_reject(cmd.instrument,
                  cmd.cl_ord_id,
                  RejectReason::UnknownOrder,
                  0,
                  "replace: original unknown");
      return;
    }
    // Same price, smaller quantity: order.amend.keepPriority keeps the venue order and its
    // place in the queue. Anything else has to be a cancelReplace. So does an order that has
    // used up its MAX_NUM_ORDER_AMENDS budget: past it the venue answers -2038 and the order
    // would sit at the wrong size until the next requote.
    amend_in_place = cfg_.amend_keep_priority && is_quantity_reduction(cmd, *shadow) &&
                     shadow->amends < cfg_.max_order_amends;
    if (amend_in_place) {
      weight = kAmendWeight;
      is_order = false;
    }
    if (!rate_.can_send(weight, now, is_order)) {
      ++stats_.rate_limit_cooldowns;
      emit_reject(
          cmd.instrument, cmd.cl_ord_id, RejectReason::VenueRateLimit, 0, "local rate limit");
      return;
    }
    OrderShadow copy = *shadow;
    copy.price = cmd.price;
    copy.qty = cmd.qty;
    // The venue counts amendments per order, and the amended order is the same order.
    copy.amends = amend_in_place ? static_cast<std::uint16_t>(copy.amends + 1) : 0;
    shadows_.assign(cmd.cl_ord_id, copy);
    shadow = shadows_.find(cmd.orig_cl_ord_id);
  }
  if (cfg_.ws_order_api && order_conn_.is_live()) {
    const Cycles before_encode = rdtscp();
    const std::size_t n =
        encoder_->encode_ws(cmd, shadow, venue_time_ms(), request_buf_, amend_in_place);
    const Cycles after_encode = rdtscp();
    if (n > 0 && order_conn_.send_text(std::string_view(request_buf_, n))) {
      wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
      batch_.note(cmd);
      rate_.on_sent(weight, now, is_order);
      switch (cmd.kind) {
        case OrderCommandKind::New:
          ++stats_.orders_sent;
          break;
        case OrderCommandKind::Cancel:
          ++stats_.cancels_sent;
          break;
        case OrderCommandKind::Replace:
          ++stats_.replaces_sent;
          if (amend_in_place) ++amends_sent_;
          break;
      }
      return;
    }
    ++stats_.order_send_failures;
  }
  send_command_rest(cmd, shadow, amend_in_place);
}

void BinanceVenue::send_command_rest(const OrderCommand& cmd,
                                     const OrderShadow* shadow,
                                     bool amend_in_place) {
  if (rest_ == nullptr || (rest_hard_stopped_ && cmd.kind != OrderCommandKind::Cancel)) {
    if (cmd.kind == OrderCommandKind::Cancel) {
      emit_cancel_reject(
          cmd.instrument, cmd.cl_ord_id, RejectReason::VenueReject, 0, "no order channel");
    } else {
      emit_reject(cmd.instrument, cmd.cl_ord_id, RejectReason::VenueReject, 0, "no order channel");
      shadows_.erase(cmd.cl_ord_id);
    }
    ++stats_.order_send_failures;
    return;
  }
  RestRequest rr;
  const Cycles before_encode = rdtscp();
  if (!encoder_->encode_rest(cmd, shadow, venue_time_ms(), rr, amend_in_place)) {
    emit_reject(cmd.instrument, cmd.cl_ord_id, RejectReason::VenueReject, 0, "encode failed");
    return;
  }
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  const std::string headers = api_headers();
  const Cycles after_encode = rdtscp();
  OrderCommand copy = cmd;
  copy.venue_order_id = nullptr;
  copy.header = nullptr;
  std::weak_ptr<int> alive = alive_;
  const bool queued =
      rest_->request(rr.method,
                     target,
                     headers,
                     {},
                     [this, alive, copy, amend_in_place](const net::HttpResponse& r) {
                       if (alive.expired()) return;
                       handle_rest_order_response(copy, r, amend_in_place);
                     });
  if (!queued) {
    emit_reject(cmd.instrument, cmd.cl_ord_id, RejectReason::TransportFull, 0, "rest queue full");
    ++stats_.order_send_failures;
    return;
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  rate_.on_sent(rr.weight, now_ns(), rr.is_order);
  switch (cmd.kind) {
    case OrderCommandKind::New:
      ++stats_.orders_sent;
      break;
    case OrderCommandKind::Cancel:
      ++stats_.cancels_sent;
      break;
    case OrderCommandKind::Replace:
      ++stats_.replaces_sent;
      if (amend_in_place) ++amends_sent_;
      break;
  }
}

void BinanceVenue::handle_rest_order_response(const OrderCommand& cmd,
                                              const net::HttpResponse& r,
                                              bool amend_in_place) {
  ++stats_.rest_requests;
  note_rate_headers(r);
  const RequestKind kind = cmd.kind == OrderCommandKind::New      ? RequestKind::New
                           : cmd.kind == OrderCommandKind::Cancel ? RequestKind::Cancel
                           : amend_in_place                       ? RequestKind::Amend
                                                                  : RequestKind::Replace;
  if (r.error != net::NetError::None) {
    ++stats_.rest_errors;
    // Send status unknown (timeout / closed): reconcile rather than guess.
    if (kind == RequestKind::Cancel) {
      emit_cancel_reject(
          cmd.instrument, cmd.cl_ord_id, RejectReason::VenueReject, 0, net::to_string(r.error));
    } else {
      emit_reject(
          cmd.instrument, cmd.cl_ord_id, RejectReason::VenueReject, 0, net::to_string(r.error));
    }
    request_open_orders();
    return;
  }
  // Re-shape the REST body into a WS API response so both paths share one handler.
  const RequestId rid = make_request_id(kind, cmd.cl_ord_id);
  std::string wrapped = fmt::format(R"({{"id":"{}","status":{},"{}":{}}})",
                                    rid.view(),
                                    r.status,
                                    r.status >= 200 && r.status < 300 ? "result" : "error",
                                    r.body);
  const PaddedJson padded(wrapped);
  WsApiResponse resp;
  if (ws_api_decoder_->decode(padded.view(), resp) != ParseStatus::Ok) {
    emit_reject(cmd.instrument,
                cmd.cl_ord_id,
                RejectReason::VenueReject,
                r.status,
                "unparseable REST reply");
    return;
  }
  if (r.status >= 400 && !resp.is_error) resp.is_error = true;
  handle_order_response(kind, cmd.cl_ord_id, resp);
}

void BinanceVenue::note_rate_headers(const net::HttpResponse& r) {
  // rest-api.md "IP Limits" / "Unfilled Order Count" headers.
  rate_.on_headers(
      header_int(r, "X-MBX-USED-WEIGHT-1M"), header_int(r, "X-MBX-ORDER-COUNT-10S"), now_ns());
}

// First HardStop / Fatal error: the engine trips this venue's kill switch (quotes pulled,
// new orders refused by risk); the other venues keep trading.
void BinanceVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void BinanceVenue::apply_action(VenueAction action,
                                int code,
                                std::string_view msg,
                                std::int64_t retry_after_ms) {
  switch (action) {
    case VenueAction::None:
      break;
    case VenueAction::Backoff:
      rate_.cooldown(1'000'000'000, now_ns());
      break;
    case VenueAction::RateLimit: {
      std::int64_t wait = kDefaultCooldownNs;
      if (retry_after_ms >
          1'000'000'000'000LL) {  // absolute epoch ms (WS API error.data.retryAfter)
        wait = std::max<std::int64_t>(0, retry_after_ms - venue_time_ms()) * kNsPerMs;
      } else if (retry_after_ms > 0) {
        wait = retry_after_ms * kNsPerMs;  // Retry-After seconds converted by the caller
      }
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
      FASTMM_LOG_ERROR("{}: venue rejected a filter/precision rule ({} {}); check tick/lot config",
                       cfg_.name,
                       code,
                       msg);
      break;
    case VenueAction::HardStop:
      rate_.hard_stop();
      rest_hard_stopped_ = true;
      FASTMM_LOG_ERROR("{}: HTTP 418 IP ban: REST stopped until restart", cfg_.name);
      trip_venue_kill(KillReason::VenueHardStop);
      break;
    case VenueAction::Fatal:
      fatal_ = true;
      FASTMM_LOG_ERROR("{}: fatal venue error ({} {}); order entry disabled", cfg_.name, code, msg);
      trip_venue_kill(KillReason::VenueFatal);
      break;
  }
}

// ---- events to the engine -----------------------------------------------------------------

void BinanceVenue::emit_connection_state(Channel ch, ConnState state, std::int32_t reason) {
  if (order_sink_ == nullptr) return;
  // Market-data state travels with the market data (ordering vs deltas matters); order and
  // user channel states travel with the order events.
  const bool md = ch == Channel::Md;
  venues::emit_connection_state(md ? *md_sink_ : *order_sink_, id_, md ? 0 : 1, state, reason);
  FASTMM_LOG_INFO("{}: {} channel -> {}",
                  cfg_.name,
                  ch == Channel::Md     ? "md"
                  : ch == Channel::User ? "user"
                                        : "order",
                  state);
}

void BinanceVenue::emit_reject(
    InstrumentId inst, ClientOrderId id, RejectReason reason, int code, std::string_view text) {
  sent_.answered(id);
  emit_order_reject(*order_sink_, id_, inst, id, reason, code, text);
  ++stats_.order_events;
}

void BinanceVenue::emit_cancel_reject(
    InstrumentId inst, ClientOrderId id, RejectReason reason, int code, std::string_view text) {
  venues::emit_cancel_reject(*order_sink_, id_, inst, id, reason, code, text);
  ++stats_.order_events;
}

void BinanceVenue::emit_ack(InstrumentId inst,
                            ClientOrderId id,
                            std::int64_t order_id,
                            bool amended_in_place) {
  // GET /api/v3/myTrades names the order by orderId only, so the pairing has to be kept here.
  // An amend in place keeps the orderId under a new client id: the latest pairing wins.
  if (order_id > 0) static_cast<void>(order_ids_.assign(static_cast<std::uint64_t>(order_id), id));
  emit_order_ack(*order_sink_,
                 id_,
                 inst,
                 id,
                 IdText(order_id).view(),
                 amended_in_place ? OrderAckMsg::kAmendedInPlace : 0);
  ++stats_.order_events;
}

void BinanceVenue::emit_cancel_ack(InstrumentId inst,
                                   ClientOrderId id,
                                   std::int64_t order_id,
                                   std::string_view executed_qty) {
  const auto q = parse_qty(executed_qty);
  venues::emit_cancel_ack(*order_sink_, id_, inst, id, IdText(order_id).view(), q ? *q : Qty{});
  ++stats_.order_events;
}

// Decodes the whole open-order snapshot before anything reaches the engine: Oms::reconcile_end()
// cancels every order the snapshot does not name, so a reply that did not parse must not be
// emitted as an empty snapshot.
void BinanceVenue::emit_reconcile(std::string_view json,
                                  bool rest_array,
                                  ClientOrderId sent_watermark) {
  reconcile_records_.clear();
  const PaddedJson padded(json);
  const ParseStatus st =
      ws_api_decoder_->decode_open_orders(padded.view(), rest_array, [&](const OpenOrderRecord& o) {
        const InstrumentId inst = instrument_of(o.symbol);
        if (!inst.valid()) return;  // another symbol on this account: not ours
        ReconcileMsg m{};
        init_header(m, EventType::Reconcile, inst, id_);
        m.kind = ReconcileMsg::Kind::OpenOrder;
        m.side = o.side == "SELL" ? Side::Sell : Side::Buy;
        m.state = o.status == "PARTIALLY_FILLED" ? OrderState::PartiallyFilled : OrderState::Live;
        if (const auto id = decode_cl_ord_id(o.client_order_id)) m.cl_ord_id = *id;
        if (o.order_id > 0 && m.cl_ord_id.valid())
          static_cast<void>(order_ids_.assign(static_cast<std::uint64_t>(o.order_id), m.cl_ord_id));
        m.venue_order_id.assign(IdText(o.order_id).view());
        if (const auto p = parse_price(o.price)) m.price = *p;
        if (const auto q = parse_qty(o.orig_qty)) m.orig_qty = *q;
        if (const auto q = parse_qty(o.executed_qty)) m.cum_qty = *q;
        m.hdr.recv_ts = wall_now();
        reconcile_records_.push_back(m);
      });
  if (st != ParseStatus::Ok) {
    FASTMM_LOG_WARN("{}: open orders reply could not be parsed; reconciliation skipped", cfg_.name);
    reconcile_records_.clear();
    return;
  }
  ReconcileMsg begin{};
  init_header(begin, EventType::Reconcile, InstrumentId::invalid(), id_);
  begin.kind = ReconcileMsg::Kind::Begin;
  SentWatermark::stamp(begin, sent_watermark);
  if (exec_snapshot_exact_) begin.flags |= ReconcileMsg::kExecutionsExact;
  exec_snapshot_exact_ = false;
  begin.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(begin.hdr));
  for (const ReconcileMsg& m : reconcile_records_) static_cast<void>(order_sink_->push(m.hdr));
  ReconcileMsg end{};
  init_header(end, EventType::Reconcile, InstrumentId::invalid(), id_);
  end.kind = ReconcileMsg::Kind::End;
  end.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(end.hdr));
  FASTMM_LOG_INFO("{}: reconciled {} open orders", cfg_.name, reconcile_records_.size());
  sweep_shadows(sent_watermark);
  reconcile_records_.clear();
}

// An order's shadow is dropped when its terminal event arrives. When that event is lost with a
// connection - the REST cancel-all that follows an order-channel drop is the usual way - the
// shadow stays, and the table is fixed-size: enough of them and a new order gets no shadow, so its
// replace and cancel are refused as "original unknown". A snapshot settles it: an order the venue
// does not hold, sent before the snapshot was asked for, is over. Orders sent after the request
// (ids above the watermark) may simply not have reached the venue yet and keep their shadows.
void BinanceVenue::sweep_shadows(ClientOrderId sent_watermark) {
  if (!sent_watermark.valid()) return;  // no watermark: nothing tells old from in flight
  std::vector<ClientOrderId> dead;
  shadows_.for_each_key([&](ClientOrderId id) {
    if (id.value > sent_watermark.value) return;
    for (const ReconcileMsg& m : reconcile_records_) {
      if (m.cl_ord_id == id) return;
    }
    dead.push_back(id);
  });
  for (const ClientOrderId id : dead) shadows_.erase(id);
  if (!dead.empty()) {
    stats_.shadows_swept += dead.size();
    FASTMM_LOG_INFO(
        "{}: dropped {} order shadow(s) the venue no longer holds", cfg_.name, dead.size());
  }
}

// ---- control requests -----------------------------------------------------------------------

void BinanceVenue::request_open_orders() {
  request_open_orders(sent_.value(now_ns()));
}

// `watermark` bounds what the snapshot may conclude: the engine's orders above it had not been
// sent when it was asked for, so their absence means nothing. The start-up sweep passes an empty
// id, which makes the whole snapshot read-only for our own orders - an order sent over REST before
// the order channel came up can still be in flight, and its absence is not proof that it is gone.
// Orders the snapshot reports that the engine does not know are still cancelled: that is the point
// of the sweep.
void BinanceVenue::request_open_orders(ClientOrderId watermark) {
  if (cfg_.dry_run || !connected_ || !signer_.usable()) return;
  // One reconciliation at a time: a second request while the executions are being fetched is
  // remembered and served once they are in, so its snapshot is exact too.
  if (exec_replay_active_) {
    oo_wanted_ = true;
    oo_wanted_watermark_ = watermark;
    return;
  }
  // Latched before the replay starts: a query that cannot be issued at all finishes inside
  // request_executions(), and the snapshot it releases must already be the one that was asked for.
  oo_wanted_ = true;
  oo_wanted_watermark_ = watermark;
  if (request_executions()) return;
  oo_wanted_ = false;
  send_open_orders(watermark);
}

void BinanceVenue::send_open_orders(ClientOrderId watermark) {
  if (cfg_.ws_order_api && order_conn_.is_live()) {
    const std::size_t n = encoder_->encode_ws_open_orders({}, "oo", venue_time_ms(), request_buf_);
    if (n > 0 && order_conn_.send_text(std::string_view(request_buf_, n))) {
      rate_.on_sent(80, now_ns());
      oo_watermarks_.push_back(watermark);
      return;
    }
  }
  if (rest_ == nullptr || rest_hard_stopped_) return;
  RestRequest rr;
  if (!encoder_->encode_rest_open_orders({}, venue_time_ms(), rr)) return;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, watermark](const net::HttpResponse& r) {
        if (alive.expired()) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET openOrders failed: status={} err={}",
                          cfg_.name,
                          r.status,
                          net::to_string(r.error));
          return;
        }
        emit_reconcile(r.body, /*rest_array=*/true, watermark);
      });
  if (queued) rate_.on_sent(rr.weight, now_ns());
}

// ---- execution replay -------------------------------------------------------------------------
//
// GET /api/v3/myTrades per subscribed symbol, before every open-order snapshot. Every execution it
// returns is emitted as an ordinary fill carrying Binance's trade id, so the OMS keeps the ones it
// never saw and drops the rest. This is what makes a fill that *finished* an order recoverable: the
// snapshot no longer mentions such an order at all, so nothing else would ever report it.
//
// Where the replay starts, in order of preference: the trade id after the last one this connector
// forwarded for that symbol (`fromId`, which the venue will not take together with a time range),
// otherwise the venue time of the last execution the engine booked (`startTime`, which the store
// seeds at start-up). Binance looks back at most 24 hours, so a watermark older than that is
// clamped and the replay reports itself as incomplete: the snapshot then carries no
// kExecutionsExact and the engine says the reconciliation was an estimate instead of pretending it
// was exact.

std::size_t BinanceVenue::exec_slot(InstrumentId id) const noexcept {
  for (std::size_t i = 0; i < subscribed_.size(); ++i)
    if (subscribed_[i] == id) return i;
  return subscribed_.size();
}

void BinanceVenue::resume_executions(std::int64_t since_venue_ms,
                                     const std::vector<std::string>& known) {
  exec_since_ms_ = since_venue_ms;
  known_exec_ids_ = {known.begin(), known.end()};
}

void BinanceVenue::resume_trade_ids(
    const std::vector<std::pair<InstrumentId, std::int64_t>>& next_ids) {
  resume_from_ids_ = next_ids;
}

bool BinanceVenue::request_executions(std::int64_t since_venue_ms) {
  if (cfg_.dry_run || !connected_ || !signer_.usable()) return false;
  if (rest_ == nullptr || rest_hard_stopped_ || subscribed_.empty()) return false;
  if (exec_replay_active_) return true;
  // An explicit start overrides both the time watermark and the per-symbol trade ids: the caller
  // is saying it knows of executions this connector never heard about.
  if (since_venue_ms > 0) {
    exec_since_ms_ = since_venue_ms;
    exec_from_id_.assign(subscribed_.size(), 0);
  }
  exec_from_id_.resize(subscribed_.size(), 0);
  // A restart's exact start: the trade after the last one the earlier session booked.
  for (const auto& [id, next] : resume_from_ids_) {
    if (const std::size_t slot = exec_slot(id); slot < exec_from_id_.size() && next > 0)
      exec_from_id_[slot] = next;
  }
  resume_from_ids_.clear();
  exec_last_ns_ = net::Reactor::now_ns();
  exec_replay_active_ = true;
  exec_replay_ok_ = true;
  ++stats_.execution_queries;
  // Held by the loop itself, so a reply that lands inside it cannot finish the replay early.
  exec_pending_ = 1;
  for (const InstrumentId id : subscribed_) {
    if (!request_executions_for(id)) exec_replay_ok_ = false;
  }
  finish_execution_replay(true);
  return true;
}

bool BinanceVenue::request_executions_for(InstrumentId id) {
  const std::string_view symbol =
      symbols_ == nullptr ? std::string_view{} : symbols_->venue_symbol(id);
  if (symbol.empty()) return false;
  const std::size_t slot = exec_slot(id);
  const std::int64_t from_id = slot < exec_from_id_.size() ? exec_from_id_[slot] : 0;
  std::int64_t start_ms = exec_since_ms_;
  // Complete unless the watermark asks for more history than the venue will look back over. A
  // connector that was never told where to start has nothing earlier to miss.
  bool complete = true;
  if (from_id <= 0) {
    const std::int64_t floor_ms = venue_time_ms() - kMyTradesMaxLookbackMs;
    if (start_ms < floor_ms) {
      complete = start_ms <= 0;
      start_ms = floor_ms;
    }
  }
  RestRequest rr;
  if (!encoder_->encode_rest_my_trades(
          symbol, from_id, start_ms, kMyTradesLimit, venue_time_ms(), rr))
    return false;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  ++exec_pending_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, id, complete](const net::HttpResponse& r) {
        if (alive.expired()) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.rest_errors;
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR(
              "{}: GET myTrades failed: status={} err={}; this reconciliation cannot book the "
              "fills the private stream missed",
              cfg_.name,
              r.status,
              net::to_string(r.error));
          finish_execution_replay(false);
          return;
        }
        emit_executions(id, r.body);
        finish_execution_replay(complete);
      });
  if (!queued) {
    --exec_pending_;
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: no room to ask for the account's executions", cfg_.name);
    return false;
  }
  rate_.on_sent(rr.weight, now_ns());
  return true;
}

void BinanceVenue::emit_executions(InstrumentId id, std::string_view json) {
  if (instruments_ == nullptr || !instruments_->contains(id)) return;
  const Instrument& inst = instruments_->get(id);
  const std::size_t slot = exec_slot(id);
  const PaddedJson padded(json);
  std::int64_t high_id = 0;
  std::int64_t high_ms = 0;
  std::size_t count = 0;
  const ParseStatus st =
      ws_api_decoder_->decode_my_trades(padded.view(), [&](const MyTradeRecord& t) {
        if (!known_exec_ids_.empty() &&
            known_exec_ids_.count(std::string(IdText(t.id).view())) != 0) {
          if (t.id > high_id) high_id = t.id;
          if (t.time_ms > high_ms) high_ms = t.time_ms;
          return;  // the earlier session booked it
        }
        const ClientOrderId* mapped = order_ids_.find(static_cast<std::uint64_t>(t.order_id));
        if (!emit_trade_history_fill(
                *order_sink_, id_, inst, id, mapped != nullptr ? *mapped : ClientOrderId{}, t))
          return;
        ++stats_.order_events;
        ++stats_.executions_fetched;
        ++count;
        if (t.id > high_id) high_id = t.id;
        if (t.time_ms > high_ms) high_ms = t.time_ms;
      });
  if (st != ParseStatus::Ok) {
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: myTrades reply could not be parsed; its executions are lost", cfg_.name);
    exec_replay_ok_ = false;
    return;
  }
  if (slot < exec_from_id_.size() && high_id > 0) exec_from_id_[slot] = high_id + 1;
  if (high_ms > exec_since_ms_) exec_since_ms_ = high_ms;
  if (count >= static_cast<std::size_t>(kMyTradesLimit)) {
    // A full page is not proof there is nothing behind it. The next replay carries on from where
    // this one stopped, so nothing is lost, but this snapshot cannot claim to be exact.
    exec_replay_ok_ = false;
    FASTMM_LOG_WARN(
        "{}: myTrades returned a full page ({}); more executions are waiting", cfg_.name, count);
  }
  if (count > 0) FASTMM_LOG_INFO("{}: replayed {} execution(s)", cfg_.name, count);
}

void BinanceVenue::finish_execution_replay(bool ok) {
  if (!ok) exec_replay_ok_ = false;
  if (exec_pending_ > 0) --exec_pending_;
  if (exec_pending_ > 0) return;
  exec_replay_active_ = false;
  exec_snapshot_exact_ = exec_replay_ok_;
  if (!exec_replay_ok_) exec_retry_wanted_ = true;
  if (!oo_wanted_) return;
  oo_wanted_ = false;
  const ClientOrderId wm = oo_wanted_watermark_;
  oo_wanted_watermark_ = ClientOrderId{};
  send_open_orders(wm);
}

void BinanceVenue::request_server_time() {
  if (rest_ == nullptr || time_request_pending_ || rest_hard_stopped_) return;
  time_request_pending_ = true;
  time_request_sent_ns_ = now_ns();
  std::weak_ptr<int> alive = alive_;
  const bool queued =
      rest_->request("GET", "/api/v3/time", {}, {}, [this, alive](const net::HttpResponse& r) {
        if (alive.expired()) return;
        time_request_pending_ = false;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.rest_errors;
          return;
        }
        std::int64_t server_ms = 0;
        if (!decode_server_time(r.body, server_ms).empty()) return;
        const std::int64_t local_recv_ms = wall_now().ns / kNsPerMs;
        // Receive-time estimate: the first request on a fresh connection includes the TCP/TLS
        // handshake, which a send/receive midpoint would count as clock skew.
        clock_offset_ms_ = server_ms - local_recv_ms;
        clock_sync_ns_ = now_ns();
        clock_resync_wanted_ = false;
        stats_.clock_offset_ms = clock_offset_ms_;
        if (clock_offset_ms_ > 1000 || clock_offset_ms_ < -1000)
          FASTMM_LOG_WARN("{}: clock offset to venue is {} ms (recvWindow {} ms)",
                          cfg_.name,
                          clock_offset_ms_.load(),
                          cfg_.recv_window_ms);
      });
  if (!queued) time_request_pending_ = false;
  rate_.on_sent(1, now_ns());  // GET /api/v3/time: "IP Weight 1" (general endpoints, 2026-09-14)
}

void BinanceVenue::request_listen_key() {
  // Legacy listenKey flow, kept for the local simulator and older deployments: POST
  // /api/v3/userDataStream returns {"listenKey": "..."}; keepalive with PUT every 30 minutes
  // (key valid 60 minutes, per the documentation removed on 2025-10-24). Binance deprecated
  // listenKey streams on 2025-04-07 and removed POST/PUT/DELETE /api/v3/userDataStream (and
  // userDataStream.start/ping/stop) from 2026-02-20 (spot API changelog); the testnet answers
  // 410 Gone (observed 2026-09-13), in which case we switch to userDataStream.subscribe.
  if (rest_ == nullptr || !signer_.usable()) return;
  std::weak_ptr<int> alive = alive_;
  rest_->request("POST",
                 "/api/v3/userDataStream",
                 api_headers(),
                 {},
                 [this, alive](const net::HttpResponse& r) {
                   if (alive.expired()) return;
                   ++stats_.rest_requests;
                   if (r.status == 410) {
                     FASTMM_LOG_WARN(
                         "{}: listenKey user streams are gone (HTTP 410); switching to the "
                         "WebSocket API user stream",
                         cfg_.name);
                     cfg_.user_stream = UserStreamMode::WsApi;
                     open_user();
                     return;
                   }
                   if (!r.ok() || !decode_listen_key(r.body, listen_key_).empty()) {
                     ++stats_.rest_errors;
                     FASTMM_LOG_WARN(
                         "{}: listenKey request failed (status {}); retrying on the next timer",
                         cfg_.name,
                         r.status);
                     listen_key_.clear();
                     return;
                   }
                   listen_key_refresh_ns_ = now_ns();
                   open_user();
                 });
}

void BinanceVenue::keepalive_listen_key() {
  if (rest_ == nullptr || listen_key_.empty()) return;
  const std::string target = "/api/v3/userDataStream?listenKey=" + listen_key_;
  std::weak_ptr<int> alive = alive_;
  rest_->request("PUT", target, api_headers(), {}, [this, alive](const net::HttpResponse& r) {
    if (alive.expired()) return;
    ++stats_.rest_requests;
    if (r.ok()) {
      listen_key_refresh_ns_ = now_ns();
      return;
    }
    ++stats_.rest_errors;
    // Expired: get a fresh key and reconnect the user stream, then reconcile (6.4).
    FASTMM_LOG_WARN(
        "{}: listenKey keepalive failed (status {}); requesting a new key", cfg_.name, r.status);
    listen_key_.clear();
    user_conn_.close();
    request_listen_key();
  });
}

void BinanceVenue::cancel_all_async() {
  // No rest_hard_stopped_ check: cancelling is what a hard stop asks for.
  if (rest_ == nullptr || !signer_.usable()) return;
  for (InstrumentId id : subscribed_) {
    RestRequest rr;
    if (!encoder_->encode_rest_cancel_all(symbols_->venue_symbol(id), venue_time_ms(), rr))
      continue;
    const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
    std::weak_ptr<int> alive = alive_;
    rest_->request(
        "DELETE", target, api_headers(), {}, [this, alive, id](const net::HttpResponse& r) {
          if (alive.expired() || r.error == net::NetError::Canceled) return;
          ++stats_.rest_requests;
          note_rate_headers(r);
          int code = 0;
          std::string msg;
          const bool nothing_open =
              r.status == 400 && decode_rest_error(r.body, code, msg) && code == -2011;
          if (!r.ok() && !nothing_open) {
            ++stats_.rest_errors;
            FASTMM_LOG_ERROR("{}: cancel-all for {} failed: status={} err={}",
                             cfg_.name,
                             symbols_->venue_symbol(id),
                             r.status,
                             net::to_string(r.error));
          }
        });
  }
}

bool BinanceVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr) return true;
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  bool all_ok = true;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    BinanceOrderEncoder enc(signer_, *symbols_, cfg_.recv_window_ms);  // thread-local copy
    for (InstrumentId id : subscribed_) {
      RestRequest rr;
      if (!enc.encode_rest_cancel_all(symbols_->venue_symbol(id), venue_time_ms(), rr)) {
        all_ok = false;
        continue;
      }
      const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
      // A rate limit is the one refusal worth waiting out here: the kill switch has no other
      // remedy than this call, and a caller that takes the first `false` as final leaves the book
      // on. Bounded, because a real IP ban lasts minutes and the caller must not hang on it -
      // past these attempts the false is the truth and the operator is told.
      HttpReply reply = http.request("DELETE", target, api_headers());
      for (int attempt = 0;
           attempt < kCancelAllRateLimitRetries && (reply.status == 418 || reply.status == 429);
           ++attempt) {
        FASTMM_LOG_WARN("{}: kill-switch cancel-all for {} rate limited ({}); retrying",
                        cfg_.name,
                        symbols_->venue_symbol(id),
                        reply.status);
        std::this_thread::sleep_for(std::chrono::milliseconds(kCancelAllRetryMs));
        RestRequest again;
        if (!enc.encode_rest_cancel_all(symbols_->venue_symbol(id), venue_time_ms(), again)) break;
        reply = http.request("DELETE",
                             std::string(again.path) + "?" + std::string(again.query.view()),
                             api_headers());
      }
      int code = 0;
      std::string msg;
      const bool nothing_open =
          reply.status == 400 && decode_rest_error(reply.body, code, msg) && code == -2011;
      if (!reply.ok() && !nothing_open) {
        all_ok = false;
        FASTMM_LOG_ERROR("{}: kill-switch cancel-all for {} failed: {} {}",
                         cfg_.name,
                         symbols_->venue_symbol(id),
                         reply.status,
                         reply.error.empty() ? reply.body.substr(0, 120) : reply.error);
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

// ---- housekeeping ---------------------------------------------------------------------------

void BinanceVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (cfg_.user_stream == UserStreamMode::ListenKey && !cfg_.dry_run) {
    if (listen_key_.empty()) {
      request_listen_key();
    } else if (now - listen_key_refresh_ns_ >= kListenKeyKeepaliveNs) {
      keepalive_listen_key();
    }
  }
  if (clock_resync_wanted_ || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  // A replay that could not be completed left fills unaccounted for, and the next reconnect may be
  // hours away. Ask again until the venue answers, keeping the watermark where it was so nothing is
  // skipped; the snapshot is not repeated, only the executions.
  if (exec_retry_wanted_ && !exec_replay_active_ && now - exec_retry_ns_ >= kExecutionRetryNs) {
    exec_retry_ns_ = now;
    exec_retry_wanted_ = false;
    static_cast<void>(request_executions());
  }
  // And while nothing is wrong: the watermark moves only when a replay runs and the OMS remembers a
  // bounded number of executions, so a reconnect after hours of streaming would replay more than
  // it can recognise. A replay a minute keeps that short, and books a fill the private stream
  // dropped without disconnecting.
  if (!exec_replay_active_ && !exec_retry_wanted_ && now - exec_last_ns_ >= kExecutionSweepNs) {
    static_cast<void>(request_executions());
  }
  publish_status();
  raw_md_.flush();
  raw_user_.flush();
  raw_order_.flush();
  if (reactor_ != nullptr && housekeeping_timer_ == net::kInvalidTimer) {
    std::weak_ptr<int> alive = alive_;
    housekeeping_timer_ = reactor_->add_timer_after(kHousekeepingNs, [this, alive] {
      if (alive.expired()) return;
      housekeeping_timer_ = net::kInvalidTimer;
      on_timer(now_ns());
    });
  }
}

void BinanceVenue::publish_status() noexcept {
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_;
  wire_.summarize(
      tsc_calibration(), stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
}

VenueStatus BinanceVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

std::string derive_sbe_ws_url(std::string_view ws_url) {
  const auto u = net::Url::parse(ws_url);
  if (!u) return {};
  std::string host(u->host);
  if (host.starts_with("stream.")) {
    host.insert(6, "-sbe");
  } else if (host.starts_with("demo-stream.")) {
    host.insert(11, "-sbe");
  } else {
    return {};
  }
  net::Url copy = *u;
  copy.host = host;
  return origin_of(copy) + std::string(u->path);
}

// Ed25519 private key (PKCS#8 PEM) from a file or an environment variable holding the PEM.
// Live sessions refuse a key that does not parse; a dry run carries on without it (market data
// needs only the API key).
std::string load_ed25519_pem(const std::string& venue,
                             const std::string& path,
                             const std::string& env,
                             bool dry_run) {
  std::string pem;
  std::string source;
  if (!env.empty()) {
    source = "$" + env;
    if (const char* e = std::getenv(env.c_str()); e != nullptr) pem = e;
  } else if (!path.empty()) {
    source = path;
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    pem = ss.str();
  }
  if (!net::Ed25519Key::from_private_pem(pem).has_private()) {
    const std::string why = "venue '" + venue +
                            "': key_type = ed25519 needs private_key_file or private_key_env " +
                            "with a PKCS#8 Ed25519 private key" +
                            (source.empty() ? "" : " (" + source + " is not one)");
    if (!dry_run) throw std::invalid_argument(why);
    FASTMM_LOG_WARN("{}; dry run continues without order entry", why);
  }
  return pem;
}

BinanceVenueConfig make_binance_config(const VenueSectionView& v, bool dry_run) {
  BinanceVenueConfig c;
  c.name = v.name;
  c.ws_url = v.ws_url;
  c.ws_api_url = v.ws_api_url;
  c.rest_url = v.rest_url;
  c.recv_window_ms = v.recv_window_ms > 0 ? v.recv_window_ms : kDefaultRecvWindowMs;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.supports_replace = v.supports_replace;
  c.dry_run = dry_run;
  c.credentials.api_key = v.api_key;
  c.credentials.secret.value = v.api_secret;
  const VenueExtras x(v.extra);
  auto extra = [&](const char* key) { return x.get(key); };
  const std::string key_type = extra("key_type");
  if (!key_type.empty() && key_type != "hmac" && key_type != "ed25519")
    throw std::invalid_argument("venue '" + v.name + "': key_type must be hmac or ed25519");
  if (key_type == "ed25519") {
    c.credentials.type = KeyType::Ed25519;
    c.credentials.private_key_pem.value =
        load_ed25519_pem(v.name, extra("private_key_file"), extra("private_key_env"), dry_run);
  }
  const std::string md_format = extra("md_format");
  if (md_format == "sbe") {
    c.md_format = MdFormat::Sbe;
  } else if (!md_format.empty() && md_format != "json") {
    throw std::invalid_argument("venue '" + v.name + "': md_format must be json or sbe");
  }
  if (c.md_format == MdFormat::Sbe) {
    // sbe-market-data-streams.md: "An API Key is necessary for access"; only Ed25519 keys.
    if (c.credentials.api_key.empty() || c.credentials.type != KeyType::Ed25519)
      throw std::invalid_argument("venue '" + v.name +
                                  "': md_format = \"sbe\" needs api_key with key_type = "
                                  "\"ed25519\" (SBE streams accept Ed25519 keys only)");
    c.sbe_ws_url = extra("sbe_ws_url");
    if (c.sbe_ws_url.empty()) c.sbe_ws_url = derive_sbe_ws_url(c.ws_url);
    if (c.sbe_ws_url.empty())
      throw std::invalid_argument("venue '" + v.name +
                                  "': md_format = \"sbe\" needs sbe_ws_url (cannot derive it "
                                  "from ws_url " +
                                  c.ws_url + ")");
  }
  const std::string us = extra("user_stream");
  if (us == "ws_api") c.user_stream = UserStreamMode::WsApi;
  if (us == "listen_key") c.user_stream = UserStreamMode::ListenKey;
  if (us == "none") c.user_stream = UserStreamMode::None;
  if (extra("order_api") == "rest") c.ws_order_api = false;
  c.depth_limit =
      static_cast<int>(std::clamp<std::int64_t>(x.integer("depth_limit", c.depth_limit), 5, 5000));
  c.stale_ms = static_cast<std::uint32_t>(x.integer("stale_ms", c.stale_ms));
  c.dead_ms = static_cast<std::uint32_t>(x.integer("dead_ms", c.dead_ms));
  c.position_from_balance = x.flag("position_from_balance", false);
  c.fetch_fees = x.flag("fetch_fees", false);
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.amend_keep_priority = x.flag("amend_keep_priority", true);
  c.max_order_amends = static_cast<std::uint16_t>(
      std::clamp<std::int64_t>(x.integer("max_order_amends", c.max_order_amends), 0, 65535));
  c.emit_ack_from_response = x.flag("emit_ack_from_response", true);
  return c;
}

}  // namespace fastmm::venues::binance
