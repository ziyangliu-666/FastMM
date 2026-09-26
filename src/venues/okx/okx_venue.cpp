#include "fastmm/venues/okx/okx_venue.hpp"

#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fastmm::venues::okx {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kClockResyncNs = 30LL * 60 * 1'000'000'000;
// OKX's limits are counted over 2 s: a rate-limit reply pauses sending that long.
constexpr std::int64_t kRateLimitCooldownNs = 2'000'000'000;
constexpr std::int64_t kDayMs = 24LL * 3600 * 1000;
// orders-pending: 100 a page; more than this many pages is a runaway, not a book to reconcile.
constexpr std::size_t kMaxReconcilePages = 40;
constexpr std::size_t kPageLimit = 100;
constexpr std::int64_t kExecutionRetryNs = 5'000'000'000;
constexpr std::int64_t kExecutionSweepNs = 60 * 1'000'000'000LL;
// GET /api/v5/trade/fills covers "the last 3 days", fills-history "the last 3 months". A start
// within an hour of the 3 days already reads the history.
constexpr std::int64_t kHourMs = 3600LL * 1000;
constexpr std::int64_t kFillsRecentMs = 3 * kDayMs - kHourMs;
constexpr std::int64_t kHistoryMs = 90 * kDayMs;
// GET /api/v5/account/bills covers 7 days, bills-archive 3 months.
constexpr std::int64_t kBillsRecentMs = 7 * kDayMs - kHourMs;
constexpr std::size_t kMaxExecPagesPerWindow = 20;
constexpr std::size_t kMaxExecRequests = 100;
constexpr std::size_t kMaxFundingPages = 20;
// A replay that found nothing moves its watermark up to this long before the query: a fill or a
// bill the venue indexes late is still found by the next one.
constexpr std::int64_t kSettleMs = 5LL * 60 * 1000;
constexpr std::int64_t kPositionSettleNs = 1'000'000'000;
constexpr std::int64_t kFundingQueryDelayNs = 1'000'000'000;

// A fee in the settlement currency (the instrument's quote) is Quote, in its base Base.
FeeAsset fee_asset_of(const Instrument& in, Notional fee, std::string_view ccy) noexcept {
  if (fee.is_zero() || ccy.empty() || iequals_symbol(in.quote.view(), ccy)) return FeeAsset::Quote;
  if (iequals_symbol(in.base.view(), ccy)) return FeeAsset::Base;
  return FeeAsset::Other;
}

// Replaces the path of a ws(s) URL: .../ws/v5/public -> .../ws/v5/private.
std::string with_path(const std::string& url, std::string_view path) {
  const auto u = net::Url::parse(url);
  if (!u) return {};
  return origin_of(*u) + std::string(path);
}

std::string reply_error(const HttpReply& r) {
  return r.error.empty() ? fmt::format("HTTP {} {}", r.status, r.body.substr(0, 200)) : r.error;
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

OkxVenue::OkxVenue(VenueId id, OkxVenueConfig cfg)
    : id_(id),
      cfg_(std::move(cfg)),
      signer_(cfg_.credentials),
      rate_(cfg_.rate_threshold),
      dms_(cfg_.dry_run ? 0 : static_cast<std::int64_t>(cfg_.dead_mans_switch_s) * 1000) {
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  inst_codes_.fill(-1);
  std::memset(scratch_, 0, sizeof scratch_);
}

OkxVenue::~OkxVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps OkxVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = cfg_.ws_order_api;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

std::int64_t OkxVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_.load(std::memory_order_relaxed);
}

std::string OkxVenue::rest_headers(const RestRequest& rr) const {
  return signer_.rest_headers(venue_time_ms(), rr.method, rr.path, rr.body, cfg_.simulated);
}

bool OkxVenue::md_needs_login() const noexcept {
  return cfg_.depth_channel != OkxDepthChannel::Books && signer_.usable();
}

net::ConnectionConfig OkxVenue::ws_config(const std::string& url,
                                          bool manual_auth,
                                          bool manual_subscribe) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  // Our "ping" every ping_interval_ms is answered with "pong", so a quiet private or order
  // connection still hears from the venue within the dead threshold.
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, cfg_.ping_interval_ms * 2 + 5000);
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = 0;  // no documented connection lifetime
  c.manual_auth = manual_auth;
  c.manual_subscribe = manual_subscribe;
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> OkxVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  // instIdCode may differ between production and demo: public requests carry the header too.
  const std::string public_headers = cfg_.simulated ? "x-simulated-trading: 1\r\n" : "";
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    {
      const HttpReply t = http.get("/api/v5/public/time", public_headers);
      std::int64_t server_ms = 0;
      if (t.ok() && decode_server_time(t.body, server_ms).empty()) {
        // The receive time, not a send/receive midpoint: the first request includes the TCP and
        // TLS handshakes, which would bias a midpoint by half of them.
        clock_offset_ms_.store(server_ms - wall_now().ns / kNsPerMs);
        clock_sync_ns_ = now_ns();
      }
    }
    for (Instrument* inst : mine) {
      const HttpReply reply = http.get(
          fmt::format("/api/v5/public/instruments?instType=SWAP&instId={}", inst->symbol.view()),
          public_headers);
      if (!reply.ok()) {
        if (!cfg_.allow_offline_reference_data)
          return fail(fmt::format("{}: instruments failed: {}", cfg_.name, reply_error(reply)));
        FASTMM_LOG_WARN("{}: instruments failed ({}); keeping configured tick/lot",
                        cfg_.name,
                        reply_error(reply));
        continue;
      }
      std::vector<InstrumentInfo> infos;
      if (const std::string err = decode_instruments(reply.body, infos); !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
      const InstrumentInfo* f = nullptr;
      for (const InstrumentInfo& i : infos) {
        if (iequals_symbol(i.inst_id, inst->symbol.view())) f = &i;
      }
      if (f == nullptr)
        return fail(fmt::format(
            "{}: {} not in public/instruments?instType=SWAP", cfg_.name, inst->symbol.view()));
      if (f->inst_type != "SWAP" || f->ct_type != "linear")
        return fail(fmt::format(
            "{}: {} is a {} {} contract; only linear (USDT-margined) swaps are supported",
            cfg_.name,
            inst->symbol.view(),
            f->ct_type.empty() ? "?" : f->ct_type,
            f->inst_type));
      if (!f->tick.is_positive() || !f->lot.is_positive() || !f->ct_val.is_positive() ||
          !f->ct_mult.is_positive())
        return fail(fmt::format("{}: {} has an invalid tickSz, lotSz, ctVal or ctMult",
                                cfg_.name,
                                inst->symbol.view()));
      if (f->settle_ccy.empty() || f->ct_val_ccy.empty())
        return fail(
            fmt::format("{}: {} has no settleCcy or ctValCcy", cfg_.name, inst->symbol.view()));
      // Contracts are the unit: one is ctVal * ctMult of ctValCcy (0.01 BTC for BTC-USDT-SWAP).
      const Qty mult = f->ct_mult == Qty::from_int(1)
                           ? f->ct_val
                           : Qty::from_raw(mul_raw(f->ct_val, f->ct_mult));
      if (inst->tick != f->tick || inst->lot != f->lot || inst->contract_multiplier != mult) {
        FASTMM_LOG_WARN(
            "{}: {} tick/lot/multiplier from public/instruments override config ({} / {} / {} -> "
            "{} / {} / {}); quantities are contracts",
            cfg_.name,
            inst->symbol.view(),
            inst->tick,
            inst->lot,
            inst->contract_multiplier,
            f->tick,
            f->lot,
            mult);
      }
      inst->tick = f->tick;
      inst->lot = f->lot;
      inst->min_qty = f->min_sz.is_positive() ? f->min_sz : f->lot;
      inst->max_qty = f->max_limit_sz;
      inst->min_notional = Notional{};  // OKX sets none for swaps: minSz is the floor
      inst->max_notional = Notional{};
      inst->contract_multiplier = mult;
      inst->expiry_ns = 0;
      inst->flags = static_cast<std::uint8_t>((inst->flags | Instrument::kReduceOnlySupported) &
                                              ~Instrument::kInverse);
      if (inst->asset_class != AssetClass::Perpetual) {
        FASTMM_LOG_WARN(
            "{}: {} is a perpetual; asset_class set to perpetual", cfg_.name, inst->symbol.view());
        inst->asset_class = AssetClass::Perpetual;
      }
      // [accounting] settles a linear instrument in its quote: that has to be settleCcy.
      if (!inst->quote.empty() && !iequals_symbol(inst->quote.view(), f->settle_ccy))
        FASTMM_LOG_WARN("{}: {} settles in {}, not the configured quote {}; using {}",
                        cfg_.name,
                        inst->symbol.view(),
                        f->settle_ccy,
                        inst->quote.view(),
                        f->settle_ccy);
      if (!inst->quote.assign(f->settle_ccy) || !inst->base.assign(f->ct_val_ccy))
        return fail(fmt::format("{}: {} currency names too long", cfg_.name, inst->symbol.view()));
      inst_codes_[inst->id.value] = f->inst_id_code;
      if (f->inst_id_code < 0)
        FASTMM_LOG_WARN(
            "{}: {} has no instIdCode; its orders go over REST", cfg_.name, inst->symbol.view());
      if (f->state == "post_only") {
        FASTMM_LOG_WARN("{}: {} accepts post-only orders only (state post_only)",
                        cfg_.name,
                        inst->symbol.view());
      } else if (f->state != "live") {
        FASTMM_LOG_ERROR(
            "{}: {} state is {} (not live): disabled", cfg_.name, inst->symbol.view(), f->state);
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
  FASTMM_LOG_INFO("{}: reference data loaded for {} swaps", cfg_.name, mine.size());
  if (!cfg_.dry_run && signer_.usable()) {
    if (std::string err = check_account(); !err.empty()) return fail(std::move(err));
  }
  return {};
}

// GET /api/v5/account/config: posMode (net_mode | long_short_mode) and acctLv (1 spot, 2 futures,
// 3 multi-currency margin, 4 portfolio margin). Orders without posSide are refused in long/short
// mode, and the spot mode trades no swaps: both are settings a retry does not change, and so is a
// key the venue refuses. A failure to reach the venue is not.
std::string OkxVenue::check_account() {
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    RestRequest rr;
    OkxOrderEncoder::encode_rest_account_config(rr);
    const HttpReply reply = http.request("GET", rr.path, rest_headers(rr));
    int code = -1;
    std::string msg;
    if (reply.status > 0 && decode_envelope(reply.body, code, msg) && code != 0) {
      refused_account_settings_ = map_error(code, msg).action == VenueAction::Fatal;
      return fmt::format("{}: account config refused: code {} {}", cfg_.name, code, msg);
    }
    if (!reply.ok())
      return fmt::format("{}: account config failed: {}", cfg_.name, reply_error(reply));
    AccountConfig ac;
    if (std::string err = decode_account_config(reply.body, ac); !err.empty())
      return fmt::format("{}: {}", cfg_.name, err);
    if (ac.pos_mode != "net_mode") {
      refused_account_settings_ = true;
      return fmt::format(
          "{}: the account is in {} (posMode); the okx connector trades net mode only: switch it "
          "with POST /api/v5/account/set-position-mode {{\"posMode\":\"net_mode\"}}",
          cfg_.name,
          ac.pos_mode);
    }
    if (ac.acct_lv == "1") {
      refused_account_settings_ = true;
      return fmt::format(
          "{}: the account is in spot mode (acctLv 1), which cannot trade swaps: switch it to "
          "futures, multi-currency or portfolio margin mode",
          cfg_.name);
    }
    FASTMM_LOG_INFO("{}: account mode acctLv {}, posMode {}, tdMode {}",
                    cfg_.name,
                    ac.acct_lv,
                    ac.pos_mode,
                    to_string(cfg_.td_mode));
  } catch (const std::exception& e) {
    return fmt::format("{}: account config failed: {}", cfg_.name, std::string_view(e.what()));
  }
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void OkxVenue::attach(const SymbolTable& symbols,
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
      std::make_unique<OkxMdFeed>(symbols,
                                  id_,
                                  md_sink,
                                  ResubscribeRequester{&OkxVenue::resubscribe_requester, this},
                                  cfg_.depth_channel);
  private_parser_ = std::make_unique<OkxPrivateParser>(symbols, instruments, id_);
  encoder_ = std::make_unique<OkxOrderEncoder>(symbols, cfg_.td_mode);
  for (const Instrument& in : instruments) {
    if (in.venue == id_) encoder_->set_inst_id_code(in.id, inst_codes_[in.id.value]);
  }
  decoder_ = std::make_unique<OkxResponseDecoder>();
}

void OkxVenue::subscribe(std::span<const InstrumentId> instruments) {
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

InstrumentId OkxVenue::subscribed_instrument(std::string_view inst_id) const noexcept {
  const InstrumentId id = symbols_->find(id_, inst_id);
  if (!id.valid()) return id;
  if (std::find(subscribed_.begin(), subscribed_.end(), id) == subscribed_.end())
    return InstrumentId::invalid();
  return id;
}

void OkxVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("OkxVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  // Nobody said where the replays should start, so they start here: this session can only have
  // missed what happened after it connected.
  if (exec_since_ms_ <= 0) exec_since_ms_ = venue_time_ms();
  if (funding_since_ms_ <= 0) funding_since_ms_ = exec_since_ms_;
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
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, private={}, ws_orders={}, demo={}, depth={})",
                  cfg_.name,
                  cfg_.dry_run,
                  !cfg_.dry_run && signer_.usable(),
                  cfg_.ws_order_api,
                  cfg_.simulated,
                  to_string(cfg_.depth_channel));
}

void OkxVenue::disconnect() {
  if (!connected_) return;
  // A requested shutdown cancels its own orders (Session::stop runs cancel_all()), so the
  // countdown has nothing left to protect: stop it, or it cancels whatever the account holds a
  // minute from now (it is account-wide). On its own blocking connection, as cancel_all() does: a
  // request queued on the REST channel behind another would be dropped by the reset below.
  if (dms_.enabled() && dms_.ever_armed() && signer_.usable()) stop_cancel_all_after();
  dms_.disarm();
  connected_ = false;
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  md_conn_.close();
  private_conn_.close();
  trade_conn_.close();
  // Replies to requests the reset aborts are ignored: a later connect() starts afresh.
  ++generation_;
  exec_replay_active_ = false;
  funding_active_ = false;
  funding_due_ns_ = 0;
  oo_wanted_ = false;
  reconcile_in_flight_ = false;
  time_request_pending_ = false;
  if (rest_) rest_->reset();
  md_feed_->on_disconnected();
  raw_md_.flush();
  raw_private_.flush();
  raw_trade_.flush();
  publish_status();
}

void OkxVenue::open_rest() {
  RestChannelConfig rc;
  rc.base_url = cfg_.rest_url;
  rc.ca_file = cfg_.ca_file;
  rc.insecure_tls = cfg_.insecure_tls;
  rc.timeout_ms = cfg_.http_timeout_ms;
  // Reconciliation, both replays, the dead man's switch and the clock can be in flight at once.
  rc.max_queue = 32;
  rest_ = std::make_unique<RestChannel>(*reactor_, rc);
}

void OkxVenue::open_md() {
  md_conn_.open(*reactor_, ws_config(cfg_.ws_public_url, md_needs_login(), false), md_handler_);
  md_conn_.connect();
}

void OkxVenue::open_private() {
  private_conn_.open(*reactor_, ws_config(cfg_.ws_private_url, true, true), private_handler_);
  private_conn_.connect();
}

void OkxVenue::open_trade() {
  trade_conn_.open(*reactor_, ws_config(cfg_.ws_trade_url, true, false), trade_handler_);
  trade_conn_.connect();
}

template <class Slot>
void OkxVenue::send_login(Slot& slot) {
  const std::size_t n =
      OkxOrderEncoder::encode_login(signer_, venue_time_ms() / 1000, request_buf_);
  if (n == 0 || !slot.send_text(std::string_view(request_buf_, n)))
    FASTMM_LOG_ERROR("{}: could not send the login request", cfg_.name);
}

// ---- market data ------------------------------------------------------------------------------

void OkxVenue::on_md_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating && md_needs_login()) send_login(md_conn_);
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
        if (OkxBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
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

void OkxVenue::on_md_open() {
  for (const std::string& p : md_feed_->subscription_payloads()) {
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void OkxVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (OkxBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void OkxVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Ok) return;
  const MdDecodeResult& r = md_feed_->last();
  switch (r.control) {
    case ControlOp::Login:
      if (r.control_success) {
        FASTMM_LOG_INFO("{}: market-data connection logged in", cfg_.name);
        md_conn_.auth_done();
      } else {
        FASTMM_LOG_ERROR("{}: market-data login failed: {} {}", cfg_.name, r.code, r.msg);
        const VenueAction a = map_error(r.code, r.msg).action;
        apply_action(a == VenueAction::None ? VenueAction::Fatal : a, r.code, r.msg);
      }
      return;
    case ControlOp::Error:
      // 64003: the fee tier is below what a tbt depth channel needs; 60018: no such channel.
      FASTMM_LOG_ERROR("{}: market-data request refused: {} {}", cfg_.name, r.code, r.msg);
      if (md_needs_login() && md_state_ != ConnState::Live) {
        const VenueAction a = map_error(r.code, r.msg).action;
        if (a == VenueAction::Fatal || a == VenueAction::ResyncClock)
          apply_action(a, r.code, r.msg);
      }
      return;
    case ControlOp::Notice:
      FASTMM_LOG_WARN("{}: market-data notice {} {}", cfg_.name, r.code, r.msg);
      return;
    default:
      break;
  }
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  }
}

void OkxVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;  // the reconnect subscribes everything again
  for (const std::string& p : md_feed_->resubscribe_payloads(id))
    static_cast<void>(md_conn_.send_text(p));
  FASTMM_LOG_INFO(
      "{}: resubscribing {} for a fresh snapshot", cfg_.name, symbols_->venue_symbol(id));
}

// ---- private stream -------------------------------------------------------------------------

void OkxVenue::on_private_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating) send_login(private_conn_);
  const ConnState mapped = map_conn_state(s);
  stats_.user = private_channel_state(s);
  if (mapped == private_state_) return;
  const ConnState prev = private_state_;
  private_state_ = mapped;
  if (mapped == ConnState::Live) {
    if (prev != ConnState::Stale) emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
    FASTMM_LOG_INFO("{}: private channel -> Live", cfg_.name);
    // Reconcile after a reconnect, not when a quiet channel returns from Stale. On the first
    // connect, sweep for orders a session that died left resting: their ids belong to an earlier
    // epoch, so the engine cancels them. The replay before the snapshot also books what happened
    // while nothing ran.
    if (private_was_live_ && prev != ConnState::Stale) {
      request_open_orders();
    } else if (!private_was_live_) {
      sweep_next_ = true;
      request_open_orders();
    }
    private_was_live_ = true;
  } else if ((mapped == ConnState::Disconnected || mapped == ConnState::Connecting) &&
             (prev == ConnState::Live || prev == ConnState::Stale)) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: private channel lost", cfg_.name);
  }
}

void OkxVenue::on_private_open() {
  constexpr std::string_view kChannels[] = {"orders", "positions", "balance_and_position"};
  const std::size_t n =
      OkxOrderEncoder::encode_private_subscribe("private", kChannels, request_buf_);
  if (n == 0 || !private_conn_.send_text(std::string_view(request_buf_, n)))
    FASTMM_LOG_ERROR("{}: could not subscribe the private channels", cfg_.name);
}

ClientOrderId OkxVenue::current_id(ClientOrderId link) const noexcept {
  if (const ClientOrderId* cur = aliases_.find(link)) return *cur;
  return link;
}

void OkxVenue::forget_order(ClientOrderId id) noexcept {
  if (const OrderShadow* s = shadows_.find(id)) {
    if (s->link_id.valid() && s->link_id != id) aliases_.erase(s->link_id);
  }
  shadows_.erase(id);
}

// An amend the venue applied: later events for the original clOrdId are the new id's.
void OkxVenue::on_amend_result(const OrderAckMsg& ack) noexcept {
  const OrderShadow* shadow = shadows_.find(ack.cl_ord_id);
  if (shadow == nullptr) return;
  const ClientOrderId id = ack.cl_ord_id;
  const ClientOrderId link = shadow->link_id;
  const ClientOrderId orig = shadow->replaces;
  if (link.valid() && link != id) aliases_.assign(link, id);
  if (orig.valid() && orig != id) shadows_.erase(orig);
}

void OkxVenue::on_private_text(std::string_view t, std::int64_t ts) {
  if (raw_private_.enabled()) raw_private_.record(ts, t);
  const Cycles t0 = rdtscp();
  const PrivateDecodeResult r = private_parser_->decode(t, wall_now(), t0, scratch_);
  if (r.funding_event && funding_due_ns_ == 0) funding_due_ns_ = now_ns() + kFundingQueryDelayNs;
  if (r.status == ParseStatus::Ok) {
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      switch (h->type) {
        case EventType::PositionUpdate: {
          // Compared with the fills in check_positions(), not forwarded as is: the positions and
          // orders channels are not ordered against each other.
          const auto& p = *reinterpret_cast<const PositionUpdateMsg*>(h);
          if (cfg_.position_from_stream && p.hdr.instrument.value < kMaxInstruments) {
            PositionCheck& pc = positions_[p.hdr.instrument.value];
            if (!pc.pending || pc.venue != p.qty) pc.last_event_ns = now_ns();
            pc.venue = p.qty;
            pc.venue_avg = p.avg_px;
            pc.pending = true;
          }
          continue;
        }
        case EventType::OrderAck: {
          auto* m = reinterpret_cast<OrderAckMsg*>(h);
          if ((m->flags & OrderAckMsg::kAmendedInPlace) != 0) {
            on_amend_result(*m);  // the id is the amend's own (reqId)
          } else {
            m->cl_ord_id = current_id(m->cl_ord_id);
          }
          break;
        }
        case EventType::OrderReject:
          // Only an amend is rejected on this channel, under its own id; the order it meant to
          // change is still working.
          shadows_.erase(reinterpret_cast<OrderRejectMsg*>(h)->cl_ord_id);
          break;
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
          note_fill(*m);
          break;
        }
        default:
          break;
      }
      static_cast<void>(order_sink_->push(*h));
      ++stats_.order_events;
    }
  } else {
    switch (r.control) {
      case ControlOp::Login:
        if (r.control_success) {
          FASTMM_LOG_INFO("{}: private stream logged in", cfg_.name);
          private_conn_.auth_done();
        } else {
          FASTMM_LOG_ERROR("{}: private login failed: {} {}", cfg_.name, r.code, r.msg);
          const VenueAction a = map_error(r.code, r.msg).action;
          apply_action(a == VenueAction::None ? VenueAction::Fatal : a, r.code, r.msg);
        }
        return;
      case ControlOp::Subscribe:
        // The orders channel is what the session cannot run without.
        if (r.channel == "orders") private_conn_.subscribe_done();
        return;
      case ControlOp::Error: {
        FASTMM_LOG_ERROR("{}: private request refused: {} {}", cfg_.name, r.code, r.msg);
        const VenueAction a = map_error(r.code, r.msg).action;
        if (a == VenueAction::Fatal || a == VenueAction::ResyncClock)
          apply_action(a, r.code, r.msg);
        return;
      }
      case ControlOp::ChannelConnCount:
        if (!r.control_success)
          FASTMM_LOG_ERROR(
              "{}: too many connections on a private channel: {} {}", cfg_.name, r.code, r.msg);
        return;
      case ControlOp::Notice:
        FASTMM_LOG_WARN("{}: private notice {} {}", cfg_.name, r.code, r.msg);
        return;
      default:
        break;
    }
    if (r.status == ParseStatus::Malformed)
      FASTMM_LOG_WARN("{}: malformed private frame", cfg_.name);
  }
  if (private_parser_->stats().hedge_positions != 0 && !fatal_) {
    // Someone switched the account to long/short mode while the session ran.
    FASTMM_LOG_ERROR("{}: the positions channel reports a long/short-mode position", cfg_.name);
    apply_action(VenueAction::Fatal, 0, "long/short-mode position");
  }
}

// ---- trade channel --------------------------------------------------------------------------

void OkxVenue::on_trade_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating) send_login(trade_conn_);
  const ConnState mapped = map_conn_state(s);
  stats_.order = private_channel_state(s);
  if (mapped == trade_state_) return;
  const ConnState prev = trade_state_;
  trade_state_ = mapped;
  if (mapped == ConnState::Live) {
    // Orders were cancelled over REST while the trade channel was down; reconcile on a real
    // reconnect (not the first connect, not a return from Stale).
    const bool reconnected = trade_was_live_ && prev != ConnState::Stale;
    trade_was_live_ = true;
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
    // runs the synchronous cancel_all().
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void OkxVenue::on_trade_text(std::string_view t, std::int64_t ts) {
  if (raw_trade_.enabled()) raw_trade_.record(ts, t);
  TradeResponse r;
  const ParseStatus st = decoder_->decode(t, r);
  if (st == ParseStatus::Malformed) {
    FASTMM_LOG_WARN("{}: malformed trade frame", cfg_.name);
    return;
  }
  if (st != ParseStatus::Ok) return;
  if (r.event == "login") {
    if (r.code == 0) {
      FASTMM_LOG_INFO("{}: trade connection logged in", cfg_.name);
      trade_conn_.auth_done();
      return;
    }
  }
  if (!r.event.empty()) {
    FASTMM_LOG_ERROR("{}: trade connection {}: {} {}", cfg_.name, r.event, r.code, r.msg);
    if (trade_state_ != ConnState::Live) {
      const VenueAction a = map_error(r.code, r.msg).action;
      apply_action(a == VenueAction::None ? VenueAction::Fatal : a, r.code, r.msg);
    }
    return;
  }
  if (const auto req = parse_request_id(r.id)) {
    handle_order_response(req->first, req->second, r);
    return;
  }
  if (!r.ok())
    FASTMM_LOG_WARN("{}: trade error for '{}': {} {}", cfg_.name, r.id, r.code, r.reason_msg());
}

void OkxVenue::handle_order_response(RequestKind kind, ClientOrderId id, const TradeResponse& r) {
  const OrderShadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  const int code = r.reason_code();
  const std::string_view msg = r.reason_msg();
  const ErrorMapping m = map_error(code, msg);
  switch (kind) {
    case RequestKind::New:
      if (r.ok()) {
        if (cfg_.emit_ack_from_response) emit_order_ack(*order_sink_, id_, inst, id, r.ord_id);
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, code, msg);
        forget_order(id);
        apply_action(m.action, code, msg);
      }
      ++stats_.order_events;
      return;
    case RequestKind::Cancel:
      if (r.ok()) {
        // "sCode 0 only means accepted": the orders channel reports the cancel with accFillSz.
        // Without it the response is all we will get.
        if (!private_conn_.is_live()) {
          emit_cancel_ack(*order_sink_, id_, inst, id, r.ord_id, Qty{});
          forget_order(id);
          ++stats_.order_events;
        }
      } else {
        emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, code, msg);
        apply_action(m.action, code, msg);
        ++stats_.order_events;
      }
      return;
    case RequestKind::Replace:
      if (r.ok()) {
        // Accepted, not applied: the orders channel says which (amendResult under reqId). Without
        // the channel the response is all we will get.
        if (!private_conn_.is_live()) {
          OrderAckMsg ack{};
          init_header(ack, EventType::OrderAck, inst, id_);
          ack.cl_ord_id = id;
          ack.flags = OrderAckMsg::kAmendedInPlace;
          on_amend_result(ack);
          emit_order_ack(*order_sink_, id_, inst, id, r.ord_id, OrderAckMsg::kAmendedInPlace);
          ++stats_.order_events;
        }
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, code, msg);
        shadows_.erase(id);
        apply_action(m.action, code, msg);
        ++stats_.order_events;
      }
      return;
    case RequestKind::Amend:
    case RequestKind::Other:
      return;
  }
}

// ---- outbound ---------------------------------------------------------------------------------

void OkxVenue::on_wake() {
  drain_outbound();
}

template <class Ring>
void OkxVenue::write_orders(Ring& ring) {
  drain_outbound_coalesced(
      ring,
      wire_,
      [this] {
        trade_conn_.cork();
        batch_.clear();
      },
      [this](const EventHeader& h) {
        if (const auto cmd = OrderCommand::from(h)) {
          sent_.note(*cmd);
          send_command(*cmd);
        } else if (is_reconcile_request(h)) {
          request_open_orders();
        }
      },
      [this] {
        if (trade_conn_.uncork()) return true;
        fail_batch();
        return false;
      });
}

void OkxVenue::fail_batch() {
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

void OkxVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void OkxVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

void OkxVenue::send_command(const OrderCommand& cmd) {
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
  // A venue-fatal error and a REST hard stop stop new orders, never cancels.
  if (fatal_ && !is_cancel) return refuse(RejectReason::VenueKilled, "venue fatal");
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
    const std::size_t n = encoder_->encode_ws(cmd, shadow, request_buf_);
    const Cycles after_encode = rdtscp();
    if (n > 0 && trade_conn_.send_text(std::string_view(request_buf_, n))) {
      wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
      batch_.note(cmd);
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
    // n == 0 with a known instrument is an instrument without instIdCode: REST takes instId.
    if (n > 0) ++stats_.order_send_failures;
  }
  send_command_rest(cmd, shadow);
}

void OkxVenue::send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow) {
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  RestRequest rr;
  const Cycles before_encode = rdtscp();
  auto refuse = [&](RejectReason reason, std::string_view why) {
    if (is_cancel) {
      emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
    } else {
      emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
      shadows_.erase(cmd.cl_ord_id);
    }
    ++stats_.order_send_failures;
  };
  if (rest_ == nullptr || (rest_hard_stopped_ && !is_cancel) ||
      !encoder_->encode_rest(cmd, shadow, rr))
    return refuse(RejectReason::VenueReject, "no order channel");
  const std::string headers = rest_headers(rr);
  const Cycles after_encode = rdtscp();
  OrderCommand copy = cmd;
  copy.venue_order_id = nullptr;
  copy.header = nullptr;
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued = rest_->request(
      rr.method, rr.path, headers, rr.body, [this, alive, gen, copy](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        handle_rest_order_response(copy, r);
      });
  if (!queued) return refuse(RejectReason::TransportFull, "rest queue full");
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

void OkxVenue::handle_rest_order_response(const OrderCommand& cmd, const net::HttpResponse& r) {
  ++stats_.rest_requests;
  const RequestKind kind = cmd.kind == OrderCommandKind::New      ? RequestKind::New
                           : cmd.kind == OrderCommandKind::Cancel ? RequestKind::Cancel
                                                                  : RequestKind::Replace;
  TradeResponse tr;
  if (r.error != net::NetError::None) {
    ++stats_.rest_errors;
    tr.code = -1;
    tr.s_code = -1;
    tr.msg = net::to_string(r.error);
    handle_order_response(kind, cmd.cl_ord_id, tr);
    request_open_orders();  // send status unknown: reconcile rather than guess
    return;
  }
  const PaddedJson padded(r.body);
  if (decoder_->decode(padded.view(), tr) != ParseStatus::Ok) {
    ++stats_.rest_errors;
    const ErrorMapping hm = map_http_status(r.status);
    tr = TradeResponse{};
    tr.code = r.status;
    tr.s_code = r.status;
    tr.msg = "unparseable REST reply";
    handle_order_response(kind, cmd.cl_ord_id, tr);
    apply_action(hm.action, r.status, tr.msg);
    return;
  }
  if (r.status != 200 && tr.code == 0) tr.code = r.status;
  handle_order_response(kind, cmd.cl_ord_id, tr);
}

void OkxVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void OkxVenue::apply_action(VenueAction action, int code, std::string_view msg) {
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

// ---- reconciliation -------------------------------------------------------------------------

// One reconciliation at a time: the account's fills are replayed first (request_executions), then
// the open-order snapshot is read. A request while the replay runs is served by its end.
void OkxVenue::request_open_orders() {
  if (cfg_.dry_run || !connected_ || !signer_.usable() || reconcile_in_flight_) return;
  oo_wanted_ = true;
  if (exec_replay_active_) return;
  if (request_executions()) return;
  oo_wanted_ = false;
  exec_snapshot_exact_ = false;
  send_open_orders();
}

// Nothing reaches the engine before every page parsed: Oms::reconcile_end() cancels every order
// the snapshot does not name, so a truncated or unparsed snapshot would cancel live orders.
void OkxVenue::send_open_orders() {
  if (reconcile_in_flight_) return;
  reconcile_records_.clear();
  reconcile_positions_.clear();
  reconcile_pages_ = 0;
  // The start-up sweep says nothing about our own orders: one sent before the snapshot was asked
  // for can still be in flight.
  reconcile_watermark_ = sweep_next_ ? ClientOrderId{} : sent_.value();
  sweep_next_ = false;
  request_open_orders_page({});
}

void OkxVenue::request_open_orders_page(const std::string& after) {
  if (cfg_.dry_run || !connected_ || !signer_.usable() || rest_ == nullptr || rest_hard_stopped_) {
    reconcile_in_flight_ = false;
    return;
  }
  RestRequest rr;
  OkxOrderEncoder::encode_rest_orders_pending(after, rr);
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  reconcile_in_flight_ = true;
  const bool queued = rest_->request(
      "GET", rr.path, rest_headers(rr), {}, [this, alive, gen](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        reconcile_in_flight_ = false;
        ++stats_.rest_requests;
        std::vector<PendingOrder> rows;
        std::string err;
        if (!r.ok()) {
          err = fmt::format("status={} err={}", r.status, net::to_string(r.error));
        } else {
          err = decode_pending_orders(r.body, rows);
        }
        if (!err.empty()) {
          // A non-zero code comes back with HTTP 200 as well: rate limits (50011) and clock or
          // signature errors (50102, 50113) among others.
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: orders-pending failed ({}); reconciliation skipped", cfg_.name, err);
          int code = -1;
          std::string msg;
          if (decode_envelope(r.body, code, msg)) {
            const VenueAction a = map_error(code, msg).action;
            if (a != VenueAction::Reconcile) apply_action(a, code, msg);
          }
          reconcile_records_.clear();
          return;
        }
        for (const PendingOrder& o : rows) {
          const InstrumentId inst = subscribed_instrument(o.inst_id);
          if (!inst.valid()) continue;
          ReconcileMsg m{};
          init_header(m, EventType::Reconcile, inst, id_);
          m.kind = ReconcileMsg::Kind::OpenOrder;
          m.side = o.side == "sell" ? Side::Sell : Side::Buy;
          m.state = o.state == "partially_filled" ? OrderState::PartiallyFilled : OrderState::Live;
          if (const auto cl = decode_cl_ord_id(o.cl_ord_id)) m.cl_ord_id = current_id(*cl);
          m.venue_order_id.assign(o.ord_id);
          m.price = o.px;
          m.orig_qty = o.sz;
          m.cum_qty = o.acc_fill_sz;
          m.hdr.recv_ts = wall_now();
          reconcile_records_.push_back(m);
        }
        if (rows.size() >= kPageLimit) {
          if (++reconcile_pages_ >= kMaxReconcilePages) {
            FASTMM_LOG_WARN("{}: more than {} pages of open orders; reconciliation skipped",
                            cfg_.name,
                            kMaxReconcilePages);
            reconcile_records_.clear();
            return;
          }
          request_open_orders_page(rows.back().ord_id);
          return;
        }
        request_positions();
      });
  if (!queued) {
    reconcile_in_flight_ = false;
    reconcile_records_.clear();
  }
}

void OkxVenue::request_positions() {
  const auto abandon = [this] {
    reconcile_in_flight_ = false;
    reconcile_records_.clear();
    reconcile_positions_.clear();
  };
  if (cfg_.dry_run || !connected_ || rest_ == nullptr || rest_hard_stopped_) return abandon();
  RestRequest rr;
  OkxOrderEncoder::encode_rest_positions(rr);
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  reconcile_in_flight_ = true;
  const bool queued = rest_->request(
      "GET",
      rr.path,
      rest_headers(rr),
      {},
      [this, alive, gen, abandon](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        reconcile_in_flight_ = false;
        ++stats_.rest_requests;
        std::string err;
        if (!r.ok()) {
          err = fmt::format("status={} err={}", r.status, net::to_string(r.error));
        } else {
          err = decode_positions(r.body, reconcile_positions_);
        }
        if (!err.empty()) {
          // Positions are part of the snapshot: without them nothing is emitted.
          ++stats_.rest_errors;
          FASTMM_LOG_WARN(
              "{}: account/positions failed ({}); reconciliation skipped", cfg_.name, err);
          return abandon();
        }
        emit_reconcile();
      });
  if (!queued) abandon();
}

void OkxVenue::emit_reconcile() {
  ReconcileMsg begin{};
  init_header(begin, EventType::Reconcile, InstrumentId::invalid(), id_);
  begin.kind = ReconcileMsg::Kind::Begin;
  SentWatermark::stamp(begin, reconcile_watermark_);
  if (exec_snapshot_exact_) begin.flags |= ReconcileMsg::kExecutionsExact;
  exec_snapshot_exact_ = false;
  begin.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(begin.hdr));
  for (const ReconcileMsg& m : reconcile_records_) static_cast<void>(order_sink_->push(m.hdr));
  // Only open positions are listed: a subscribed instrument absent from the list is flat.
  bool long_short = false;
  for (InstrumentId id : subscribed_) {
    const std::string_view sym = symbols_->venue_symbol(id);
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, id, id_);
    m.kind = ReconcileMsg::Kind::Position;
    for (const PositionRecord& p : reconcile_positions_) {
      if (!iequals_symbol(p.inst_id, sym)) continue;
      if (p.pos_side == "long" || p.pos_side == "short") {
        long_short = long_short || !p.qty.is_zero();
        continue;
      }
      m.position_qty = p.qty;
      m.avg_px = p.avg_px;
    }
    m.hdr.recv_ts = wall_now();
    static_cast<void>(order_sink_->push(m.hdr));
    PositionCheck& pc = positions_[id.value];
    pc.tracked = m.position_qty;
    pc.venue = m.position_qty;
    pc.venue_avg = m.avg_px;
    pc.pending = false;
    FASTMM_LOG_INFO("{}: {} position {} contracts @ {}", cfg_.name, sym, m.position_qty, m.avg_px);
  }
  reconcile_positions_.clear();
  ReconcileMsg end{};
  init_header(end, EventType::Reconcile, InstrumentId::invalid(), id_);
  end.kind = ReconcileMsg::Kind::End;
  end.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(end.hdr));
  FASTMM_LOG_INFO("{}: reconciled {} open orders", cfg_.name, reconcile_records_.size());
  reconcile_records_.clear();
  if (long_short) {
    FASTMM_LOG_ERROR("{}: account/positions reports a long/short-mode position", cfg_.name);
    apply_action(VenueAction::Fatal, 0, "long/short-mode position");
  }
}

// ---- execution replay -------------------------------------------------------------------------
//
// GET /api/v5/trade/fills (the last 3 days; fills-history, 3 months, for an older watermark),
// account-wide for instType SWAP, before every open-order snapshot and once a minute. Each row on a
// subscribed instrument is emitted as an ordinary fill carrying its tradeId, so the OMS keeps the
// ones it never saw; that recovers a fill that finished an order, which the snapshot no longer
// mentions. Rows come newest first and are paged with `after` = the last billId; a window is read
// to its last page and emitted oldest first, then the watermark moves to its newest `ts`.

void OkxVenue::resume_executions(std::int64_t since_venue_ms,
                                 const std::vector<std::string>& known) {
  exec_since_ms_ = since_venue_ms;
  exec_edge_ids_.clear();
  known_exec_ids_ = {known.begin(), known.end()};
  funding_since_ms_ = since_venue_ms;
  funding_edge_ids_.clear();
}

bool OkxVenue::request_executions(std::int64_t since_venue_ms) {
  if (cfg_.dry_run || !connected_ || !signer_.usable()) return false;
  if (rest_ == nullptr || rest_hard_stopped_ || subscribed_.empty()) return false;
  if (exec_replay_active_) return true;
  if (since_venue_ms > 0) {
    exec_since_ms_ = since_venue_ms;
    exec_edge_ids_.clear();
    funding_since_ms_ = since_venue_ms;
    funding_edge_ids_.clear();
  }
  // The funding payments go alongside: they change no position, so the snapshot does not wait.
  request_funding({});
  exec_last_ns_ = now_ns();
  exec_replay_active_ = true;
  exec_replay_ok_ = true;
  exec_requests_ = 0;
  ++stats_.execution_queries;
  start_execution_window();
  return true;
}

void OkxVenue::start_execution_window() {
  const std::int64_t now = venue_time_ms();
  if (exec_since_ms_ <= 0) exec_since_ms_ = now;
  if (exec_since_ms_ < now - kHistoryMs) {
    FASTMM_LOG_ERROR("{}: fills before {} are beyond OKX's 3 months; replaying from there",
                     cfg_.name,
                     now - kHistoryMs);
    exec_since_ms_ = now - kHistoryMs;
    exec_edge_ids_.clear();
    exec_replay_ok_ = false;
  }
  exec_history_ = exec_since_ms_ < now - kFillsRecentMs;
  exec_window_start_ = exec_since_ms_;
  exec_window_end_ = 0;
  exec_rows_.clear();
  exec_window_pages_ = 0;
  exec_window_low_ms_ = 0;
  request_executions_page({});
}

void OkxVenue::request_executions_page(const std::string& after) {
  if (++exec_requests_ > kMaxExecRequests) {
    FASTMM_LOG_WARN("{}: fill replay stopped after {} requests; it continues later",
                    cfg_.name,
                    kMaxExecRequests);
    finish_execution_replay(false);
    return;
  }
  if (rest_ == nullptr) {
    finish_execution_replay(false);
    return;
  }
  RestRequest rr;
  // `begin` one millisecond early: whether OKX's bound is inclusive is not documented, and the
  // rows at the watermark's own millisecond are skipped by id.
  OkxOrderEncoder::encode_rest_fills(
      exec_history_, exec_window_start_ - 1, exec_window_end_, after, kPageLimit, rr);
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued = rest_->request(
      "GET", rr.path, rest_headers(rr), {}, [this, alive, gen](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        ++stats_.rest_requests;
        std::vector<FillRecord> rows;
        std::string err;
        if (!r.ok()) {
          err = fmt::format("status={} err={}", r.status, net::to_string(r.error));
        } else {
          err = decode_fills(r.body, rows);
        }
        if (!err.empty()) {
          ++stats_.rest_errors;
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR(
              "{}: GET {} failed ({}); this reconciliation cannot book the fills the private "
              "stream missed",
              cfg_.name,
              exec_history_ ? "fills-history" : "fills",
              err);
          int code = -1;
          std::string msg;
          if (decode_envelope(r.body, code, msg)) {
            const VenueAction a = map_error(code, msg).action;
            if (a != VenueAction::Reconcile) apply_action(a, code, msg);
          }
          finish_execution_replay(false);
          return;
        }
        for (FillRecord& f : rows) {
          if (exec_window_low_ms_ == 0 || f.ts_ms < exec_window_low_ms_)
            exec_window_low_ms_ = f.ts_ms;
        }
        const std::size_t n = rows.size();
        const std::string last_bill = n > 0 ? rows.back().bill_id : std::string();
        for (FillRecord& f : rows) exec_rows_.push_back(std::move(f));
        if (n < kPageLimit) {
          on_executions_window_done();
          return;
        }
        if (++exec_window_pages_ < kMaxExecPagesPerWindow) {
          request_executions_page(last_bill);
          return;
        }
        // Too many rows for one window: what was read is its newest part. Read the window again
        // up to its oldest row seen, so that it can still be emitted oldest first.
        if (exec_window_low_ms_ <= exec_window_start_) {
          finish_execution_replay(false);
          return;
        }
        exec_window_end_ = exec_window_low_ms_ + 1;
        exec_rows_.clear();
        exec_window_pages_ = 0;
        exec_window_low_ms_ = 0;
        request_executions_page({});
      });
  if (!queued) {
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: no room to ask for the account's fills", cfg_.name);
    finish_execution_replay(false);
  }
}

void OkxVenue::on_executions_window_done() {
  const bool narrowed = exec_window_end_ > 0;
  emit_executions();
  if (narrowed) {
    start_execution_window();
    return;
  }
  finish_execution_replay(true);
}

void OkxVenue::emit_executions() {
  // Received newest first; reversed, rows with equal times keep the venue's order.
  std::reverse(exec_rows_.begin(), exec_rows_.end());
  std::stable_sort(exec_rows_.begin(),
                   exec_rows_.end(),
                   [](const FillRecord& a, const FillRecord& b) { return a.ts_ms < b.ts_ms; });
  std::size_t count = 0;
  std::int64_t newest = 0;
  for (const FillRecord& f : exec_rows_) {
    newest = std::max(newest, f.ts_ms);
    const InstrumentId inst = subscribed_instrument(f.inst_id);
    if (!inst.valid()) continue;  // another instrument on this account: not ours
    // Below the watermark: before the `begin` asked for, so read by an earlier replay.
    if (f.ts_ms < exec_since_ms_) continue;
    if (f.ts_ms == exec_since_ms_ && exec_edge_ids_.count(f.trade_id) != 0) continue;
    if (known_exec_ids_.count(f.trade_id) != 0) continue;  // the earlier session booked it
    const Notional fee = Notional{} - f.fee;  // OKX: negative charged; FastMM: positive paid
    ClientOrderId cl{};
    if (const auto id = decode_cl_ord_id(f.cl_ord_id)) cl = current_id(*id);
    emit_replayed_fill(*order_sink_,
                       id_,
                       inst,
                       cl,
                       f.ord_id,
                       f.trade_id,
                       f.side == "sell" ? Side::Sell : Side::Buy,
                       f.px,
                       f.sz,
                       fee,
                       fee_asset_of(instruments_->get(inst), fee, f.fee_ccy),
                       f.exec_type == "M" ? Liquidity::Maker : Liquidity::Taker,
                       f.fill_time_ms);
    ++stats_.order_events;
    ++stats_.executions_fetched;
    ++count;
  }
  // The watermark moves to the newest row read; with none, to kSettleMs before the query, so a
  // quiet account keeps asking the recent endpoint.
  std::int64_t since = exec_since_ms_;
  if (newest > 0) {
    since = std::max(since, newest);
  } else if (exec_window_end_ == 0) {
    since = std::max(since, venue_time_ms() - kSettleMs);
  }
  if (since != exec_since_ms_) exec_edge_ids_.clear();
  exec_since_ms_ = since;
  for (const FillRecord& f : exec_rows_) {
    if (f.ts_ms == since) exec_edge_ids_.insert(f.trade_id);
  }
  exec_rows_.clear();
  if (count > 0) FASTMM_LOG_INFO("{}: replayed {} fill(s)", cfg_.name, count);
}

void OkxVenue::finish_execution_replay(bool ok) {
  if (!ok) exec_replay_ok_ = false;
  exec_replay_active_ = false;
  exec_rows_.clear();
  if (!exec_replay_ok_) {
    exec_retry_wanted_ = true;
    exec_retry_ns_ = now_ns();  // the retry comes kExecutionRetryNs after the failure
  }
  if (!oo_wanted_) return;
  oo_wanted_ = false;
  exec_snapshot_exact_ = exec_replay_ok_;
  send_open_orders();
}

// ---- funding ----------------------------------------------------------------------------------
//
// GET /api/v5/account/bills?instType=SWAP&type=8 (funding fee; bills-archive beyond 7 days), from
// its own watermark, with every execution replay, a second after a balance_and_position push with
// eventType funding_fee, and again from the housekeeping timer after a failure. Each bill on a
// subscribed instrument is one FundingMsg: balChg (positive received) in ccy, billId as its id.

void OkxVenue::request_funding(const std::string& after) {
  if (cfg_.dry_run || !connected_ || !signer_.usable()) return;
  if (rest_ == nullptr || rest_hard_stopped_ || subscribed_.empty()) return;
  const std::int64_t now = venue_time_ms();
  if (after.empty()) {
    if (funding_active_) return;
    funding_active_ = true;
    funding_rows_.clear();
    funding_pages_ = 0;
    if (funding_since_ms_ <= 0) funding_since_ms_ = exec_since_ms_ > 0 ? exec_since_ms_ : now;
    if (funding_since_ms_ < now - kHistoryMs) {
      FASTMM_LOG_ERROR(
          "{}: funding before {} is beyond OKX's 3 months", cfg_.name, now - kHistoryMs);
      funding_since_ms_ = now - kHistoryMs;
      funding_edge_ids_.clear();
    }
    funding_archive_ = funding_since_ms_ < now - kBillsRecentMs;
  }
  RestRequest rr;
  OkxOrderEncoder::encode_rest_funding_bills(
      funding_archive_, funding_since_ms_ - 1, after, kPageLimit, rr);
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued = rest_->request(
      "GET", rr.path, rest_headers(rr), {}, [this, alive, gen](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        ++stats_.rest_requests;
        std::vector<BillRecord> rows;
        std::string err;
        if (!r.ok()) {
          err = fmt::format("status={} err={}", r.status, net::to_string(r.error));
        } else {
          err = decode_bills(r.body, rows);
        }
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_ERROR("{}: GET {} failed ({}); funding is asked again",
                           cfg_.name,
                           funding_archive_ ? "bills-archive" : "bills",
                           err);
          funding_active_ = false;
          funding_retry_wanted_ = true;
          funding_retry_ns_ = now_ns();
          return;
        }
        const std::size_t n = rows.size();
        const std::string last_bill = n > 0 ? rows.back().bill_id : std::string();
        for (BillRecord& b : rows) funding_rows_.push_back(std::move(b));
        if (n >= kPageLimit) {
          if (++funding_pages_ < kMaxFundingPages) {
            request_funding(last_bill);
            return;
          }
          FASTMM_LOG_ERROR("{}: more than {} pages of funding bills; asked again later",
                           cfg_.name,
                           kMaxFundingPages);
          funding_active_ = false;
          funding_retry_wanted_ = true;
          funding_retry_ns_ = now_ns();
          funding_rows_.clear();
          return;
        }
        funding_active_ = false;
        emit_funding_rows();
      });
  if (!queued) {
    funding_active_ = false;
    funding_retry_wanted_ = true;
    funding_retry_ns_ = now_ns();
    FASTMM_LOG_ERROR("{}: no room to ask for the account's funding", cfg_.name);
  }
}

void OkxVenue::emit_funding_rows() {
  std::stable_sort(funding_rows_.begin(),
                   funding_rows_.end(),
                   [](const BillRecord& a, const BillRecord& b) { return a.ts_ms < b.ts_ms; });
  std::size_t count = 0;
  std::int64_t newest = 0;
  for (const BillRecord& b : funding_rows_) {
    newest = std::max(newest, b.ts_ms);
    if (b.type != "8") continue;
    const InstrumentId inst = subscribed_instrument(b.inst_id);
    if (!inst.valid()) continue;                // not traded here
    if (b.ts_ms < funding_since_ms_) continue;  // before the `begin` asked for
    if (b.ts_ms == funding_since_ms_ && funding_edge_ids_.count(b.bill_id) != 0) continue;
    if (known_exec_ids_.count(std::string(kFundingIdPrefix) + b.bill_id) != 0) continue;
    emit_funding(*order_sink_,
                 id_,
                 inst,
                 b.bill_id,
                 b.bal_chg,
                 b.ccy.empty() ? instruments_->get(inst).settlement_ccy() : std::string_view(b.ccy),
                 b.ts_ms,
                 /*replayed=*/true);
    ++stats_.order_events;
    ++stats_.funding_fetched;
    ++count;
  }
  std::int64_t since = funding_since_ms_;
  since = newest > 0 ? std::max(since, newest) : std::max(since, venue_time_ms() - kSettleMs);
  if (since != funding_since_ms_) funding_edge_ids_.clear();
  funding_since_ms_ = since;
  for (const BillRecord& b : funding_rows_) {
    if (b.ts_ms == since) funding_edge_ids_.insert(b.bill_id);
  }
  funding_rows_.clear();
  if (count > 0) FASTMM_LOG_INFO("{}: booked {} funding payment(s)", cfg_.name, count);
}

// ---- control requests -----------------------------------------------------------------------

void OkxVenue::request_server_time() {
  if (rest_ == nullptr || time_request_pending_ || rest_hard_stopped_) return;
  time_request_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const std::string headers = cfg_.simulated ? "x-simulated-trading: 1\r\n" : "";
  const bool queued = rest_->request(
      "GET", "/api/v5/public/time", headers, {}, [this, alive, gen](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        time_request_pending_ = false;
        ++stats_.rest_requests;
        if (!r.ok()) {
          ++stats_.rest_errors;
          return;
        }
        std::int64_t server_ms = 0;
        if (!decode_server_time(r.body, server_ms).empty()) return;
        const std::int64_t offset = server_ms - wall_now().ns / kNsPerMs;
        clock_offset_ms_.store(offset);
        clock_sync_ns_ = now_ns();
        clock_resync_wanted_ = false;
        stats_.clock_offset_ms = offset;
        if (offset > 1000 || offset < -1000)
          FASTMM_LOG_WARN(
              "{}: clock offset to venue is {} ms (OKX allows 30 s)", cfg_.name, offset);
      });
  if (!queued) time_request_pending_ = false;
}

// POST /api/v5/trade/cancel-all-after: every pending order of the account is cancelled unless the
// countdown is sent again within timeOut seconds; "0" stops it. The venue answers triggerTime.
void OkxVenue::send_cancel_all_after(int timeout_s) {
  if (rest_ == nullptr || !signer_.usable()) return;
  RestRequest rr;
  if (!OkxOrderEncoder::encode_rest_cancel_all_after(timeout_s, rr)) return;
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "POST",
      rr.path,
      rest_headers(rr),
      rr.body,
      [this, alive, timeout_s](const net::HttpResponse& r) {
        if (alive.expired() || r.error == net::NetError::Canceled) return;
        ++stats_.rest_requests;
        int code = -1;
        std::string msg;
        if (r.ok() && decode_envelope(r.body, code, msg) && code == 0) {
          // The countdown runs from here, not from when the request went out.
          if (timeout_s > 0) {
            if (!dms_.ever_armed())
              FASTMM_LOG_INFO("{}: cancel-all-after armed, {} s", cfg_.name, timeout_s);
            dms_.armed(now_ns());
          }
          return;
        }
        ++stats_.rest_errors;
        // Not fatal by itself: the switch is a backstop. If it was up and lapses, on_timer()
        // kills the venue.
        FASTMM_LOG_ERROR("{}: cancel-all-after ({} s) failed: status={} code={} {}",
                         cfg_.name,
                         timeout_s,
                         r.status,
                         code,
                         msg.empty() ? r.body.substr(0, 160) : msg);
        if (code == 50011) apply_action(VenueAction::RateLimit, code, msg);
      });
  if (queued && timeout_s > 0) dms_.attempted(now_ns());
}

void OkxVenue::stop_cancel_all_after() {
  RestRequest rr;
  if (!OkxOrderEncoder::encode_rest_cancel_all_after(0, rr)) return;
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    const HttpReply reply = http.request("POST", rr.path, rest_headers(rr), rr.body);
    int code = -1;
    std::string msg;
    if (reply.ok() && decode_envelope(reply.body, code, msg) && code == 0) {
      FASTMM_LOG_INFO("{}: cancel-all-after stopped", cfg_.name);
      return;
    }
    FASTMM_LOG_ERROR(
        "{}: cancel-all-after could not be stopped ({}); it cancels the account's "
        "pending orders when it runs out",
        cfg_.name,
        reply.ok() ? fmt::format("code {} {}", code, msg) : reply_error(reply));
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR(
        "{}: cancel-all-after could not be stopped: {}", cfg_.name, std::string_view(e.what()));
  }
}

void OkxVenue::cancel_all_async() {
  if (rest_ == nullptr || !signer_.usable()) return;
  cancel_pending_async({}, 0);
}

// No rest_hard_stopped_ check: cancelling is what a hard stop asks for.
void OkxVenue::cancel_pending_async(const std::string& after, std::size_t pages) {
  RestRequest rr;
  OkxOrderEncoder::encode_rest_orders_pending(after, rr);
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  static_cast<void>(rest_->request(
      "GET", rr.path, rest_headers(rr), {}, [this, alive, gen, pages](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        ++stats_.rest_requests;
        std::vector<PendingOrder> rows;
        const std::string err =
            r.ok() ? decode_pending_orders(r.body, rows) : fmt::format("status={}", r.status);
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_ERROR("{}: cancel-all: orders-pending failed ({})", cfg_.name, err);
          return;
        }
        std::vector<OkxOrderEncoder::CancelEntry> batch;
        auto flush = [&] {
          RestRequest cr;
          if (batch.empty() || !OkxOrderEncoder::encode_rest_cancel_batch(batch, cr)) return;
          batch.clear();
          static_cast<void>(rest_->request(
              "POST",
              cr.path,
              rest_headers(cr),
              cr.body,
              [this, alive](const net::HttpResponse& cr2) {
                if (alive.expired() || cr2.error == net::NetError::Canceled) return;
                ++stats_.rest_requests;
                std::vector<std::string> failed;
                const std::string e2 = cr2.ok() ? decode_cancel_batch(cr2.body, failed)
                                                : fmt::format("status={}", cr2.status);
                if (!e2.empty() || !failed.empty()) {
                  ++stats_.rest_errors;
                  FASTMM_LOG_ERROR("{}: cancel-all: cancel-batch-orders failed: {}{}",
                                   cfg_.name,
                                   e2,
                                   failed.empty() ? std::string() : failed.front());
                }
              }));
        };
        for (const PendingOrder& o : rows) {
          if (!subscribed_instrument(o.inst_id).valid()) continue;
          batch.push_back({o.inst_id, o.ord_id});
          if (batch.size() == kMaxBatch) flush();
        }
        flush();
        if (rows.size() >= kPageLimit && pages + 1 < kMaxReconcilePages)
          cancel_pending_async(rows.back().ord_id, pages + 1);
      }));
}

bool OkxVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr) return true;
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  bool all_ok = true;
  std::size_t cancelled = 0;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    std::string after;
    for (std::size_t page = 0; page < kMaxReconcilePages; ++page) {
      RestRequest rr;
      OkxOrderEncoder::encode_rest_orders_pending(after, rr);
      const HttpReply reply = http.request("GET", rr.path, rest_headers(rr));
      std::vector<PendingOrder> rows;
      const std::string err =
          reply.ok() ? decode_pending_orders(reply.body, rows) : reply_error(reply);
      if (!err.empty()) {
        FASTMM_LOG_ERROR("{}: kill-switch cancel-all: orders-pending failed: {}", cfg_.name, err);
        return false;
      }
      std::vector<OkxOrderEncoder::CancelEntry> batch;
      auto flush = [&] {
        RestRequest cr;
        if (batch.empty() || !OkxOrderEncoder::encode_rest_cancel_batch(batch, cr)) return;
        const std::size_t n = batch.size();
        batch.clear();
        const HttpReply c = http.request("POST", cr.path, rest_headers(cr), cr.body);
        std::vector<std::string> failed;
        const std::string e = c.ok() ? decode_cancel_batch(c.body, failed) : reply_error(c);
        if (!e.empty() || !failed.empty()) {
          all_ok = false;
          FASTMM_LOG_ERROR("{}: kill-switch cancel-batch-orders failed: {}{}",
                           cfg_.name,
                           e,
                           failed.empty() ? std::string() : failed.front());
        } else {
          cancelled += n;
        }
      };
      // Every pending order on a subscribed instrument, FastMM's or not, as the other connectors'
      // per-symbol cancel-all does.
      for (const PendingOrder& o : rows) {
        bool ours = false;
        for (InstrumentId id : subscribed_)
          ours = ours || iequals_symbol(symbols_->venue_symbol(id), o.inst_id);
        if (!ours) continue;
        batch.push_back({o.inst_id, o.ord_id});
        if (batch.size() == kMaxBatch) flush();
      }
      flush();
      if (rows.size() < kPageLimit) break;
      after = rows.back().ord_id;
    }
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR(
        "{}: kill-switch cancel-all failed: {}", cfg_.name, std::string_view(e.what()));
    return false;
  }
  if (all_ok) FASTMM_LOG_INFO("{}: kill-switch cancel-all ok ({} orders)", cfg_.name, cancelled);
  return all_ok;
}

// ---- housekeeping -----------------------------------------------------------------------------

void OkxVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (now - last_ping_ns_ >= static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs) {
    last_ping_ns_ = now;
    // "If there's a network problem ... send the string 'ping' and expect a 'pong'".
    if (md_conn_.is_live()) static_cast<void>(md_conn_.send_text("ping"));
    if (private_conn_.is_live()) static_cast<void>(private_conn_.send_text("ping"));
    if (trade_conn_.is_live()) static_cast<void>(trade_conn_.send_text("ping"));
  }
  if (clock_resync_wanted_ || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  if (!cfg_.dry_run && signer_.usable()) {
    // A replay that could not be completed left fills unaccounted for, and the next reconnect may
    // be hours away. Ask again until the venue answers; the watermark moved only past what was
    // booked.
    if (exec_retry_wanted_ && !exec_replay_active_ && now - exec_retry_ns_ >= kExecutionRetryNs) {
      exec_retry_ns_ = now;
      exec_retry_wanted_ = false;
      static_cast<void>(request_executions());
    }
    // And while nothing is wrong: once a minute, which books a fill the private stream dropped
    // without disconnecting and keeps the replay within what the OMS can deduplicate.
    if (!exec_replay_active_ && !exec_retry_wanted_ && now - exec_last_ns_ >= kExecutionSweepNs)
      static_cast<void>(request_executions());
    if (funding_due_ns_ != 0 && now >= funding_due_ns_ && !funding_active_) {
      funding_due_ns_ = 0;
      request_funding({});
    }
    if (funding_retry_wanted_ && !funding_active_ && now - funding_retry_ns_ >= kExecutionRetryNs) {
      funding_retry_ns_ = now;
      funding_retry_wanted_ = false;
      request_funding({});
    }
    // Venue-side dead man's switch, refreshed from the housekeeping timer: the thread that would
    // stop if this process died. If the window ran out anyway the venue has cancelled every
    // order of the account and this process still runs: it must not put the quotes back.
    if (dms_.expired(now)) {
      FASTMM_LOG_ERROR(
          "{}: cancel-all-after not refreshed within {} ms; the venue has cancelled this "
          "account's orders",
          cfg_.name,
          dms_.window_ms());
      dms_.disarm();
      fatal_ = true;
      trip_venue_kill(KillReason::DeadMansSwitchLost);
    }
    if (dms_.due(now) && !fatal_) send_cancel_all_after(static_cast<int>(dms_.window_ms() / 1000));
    check_positions(now);
  }
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

void OkxVenue::note_fill(const OrderFillMsg& f) noexcept {
  if (f.hdr.instrument.value >= kMaxInstruments) return;
  PositionCheck& p = positions_[f.hdr.instrument.value];
  p.tracked = f.side == Side::Buy ? p.tracked + f.qty : p.tracked - f.qty;
  p.last_event_ns = now_ns();
}

void OkxVenue::check_positions(std::int64_t now) {
  if (order_sink_ == nullptr || reconcile_in_flight_ || exec_replay_active_) return;
  for (InstrumentId id : subscribed_) {
    PositionCheck& p = positions_[id.value];
    if (!p.pending || now - p.last_event_ns < kPositionSettleNs) continue;
    p.pending = false;
    if (p.venue == p.tracked) continue;
    FASTMM_LOG_WARN(
        "{}: {} position {} from the positions channel differs from the fills ({}); correcting "
        "the engine",
        cfg_.name,
        symbols_->venue_symbol(id),
        p.venue,
        p.tracked);
    PositionUpdateMsg m{};
    init_header(m, EventType::PositionUpdate, id, id_);
    m.qty = p.venue;
    m.avg_px = p.venue_avg;
    m.hdr.recv_ts = wall_now();
    m.hdr.t0_cycles = rdtscp();
    static_cast<void>(order_sink_->push(m.hdr));
    ++stats_.order_events;
    p.tracked = p.venue;
  }
}

void OkxVenue::publish_status() noexcept {
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_.load(std::memory_order_relaxed);
  wire_.summarize(
      tsc_calibration(), stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
}

VenueStatus OkxVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

OkxVenueConfig make_okx_config(const VenueSection& v, bool dry_run) {
  OkxVenueConfig c;
  c.name = v.name;
  c.ws_public_url = v.ws_url;
  c.ws_trade_url = v.ws_api_url;
  c.rest_url = v.rest_url;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.supports_replace = v.supports_replace;
  c.dry_run = dry_run;
  c.simulated = v.testnet;
  c.credentials.api_key = v.api_key;
  c.credentials.secret.value = v.api_secret;
  c.credentials.passphrase.value = v.api_passphrase;
  const VenueExtras x(v.extra);
  auto bad = [&](const char* key, std::string_view why) {
    return std::invalid_argument(
        fmt::format("venues.{}.{}: {} (\"{}\")", v.name, key, why, x.get(key)));
  };
  c.ws_private_url = x.get("ws_private_url");
  if (c.ws_private_url.empty()) c.ws_private_url = with_path(c.ws_public_url, "/ws/v5/private");
  if (c.ws_trade_url.empty()) c.ws_trade_url = c.ws_private_url;
  if (const std::string m = x.get("td_mode"); m == "isolated") {
    c.td_mode = TdMode::Isolated;
  } else if (!m.empty() && m != "cross") {
    throw bad("td_mode", "expected \"cross\" or \"isolated\"");
  }
  if (const std::string d = x.get("depth_channel"); d == "books50-l2-tbt") {
    c.depth_channel = OkxDepthChannel::Books50L2Tbt;
  } else if (d == "books-l2-tbt") {
    c.depth_channel = OkxDepthChannel::BooksL2Tbt;
  } else if (!d.empty() && d != "books") {
    throw bad("depth_channel", "expected \"books\", \"books50-l2-tbt\" or \"books-l2-tbt\"");
  }
  if (const std::string o = x.get("order_api"); o == "rest") {
    c.ws_order_api = false;
  } else if (!o.empty() && o != "ws") {
    throw bad("order_api", "expected \"ws\" or \"rest\"");
  }
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("dead_ms", c.dead_ms)));
  // OKX closes a connection after 30 s without data: the ping has to come sooner.
  c.ping_interval_ms = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(x.integer("ping_interval_ms", c.ping_interval_ms), 1000, 25'000));
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("orders_per_second", c.orders_per_second)));
  const std::int64_t dms = x.integer("dead_mans_switch_s", c.dead_mans_switch_s);
  c.dead_mans_switch_s =
      dms <= 0
          ? 0
          : static_cast<int>(std::clamp<std::int64_t>(dms, kMinCancelAfterS, kMaxCancelAfterS));
  c.position_from_stream = x.flag("position_from_stream", true);
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.emit_ack_from_response = x.flag("emit_ack_from_response", true);
  // Three credentials: a key without its passphrase logs in nowhere.
  if (!dry_run && !c.credentials.api_key.empty() && c.credentials.passphrase.value.empty())
    throw std::invalid_argument(fmt::format(
        "venues.{}.api_passphrase: required with api_key (the passphrase set when the OKX API key "
        "was created, written as \"${{VARIABLE}}\")",
        v.name));
  if (c.depth_channel != OkxDepthChannel::Books && !c.credentials.usable()) {
    if (!dry_run)
      throw bad("depth_channel",
                "the tbt channels need a login: api_key, api_secret, api_passphrase");
    FASTMM_LOG_WARN("{}: {} needs a login; a dry run without keys subscribes books",
                    v.name,
                    to_string(c.depth_channel));
    c.depth_channel = OkxDepthChannel::Books;
  }
  // Demo keys work only against the demo hosts and the reverse (50101): refuse the mix here.
  if (const auto u = net::Url::parse(c.ws_public_url)) {
    const bool demo_host = u->host.find("wspap.") != std::string_view::npos;
    const bool live_host = u->host == "ws.okx.com" || u->host == "wsaws.okx.com";
    if (demo_host && !c.simulated)
      throw std::invalid_argument(fmt::format(
          "venues.{}.testnet: false with the demo host {}; demo trading needs testnet = true",
          v.name,
          u->host));
    if (live_host && c.simulated)
      throw std::invalid_argument(fmt::format(
          "venues.{}.testnet: true (the default) with the production host {}: set testnet = false "
          "for production, or use wss://wspap.okx.com:8443 for demo trading",
          v.name,
          u->host));
  }
  return c;
}

}  // namespace fastmm::venues::okx
