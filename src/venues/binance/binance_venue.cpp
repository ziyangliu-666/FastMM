#include "fastmm/venues/binance/binance_venue.hpp"

#include "fastmm/venues/binance/binance_rest_decoder.hpp"
#include "fastmm/venues/binance/binance_trade_history.hpp"
#include "fastmm/venues/blocking_control.hpp"
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
// rest-api.md "Account trade list": limit max 1000, and "the time between startTime and endTime
// can't be longer than 24 hours". The history itself is not limited.
constexpr int kMyTradesLimit = 1000;
constexpr std::int64_t kMyTradesWindowMs = 24LL * 3600 * 1000;
constexpr std::size_t kMyTradesMaxPages = 10;  // per symbol and replay (weight 20 each)
constexpr std::uint32_t kMyTradesWeight = 20;
// How long a blocking start-up request waits for its weight (wait_for_weight): a window and some.
constexpr std::int64_t kStartupWaitNs = 65'000'000'000;
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
    : id_(id),
      cfg_(std::move(cfg)),
      md_venue_(cfg_.pool_of.valid() ? cfg_.pool_of : id),
      signer_(cfg_.credentials),
      rate_(cfg_.rate_threshold) {
  // Binance counts request weight per IP: the accounts of a pool spend one window.
  if (cfg_.share_ip_weight) rate_.share_ip(shared_rate("binance_spot " + cfg_.rest_url));
  if (cfg_.user_stream == UserStreamMode::Auto)
    cfg_.user_stream = signer_.usable() ? UserStreamMode::WsApi : UserStreamMode::None;
  if (cfg_.dry_run) cfg_.user_stream = UserStreamMode::None;
  std::memset(scratch_, 0, sizeof scratch_);
  ReplayLimits limits;
  limits.window_ms = kMyTradesWindowMs;
  limits.page_rows = kMyTradesLimit;
  limits.max_pages = kMyTradesMaxPages;
  exec_replay_.setup(
      cfg_.name,
      "execution(s)",
      limits,
      {[this] { return exec_ready(); },
       [this] { return venue_time_ms(); },
       [this](const ReplayQuery& q) { return query_executions(q); },
       [this](bool complete) { reconcile_.replay_done(complete); },
       [this](const ReplayLookup& l) { return lookup_order(l); },
       [this] { return rate_.can_send(kMyTradesWeight, now_ns(), false, RateLimiter::kBulkShare); },
       // A sweep reads the instruments with order activity since their watermark.
       [this](std::size_t stream, std::int64_t since_ms) {
         return stream >= subscribed_.size() ||
                activity_.since(subscribed_[stream], since_ms, venue_time_ms(), now_ns());
       }},
      [this](std::size_t stream, const MyTradeRow& t) { return emit_execution(stream, t); },
      [this](std::size_t, const MyTradeRow& t) {
        return t.order_id > 0 && order_ids_.find(static_cast<std::uint64_t>(t.order_id)) == nullptr
                   ? std::string(IdText(t.order_id).view())
                   : std::string{};
      });
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
  return symbols_ != nullptr ? symbols_->find(md_venue_, symbol) : InstrumentId::invalid();
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
  // A pool member loads the primary's symbols too: the same filters, and its own rate limits and
  // clock offset come with them.
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == md_venue_) mine.push_back(&instruments.get(inst.id));
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
    publish_status();  // the offset is known before the venue connects (fastmm-gateway reads it)
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
      // One per symbol, before the connections open: within the bulk share of the weight.
      wait_for_weight(rate_, rr.weight, now_ns(), kStartupWaitNs, [](std::int64_t ns) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
      });
      const HttpReply reply = http.request(
          "GET", std::string(rr.path) + "?" + std::string(rr.query.view()), api_headers());
      rate_.on_sent(rr.weight, now_ns());
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
  md_feed_->set_log_name(cfg_.name);
  user_parser_ = std::make_unique<BinanceUserParser>(symbols, instruments, id_);
  user_parser_->set_symbol_venue(md_venue_);
  encoder_ = std::make_unique<BinanceOrderEncoder>(signer_, symbols, cfg_.recv_window_ms);
  ws_api_decoder_ = std::make_unique<BinanceWsApiDecoder>();
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments, md_venue_);
  user_parser_->set_balance_assets(&reconcile_.assets());
}

// A pool member subscribes the primary's instruments for its order path (execution replay,
// cancel-all, open orders) and nothing for market data: it has no books.
void BinanceVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != md_venue_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_ && !pool_member()) md_feed_->add_instrument(id);
  }
  exec_replay_.set_streams(subscribed_.size());
  std::size_t tracked = 0;
  for (InstrumentId id : subscribed_) tracked = std::max<std::size_t>(tracked, id.value + 1U);
  activity_.track(tracked, now_ns());
  stats_.books_total = pool_member() ? 0 : static_cast<std::uint32_t>(subscribed_.size());
  depth_limit_ = cfg_.depth_limit > 0 ? cfg_.depth_limit : auto_depth_limit(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
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
  reconcile_.open(!cfg_.dry_run && signer_.usable());
  // Nobody said where the execution replay should start, so it starts here: this session can only
  // have missed what happened after it connected, and replaying further back would book another
  // session's fills into a position that starts at zero.
  exec_replay_.set_streams(subscribed_.size());
  exec_replay_.start_at(venue_time_ms());
  // A restart's exact start: the trade after the last one the earlier session booked.
  for (const auto& [id, next] : resume_from_ids_) {
    const auto it = std::find(subscribed_.begin(), subscribed_.end(), id);
    if (it != subscribed_.end())
      exec_replay_.set_cursor(static_cast<std::size_t>(it - subscribed_.begin()), next);
  }
  resume_from_ids_.clear();
  for (auto& [id, ids] : resume_known_ids_) {
    const auto it = std::find(subscribed_.begin(), subscribed_.end(), id);
    if (it != subscribed_.end())
      exec_replay_.set_known_ids(static_cast<std::size_t>(it - subscribed_.begin()),
                                 std::move(ids));
  }
  resume_known_ids_.clear();
  exec_replay_.open(!cfg_.dry_run && signer_.usable());
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    raw_user_.open(cfg_.record_raw_dir, cfg_.name, "user");
    raw_order_.open(cfg_.record_raw_dir, cfg_.name, "order");
  }
  open_rest();
  request_server_time();
  if (!pool_member()) open_md();  // a member reads no market data: the primary has the books
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
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, user_stream={}, ws_orders={}, pool_member={})",
                  cfg_.name,
                  cfg_.dry_run,
                  static_cast<int>(cfg_.user_stream),
                  cfg_.ws_order_api,
                  pool_member());
}

void BinanceVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  // Before the reset below: a replay or snapshot request it aborts must not start another one
  // (it used to, on a fresh REST connection, during shutdown), nor count as a failed query.
  reconcile_.close();
  exec_replay_.close();
  oo_ws_generation_ = 0;
  bal_ws_generation_ = 0;
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
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
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
  // Within the bulk share of the weight: a start with many symbols sends what fits and the depth
  // sync asks again from the housekeeping timer for the rest.
  const std::uint32_t weight = depth_weight(depth_limit_);
  if (!rate_.can_send(weight, now_ns(), false, RateLimiter::kBulkShare)) {
    md_feed_->on_snapshot_failed(id, now_ns());
    return;
  }
  const std::string target =
      fmt::format("/api/v3/depth?symbol={}&limit={}", symbols_->venue_symbol(id), depth_limit_);
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
    const std::int64_t now = r.count > 0 ? now_ns() : 0;
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      off += h->len;
      if (h->type == EventType::PositionUpdate) {
        if (!cfg_.position_from_balance) continue;
      } else {
        activity_.note(h->instrument, now);
      }
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
    // them (ReconcileDriver::sweep).
    const bool first_connect = !order_was_live_;
    order_was_live_ = true;
    // Stale is not reported for a quiet order channel, so neither is the return from it.
    if (prev != ConnState::Stale) emit_connection_state(Channel::Order, ConnState::Live);
    drain_outbound();  // anything queued while the channel was down
    if (reconnected) {
      reconcile_.request();
    } else if (first_connect) {
      reconcile_.sweep();
    }
    return;
  }
  if (mapped == ConnState::Stale) return;  // quiet order channels are normal
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    session_logged_on_ = false;
    encoder_->set_session_authenticated(false);
    emit_connection_state(Channel::Order, ConnState::Disconnected);
    // An openOrders.status on the closed connection gets no reply.
    if (oo_ws_generation_ != 0 && reconcile_.current(oo_ws_generation_))
      reconcile_.transport_lost();
    oo_ws_generation_ = 0;
    // Nor does an account.status: asked again after the driver's retry delay.
    if (bal_ws_generation_ != 0) reconcile_.balances_fetched(bal_ws_generation_, false, 0);
    bal_ws_generation_ = 0;
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
  budget_pub_.store(budget_of(rate_, now_ns()));
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
    const std::uint64_t gen = oo_ws_generation_;
    oo_ws_generation_ = 0;
    if (r.is_error) {
      FASTMM_LOG_WARN("{}: openOrders.status failed: {} {}", cfg_.name, r.code, r.msg);
      reconcile_.fetched(gen, false);
      return;
    }
    on_open_orders(gen, raw, /*rest_array=*/false);
    return;
  }
  if (r.id == "bal") {
    const std::uint64_t gen = bal_ws_generation_;
    bal_ws_generation_ = 0;
    if (r.is_error) {
      FASTMM_LOG_WARN("{}: account.status failed: {} {}", cfg_.name, r.code, r.msg);
      reconcile_.balances_fetched(gen, false, 0);
      return;
    }
    on_account(gen, raw);
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
        if (cfg_.emit_ack_from_response) emit_ack(inst, id, r.order_id, r.transact_time_ms);
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
        emit_cancel_ack(inst, target, r.order_id, r.executed_qty, r.transact_time_ms);
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
        if (cfg_.emit_ack_from_response) emit_ack(inst, id, r.order_id, r.transact_time_ms, true);
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
          emit_cancel_ack(
              inst, orig, r.cancel_order_id, r.cancel_executed_qty, r.cancel_transact_time_ms);
          shadows_.erase(orig);
        }
        if (cfg_.emit_ack_from_response) emit_ack(inst, id, r.order_id, r.transact_time_ms);
        return;
      }
      // STOP_ON_FAILURE: cancel failed -> nothing changed; cancel ok, new failed -> the
      // original is gone and the replacement was rejected (-2021).
      if (r.cancel_result == "SUCCESS" && orig.valid()) {
        emit_cancel_ack(
            inst, orig, r.cancel_order_id, r.cancel_executed_qty, r.cancel_transact_time_ms);
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
  activity_.note(cmd.instrument, now);
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
    OrderShadow s{cmd.side, cmd.type, cmd.tif, cmd.instrument, cmd.price, cmd.qty};
    s.sent_seq = sent_.last_seq();
    if (FASTMM_UNLIKELY(shadows_.assign(cmd.cl_ord_id, s) == nullptr)) {
      refuse_untracked(cmd);
      return;
    }
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
    copy.sent_seq = sent_.last_seq();
    if (FASTMM_UNLIKELY(shadows_.assign(cmd.cl_ord_id, copy) == nullptr)) {
      refuse_untracked(cmd);
      return;
    }
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

void BinanceVenue::refuse_untracked(const OrderCommand& cmd) {
  shadow_overflow_.refused(cfg_.name, shadows_.size());
  emit_reject(cmd.instrument, cmd.cl_ord_id, RejectReason::OrderTableFull, 0, "order table full");
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
  // Its reply comes over REST: losing the WebSocket order connection does not settle it.
  if (cmd.kind != OrderCommandKind::Cancel) sent_.sent_over_rest(cmd.cl_ord_id);
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
  budget_pub_.store(budget_of(rate_, now_ns()));
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
                            std::int64_t venue_ms,
                            bool amended_in_place) {
  // GET /api/v3/myTrades names the order by orderId only, so the pairing has to be kept here.
  // An amend in place keeps the orderId under a new client id: the latest pairing wins.
  if (order_id > 0) static_cast<void>(order_ids_.assign(static_cast<std::uint64_t>(order_id), id));
  emit_order_ack(*order_sink_,
                 id_,
                 inst,
                 id,
                 IdText(order_id).view(),
                 amended_in_place ? OrderAckMsg::kAmendedInPlace : 0,
                 ts_from_ms(venue_ms));
  ++stats_.order_events;
}

void BinanceVenue::emit_cancel_ack(InstrumentId inst,
                                   ClientOrderId id,
                                   std::int64_t order_id,
                                   std::string_view executed_qty,
                                   std::int64_t venue_ms) {
  const auto q = parse_qty(executed_qty);
  venues::emit_cancel_ack(
      *order_sink_, id_, inst, id, IdText(order_id).view(), q ? *q : Qty{}, ts_from_ms(venue_ms));
  ++stats_.order_events;
}

// Decodes the whole open-order snapshot before anything reaches the engine: Oms::reconcile_end()
// cancels every order the snapshot does not name, so a reply that did not parse must not be
// emitted as an empty snapshot.
void BinanceVenue::on_open_orders(std::uint64_t generation,
                                  std::string_view json,
                                  bool rest_array) {
  if (!reconcile_.current(generation)) return;
  const PaddedJson padded(json);
  const ParseStatus st =
      ws_api_decoder_->decode_open_orders(padded.view(), rest_array, [&](const OpenOrderRecord& o) {
        const InstrumentId inst = instrument_of(o.symbol);
        if (!inst.valid()) return;  // another symbol on this account: not ours
        ReconcileMsg& m = reconcile_.add_order(inst);
        m.side = o.side == "SELL" ? Side::Sell : Side::Buy;
        m.state = o.status == "PARTIALLY_FILLED" ? OrderState::PartiallyFilled : OrderState::Live;
        if (const auto id = decode_cl_ord_id(o.client_order_id)) m.cl_ord_id = *id;
        if (o.order_id > 0 && m.cl_ord_id.valid())
          static_cast<void>(order_ids_.assign(static_cast<std::uint64_t>(o.order_id), m.cl_ord_id));
        m.venue_order_id.assign(IdText(o.order_id).view());
        if (const auto p = parse_price(o.price)) m.price = *p;
        if (const auto q = parse_qty(o.orig_qty)) m.orig_qty = *q;
        if (const auto q = parse_qty(o.executed_qty)) m.cum_qty = *q;
      });
  if (st != ParseStatus::Ok)
    FASTMM_LOG_WARN("{}: open orders reply could not be parsed", cfg_.name);
  reconcile_.fetched(generation, st == ParseStatus::Ok);
}

void BinanceVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const OrderShadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void BinanceVenue::drop_shadow(ClientOrderId id) {
  shadows_.erase(id);
}

// ---- control requests -----------------------------------------------------------------------

void BinanceVenue::request_open_orders() {
  reconcile_.request();
}

// The executions first (request_executions), then the snapshot (ReconcileDriver).
bool BinanceVenue::replay_executions() {
  return exec_replay_.run();
}

bool BinanceVenue::fetch_snapshot(std::uint64_t generation) {
  if (!connected_) return false;
  if (cfg_.ws_order_api && order_conn_.is_live()) {
    const std::size_t n = encoder_->encode_ws_open_orders({}, "oo", venue_time_ms(), request_buf_);
    if (n > 0 && order_conn_.send_text(std::string_view(request_buf_, n))) {
      rate_.on_sent(80, now_ns());
      oo_ws_generation_ = generation;
      return true;
    }
  }
  if (rest_ == nullptr || rest_hard_stopped_) return false;
  RestRequest rr;
  if (!encoder_->encode_rest_open_orders({}, venue_time_ms(), rr)) return false;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired()) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET openOrders failed: status={} err={}",
                          cfg_.name,
                          r.status,
                          net::to_string(r.error));
          reconcile_.fetched(generation, false);
          return;
        }
        on_open_orders(generation, r.body, /*rest_array=*/true);
      });
  if (queued) rate_.on_sent(rr.weight, now_ns());
  return queued;
}

// The balance leg. The reply carries no server time (updateTime is when the account last changed,
// not when it was read), so the snapshot is stamped with the venue clock when it arrived.
bool BinanceVenue::fetch_balances(std::uint64_t generation) {
  if (!connected_) return false;
  if (cfg_.ws_order_api && order_conn_.is_live() && bal_ws_generation_ == 0) {
    const std::size_t n = encoder_->encode_ws_account_status("bal", venue_time_ms(), request_buf_);
    if (n > 0 && order_conn_.send_text(std::string_view(request_buf_, n))) {
      rate_.on_sent(20, now_ns());
      bal_ws_generation_ = generation;
      return true;
    }
  }
  if (rest_ == nullptr || rest_hard_stopped_) return false;
  RestRequest rr;
  if (!encoder_->encode_rest_account(venue_time_ms(), rr)) return false;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired()) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET account failed: status={} err={}",
                          cfg_.name,
                          r.status,
                          net::to_string(r.error));
          reconcile_.balances_fetched(generation, false, 0);
          return;
        }
        on_account(generation, r.body);
      });
  if (queued) rate_.on_sent(rr.weight, now_ns());
  return queued;
}

void BinanceVenue::on_account(std::uint64_t generation, std::string_view json) {
  if (!reconcile_.balances_current(generation)) return;
  std::vector<AccountBalance> rows;
  std::int64_t update_ms = 0;
  if (const std::string err = decode_account_balances(json, rows, update_ms); !err.empty()) {
    FASTMM_LOG_WARN("{}: {}", cfg_.name, err);
    reconcile_.balances_fetched(generation, false, 0);
    return;
  }
  for (const AccountBalance& b : rows)
    reconcile_.add_balance(b.asset, BalanceFields::spot(b.free, b.locked));
  reconcile_.balances_fetched(generation, true, venue_time_ms());
}

// ---- execution replay -------------------------------------------------------------------------
//
// GET /api/v3/myTrades per subscribed symbol, before every open-order snapshot and once a minute
// (ReplayScheduler). Every execution it returns is emitted as an ordinary fill carrying Binance's
// trade id, so the OMS keeps the ones it never saw and drops the rest. This is what makes a fill
// that *finished* an order recoverable: the snapshot no longer mentions such an order at all, so
// nothing else would ever report it.
//
// Where the replay starts: the trade id after the last one this connector read for that symbol
// (`fromId`, which the venue will not take together with a time range), otherwise the time
// watermark (`startTime`), from the store at start-up or connect(), in windows of 24 hours. A full
// page is followed by the next within the same replay.

void BinanceVenue::resume_executions(std::int64_t since_venue_ms,
                                     const std::vector<std::string>& known) {
  exec_replay_.resume(since_venue_ms, known);
}

void BinanceVenue::resume_trade_ids(
    const std::vector<std::pair<InstrumentId, std::int64_t>>& next_ids) {
  resume_from_ids_ = next_ids;
}

void BinanceVenue::resume_known_trade_ids(
    const std::vector<std::pair<InstrumentId, std::vector<std::int64_t>>>& known) {
  resume_known_ids_ = known;
}

bool BinanceVenue::request_executions(std::int64_t since_venue_ms) {
  // An explicit start overrides both the time watermark and the per-symbol trade ids: the caller
  // is saying it knows of executions this connector never heard about.
  if (since_venue_ms > 0 && !exec_replay_.active()) exec_replay_.restart_from(since_venue_ms);
  return exec_replay_.run();
}

bool BinanceVenue::exec_ready() const noexcept {
  return !cfg_.dry_run && connected_ && signer_.usable() && rest_ != nullptr &&
         !rest_hard_stopped_ && !subscribed_.empty();
}

void BinanceVenue::replay_query_failed(std::string_view what,
                                       const net::HttpResponse& r,
                                       std::string_view then) {
  ++stats_.rest_errors;
  int code = 0;
  std::string msg;
  if (r.error == net::NetError::None && (r.status == 429 || r.status == 418)) {
    // The status says more than the body's -1003, which both carry: 429 waits Retry-After, 418
    // is the ban a query sent during that wait earns, and stops REST.
    apply_action(map_http_status(r.status).action,
                 r.status,
                 r.body.substr(0, 120),
                 header_int(r, "Retry-After") * 1000);
  } else if (r.error == net::NetError::None && decode_rest_error(r.body, code, msg)) {
    const ErrorMapping m = map_error(code, msg);
    if (m.action != VenueAction::Reconcile) apply_action(m.action, code, msg, -1);
  }
  FASTMM_LOG_ERROR("{}: GET {} failed: status={} err={} {}; {}",
                   cfg_.name,
                   what,
                   r.status,
                   net::to_string(r.error),
                   r.body.substr(0, 120),
                   then);
}

bool BinanceVenue::query_executions(const ReplayQuery& q) {
  if (rest_ == nullptr || rest_hard_stopped_ || q.stream >= subscribed_.size()) return false;
  const std::string_view symbol =
      symbols_ == nullptr ? std::string_view{} : symbols_->venue_symbol(subscribed_[q.stream]);
  RestRequest rr;
  if (symbol.empty() ||
      !encoder_->encode_rest_my_trades(
          symbol, q.from_id, q.start_ms, q.end_ms, kMyTradesLimit, venue_time_ms(), rr))
    return false;
  // Not while the venue asked for a pause or the bulk share of the weight is spent: the replay
  // asks again (Hooks::can_query keeps most queries from getting this far).
  if (!rate_.can_send(rr.weight, now_ns(), false, RateLimiter::kBulkShare)) return false;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, q](const net::HttpResponse& r) {
        // A reply the shutdown (or a later replay) aborted is nobody's: not even a failure.
        if (alive.expired() || !exec_replay_.expects(q)) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.execution_query_errors;
          replay_query_failed("myTrades", r);
          exec_replay_.failed(q);
          return;
        }
        // The rows own their text: a window waits for its lookups past this reply.
        const PaddedJson padded(r.body);
        ReplayPage<MyTradeRow> page;
        const ParseStatus st =
            ws_api_decoder_->decode_my_trades(padded.view(), [&](const MyTradeRecord& t) {
              page.rows.push_back(
                  {t.time_ms, t.id, std::string(IdText(t.id).view()), MyTradeRow::of(t)});
            });
        if (st != ParseStatus::Ok) {
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR("{}: myTrades reply could not be parsed; asked again", cfg_.name);
          exec_replay_.failed(q);
          return;
        }
        exec_replay_.answer(q, std::move(page));
      });
  if (!queued) {
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: no room to ask for the account's executions", cfg_.name);
    return false;
  }
  rate_.on_sent(rr.weight, now_ns());
  return true;
}

bool BinanceVenue::emit_execution(std::size_t stream, const MyTradeRow& t) {
  if (stream >= subscribed_.size() || instruments_ == nullptr) return false;
  const InstrumentId id = subscribed_[stream];
  if (!instruments_->contains(id)) return false;
  const ClientOrderId* mapped = order_ids_.find(static_cast<std::uint64_t>(t.order_id));
  if (!emit_trade_history_fill(*order_sink_,
                               id_,
                               instruments_->get(id),
                               id,
                               mapped != nullptr ? *mapped : ClientOrderId{},
                               t.view(),
                               exec_replay_.emitting_unresolved() ? OrderFillMsg::kUnresolved : 0))
    return false;
  ++stats_.order_events;
  ++stats_.executions_fetched;
  return true;
}

// myTrades names an order by orderId only, and order_ids_ holds only the orders this process saw
// acknowledged: an order a session placed that ended before the answer came (or before this
// process started, behind fastmm-gateway) is asked for, so that its fill names its client order id
// and with it the session (epoch) and strategy it belongs to. The execution replay bounds and
// retries these (ReplayScheduler); the rate limiter's headroom comes first.
bool BinanceVenue::lookup_order(const ReplayLookup& l) {
  if (rest_ == nullptr || rest_hard_stopped_ || l.stream >= subscribed_.size()) return false;
  const std::string_view symbol =
      symbols_ == nullptr ? std::string_view{} : symbols_->venue_symbol(subscribed_[l.stream]);
  const auto parsed = parse_int64(l.order_id);
  if (!parsed) return false;
  const std::int64_t order_id = *parsed;
  RestRequest rr;
  if (symbol.empty() || !encoder_->encode_rest_query_order(symbol, order_id, venue_time_ms(), rr))
    return false;
  if (!rate_.can_send(rr.weight, now_ns(), false, RateLimiter::kBulkShare)) return false;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, l, order_id](const net::HttpResponse& r) {
        if (alive.expired() || !connected_) return;  // a shutdown's reset: nobody waits
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          replay_query_failed("order", r, "its fill names no order yet");
          exec_replay_.looked_up(l, LookupResult::Failed);
          return;
        }
        const PaddedJson padded(r.body);
        std::int64_t oid = 0;
        std::string_view client;
        if (ws_api_decoder_->decode_order_ids(padded.view(), oid, client) != ParseStatus::Ok ||
            oid != order_id) {
          FASTMM_LOG_WARN("{}: GET order {} reply could not be read", cfg_.name, order_id);
          exec_replay_.looked_up(l, LookupResult::Failed);
          return;
        }
        const auto cl = decode_cl_ord_id(client);
        if (!cl) {
          exec_replay_.looked_up(l, LookupResult::NotOurs);
          return;
        }
        static_cast<void>(order_ids_.assign(static_cast<std::uint64_t>(oid), *cl));
        exec_replay_.looked_up(l, LookupResult::Named);
      });
  if (!queued) return false;
  rate_.on_sent(rr.weight, now_ns());
  return true;
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
  BlockingControl control(cfg_);
  BinanceOrderEncoder enc(signer_, *symbols_, cfg_.recv_window_ms);  // thread-local copy
  return control.per_target(
      "kill-switch cancel-all",
      std::span<const InstrumentId>(subscribed_),
      [&](InstrumentId id) { return symbols_->venue_symbol(id); },
      [&](InstrumentId id, BlockingRequest& q) {
        RestRequest rr;
        if (!enc.encode_rest_cancel_all(symbols_->venue_symbol(id), venue_time_ms(), rr))
          return false;
        q.method = "DELETE";
        q.target = std::string(rr.path) + "?" + std::string(rr.query.view());
        q.headers = api_headers();
        return true;
      },
      [](const HttpReply& reply, std::string& why) {
        int code = 0;
        std::string msg;
        const bool decoded = decode_rest_error(reply.body, code, msg);
        // -2011 "Unknown order sent": there was nothing open to cancel.
        if (reply.ok() || (reply.status == 400 && decoded && code == -2011)) return true;
        if (decoded) why = fmt::format("code {} {}", code, msg);
        return false;
      });
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
  reconcile_.on_timer(now);
  // The execution replay: a retry 5 s after an incomplete one (the next reconnect may be hours
  // away), and one a minute while all is well, which keeps the watermark within what the OMS can
  // deduplicate and books a fill the private stream dropped without disconnecting.
  exec_replay_.on_timer(now);
  shadow_overflow_.check(cfg_.name, shadows_.size(), decltype(shadows_)::kMaxSize);
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
  stats_.shadows = shadows_.size();
  stats_.shadows_swept = reconcile_.shadows_swept();
  stats_.shadows_refused = shadow_overflow_.count();
  stats_.execution_queries = exec_replay_.replays();
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_;
  wire_.summarize(
      tsc_calibration(), stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
  budget_pub_.store(budget_of(rate_, now_ns()));
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
  if (const std::int64_t d = x.integer("depth_limit", 0); d > 0)
    c.depth_limit = static_cast<int>(std::clamp<std::int64_t>(d, 5, 5000));
  c.stale_ms = static_cast<std::uint32_t>(x.integer("stale_ms", c.stale_ms));
  c.dead_ms = static_cast<std::uint32_t>(x.integer("dead_ms", c.dead_ms));
  check_liveness(v.name, x.integer("stale_ms", c.stale_ms), x.integer("dead_ms", c.dead_ms));
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
