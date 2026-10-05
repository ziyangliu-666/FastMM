#include "fastmm/venues/gemini/gemini_venue.hpp"

#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/gemini/gemini_error_map.hpp"
#include "fastmm/venues/order_events.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fastmm::venues::gemini {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kClockResyncNs = 30LL * 60 * 1'000'000'000;
// A rate-limit reply pauses sending this long ("five additional requests that are queued", then
// 429; the limits are counted per minute on REST and per 10 s on the WebSocket).
constexpr std::int64_t kRateLimitCooldownNs = 1'000'000'000;
constexpr std::int64_t kSettleMs = 60'000;

// "?k=v" or "&k=v" after whatever query the URL already has.
std::string with_param(const std::string& url, std::string_view param) {
  if (param.empty()) return url;
  std::string u = url;
  u += url.find('?') == std::string::npos ? '?' : '&';
  u += param;
  return u;
}

std::string reply_error(const HttpReply& r) {
  return r.error.empty() ? fmt::format("HTTP {} {}", r.status, r.body.substr(0, 200)) : r.error;
}

std::string response_error(const net::HttpResponse& r) {
  if (r.error != net::NetError::None) return std::string(net::to_string(r.error));
  return fmt::format("HTTP {} {}", r.status, std::string_view(r.body).substr(0, 200));
}

bool rate_limited_reply(const HttpReply& r) {
  std::string reason;
  std::string message;
  return decode_error(r.body, reason, message) &&
         map_reason(reason).action == VenueAction::RateLimit;
}

BlockingRetry gemini_blocking_retry() {
  BlockingRetry r;
  r.rate_limited = &rate_limited_reply;
  return r;
}

FeeAsset fee_asset_of(const Instrument& in, Notional fee, std::string_view ccy) noexcept {
  if (fee.is_zero() || ccy.empty() || iequals_symbol(in.quote.view(), ccy)) return FeeAsset::Quote;
  if (iequals_symbol(in.base.view(), ccy)) return FeeAsset::Base;
  return FeeAsset::Other;
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

GeminiVenue::GeminiVenue(VenueId id, GeminiVenueConfig cfg)
    : id_(id),
      cfg_(std::move(cfg)),
      signer_(cfg_.credentials),
      rate_(cfg_.rate_threshold),
      heartbeat_(cfg_.dry_run || !cfg_.heartbeat ? 0 : kHeartbeatWindowMs, 1, 2) {
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  std::memset(scratch_, 0, sizeof scratch_);
  ReplayLimits limits;
  limits.page_rows = kTradesPageLimit;
  limits.settle_ms = kSettleMs;
  exec_replay_.setup(cfg_.name,
                     "trade(s)",
                     limits,
                     {[this] { return replay_ready(); },
                      [this] { return venue_time_ms(); },
                      [this](const ReplayQuery& q) { return query_trades(q); },
                      [this](bool complete) { reconcile_.replay_done(complete); },
                      {},  // rows carry client_order_id: no lookup
                      {}},
                     [this](std::size_t s, const TradeRow& t) { return emit_trade(s, t); });
  ReplayLimits funding;
  funding.settle_ms = kSettleMs;
  funding_replay_.setup(cfg_.name,
                        "funding payment(s)",
                        funding,
                        {[this] { return replay_ready() && any_perpetual(); },
                         [this] { return venue_time_ms(); },
                         [this](const ReplayQuery& q) { return query_funding(q); },
                         [](bool) {},
                         {},
                         {}},
                        [this](std::size_t, const FundingRow& f) { return emit_funding_row(f); });
  funding_replay_.set_streams(1);
}

GeminiVenue::~GeminiVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps GeminiVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = false;
  c.supports_post_only = true;
  c.ws_order_entry = true;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

std::int64_t GeminiVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_.load(std::memory_order_relaxed);
}

std::string GeminiVenue::md_url() const {
  return with_param(cfg_.ws_md_url, "snapshot=-1");
}

std::string GeminiVenue::order_url() const {
  return with_param(cfg_.ws_order_url, cfg_.cancel_on_disconnect ? "cancelOnDisconnect=true" : "");
}

std::int64_t GeminiVenue::next_nonce() noexcept {
  const std::int64_t now = venue_time_ms();
  std::int64_t last = last_nonce_.load(std::memory_order_relaxed);
  std::int64_t next = 0;
  do {
    next = std::max(now, last + 1);
  } while (!last_nonce_.compare_exchange_weak(last, next, std::memory_order_relaxed));
  return next;
}

// A fresh nonce for every upgrade: "the value must increase across connections for the same key".
std::string GeminiVenue::ws_auth_headers() {
  return signer_.ws_headers(next_nonce());
}

net::ConnectionConfig GeminiVenue::ws_config(const std::string& url) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  // Our `ping` every ping_interval_ms is answered, so a quiet order connection still hears from
  // the venue within the dead threshold.
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, cfg_.ping_interval_ms * 2 + 5000);
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = 0;  // no documented connection lifetime
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> GeminiVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  try {
    BlockingHttp http(cfg_.rest_url, blocking_options(cfg_));
    for (Instrument* inst : mine) {
      std::string sym(inst->symbol.view());
      std::transform(sym.begin(), sym.end(), sym.begin(), [](char c) { return ascii_lower(c); });
      const HttpReply reply = http.get("/v1/symbols/details/" + sym);
      if (!reply.ok()) {
        if (!cfg_.allow_offline_reference_data)
          return fail(
              fmt::format("{}: symbols/details/{} failed: {}", cfg_.name, sym, reply_error(reply)));
        FASTMM_LOG_WARN("{}: symbols/details/{} failed ({}); keeping configured tick/lot",
                        cfg_.name,
                        sym,
                        reply_error(reply));
        continue;
      }
      SymbolDetails d;
      if (std::string err = decode_symbol_details(reply.body, d); !err.empty())
        return fail(fmt::format("{}: {}: {}", cfg_.name, sym, err));
      const bool perp = d.product_type == "swap";
      if (perp && d.contract_type != "linear")
        return fail(fmt::format("{}: {} is a {} perpetual; only linear perpetuals are supported",
                                cfg_.name,
                                sym,
                                d.contract_type.empty() ? "?" : d.contract_type));
      if (!perp && d.product_type != "spot")
        return fail(fmt::format("{}: {} has product_type {}", cfg_.name, sym, d.product_type));
      if (!d.tick.is_positive() || !d.lot.is_positive())
        return fail(
            fmt::format("{}: {} has an invalid quote_increment or tick_size", cfg_.name, sym));
      if (inst->tick != d.tick || inst->lot != d.lot) {
        FASTMM_LOG_WARN("{}: {} tick/lot from symbols/details override config ({} / {} -> {} / {})",
                        cfg_.name,
                        sym,
                        inst->tick,
                        inst->lot,
                        d.tick,
                        d.lot);
      }
      inst->tick = d.tick;
      inst->lot = d.lot;
      inst->min_qty = d.min_qty.is_positive() ? d.min_qty : d.lot;
      inst->min_notional = Notional{};
      inst->max_notional = Notional{};
      inst->contract_multiplier = Qty::from_int(1);  // 1 base unit per contract
      inst->expiry_ns = 0;
      // No reduce-only on Gemini: the engine's flatten orders go out as plain IOCs.
      inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kInverse &
                                              ~Instrument::kReduceOnlySupported);
      const AssetClass want = perp ? AssetClass::Perpetual : AssetClass::Spot;
      if (inst->asset_class != want) {
        FASTMM_LOG_WARN("{}: {} is {}; asset_class set accordingly",
                        cfg_.name,
                        sym,
                        perp ? "a perpetual" : "spot");
        inst->asset_class = want;
      }
      if (!inst->quote.empty() && !iequals_symbol(inst->quote.view(), d.quote))
        FASTMM_LOG_WARN("{}: {} is quoted in {}, not the configured {}; using {}",
                        cfg_.name,
                        sym,
                        d.quote,
                        inst->quote.view(),
                        d.quote);
      if (!inst->quote.assign(d.quote) || !inst->base.assign(d.base))
        return fail(fmt::format("{}: {} currency names too long", cfg_.name, sym));
      if (perp && !d.collateral.empty() && !iequals_symbol(d.collateral, d.quote))
        FASTMM_LOG_INFO("{}: {} collateral (contract_price_currency) is {}; PnL is booked in {}",
                        cfg_.name,
                        sym,
                        d.collateral,
                        d.quote);
      if (d.status == "post_only" || d.status == "limit_only") {
        FASTMM_LOG_WARN("{}: {} status is {}", cfg_.name, sym, d.status);
      } else if (d.status != "open") {
        FASTMM_LOG_ERROR("{}: {} status is {} (not open): disabled", cfg_.name, sym, d.status);
        inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kEnabled);
      }
    }
  } catch (const std::exception& e) {
    if (!cfg_.allow_offline_reference_data)
      return fail(
          fmt::format("{}: reference data failed: {}", cfg_.name, std::string_view(e.what())));
    FASTMM_LOG_WARN("{}: reference data failed ({}); keeping the configured values",
                    cfg_.name,
                    std::string_view(e.what()));
  }
  publish_status();
  FASTMM_LOG_INFO("{}: reference data loaded for {} instruments", cfg_.name, mine.size());
  if (!cfg_.dry_run && signer_.usable()) {
    const bool perpetuals = std::any_of(mine.begin(), mine.end(), [](const Instrument* i) {
      return i->asset_class == AssetClass::Perpetual;
    });
    if (std::string err = check_key(perpetuals); !err.empty()) return fail(std::move(err));
  }
  return {};
}

// POST /v1/orders with the key, and POST /v1/positions when a perpetual is configured: a key the
// venue refuses (signature, role, IP allowlist) and an account that is not a derivatives account
// ("AccountNotOfTypeRequired": "Account is not of required type: derivatives", the sandbox's
// Primary exchange account, 2026-09-30) are settings a retry does not change; a venue that cannot
// be reached is not.
std::string GeminiVenue::check_key(bool perpetuals) {
  try {
    BlockingHttp http(cfg_.rest_url, blocking_options(cfg_));
    const auto refused = [&](const HttpReply& reply, std::string_view what) -> std::string {
      std::string reason;
      std::string message;
      if (reply.status > 0 && !reply.ok() && decode_error(reply.body, reason, message)) {
        if (reason == "AccountNotOfTypeRequired") {
          refused_account_settings_ = true;
          return fmt::format(
              "{}: the API key's account is not a derivatives account ({}: {}); perpetuals trade "
              "in a derivatives account, with a key scoped to it",
              cfg_.name,
              reason,
              message);
        }
        const VenueAction a = map_reason(reason).action;
        refused_account_settings_ = a == VenueAction::Fatal || a == VenueAction::HardStop;
        return fmt::format("{}: {} refused: {} {}", cfg_.name, what, reason, message);
      }
      if (!reply.ok()) return fmt::format("{}: {} failed: {}", cfg_.name, what, reply_error(reply));
      return {};
    };
    const RestRequest orders = GeminiOrderEncoder::active_orders(next_nonce());
    if (std::string err =
            refused(http.request("POST", orders.target, signer_.rest_headers(orders.payload)),
                    "the API key");
        !err.empty())
      return err;
    if (perpetuals) {
      const RestRequest pos = GeminiOrderEncoder::positions(next_nonce());
      if (std::string err = refused(
              http.request("POST", pos.target, signer_.rest_headers(pos.payload)), "positions");
          !err.empty())
        return err;
    }
    FASTMM_LOG_INFO("{}: API key accepted", cfg_.name);
  } catch (const std::exception& e) {
    return fmt::format("{}: key check failed: {}", cfg_.name, std::string_view(e.what()));
  }
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void GeminiVenue::attach(const SymbolTable& symbols,
                         const InstrumentTable& instruments,
                         EventSink& md_sink,
                         EventSink& order_sink,
                         MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ = std::make_unique<GeminiMdFeed>(
      symbols, id_, md_sink, InstrumentCallback{&GeminiVenue::resubscribe_requester, this});
  md_feed_->set_log_name(cfg_.name);
  private_parser_ = std::make_unique<GeminiPrivateParser>(symbols, id_);
  encoder_ = std::make_unique<GeminiOrderEncoder>(symbols);
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments);
}

void GeminiVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id, is_perpetual(id));
  }
  exec_replay_.set_streams(subscribed_.size());
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (connected_ && md_conn_.opened()) {
    md_conn_.close();
    md_conn_.open(*reactor_, ws_config(md_url()), md_handler_);
    md_conn_.connect();
  }
}

InstrumentId GeminiVenue::subscribed_instrument(std::string_view symbol) const noexcept {
  const InstrumentId id = symbols_->find(id_, symbol);
  if (!id.valid()) return id;
  if (std::find(subscribed_.begin(), subscribed_.end(), id) == subscribed_.end())
    return InstrumentId::invalid();
  return id;
}

bool GeminiVenue::is_perpetual(InstrumentId id) const noexcept {
  return instruments_ != nullptr && instruments_->contains(id) &&
         instruments_->get(id).asset_class == AssetClass::Perpetual;
}

bool GeminiVenue::any_perpetual() const noexcept {
  return std::any_of(
      subscribed_.begin(), subscribed_.end(), [this](InstrumentId id) { return is_perpetual(id); });
}

void GeminiVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("GeminiVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  const bool keyed = !cfg_.dry_run && signer_.usable();
  reconcile_.open(keyed);
  // Nobody said where the replays should start, so they start here: this session can only have
  // missed what happened after it connected.
  exec_replay_.start_at(venue_time_ms());
  exec_replay_.open(keyed);
  funding_replay_.start_at(venue_time_ms());
  funding_replay_.open(keyed);
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    if (!cfg_.dry_run) raw_order_.open(cfg_.record_raw_dir, cfg_.name, "order");
  }
  rest_ = std::make_unique<RestChannel>(reactor, rest_channel_config(cfg_, subscribed_.size()));
  md_conn_.open(reactor, ws_config(md_url()), md_handler_);
  md_conn_.connect();
  if (keyed) {
    net::ConnectionConfig oc = ws_config(order_url());
    oc.manual_subscribe = true;  // Live once orders@account is subscribed
    oc.make_headers = [this] { return ws_auth_headers(); };
    order_conn_.open(reactor, std::move(oc), order_handler_);
    order_conn_.connect();
  }
  last_ping_ns_ = now_ns();
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO(
      "{}: connecting (dry_run={}, orders={}, sandbox={}, cancelOnDisconnect={}, "
      "heartbeat={})",
      cfg_.name,
      cfg_.dry_run,
      keyed,
      cfg_.sandbox,
      cfg_.cancel_on_disconnect,
      heartbeat_.enabled());
}

void GeminiVenue::disconnect() {
  if (!connected_) return;
  heartbeat_.reset();
  connected_ = false;
  reconcile_.close();  // before the reset below: nothing it aborts asks again
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  md_conn_.close();
  order_conn_.close();
  ++generation_;
  exec_replay_.close();
  funding_replay_.close();
  time_request_pending_ = false;
  if (rest_) rest_->reset();
  md_feed_->on_disconnected();
  raw_md_.flush();
  raw_order_.flush();
  publish_status();
}

bool GeminiVenue::rest_post(const RestRequest& rr,
                            std::function<void(const net::HttpResponse&)> done) {
  if (rest_ == nullptr || !signer_.usable()) return false;
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  return rest_->request("POST",
                        rr.target,
                        signer_.rest_headers(rr.payload),
                        {},
                        [this, alive, gen, done = std::move(done)](const net::HttpResponse& r) {
                          if (alive.expired() || gen != generation_) return;
                          ++stats_.rest_requests;
                          done(r);
                        });
}

void GeminiVenue::apply_rest_error(const net::HttpResponse& r, std::string_view what) {
  ++stats_.rest_errors;
  std::string reason;
  std::string message;
  if (r.error == net::NetError::None && decode_error(r.body, reason, message)) {
    const VenueAction a = map_reason(reason).action;
    FASTMM_LOG_WARN("{}: {} failed: {} {}", cfg_.name, what, reason, message);
    if (a != VenueAction::Reconcile) apply_action(a, r.status, reason);
    return;
  }
  FASTMM_LOG_WARN("{}: {} failed: {}", cfg_.name, what, response_error(r));
  if (r.error == net::NetError::None) {
    const VenueAction a = map_http_status(r.status).action;
    if (a != VenueAction::Reconcile) apply_action(a, r.status, "HTTP status");
  }
}

// ---- market data ------------------------------------------------------------------------------

void GeminiVenue::on_md_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
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
        if (GeminiBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
      }
      break;
    case ConnState::Disconnected:
    case ConnState::Connecting:
      if (prev == ConnState::Live || prev == ConnState::Stale) {
        md_feed_->on_disconnected();
        emit_connection_state(*md_sink_, id_, 0, ConnState::Disconnected);
        FASTMM_LOG_WARN("{}: md channel lost", cfg_.name);
      }
      time_request_pending_ = false;
      break;
    default:
      break;
  }
}

void GeminiVenue::on_md_open() {
  md_feed_->on_connected();
  for (const std::string& p : md_feed_->subscription_payloads()) {
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  const std::size_t n = GeminiOrderEncoder::encode_method("time", "time", request_buf_);
  time_request_pending_ = n > 0 && md_conn_.send_text(std::string_view(request_buf_, n));
}

void GeminiVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (GeminiBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void GeminiVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Ok) return;
  const MdDecodeResult& r = md_feed_->last();
  if (r.control.present) {
    if (r.control.id == "time") {
      time_request_pending_ = false;
      if (r.control.server_time_ms > 0) {
        // The receive time, not a send/receive midpoint, as the other connectors do.
        const std::int64_t offset = r.control.server_time_ms - wall_now().ns / kNsPerMs;
        clock_offset_ms_.store(offset);
        clock_sync_ns_ = now_ns();
        stats_.clock_offset_ms = offset;
        if (offset > 1000 || offset < -1000)
          FASTMM_LOG_WARN(
              "{}: clock offset to venue is {} ms (nonces allow 30 s)", cfg_.name, offset);
      }
      return;
    }
    if (r.control.status != 200)
      FASTMM_LOG_ERROR("{}: market-data request {} refused: {} {} {}",
                       cfg_.name,
                       r.control.id,
                       r.control.status,
                       r.control.error_code,
                       r.control.msg);
    return;
  }
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  }
}

void GeminiVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;  // the reconnect subscribes everything again
  for (const std::string& p : md_feed_->resubscribe_payloads(id))
    static_cast<void>(md_conn_.send_text(p));
  FASTMM_LOG_INFO(
      "{}: resubscribing {} for a fresh snapshot", cfg_.name, symbols_->venue_symbol(id));
}

// `time` on the market-data connection: {"id":"time","status":200,"result":{"serverTime":ms}}.
void GeminiVenue::request_server_time() {
  if (time_request_pending_ || !md_conn_.is_live()) return;
  const std::size_t n = GeminiOrderEncoder::encode_method("time", "time", request_buf_);
  if (n > 0 && md_conn_.send_text(std::string_view(request_buf_, n))) time_request_pending_ = true;
}

// ---- order connection -------------------------------------------------------------------------

void GeminiVenue::on_order_open() {
  constexpr std::string_view kStreams[] = {"orders@account"};
  const std::size_t n = GeminiOrderEncoder::encode_subscribe("orders", kStreams, request_buf_);
  if (n == 0 || !order_conn_.send_text(std::string_view(request_buf_, n)))
    FASTMM_LOG_ERROR("{}: could not subscribe orders@account", cfg_.name);
  // A request of its own: a refusal leaves order entry alone (fills then ask for the balances).
  constexpr std::string_view kBalances[] = {"balances@account"};
  const std::size_t m = GeminiOrderEncoder::encode_subscribe("balances", kBalances, request_buf_);
  if (m == 0 || !order_conn_.send_text(std::string_view(request_buf_, m)))
    FASTMM_LOG_WARN("{}: could not subscribe balances@account", cfg_.name);
}

void GeminiVenue::on_order_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.user = private_channel_state(s);
  stats_.order = stats_.user;
  if (mapped == order_state_) return;
  const ConnState prev = order_state_;
  order_state_ = mapped;
  // Requests in flight on a connection that is gone are never answered on it: they no longer
  // hold the snapshot watermark back (the reconnect's snapshot settles them).
  if (mapped != ConnState::Live && mapped != ConnState::Stale) sent_.connection_lost();
  if (mapped == ConnState::Live) {
    if (prev != ConnState::Stale) {
      emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
      FASTMM_LOG_INFO("{}: order channel -> Live", cfg_.name);
    }
    // Reconcile after a reconnect, not when a quiet channel returns from Stale. On the first
    // connect, sweep for orders a session that died left resting.
    if (order_was_live_ && prev != ConnState::Stale) {
      reconcile_.request();
    } else if (!order_was_live_) {
      reconcile_.sweep();
    }
    order_was_live_ = true;
    drain_outbound();
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: order channel lost", cfg_.name);
    balance_stream_ = false;
    // A replay or snapshot over REST is not affected. cancelOnDisconnect has the venue cancel the
    // orders placed on the connection; the session cancel also takes those it could not see go.
    // disconnect() clears connected_ first: a requested shutdown runs cancel_all() itself.
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_session_async();
  }
}

void GeminiVenue::on_order_text(std::string_view t, std::int64_t ts) {
  if (raw_order_.enabled()) raw_order_.record(ts, t);
  const Cycles t0 = rdtscp();
  const PrivateDecodeResult r = private_parser_->decode(t, wall_now(), t0, scratch_);
  if (r.status == ParseStatus::Ok && r.balance.present) {
    on_balance_update(r.balance);
    return;
  }
  if (r.status == ParseStatus::Ok) {
    auto* h = reinterpret_cast<EventHeader*>(scratch_);
    h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
    on_order_event(*h);
    return;
  }
  if (r.control.present) {
    on_order_reply(r.control);
    return;
  }
  if (r.status == ParseStatus::Malformed) FASTMM_LOG_WARN("{}: malformed order frame", cfg_.name);
}

void GeminiVenue::on_order_event(EventHeader& h) {
  ClientOrderId id{};
  switch (h.type) {
    case EventType::OrderAck:
      id = msg_cast<OrderAckMsg>(&h).cl_ord_id;
      break;
    case EventType::OrderReject:
      id = msg_cast<OrderRejectMsg>(&h).cl_ord_id;
      break;
    case EventType::OrderCancelAck:
      id = msg_cast<OrderCancelAckMsg>(&h).cl_ord_id;
      break;
    case EventType::OrderExpired:
      id = msg_cast<OrderExpiredMsg>(&h).cl_ord_id;
      break;
    case EventType::OrderFill:
      id = msg_cast<OrderFillMsg>(&h).cl_ord_id;
      break;
    default:
      return;
  }
  OrderShadow* shadow = id.valid() ? shadows_.find(id) : nullptr;
  // The events leave out empty fields: the shadow knows the instrument and the side.
  if (shadow != nullptr && !h.instrument.valid()) h.instrument = shadow->instrument;
  bool terminal = false;
  switch (h.type) {
    case EventType::OrderAck: {
      const auto& m = reinterpret_cast<const OrderAckMsg&>(h);
      if (shadow != nullptr) {
        shadow->venue_id.assign(m.venue_order_id.view());
        if (shadow->cancel_pending) {
          shadow->cancel_pending = false;
          OrderCommand c;
          c.kind = OrderCommandKind::Cancel;
          c.instrument = shadow->instrument;
          c.venue = id_;
          c.cl_ord_id = id;
          send_cancel(c, shadow->venue_id.view());
        }
      }
      break;
    }
    case EventType::OrderFill: {
      auto& m = reinterpret_cast<OrderFillMsg&>(h);
      if (shadow != nullptr) m.side = shadow->side;
      terminal = m.leaves_qty.raw <= 0;  // `z` is left out when zero
      // What no stream reports after a fill: the derivatives margin, or every balance while
      // balances@account is not subscribed. The driver asks at most once a second.
      const bool perpetual = h.instrument.valid() && instruments_->contains(h.instrument) &&
                             instruments_->get(h.instrument).asset_class == AssetClass::Perpetual;
      if (!balance_stream_ || (perpetual && !no_margin_account_)) reconcile_.request_balances();
      break;
    }
    case EventType::OrderReject:
      // A rejected placement is answered twice, by a 400 reply and by a REJECTED event (sandbox,
      // 2026-09-30): the first one ends the shadow, the second is dropped.
      if (shadow == nullptr) return;
      terminal = true;
      break;
    case EventType::OrderCancelAck:
    case EventType::OrderExpired:
      terminal = true;
      break;
    default:
      break;
  }
  if (terminal && shadow != nullptr) shadows_.erase(id);
  sent_.answered(h);
  static_cast<void>(order_sink_->push(h));
  ++stats_.order_events;
}

void GeminiVenue::on_order_reply(const PrivateControl& c) {
  if (c.id == "orders") {
    if (c.status == 200) {
      FASTMM_LOG_INFO("{}: orders@account subscribed", cfg_.name);
      order_conn_.subscribe_done();
      return;
    }
    FASTMM_LOG_ERROR(
        "{}: orders@account refused: {} {} {}", cfg_.name, c.status, c.error_code, c.msg);
    const VenueAction a = map_ws_code(c.error_code).action;
    apply_action(a == VenueAction::None ? VenueAction::Fatal : a, c.error_code, c.msg);
    return;
  }
  if (c.id == "balances") {
    balance_stream_ = c.status == 200;
    if (balance_stream_) {
      FASTMM_LOG_INFO("{}: balances@account subscribed", cfg_.name);
    } else {
      FASTMM_LOG_WARN("{}: balances@account refused: {} {} {}; balances are asked for after fills",
                      cfg_.name,
                      c.status,
                      c.error_code,
                      c.msg);
    }
    return;
  }
  if (c.id == "ping") return;
  const auto req = parse_request_id(c.id);
  if (!req) {
    if (c.status != 200)
      FASTMM_LOG_WARN(
          "{}: request {} failed: {} {} {}", cfg_.name, c.id, c.status, c.error_code, c.msg);
    return;
  }
  const ClientOrderId id = req->second;
  OrderShadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  // The message may name the reason (-2010 "order rejected"); the code says the rest.
  const ErrorMapping by_msg = map_reason(c.msg);
  const ErrorMapping m = by_msg.known ? by_msg : map_ws_code(c.error_code);
  if (req->first == RequestKind::New) {
    // The venue answered the placement: it no longer holds the snapshot watermark back.
    sent_.answered(id);
    if (c.status == 200) {
      if (shadow != nullptr && !c.order_id.empty() && shadow->venue_id.empty())
        shadow->venue_id.assign(c.order_id);
      return;  // the orders@account NEW event is the ack
    }
    // The REJECTED event may have come first (both name the order): one reject.
    if (shadow == nullptr) return;
    const RejectReason reason = c.status == 429 ? RejectReason::VenueRateLimit : m.reason;
    emit_order_reject(*order_sink_, id_, inst, id, reason, c.error_code, c.msg);
    shadows_.erase(id);
    ++stats_.order_events;
    apply_action(c.status == 429 ? VenueAction::RateLimit : m.action, c.error_code, c.msg);
    return;
  }
  if (req->first == RequestKind::Cancel && c.status != 200) {
    emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, c.error_code, c.msg);
    ++stats_.order_events;
    apply_action(c.status == 429 ? VenueAction::RateLimit : m.action, c.error_code, c.msg);
    // An order the venue no longer has: the snapshot says what became of it.
    if (m.action == VenueAction::None) request_open_orders();
  }
}

// ---- outbound ---------------------------------------------------------------------------------

void GeminiVenue::on_wake() {
  drain_outbound();
}

template <class Ring>
void GeminiVenue::write_orders(Ring& ring) {
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

void GeminiVenue::fail_batch() {
  for (const BatchedOrders::Entry& e : batch_.entries()) {
    ++stats_.order_send_failures;
    unsend(stats_, e.kind);
    ++stats_.order_events;
    if (e.kind == OrderCommandKind::Cancel) {
      emit_cancel_reject(*order_sink_,
                         id_,
                         e.instrument,
                         e.cl_ord_id,
                         RejectReason::TransportFull,
                         0,
                         "order batch not written");
    } else {
      emit_order_reject(*order_sink_,
                        id_,
                        e.instrument,
                        e.cl_ord_id,
                        RejectReason::TransportFull,
                        0,
                        "order batch not written");
      shadows_.erase(e.cl_ord_id);
    }
  }
  batch_.clear();
}

void GeminiVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void GeminiVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

void GeminiVenue::refuse_untracked(const OrderCommand& cmd) {
  shadow_overflow_.refused(cfg_.name, shadows_.size());
  sent_.answered(cmd.cl_ord_id);  // the refusal is its answer: it holds no snapshot back
  emit_order_reject(*order_sink_,
                    id_,
                    cmd.instrument,
                    cmd.cl_ord_id,
                    RejectReason::OrderTableFull,
                    0,
                    "order table full");
  ++stats_.order_events;
}

void GeminiVenue::send_command(const OrderCommand& cmd) {
  note_taken(cmd);
  const std::int64_t now = now_ns();
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  auto refuse = [&](RejectReason reason, std::string_view why) {
    if (is_cancel) {
      emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
    } else {
      sent_.answered(cmd.cl_ord_id);
      emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
    }
    ++stats_.order_events;
  };
  if (cfg_.dry_run) return refuse(RejectReason::VenueKilled, "dry-run: orders disabled");
  // A venue-fatal error and a REST hard stop stop new orders, never cancels.
  if ((fatal_ || rest_hard_stopped_) && !is_cancel)
    return refuse(RejectReason::VenueKilled, "venue fatal");
  switch (cmd.kind) {
    case OrderCommandKind::Replace:
      return refuse(RejectReason::VenueReject, "gemini has no amend");
    case OrderCommandKind::Cancel: {
      OrderShadow* shadow = shadows_.find(cmd.cl_ord_id);
      std::string_view venue_id;
      if (cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty()) {
        venue_id = cmd.venue_order_id->view();
      } else if (shadow != nullptr) {
        venue_id = shadow->venue_id.view();
      }
      if (venue_id.empty()) {
        if (shadow == nullptr) return refuse(RejectReason::UnknownOrder, "cancel: order unknown");
        // Not named by the venue yet: cancelled as soon as its NEW event names it.
        shadow->cancel_pending = true;
        ++stats_.cancels_sent;
        return;
      }
      return send_cancel(cmd, venue_id);
    }
    case OrderCommandKind::New:
      break;
  }
  if (!order_conn_.is_live()) return refuse(RejectReason::TransportFull, "no order channel");
  if (!rate_.can_send(1, now, true)) {
    ++stats_.rate_limit_cooldowns;
    return refuse(RejectReason::VenueRateLimit, "local rate limit");
  }
  const OrderShadow* shadow = shadows_.assign(
      cmd.cl_ord_id, OrderShadow{cmd.instrument, cmd.side, false, sent_.last_seq(), {}});
  if (FASTMM_UNLIKELY(shadow == nullptr)) return refuse_untracked(cmd);
  const Cycles before_encode = rdtscp();
  const std::size_t n = encoder_->encode_ws(cmd, {}, request_buf_);
  const Cycles after_encode = rdtscp();
  if (n == 0 || !order_conn_.send_text(std::string_view(request_buf_, n))) {
    shadows_.erase(cmd.cl_ord_id);
    ++stats_.order_send_failures;
    return refuse(n == 0 ? RejectReason::VenueReject : RejectReason::TransportFull,
                  n == 0 ? "cannot encode" : "send failed");
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  batch_.note(cmd);
  rate_.on_sent(1, now, true);
  ++stats_.orders_sent;
}

void GeminiVenue::send_cancel(const OrderCommand& cmd, std::string_view venue_id) {
  if (order_conn_.is_live()) {
    const std::size_t n = encoder_->encode_ws(cmd, venue_id, request_buf_);
    if (n > 0 && order_conn_.send_text(std::string_view(request_buf_, n))) {
      batch_.note(cmd);
      rate_.on_sent(1, now_ns(), false);
      ++stats_.cancels_sent;
      return;
    }
  }
  send_cancel_rest(cmd.instrument, cmd.cl_ord_id, venue_id);
}

// POST /v1/order/cancel: the reply is the order's status; is_cancelled true is the cancel ack
// (the orders@account event is on the connection that is down).
void GeminiVenue::send_cancel_rest(InstrumentId inst, ClientOrderId id, std::string_view venue_id) {
  const RestRequest rr = GeminiOrderEncoder::cancel_order(next_nonce(), venue_id);
  const std::string vid(venue_id);
  const bool queued = rest_post(rr, [this, inst, id, vid](const net::HttpResponse& r) {
    std::string_view body = r.body;
    const bool cancelled = r.ok() && body.find("\"is_cancelled\":true") != std::string_view::npos;
    if (cancelled) {
      std::vector<ActiveOrder> one;
      Qty cum{};
      if (decode_active_orders("[" + std::string(body) + "]", one).empty() && !one.empty())
        cum = one[0].executed;
      emit_cancel_ack(*order_sink_, id_, inst, id, vid, cum);
      shadows_.erase(id);
      ++stats_.order_events;
      return;
    }
    std::string reason;
    std::string message;
    const bool envelope = decode_error(r.body, reason, message);
    const ErrorMapping m = envelope ? map_reason(reason) : map_http_status(r.status);
    emit_cancel_reject(*order_sink_,
                       id_,
                       inst,
                       id,
                       r.ok() ? RejectReason::VenueReject : m.reason,
                       r.status,
                       envelope ? std::string_view(reason) : std::string_view("cancel failed"));
    ++stats_.order_events;
    if (!r.ok()) apply_rest_error(r, "order/cancel");
    request_open_orders();
  });
  if (!queued) {
    emit_cancel_reject(
        *order_sink_, id_, inst, id, RejectReason::TransportFull, 0, "no order channel");
    ++stats_.order_events;
    return;
  }
  ++stats_.cancels_sent;
}

void GeminiVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void GeminiVenue::apply_action(VenueAction action, int code, std::string_view msg) {
  switch (action) {
    case VenueAction::None:
      break;
    case VenueAction::Backoff:
      rate_.cooldown(1'000'000'000, now_ns());
      break;
    case VenueAction::RateLimit:
      rate_.cooldown(kRateLimitCooldownNs, now_ns());
      ++stats_.rate_limit_cooldowns;
      FASTMM_LOG_WARN("{}: rate limited ({} {}); cooling down {} ms",
                      cfg_.name,
                      code,
                      msg,
                      kRateLimitCooldownNs / kNsPerMs);
      break;
    case VenueAction::ResyncClock:
      clock_sync_ns_ = 0;
      request_server_time();
      break;
    case VenueAction::Reconcile:
      request_open_orders();
      break;
    case VenueAction::DisableInstrument:
      FASTMM_LOG_ERROR(
          "{}: venue rejected a precision rule ({} {}); check tick/lot", cfg_.name, code, msg);
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

// ---- reconciliation -------------------------------------------------------------------------

// The trades are replayed first (request_executions), then the open-order snapshot is read
// (ReconcileDriver).
void GeminiVenue::request_open_orders() {
  reconcile_.request();
}

bool GeminiVenue::replay_executions() {
  // The funding payments go alongside: they change no position, so the snapshot does not wait.
  static_cast<void>(funding_replay_.run());
  return exec_replay_.run();
}

// Nothing reaches the engine before every reply parsed: Oms::reconcile_end() cancels every order
// the snapshot does not name, so a truncated or unparsed snapshot would cancel live orders.
bool GeminiVenue::fetch_snapshot(std::uint64_t generation) {
  if (!connected_ || rest_hard_stopped_) return false;
  return rest_post(
      GeminiOrderEncoder::active_orders(next_nonce()),
      [this, generation](const net::HttpResponse& r) {
        if (!reconcile_.current(generation)) return;
        std::vector<ActiveOrder> rows;
        const std::string err = r.ok() ? decode_active_orders(r.body, rows) : response_error(r);
        if (!err.empty()) {
          if (r.ok()) {
            ++stats_.rest_errors;
            FASTMM_LOG_WARN("{}: orders failed ({})", cfg_.name, err);
          } else {
            apply_rest_error(r, "orders");
          }
          reconcile_.fetched(generation, false);
          return;
        }
        for (const ActiveOrder& o : rows) {
          const InstrumentId inst = subscribed_instrument(o.symbol);
          if (!inst.valid()) continue;
          ReconcileMsg& m = reconcile_.add_order(inst);
          m.side = o.side == "sell" ? Side::Sell : Side::Buy;
          m.state = o.executed.is_positive() ? OrderState::PartiallyFilled : OrderState::Live;
          if (const auto cl = decode_cl_ord_id(o.client_order_id)) m.cl_ord_id = *cl;
          m.venue_order_id.assign(o.order_id);
          m.price = o.price;
          m.orig_qty = o.original;
          m.cum_qty = o.executed;
        }
        if (!any_perpetual()) {
          reconcile_.fetched(generation, true);
          return;
        }
        if (!request_positions(generation)) reconcile_.fetched(generation, false);
      });
}

bool GeminiVenue::request_positions(std::uint64_t generation) {
  return rest_post(
      GeminiOrderEncoder::positions(next_nonce()), [this, generation](const net::HttpResponse& r) {
        if (!reconcile_.current(generation)) return;
        std::vector<PositionRow> rows;
        const std::string err = r.ok() ? decode_positions(r.body, rows) : response_error(r);
        if (!err.empty()) {
          // Positions are part of the snapshot: without them nothing is emitted.
          if (r.ok()) {
            ++stats_.rest_errors;
            FASTMM_LOG_WARN("{}: positions failed ({})", cfg_.name, err);
          } else {
            apply_rest_error(r, "positions");
          }
          reconcile_.fetched(generation, false);
          return;
        }
        // Only open positions are listed: a subscribed perpetual absent from the list is flat.
        for (InstrumentId id : subscribed_) {
          if (instruments_->get(id).asset_class != AssetClass::Perpetual) continue;
          const std::string_view sym = symbols_->venue_symbol(id);
          Qty qty{};
          Price avg{};
          for (const PositionRow& p : rows) {
            if (!iequals_symbol(p.symbol, sym)) continue;
            qty = p.qty;
            avg = p.avg_px;
          }
          reconcile_.add_position(id, qty, avg);
          FASTMM_LOG_INFO("{}: {} position {} @ {}", cfg_.name, sym, qty, avg);
        }
        reconcile_.fetched(generation, true);
      });
}

// ---- balances -----------------------------------------------------------------------------------
//
// The balance leg (ReconcileDriver): /v1/balances lists every currency of the exchange account;
// the driver keeps those the instruments name. Its venue time is the latest `_timestamp` of the
// rows ("server-side monotonically increasing clock value ... to detect and filter out stale
// responses"; the sandbox stamps every row with the time of the reply), else the venue clock when
// the reply came. The balance fetch never touches the order snapshot: a failure is retried by the
// driver alone.

namespace {
// Nanoseconds to ms, rounded up: an event the venue stamped within the report's millisecond is
// taken as already in the report.
constexpr std::int64_t ceil_ms(std::int64_t ns) noexcept {
  return (ns + kNsPerMs - 1) / kNsPerMs;
}
}  // namespace

bool GeminiVenue::fetch_balances(std::uint64_t generation) {
  if (!connected_ || rest_hard_stopped_) return false;
  return rest_post(
      GeminiOrderEncoder::balances(next_nonce()), [this, generation](const net::HttpResponse& r) {
        if (!reconcile_.balances_current(generation)) return;
        std::vector<BalanceRow> rows;
        const std::string err = r.ok() ? decode_balances(r.body, rows) : response_error(r);
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: balances failed ({})", cfg_.name, err);
          reconcile_.balances_fetched(generation, false, 0);
          return;
        }
        std::int64_t venue_ms = 0;
        for (const BalanceRow& b : rows) {
          const Notional held = b.amount - b.available;
          reconcile_.add_balance(
              b.currency, BalanceFields::spot(b.available, held.is_negative() ? Notional{} : held));
          venue_ms = std::max(venue_ms, b.time_ms);
        }
        if (venue_ms == 0) venue_ms = venue_time_ms();
        if (!any_perpetual() || no_margin_account_) {
          reconcile_.balances_fetched(generation, true, venue_ms);
          return;
        }
        request_margin(generation, venue_ms);
      });
}

// The derivatives account's margin, valued in USD, as the account row. It names a symbol (the
// first perpetual subscribed); the figures are the account's.
void GeminiVenue::request_margin(std::uint64_t generation, std::int64_t venue_ms) {
  std::string_view symbol;
  for (InstrumentId id : subscribed_) {
    if (instruments_->get(id).asset_class == AssetClass::Perpetual) {
      symbol = symbols_->lower_symbol(id);
      break;
    }
  }
  const bool queued = rest_post(
      GeminiOrderEncoder::margin(next_nonce(), symbol),
      [this, generation, venue_ms](const net::HttpResponse& r) {
        if (!reconcile_.balances_current(generation)) return;
        std::string reason;
        std::string message;
        if (!r.ok() && r.error == net::NetError::None && decode_error(r.body, reason, message) &&
            reason == "AccountNotOfTypeRequired") {
          // An exchange account: no derivatives margin. The spot rows stand; not asked again.
          no_margin_account_ = true;
          FASTMM_LOG_WARN("{}: no derivatives account ({}: {}); balances without margin",
                          cfg_.name,
                          reason,
                          message);
          reconcile_.balances_fetched(generation, true, venue_ms);
          return;
        }
        MarginRow m;
        const std::string err = r.ok() ? decode_margin(r.body, m) : response_error(r);
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: margin failed ({})", cfg_.name, err);
          reconcile_.balances_fetched(generation, false, 0);
          return;
        }
        BalanceFields f;
        f.free = m.available;
        f.locked = m.initial;
        f.total = m.assets_value;
        f.equity = m.assets_value;
        f.maintenance = m.maintenance;
        reconcile_.add_balance("USD", f, BalanceMsg::kAccount);
        reconcile_.balances_fetched(generation, true, venue_ms);
      });
  if (!queued) reconcile_.balances_fetched(generation, false, 0);
}

// balances@account: each row is the asset's whole balance now (f available, c confirmed), so it
// replaces the row; an asset the update leaves out did not change. A row the parser could not keep
// (unreadable, or past its table) leaves that asset stale: the balance leg is asked for then.
void GeminiVenue::on_balance_update(const BalanceUpdate& b) {
  const std::int64_t venue_ms = b.time_ns > 0 ? ceil_ms(b.time_ns) : venue_time_ms();
  const VenueAssets& assets = reconcile_.assets();
  for (const BalanceUpdateRow& row : private_parser_->balance_rows(b)) {
    const Notional held = row.confirmed - row.available;
    if (emit_balance(*order_sink_,
                     assets,
                     id_,
                     row.asset,
                     BalanceFields::spot(row.available, held.is_negative() ? Notional{} : held),
                     venue_ms))
      ++stats_.order_events;
  }
  if (b.truncated) reconcile_.request_balances();
}

void GeminiVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const OrderShadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void GeminiVenue::drop_shadow(ClientOrderId id) {
  shadows_.erase(id);
}

// ---- execution replay -------------------------------------------------------------------------
//
// POST /v1/mytrades per subscribed symbol, before every open-order snapshot and once a minute
// (ReplayScheduler). "To retrieve your full trade history ... a timestamp ... Take the highest
// timestamp X ... timestamp set to X+1 ... until an empty list is returned": a page is the trades
// at or after `timestamp`, at most limit_trades (500), listed newest first; the next page asks
// from the newest row. Each row on a subscribed symbol is emitted as a fill carrying its tid, so
// the OMS keeps the ones it never saw. A broken trade ("full") is not forwarded.

void GeminiVenue::resume_executions(std::int64_t since_venue_ms,
                                    const std::vector<std::string>& known) {
  exec_replay_.resume(since_venue_ms, known);
  funding_replay_.resume(since_venue_ms, known);
}

bool GeminiVenue::request_executions(std::int64_t since_venue_ms) {
  if (since_venue_ms > 0 && !exec_replay_.active()) exec_replay_.restart_from(since_venue_ms);
  if (since_venue_ms > 0 && !funding_replay_.active()) funding_replay_.restart_from(since_venue_ms);
  static_cast<void>(funding_replay_.run());
  return exec_replay_.run();
}

bool GeminiVenue::replay_ready() const noexcept {
  return !cfg_.dry_run && connected_ && signer_.usable() && rest_ != nullptr &&
         !rest_hard_stopped_ && !subscribed_.empty();
}

bool GeminiVenue::query_trades(const ReplayQuery& q) {
  if (rest_hard_stopped_ || q.stream >= subscribed_.size()) return false;
  const std::string_view sym = symbols_->lower_symbol(subscribed_[q.stream]);
  const RestRequest rr =
      GeminiOrderEncoder::my_trades(next_nonce(), sym, q.start_ms, kTradesPageLimit);
  const bool queued = rest_post(rr, [this, q](const net::HttpResponse& r) {
    if (!exec_replay_.expects(q)) return;
    std::vector<TradeRow> rows;
    const std::string err = r.ok() ? decode_trades(r.body, rows) : response_error(r);
    if (!err.empty()) {
      ++stats_.execution_query_errors;
      FASTMM_LOG_ERROR(
          "{}: mytrades failed ({}); this reconciliation cannot book the fills the order stream "
          "missed",
          cfg_.name,
          err);
      if (r.ok()) {
        ++stats_.rest_errors;
      } else {
        apply_rest_error(r, "mytrades");
      }
      exec_replay_.failed(q);
      return;
    }
    // Oldest first, for the scheduler; the venue lists newest first.
    std::stable_sort(rows.begin(), rows.end(), [](const TradeRow& a, const TradeRow& b) {
      return a.time_ms < b.time_ms;
    });
    ReplayPage<TradeRow> page;
    page.rows.reserve(rows.size());
    for (TradeRow& t : rows) {
      const std::int64_t time = t.time_ms;
      std::string key = t.tid;
      page.rows.push_back({time, 0, std::move(key), std::move(t)});
    }
    exec_replay_.answer(q, std::move(page));
  });
  if (!queued) {
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: no room to ask for the account's trades", cfg_.name);
  }
  return queued;
}

bool GeminiVenue::emit_trade(std::size_t stream, const TradeRow& t) {
  InstrumentId inst = t.symbol.empty() ? InstrumentId::invalid() : subscribed_instrument(t.symbol);
  if (!inst.valid() && stream < subscribed_.size()) inst = subscribed_[stream];
  if (!inst.valid()) return false;
  if (t.break_type == "full") {
    FASTMM_LOG_WARN("{}: trade {} was broken (full): not booked", cfg_.name, t.tid);
    return false;
  }
  ClientOrderId cl{};
  if (const auto id = decode_cl_ord_id(t.client_order_id)) cl = *id;
  emit_replayed_fill(*order_sink_,
                     id_,
                     inst,
                     cl,
                     t.order_id,
                     t.tid,
                     t.buy ? Side::Buy : Side::Sell,
                     t.price,
                     t.qty,
                     t.fee,
                     fee_asset_of(instruments_->get(inst), t.fee, t.fee_currency),
                     t.aggressor ? Liquidity::Taker : Liquidity::Maker,
                     t.time_ms);
  ++stats_.order_events;
  ++stats_.executions_fetched;
  return true;
}

// ---- funding ----------------------------------------------------------------------------------
//
// POST /v1/perpetuals/fundingPayment?since=..&to=.. (the account's hourly transfers), with every
// reconciliation's replay and once a minute. A transfer has no id: the symbol and its time are.

bool GeminiVenue::query_funding(const ReplayQuery& q) {
  if (rest_hard_stopped_) return false;
  const RestRequest rr = GeminiOrderEncoder::funding_payments(next_nonce(), q.start_ms, q.end_ms);
  return rest_post(rr, [this, q](const net::HttpResponse& r) {
    if (!funding_replay_.expects(q)) return;
    std::vector<FundingRow> rows;
    const std::string err = r.ok() ? decode_funding(r.body, rows) : response_error(r);
    if (!err.empty()) {
      FASTMM_LOG_ERROR("{}: fundingPayment failed ({}); funding is asked again", cfg_.name, err);
      if (r.ok()) {
        ++stats_.rest_errors;
      } else {
        apply_rest_error(r, "fundingPayment");
      }
      funding_replay_.failed(q);
      return;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const FundingRow& a, const FundingRow& b) {
      return a.time_ms < b.time_ms;
    });
    ReplayPage<FundingRow> page;
    page.rows.reserve(rows.size());
    for (FundingRow& f : rows) {
      const std::int64_t time = f.time_ms;
      std::string key = std::string(kFundingIdPrefix) + f.symbol + ":" + std::to_string(time);
      page.rows.push_back({time, 0, std::move(key), std::move(f)});
    }
    funding_replay_.answer(q, std::move(page));
  });
}

bool GeminiVenue::emit_funding_row(const FundingRow& f) {
  const InstrumentId inst = subscribed_instrument(f.symbol);
  if (!inst.valid()) return false;  // not traded here
  const std::string id = f.symbol + ":" + std::to_string(f.time_ms);
  emit_funding(
      *order_sink_,
      id_,
      inst,
      id,
      f.amount,
      f.asset.empty() ? instruments_->get(inst).settlement_ccy() : std::string_view(f.asset),
      f.time_ms,
      /*replayed=*/true);
  ++stats_.order_events;
  ++stats_.funding_fetched;
  return true;
}

// ---- control requests -----------------------------------------------------------------------

// Order-channel loss: POST /v1/order/cancel/session, this key's orders on every symbol.
void GeminiVenue::cancel_session_async() {
  static_cast<void>(rest_post(
      GeminiOrderEncoder::cancel_session(next_nonce()), [this](const net::HttpResponse& r) {
        std::size_t n = 0;
        std::vector<std::string> rejects;
        const std::string err =
            r.ok() ? decode_cancel_result(r.body, n, rejects) : response_error(r);
        if (!err.empty() || !rejects.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_ERROR("{}: cancel/session failed: {}{}",
                           cfg_.name,
                           err,
                           rejects.empty() ? "" : rejects.front());
          return;
        }
        FASTMM_LOG_INFO("{}: cancel/session cancelled {} orders", cfg_.name, n);
      }));
}

// POST /v1/heartbeat: {"result":"ok"}.
void GeminiVenue::send_heartbeat() {
  const std::uint32_t round = heartbeat_.begin_round(now_ns(), 1);
  const bool queued = rest_post(
      GeminiOrderEncoder::heartbeat(next_nonce()), [this, round](const net::HttpResponse& r) {
        if (r.ok() && std::string_view(r.body).find("\"ok\"") != std::string_view::npos) {
          if (!heartbeat_.ever_armed()) FASTMM_LOG_INFO("{}: heartbeat accepted", cfg_.name);
          heartbeat_.confirmed(round);
          return;
        }
        // Not fatal by itself: if it was up and lapses, on_timer() kills the venue.
        apply_rest_error(r, "heartbeat");
      });
  if (queued) heartbeat_.went_out();
}

// The kill switch: POST /v1/order/cancel/session over an independent blocking connection, a
// rate-limit refusal waited out (BlockingControl). It cancels the orders this API key placed on
// every symbol (Gemini has no per-symbol cancel-all), not the account's other sessions'.
bool GeminiVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable()) return true;
  BlockingControl control(cfg_, gemini_blocking_retry());
  const HttpReply reply = control.send("kill-switch cancel/session", [&](BlockingRequest& q) {
    const RestRequest rr = GeminiOrderEncoder::cancel_session(next_nonce());
    q.method = "POST";
    q.target = rr.target;
    q.headers = signer_.rest_headers(rr.payload);
    return true;
  });
  std::size_t n = 0;
  std::vector<std::string> rejects;
  const std::string err =
      reply.ok() ? decode_cancel_result(reply.body, n, rejects) : reply_error(reply);
  const bool ok = err.empty() && rejects.empty();
  control.report("kill-switch cancel/session",
                 reply,
                 ok,
                 !err.empty()
                     ? std::string_view(err)
                     : (rejects.empty() ? std::string_view() : std::string_view(rejects.front())));
  if (ok) FASTMM_LOG_INFO("{}: kill-switch cancel-all ok ({} orders)", cfg_.name, n);
  return ok;
}

// ---- housekeeping -----------------------------------------------------------------------------

void GeminiVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (now - last_ping_ns_ >= static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs) {
    last_ping_ns_ = now;
    // No heartbeats from the venue: our `ping` is answered, which keeps dead_ms honest.
    const std::size_t n = GeminiOrderEncoder::encode_method("ping", "ping", request_buf_);
    const std::string_view ping(request_buf_, n);
    if (md_conn_.is_live()) static_cast<void>(md_conn_.send_text(ping));
    if (order_conn_.is_live()) static_cast<void>(order_conn_.send_text(ping));
  }
  if (clock_sync_ns_ == 0 || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  reconcile_.on_timer(now);
  if (!cfg_.dry_run && signer_.usable()) {
    exec_replay_.on_timer(now);
    funding_replay_.on_timer(now);
    // "Requires Heartbeat": refreshed from the housekeeping timer, the thread that would stop if
    // this process died. A lapse means the venue has cancelled the key's orders.
    switch (heartbeat_.poll(now)) {
      case CountdownDriver::Step::Lapsed:
        FASTMM_LOG_ERROR(
            "{}: no heartbeat accepted within {} ms; the venue has cancelled this key's orders",
            cfg_.name,
            heartbeat_.window_ms());
        fatal_ = true;
        trip_venue_kill(KillReason::DeadMansSwitchLost);
        break;
      case CountdownDriver::Step::Refresh:
        send_heartbeat();
        break;
      case CountdownDriver::Step::None:
        break;
    }
  }
  shadow_overflow_.check(cfg_.name, shadows_.size(), decltype(shadows_)::kMaxSize);
  publish_status();
  raw_md_.flush();
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

void GeminiVenue::publish_status() noexcept {
  stats_.shadows = shadows_.size();
  stats_.shadows_refused = shadow_overflow_.count();
  stats_.shadows_swept = reconcile_.shadows_swept();
  stats_.execution_queries = exec_replay_.replays();
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_.load(std::memory_order_relaxed);
  wire_.summarize(
      tsc_calibration(), stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
  budget_pub_.store(budget_of(rate_, now_ns(), orders_taken_));
}

VenueStatus GeminiVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

GeminiVenueConfig make_gemini_config(const VenueSection& v, bool dry_run) {
  GeminiVenueConfig c;
  c.name = v.name;
  c.ws_md_url = v.ws_url.empty() ? std::string("wss://ws.sandbox.gemini.com") : v.ws_url;
  c.ws_order_url = v.ws_api_url.empty() ? c.ws_md_url : v.ws_api_url;
  c.rest_url = v.rest_url.empty() ? std::string("https://api.sandbox.gemini.com") : v.rest_url;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.dry_run = dry_run;
  c.sandbox = v.testnet;
  c.credentials.api_key = v.api_key;
  c.credentials.secret.value = v.api_secret;
  const VenueExtras x(v.extra);
  c.cancel_on_disconnect = x.flag("cancel_on_disconnect", true);
  c.heartbeat = x.flag("heartbeat", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("dead_ms", c.dead_ms)));
  check_liveness(v.name, c.stale_ms, c.dead_ms);
  c.ping_interval_ms = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(x.integer("ping_interval_ms", c.ping_interval_ms), 1000, 60'000));
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("orders_per_second", c.orders_per_second)));
  // "Only account-scoped keys with time-based nonces are accepted" by the WebSocket API.
  if (!dry_run && c.credentials.api_key.starts_with("master-"))
    throw std::invalid_argument(fmt::format(
        "venues.{}.api_key: a master key; the WebSocket API accepts account-scoped keys only",
        v.name));
  for (const std::string* url : {&c.ws_md_url, &c.ws_order_url, &c.rest_url}) {
    const auto u = net::Url::parse(*url);
    if (!u) throw std::invalid_argument(fmt::format("venues.{}: bad URL {}", v.name, *url));
    const bool sandbox_host = u->host.find("sandbox.gemini.com") != std::string_view::npos;
    const bool live_host = !sandbox_host && u->host.find("gemini.com") != std::string_view::npos;
    if (sandbox_host && !c.sandbox)
      throw std::invalid_argument(fmt::format(
          "venues.{}.testnet: false with the sandbox host {}; the sandbox needs testnet = true",
          v.name,
          u->host));
    if (live_host && c.sandbox)
      throw std::invalid_argument(fmt::format(
          "venues.{}.testnet: true (the default) with the production host {}: set testnet = false "
          "for production, or use the *.sandbox.gemini.com hosts",
          v.name,
          u->host));
  }
  return c;
}

}  // namespace fastmm::venues::gemini
