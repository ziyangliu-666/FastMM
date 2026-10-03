#include "fastmm/venues/coinbase/coinbase_venue.hpp"

#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fastmm::venues::coinbase {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kClockResyncNs = 30LL * 60 * 1'000'000'000;
constexpr std::int64_t kRateLimitCooldownNs = 1'000'000'000;
constexpr std::int64_t kDayMs = 24LL * 3600 * 1000;
// GET /orders: 1000 a page (the maximum); the venue allows 500 open orders.
constexpr std::size_t kMaxReconcilePages = 10;
constexpr std::size_t kOrdersPageRows = 1000;
// GET /fills: 100 rows a page, newest first.
constexpr int kFillsPageRows = 100;
constexpr std::size_t kMaxFillPagesPerWindow = 20;
constexpr std::size_t kMaxFillPages = 100;
// A window of the fill replay: the venue documents no bound on start_date..end_date; a day keeps
// a page count sane after a long outage.
constexpr std::int64_t kFillWindowMs = kDayMs;
// How late the venue may show a fill in its history.
constexpr std::int64_t kSettleMs = 60'000;

// "User-Agent header is required" (HTTP 400) on every REST request, public ones included.
constexpr std::string_view kUserAgent = "User-Agent: fastmm\r\n";

std::string reply_error(const HttpReply& r) {
  return r.error.empty() ? fmt::format("HTTP {} {}", r.status, r.body.substr(0, 200)) : r.error;
}

// The venue answers a rate limit with HTTP 429 (BlockingControl waits those out by itself).
BlockingRetry coinbase_blocking_retry() {
  return BlockingRetry{};
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

CoinbaseExchangeVenue::CoinbaseExchangeVenue(VenueId id, CoinbaseVenueConfig cfg)
    : id_(id), cfg_(std::move(cfg)), signer_(cfg_.credentials), rate_(cfg_.rate_threshold) {
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  std::memset(scratch_, 0, sizeof scratch_);
  ReplayLimits limits;
  limits.window_ms = kFillWindowMs;
  limits.page_rows = kFillsPageRows;
  limits.newest_first = true;
  limits.window_pages = kMaxFillPagesPerWindow;
  limits.max_pages = kMaxFillPages;
  limits.settle_ms = kSettleMs;
  exec_replay_.setup(
      cfg_.name,
      "fill(s)",
      limits,
      {[this] { return replay_ready(); },
       [this] { return venue_time_ms(); },
       [this](const ReplayQuery& q) { return query_fills(q); },
       [this](bool complete) { reconcile_.replay_done(complete); },
       [this](const ReplayLookup& l) { return lookup_order(l); }},
      [this](std::size_t stream, const FillRow& f) { return emit_fill(stream, f); },
      // A fill names its order by the venue's id only: one this connector cannot name is asked for.
      [this](std::size_t, const FillRow& f) {
        return !f.order_id.empty() && !name_of(f.order_id).valid() ? f.order_id : std::string{};
      });
}

CoinbaseExchangeVenue::~CoinbaseExchangeVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps CoinbaseExchangeVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = false;
  c.supports_post_only = true;
  c.ws_order_entry = false;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

std::int64_t CoinbaseExchangeVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_.load(std::memory_order_relaxed);
}

std::string CoinbaseExchangeVenue::rest_headers(std::string_view method,
                                                std::string_view target,
                                                std::string_view body) const {
  return std::string(kUserAgent) + signer_.rest_headers(venue_time_ms(), method, target, body);
}

bool CoinbaseExchangeVenue::md_needs_auth() const noexcept {
  return cfg_.depth_channel == DepthChannel::Level2 && signer_.usable();
}

std::string CoinbaseExchangeVenue::signed_payload(std::string_view payload) const {
  const EpochSeconds ts = epoch_seconds(venue_time_ms(), false);
  const Base64Sha256 sig = signer_.sign_ws(ts.view());
  std::string p(payload);
  if (!p.empty() && p.back() == '}') p.pop_back();
  p += R"(,"signature":")";
  p += sig.view();
  p += R"(","key":")";
  p += signer_.api_key();
  p += R"(","passphrase":")";
  p += signer_.passphrase();
  p += R"(","timestamp":")";
  p += ts.view();
  p += "\"}";
  return p;
}

net::ConnectionConfig CoinbaseExchangeVenue::ws_config(const std::string& url,
                                                       bool manual_subscribe,
                                                       std::uint32_t dead_ms) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  c.dead_ms = dead_ms;
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = 0;
  c.manual_subscribe = manual_subscribe;
  c.extra_headers = std::string(kUserAgent);
  // A snapshot of the whole book: 1.3 MB for BTC-USD on 2026-09-30.
  c.ws.recv_capacity = std::size_t{16} << 20;
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> CoinbaseExchangeVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  BlockingHttpOptions opts = blocking_options(cfg_);
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    {
      const HttpReply t = http.get("/time", kUserAgent);
      std::int64_t server_ms = 0;
      if (t.ok() && decode_server_time(t.body, server_ms).empty()) {
        clock_offset_ms_.store(server_ms - wall_now().ns / kNsPerMs);
        clock_sync_ns_ = now_ns();
      }
    }
    for (Instrument* inst : mine) {
      const HttpReply reply =
          http.get(fmt::format("/products/{}", inst->symbol.view()), kUserAgent);
      if (!reply.ok()) {
        if (!cfg_.allow_offline_reference_data)
          return fail(fmt::format("{}: GET /products/{} failed: {}",
                                  cfg_.name,
                                  inst->symbol.view(),
                                  reply_error(reply)));
        FASTMM_LOG_WARN("{}: GET /products/{} failed ({}); keeping configured tick/lot",
                        cfg_.name,
                        inst->symbol.view(),
                        reply_error(reply));
        continue;
      }
      ProductInfo p;
      if (std::string err = decode_product(reply.body, p); !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
      if (inst->tick != p.tick || inst->lot != p.lot)
        FASTMM_LOG_WARN("{}: {} tick/lot from /products override config ({} / {} -> {} / {})",
                        cfg_.name,
                        inst->symbol.view(),
                        inst->tick,
                        inst->lot,
                        p.tick,
                        p.lot);
      inst->tick = p.tick;
      inst->lot = p.lot;
      // base_min_size is no longer sent: the smallest order is one base_increment, and
      // min_market_funds is the smallest notional.
      inst->min_qty = p.min_size.is_positive() ? p.min_size : p.lot;
      inst->min_notional = p.min_funds;
      inst->contract_multiplier = Qty::from_int(1);
      inst->expiry_ns = 0;
      if (inst->asset_class != AssetClass::Spot) {
        FASTMM_LOG_WARN(
            "{}: {} is a spot product; asset_class set to spot", cfg_.name, inst->symbol.view());
        inst->asset_class = AssetClass::Spot;
      }
      if (!p.base.empty() && !p.quote.empty() &&
          (!inst->base.assign(p.base) || !inst->quote.assign(p.quote)))
        return fail(fmt::format("{}: {} currency names too long", cfg_.name, inst->symbol.view()));
      if (p.post_only || p.limit_only)
        FASTMM_LOG_WARN("{}: {} is in {} mode",
                        cfg_.name,
                        inst->symbol.view(),
                        p.post_only ? "post-only" : "limit-only");
      if (p.status != "online" || p.trading_disabled || p.cancel_only) {
        FASTMM_LOG_ERROR("{}: {} status {}{}{}: disabled",
                         cfg_.name,
                         inst->symbol.view(),
                         p.status,
                         p.trading_disabled ? ", trading disabled" : "",
                         p.cancel_only ? ", cancel only" : "");
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
  const std::int64_t off = clock_offset_ms_.load();
  stats_.clock_offset_ms = off;
  publish_status();
  if (off > 1000 || off < -1000)
    FASTMM_LOG_WARN("{}: clock offset to venue is {} ms", cfg_.name, off);
  FASTMM_LOG_INFO("{}: reference data loaded for {} products", cfg_.name, mine.size());
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void CoinbaseExchangeVenue::attach(const SymbolTable& symbols,
                                   const InstrumentTable& instruments,
                                   EventSink& md_sink,
                                   EventSink& order_sink,
                                   MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ = std::make_unique<CoinbaseMdFeed>(
      symbols,
      id_,
      md_sink,
      ResubscribeRequester{&CoinbaseExchangeVenue::resubscribe_requester, this},
      cfg_.depth_channel);
  md_feed_->set_log_name(cfg_.name);
  private_parser_ = std::make_unique<CoinbasePrivateParser>(symbols, instruments, id_);
  encoder_ = std::make_unique<CoinbaseOrderEncoder>(symbols, cfg_.stp);
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments);
}

void CoinbaseExchangeVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id);
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  exec_replay_.set_streams(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (connected_ && md_conn_.opened()) {
    md_conn_.close();
    open_md();
  }
  if (connected_ && user_conn_.opened()) {
    user_conn_.close();
    open_user();
  }
}

InstrumentId CoinbaseExchangeVenue::subscribed_instrument(std::string_view product) const noexcept {
  const InstrumentId id = symbols_->find(id_, product);
  if (!id.valid()) return id;
  if (std::find(subscribed_.begin(), subscribed_.end(), id) == subscribed_.end())
    return InstrumentId::invalid();
  return id;
}

void CoinbaseExchangeVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("CoinbaseExchangeVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  const bool private_on = !cfg_.dry_run && signer_.usable();
  reconcile_.open(private_on);
  exec_replay_.start_at(venue_time_ms());
  exec_replay_.open(private_on);
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    if (!cfg_.dry_run) raw_user_.open(cfg_.record_raw_dir, cfg_.name, "user");
  }
  open_rest();
  request_server_time();
  open_md();
  if (private_on) open_user();
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, user={}, sandbox={}, depth={})",
                  cfg_.name,
                  cfg_.dry_run,
                  private_on,
                  cfg_.sandbox,
                  to_string(cfg_.depth_channel));
}

void CoinbaseExchangeVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  reconcile_.close();  // before the reset below: nothing it aborts asks again
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  md_conn_.close();
  user_conn_.close();
  ++generation_;
  exec_replay_.close();
  time_request_pending_ = false;
  if (rest_) rest_->reset();
  md_feed_->on_disconnected();
  raw_md_.flush();
  raw_user_.flush();
  publish_status();
}

void CoinbaseExchangeVenue::open_rest() {
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
}

void CoinbaseExchangeVenue::open_md() {
  md_conn_.open(*reactor_, ws_config(cfg_.ws_url, false, cfg_.dead_ms), md_handler_);
  md_conn_.connect();
}

void CoinbaseExchangeVenue::open_user() {
  // Live once the venue has acknowledged the user channel (subscribe_done()).
  user_conn_.open(
      *reactor_,
      ws_config(cfg_.ws_private_url, true, std::max<std::uint32_t>(cfg_.dead_ms, 10'000)),
      user_handler_);
  user_conn_.connect();
}

// ---- market data ------------------------------------------------------------------------------

void CoinbaseExchangeVenue::on_md_state(net::ConnState s) {
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
      // A book still waiting for its snapshot (the whole book: seconds of transfer) keeps
      // waiting; a synced one may have missed updates and starts over.
      for (InstrumentId id : subscribed_) {
        CoinbaseBookSync* sync = md_feed_->sync(id);
        if (sync != nullptr && sync->synced()) sync->resync(SyncReason::Explicit, now_ns());
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

void CoinbaseExchangeVenue::on_md_open() {
  for (const std::string& p : md_feed_->subscription_payloads()) {
    const bool ok = md_needs_auth() ? md_conn_.send_text(signed_payload(p)) : md_conn_.send_text(p);
    if (!ok) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void CoinbaseExchangeVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (CoinbaseBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void CoinbaseExchangeVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Ok) return;
  const MdDecodeResult& r = md_feed_->last();
  if (r.control == ControlOp::Error) {
    FASTMM_LOG_ERROR("{}: market-data request refused: {} ({})", cfg_.name, r.msg, r.reason);
    if (md_needs_auth()) {
      const VenueAction a = map_error(401, r.reason.empty() ? r.msg : r.reason).action;
      if (a == VenueAction::ResyncClock) apply_action(a, 401, r.msg);
    }
    return;
  }
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  }
}

void CoinbaseExchangeVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;  // the reconnect subscribes everything again
  for (const std::string& p : md_feed_->resubscribe_payloads(id)) {
    static_cast<void>(md_needs_auth() ? md_conn_.send_text(signed_payload(p))
                                      : md_conn_.send_text(p));
  }
  FASTMM_LOG_INFO(
      "{}: resubscribing {} for a fresh snapshot", cfg_.name, symbols_->venue_symbol(id));
}

// ---- user channel ---------------------------------------------------------------------------

void CoinbaseExchangeVenue::on_user_open() {
  std::string p = R"({"type":"subscribe","product_ids":[)";
  bool first = true;
  for (InstrumentId id : subscribed_) {
    if (!first) p += ',';
    first = false;
    p += '"';
    p += symbols_->venue_symbol(id);
    p += '"';
  }
  // The heartbeat keeps a quiet user connection talking: dead_ms catches a dead one.
  p += R"(],"channels":["user","heartbeat"]})";
  if (!user_conn_.send_text(signed_payload(p)))
    FASTMM_LOG_ERROR("{}: could not subscribe the user channel", cfg_.name);
  balance_subscribed_.clear();  // a new connection: the balance channel follows once it is Live
}

void CoinbaseExchangeVenue::on_user_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.user = private_channel_state(s);
  stats_.order = stats_.user;
  if (mapped == user_state_) return;
  const ConnState prev = user_state_;
  user_state_ = mapped;
  if (mapped == ConnState::Live) {
    if (prev != ConnState::Stale) {
      emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
      FASTMM_LOG_INFO("{}: user channel -> Live", cfg_.name);
    }
    // The first time: sweep for orders a session that died left resting. After a reconnect:
    // reconcile (the replay first books what the channel missed). Not on a return from Stale.
    if (!user_was_live_) {
      reconcile_.sweep();
    } else if (prev != ConnState::Stale) {
      reconcile_.request();
    }
    user_was_live_ = true;
    subscribe_balances();
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: user channel lost", cfg_.name);
    // Without the channel no fill or cancel reaches the engine. disconnect() clears connected_
    // first: a requested shutdown runs the synchronous cancel_all() instead.
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void CoinbaseExchangeVenue::on_user_text(std::string_view t, std::int64_t ts) {
  if (raw_user_.enabled()) raw_user_.record(ts, t);
  const Cycles t0 = rdtscp();
  const PrivateDecodeResult r = private_parser_->decode(t, wall_now(), t0, scratch_);
  if (r.status == ParseStatus::Ok) {
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      switch (h->type) {
        case EventType::OrderAck: {
          const auto* m = reinterpret_cast<const OrderAckMsg*>(h);
          remember_order(m->venue_order_id.view(), m->cl_ord_id);
          break;
        }
        case EventType::OrderCancelAck:
          forget_order(reinterpret_cast<const OrderCancelAckMsg*>(h)->cl_ord_id);
          break;
        case EventType::OrderExpired:
          forget_order(reinterpret_cast<const OrderExpiredMsg*>(h)->cl_ord_id);
          break;
        case EventType::OrderFill: {
          const auto* m = reinterpret_cast<const OrderFillMsg*>(h);
          if (m->leaves_qty.raw <= 0) forget_order(m->cl_ord_id);
          balances_after_fill_ = true;
          break;
        }
        case EventType::Balance: {
          // In the engine's spelling, and only an asset it keeps.
          auto* m = reinterpret_cast<BalanceMsg*>(h);
          const std::string_view name = reconcile_.assets().find(m->asset.view());
          if (name.empty()) continue;
          m->asset.assign(name);
          static_cast<void>(order_sink_->push(*h));
          continue;
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
  switch (r.control) {
    case PrivateControl::Subscriptions:
      // The acknowledgement lists the channels now subscribed; the user channel is the one the
      // session cannot run without.
      if (t.find(R"("name":"user")") != std::string_view::npos) user_conn_.subscribe_done();
      return;
    case PrivateControl::Error: {
      FASTMM_LOG_ERROR("{}: user channel refused: {} ({})", cfg_.name, r.msg, r.reason);
      if (user_state_ != ConnState::Live) {
        // An authentication failure: a clock off by more than 30 s, or keys the venue refuses.
        const std::string_view why = r.reason.empty() ? r.msg : r.reason;
        const VenueAction a = map_error(401, why).action;
        apply_action(a, 401, why);
      }
      return;
    }
    default:
      break;
  }
  if (r.status == ParseStatus::Malformed) FASTMM_LOG_WARN("{}: malformed user frame", cfg_.name);
}

void CoinbaseExchangeVenue::remember_order(std::string_view order_id, ClientOrderId cl) noexcept {
  const auto key = order_key(order_id);
  if (!key || !cl.valid()) return;
  if (order_names_.size() >= decltype(order_names_)::kMaxSize) order_names_.clear();
  static_cast<void>(order_names_.assign(*key, cl));
}

ClientOrderId CoinbaseExchangeVenue::name_of(std::string_view order_id) const noexcept {
  if (const TrackedOrder* o = private_parser_->find(order_id)) return o->cl;
  const auto key = order_key(order_id);
  if (!key) return ClientOrderId{};
  const ClientOrderId* cl = order_names_.find(*key);
  return cl != nullptr ? *cl : ClientOrderId{};
}

void CoinbaseExchangeVenue::forget_order(ClientOrderId id) noexcept {
  shadows_.erase(id);
}

// ---- outbound ---------------------------------------------------------------------------------

void CoinbaseExchangeVenue::on_wake() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void CoinbaseExchangeVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

template <class Ring>
void CoinbaseExchangeVenue::write_orders(Ring& ring) {
  // Each order is its own REST request: there is nothing to cork.
  drain_outbound_coalesced(
      ring,
      wire_,
      [] {},
      [this](const EventHeader& h) {
        if (const auto cmd = OrderCommand::from(h)) {
          sent_.note(*cmd, now_ns());
          send_command(*cmd);
        } else if (is_reconcile_request(h)) {
          request_open_orders();
        }
      },
      [] { return true; });
}

void CoinbaseExchangeVenue::refuse(const OrderCommand& cmd,
                                   RejectReason reason,
                                   std::string_view why) {
  if (cmd.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  } else {
    sent_.answered(cmd.cl_ord_id);  // the refusal is its answer: it holds no snapshot back
    emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  }
  ++stats_.order_events;
}

void CoinbaseExchangeVenue::send_command(const OrderCommand& cmd) {
  const std::int64_t now = now_ns();
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  if (cfg_.dry_run) return refuse(cmd, RejectReason::VenueKilled, "dry-run: orders disabled");
  // A venue-fatal error and a REST hard stop stop new orders, never cancels.
  if ((fatal_ || rest_hard_stopped_) && !is_cancel)
    return refuse(cmd, RejectReason::VenueKilled, "venue fatal");
  if (cmd.kind == OrderCommandKind::Replace)
    return refuse(cmd, RejectReason::VenueReject, "replace: not supported (cancel and new)");
  if (rest_ == nullptr) return refuse(cmd, RejectReason::VenueReject, "no order channel");
  OrderRequest rq;
  const Cycles before_encode = rdtscp();
  if (!is_cancel) {
    if (!rate_.can_send(1, now, true)) {
      ++stats_.rate_limit_cooldowns;
      return refuse(cmd, RejectReason::VenueRateLimit, "local rate limit");
    }
    if (!encoder_->encode_new(cmd, rq))
      return refuse(cmd, RejectReason::VenueReject, "order could not be encoded");
    if (shadows_.assign(cmd.cl_ord_id,
                        Shadow{cmd.instrument, cmd.side, cmd.qty, sent_.last_seq()}) == nullptr) {
      shadow_overflow_.refused(cfg_.name, shadows_.size());
      return refuse(cmd, RejectReason::OrderTableFull, "order table full");
    }
  } else if (!encoder_->encode_cancel(cmd, rq)) {
    return refuse(cmd, RejectReason::UnknownOrder, "cancel could not be encoded");
  }
  const std::string headers = rest_headers(rq.method, rq.target(), rq.payload());
  const Cycles after_encode = rdtscp();
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const ClientOrderId cl = cmd.cl_ord_id;
  const bool queued = rest_->request(rq.method,
                                     rq.target(),
                                     headers,
                                     rq.payload(),
                                     [this, alive, gen, cl, is_cancel](const net::HttpResponse& r) {
                                       if (alive.expired() || gen != generation_) return;
                                       if (is_cancel) {
                                         handle_cancel_reply(cl, r);
                                       } else {
                                         handle_new_reply(cl, r);
                                       }
                                     });
  if (!queued) {
    ++stats_.order_send_failures;
    if (!is_cancel) shadows_.erase(cl);
    return refuse(cmd, RejectReason::TransportFull, "rest queue full");
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  rate_.on_sent(1, now, !is_cancel);
  if (is_cancel) {
    ++stats_.cancels_sent;
  } else {
    // Its answer comes over REST: no WebSocket loss settles it.
    sent_.sent_over_rest(cl);
    ++stats_.orders_sent;
  }
}

void CoinbaseExchangeVenue::handle_new_reply(ClientOrderId id, const net::HttpResponse& r) {
  ++stats_.rest_requests;
  sent_.answered(id);
  const Shadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  if (r.error != net::NetError::None) {
    ++stats_.rest_errors;
    // The order may or may not be at the venue: reject it here and let the snapshot say.
    emit_order_reject(*order_sink_,
                      id_,
                      inst,
                      id,
                      RejectReason::VenueReject,
                      0,
                      fmt::format("POST /orders: {}", net::to_string(r.error)));
    ++stats_.order_events;
    forget_order(id);
    request_open_orders();
    return;
  }
  OrderRow o;
  if (r.ok() && decode_order(r.body, o).empty()) {
    if (o.status == "rejected") {
      emit_order_reject(*order_sink_,
                        id_,
                        inst,
                        id,
                        map_reject_reason(o.reject_reason),
                        r.status,
                        o.reject_reason.empty() ? std::string_view("rejected") : o.reject_reason);
      forget_order(id);
    } else {
      if (shadow != nullptr)
        static_cast<void>(
            private_parser_->learn(o.id, id, inst, shadow->side, shadow->qty, o.filled_size));
      remember_order(o.id, id);
      if (cfg_.emit_ack_from_response)
        emit_order_ack(*order_sink_, id_, inst, id, o.id, 0, Timestamp{o.created_ns});
    }
    ++stats_.order_events;
    return;
  }
  ++stats_.rest_errors;
  const std::string msg = error_message(r.body);
  const ErrorMapping m = map_error(r.status, msg);
  emit_order_reject(*order_sink_,
                    id_,
                    inst,
                    id,
                    m.reason,
                    r.status,
                    msg.empty() ? r.body.substr(0, 120) : std::string_view(msg));
  ++stats_.order_events;
  forget_order(id);
  apply_action(m.action, r.status, msg);
}

void CoinbaseExchangeVenue::handle_cancel_reply(ClientOrderId id, const net::HttpResponse& r) {
  ++stats_.rest_requests;
  const Shadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  if (r.ok()) {
    // Accepted: the user channel reports the cancel (done, with what filled). Without the channel
    // the reply is all there will be.
    if (!user_conn_.is_live()) {
      std::vector<std::string> ids;
      static_cast<void>(decode_ids(r.body, ids));
      emit_cancel_ack(
          *order_sink_, id_, inst, id, ids.empty() ? std::string_view{} : ids[0], Qty{});
      forget_order(id);
      ++stats_.order_events;
    }
    return;
  }
  ++stats_.rest_errors;
  const std::string msg =
      r.error != net::NetError::None ? std::string(net::to_string(r.error)) : error_message(r.body);
  const ErrorMapping m = map_error(r.status, msg);
  emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, r.status, msg);
  ++stats_.order_events;
  apply_action(m.action, r.status, msg);
}

void CoinbaseExchangeVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void CoinbaseExchangeVenue::apply_action(VenueAction action, int code, std::string_view msg) {
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

void CoinbaseExchangeVenue::request_open_orders() {
  reconcile_.request();
}

bool CoinbaseExchangeVenue::replay_executions() {
  return exec_replay_.run();
}

// Nothing reaches the engine before every page parsed: Oms::reconcile_end() cancels every order
// the snapshot does not name.
bool CoinbaseExchangeVenue::fetch_snapshot(std::uint64_t generation) {
  reconcile_pages_ = 0;
  snapshot_ids_.clear();
  return request_open_orders_page(generation, {});
}

bool CoinbaseExchangeVenue::request_open_orders_page(std::uint64_t generation,
                                                     const std::string& after) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  const std::string target = CoinbaseOrderEncoder::open_orders_path(after);
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET",
      target,
      rest_headers("GET", target, {}),
      {},
      [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.current(generation)) return;
        ++stats_.rest_requests;
        std::vector<OrderRow> rows;
        const std::string err = r.ok() ? decode_orders(r.body, rows)
                                       : fmt::format("status={} err={} {}",
                                                     r.status,
                                                     net::to_string(r.error),
                                                     error_message(r.body));
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET /orders failed ({})", cfg_.name, err);
          if (r.error == net::NetError::None) {
            const std::string msg = error_message(r.body);
            const VenueAction a = map_error(r.status, msg).action;
            if (a != VenueAction::Reconcile) apply_action(a, r.status, msg);
          }
          reconcile_.fetched(generation, false);
          return;
        }
        for (const OrderRow& o : rows) {
          const InstrumentId inst = subscribed_instrument(o.product_id);
          if (!inst.valid()) continue;
          ReconcileMsg& m = reconcile_.add_order(inst);
          const Side side = o.side == "sell" ? Side::Sell : Side::Buy;
          m.side = side;
          m.state = o.filled_size.is_positive() ? OrderState::PartiallyFilled : OrderState::Live;
          if (const auto cl = decode_client_oid(o.client_oid)) {
            m.cl_ord_id = *cl;
            snapshot_ids_.push_back(*cl);
            // Its later events name only the order id.
            static_cast<void>(private_parser_->learn(o.id, *cl, inst, side, o.size, o.filled_size));
            remember_order(o.id, *cl);
          }
          m.venue_order_id.assign(o.id);
          m.price = o.price;
          m.orig_qty = o.size;
          m.cum_qty = o.filled_size;
        }
        const std::string next(r.header("CB-AFTER"));
        if (rows.size() >= kOrdersPageRows && !next.empty()) {
          if (++reconcile_pages_ >= kMaxReconcilePages) {
            FASTMM_LOG_WARN("{}: more than {} pages of open orders", cfg_.name, kMaxReconcilePages);
            reconcile_.fetched(generation, false);
          } else if (!request_open_orders_page(generation, next)) {
            reconcile_.fetched(generation, false);
          }
          return;
        }
        reconcile_.fetched(generation, true);
        // The driver has swept the shadows the snapshot proved over: the user channel's table
        // keeps only orders still shadowed or still open.
        std::sort(snapshot_ids_.begin(), snapshot_ids_.end());
        const std::size_t dropped = private_parser_->sweep([this](ClientOrderId cl) {
          return shadows_.find(cl) != nullptr ||
                 std::binary_search(snapshot_ids_.begin(), snapshot_ids_.end(), cl);
        });
        if (dropped > 0)
          FASTMM_LOG_INFO(
              "{}: {} ended orders dropped from the user channel's table", cfg_.name, dropped);
      });
}

void CoinbaseExchangeVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const Shadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void CoinbaseExchangeVenue::drop_shadow(ClientOrderId id) {
  forget_order(id);
}

// ---- balances -----------------------------------------------------------------------------------

bool CoinbaseExchangeVenue::fetch_balances(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  const std::string target = "/accounts";
  std::weak_ptr<int> alive = alive_;
  return rest_->request("GET",
                        target,
                        rest_headers("GET", target, {}),
                        {},
                        [this, alive, generation](const net::HttpResponse& r) {
                          if (alive.expired() || !reconcile_.balances_current(generation)) return;
                          ++stats_.rest_requests;
                          std::vector<AccountRow> rows;
                          const std::string err = r.ok() ? decode_accounts(r.body, rows)
                                                         : fmt::format("status={} err={} {}",
                                                                       r.status,
                                                                       net::to_string(r.error),
                                                                       error_message(r.body));
                          if (!err.empty()) {
                            ++stats_.rest_errors;
                            FASTMM_LOG_WARN("{}: GET /accounts failed ({})", cfg_.name, err);
                            if (r.error == net::NetError::None) {
                              const std::string msg = error_message(r.body);
                              const VenueAction a = map_error(r.status, msg).action;
                              if (a != VenueAction::Reconcile) apply_action(a, r.status, msg);
                            }
                            reconcile_.balances_fetched(generation, false, 0);
                            return;
                          }
                          balance_accounts_.clear();
                          for (const AccountRow& a : rows) {
                            if (reconcile_.assets().tracks(a.currency))
                              balance_accounts_.push_back(a.id);
                            reconcile_.add_balance(a.currency,
                                                   BalanceFields::spot(a.available, a.hold));
                          }
                          // The reply carries no time: the venue's clock as it arrived.
                          reconcile_.balances_fetched(generation, true, venue_time_ms());
                          subscribe_balances();
                        });
}

void CoinbaseExchangeVenue::subscribe_balances() {
  if (!user_conn_.is_live() || balance_accounts_.empty() ||
      balance_accounts_ == balance_subscribed_)
    return;
  std::string p = R"({"type":"subscribe","channels":[{"name":"balance","account_ids":[)";
  for (std::size_t i = 0; i < balance_accounts_.size(); ++i) {
    if (i != 0) p += ',';
    p += '"';
    p += balance_accounts_[i];
    p += '"';
  }
  p += "]}]}";
  if (user_conn_.send_text(signed_payload(p))) {
    balance_subscribed_ = balance_accounts_;
  } else {
    FASTMM_LOG_WARN("{}: could not subscribe the balance channel", cfg_.name);
  }
}

// ---- execution replay -------------------------------------------------------------------------
//
// GET /fills per product (product_id or order_id is required), newest first by trade_id, from the
// watermark's start_date (inclusive), paged by the CB-AFTER cursor, before every open-order
// snapshot and once a minute (ReplayScheduler). Each row is emitted as a fill carrying its
// trade_id, the same id the user channel's match carries, so the OMS keeps the ones it never saw.

void CoinbaseExchangeVenue::resume_executions(std::int64_t since_venue_ms,
                                              const std::vector<std::string>& known) {
  exec_replay_.resume(since_venue_ms, known);
}

bool CoinbaseExchangeVenue::request_executions(std::int64_t since_venue_ms) {
  if (since_venue_ms > 0 && !exec_replay_.active()) exec_replay_.restart_from(since_venue_ms);
  return exec_replay_.run();
}

bool CoinbaseExchangeVenue::replay_ready() const noexcept {
  return !cfg_.dry_run && connected_ && signer_.usable() && rest_ != nullptr &&
         !rest_hard_stopped_ && !subscribed_.empty();
}

bool CoinbaseExchangeVenue::query_fills(const ReplayQuery& q) {
  if (rest_ == nullptr || rest_hard_stopped_ || q.stream >= subscribed_.size()) return false;
  const std::string target = CoinbaseOrderEncoder::fills_path(
      symbols_->venue_symbol(subscribed_[q.stream]), q.start_ms, q.end_ms, q.page, kFillsPageRows);
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET",
      target,
      rest_headers("GET", target, {}),
      {},
      [this, alive, q](const net::HttpResponse& r) {
        if (alive.expired() || !exec_replay_.expects(q)) return;
        ++stats_.rest_requests;
        std::vector<FillRow> rows;
        const std::string err = r.ok() ? decode_fills(r.body, rows)
                                       : fmt::format("status={} err={} {}",
                                                     r.status,
                                                     net::to_string(r.error),
                                                     error_message(r.body));
        if (!err.empty()) {
          ++stats_.rest_errors;
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR(
              "{}: GET /fills failed ({}); this reconciliation cannot book the fills the user "
              "channel missed",
              cfg_.name,
              err);
          if (r.error == net::NetError::None) {
            const std::string msg = error_message(r.body);
            const VenueAction a = map_error(r.status, msg).action;
            if (a != VenueAction::Reconcile) apply_action(a, r.status, msg);
          }
          exec_replay_.failed(q);
          return;
        }
        ReplayPage<FillRow> page;
        page.more = rows.size() >= static_cast<std::size_t>(kFillsPageRows);
        const std::string_view after = r.header("CB-AFTER");
        if (!after.empty()) {
          page.next = std::string(after);
        } else if (!rows.empty()) {
          page.next = std::to_string(rows.back().trade_id);
        }
        page.rows.reserve(rows.size());
        for (FillRow& f : rows) {
          const std::int64_t t = f.time_ms;
          std::string key = std::to_string(f.trade_id);
          page.rows.push_back({t, 0, std::move(key), std::move(f)});
        }
        exec_replay_.answer(q, std::move(page));
      });
  if (!queued) {
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: no room to ask for the account's fills", cfg_.name);
  }
  return queued;
}

bool CoinbaseExchangeVenue::emit_fill(std::size_t stream, const FillRow& f) {
  if (stream >= subscribed_.size()) return false;
  const InstrumentId inst = subscribed_[stream];
  if (!f.product_id.empty() && subscribed_instrument(f.product_id) != inst) return false;
  const ClientOrderId cl = name_of(f.order_id);
  emit_replayed_fill(*order_sink_,
                     id_,
                     inst,
                     cl,
                     f.order_id,
                     std::to_string(f.trade_id),
                     f.side == "sell" ? Side::Sell : Side::Buy,
                     f.price,
                     f.size,
                     f.fee,
                     FeeAsset::Quote,
                     f.liquidity == "M" ? Liquidity::Maker : Liquidity::Taker,
                     f.time_ms,
                     exec_replay_.emitting_unresolved() ? OrderFillMsg::kUnresolved : 0);
  ++stats_.order_events;
  ++stats_.executions_fetched;
  balances_after_fill_ = true;
  return true;
}

// GET /orders/<order_id>: the client_oid of an order a fill names that this connector does not
// know (it ended before this process started, or before its acknowledgement came).
bool CoinbaseExchangeVenue::lookup_order(const ReplayLookup& l) {
  if (rest_ == nullptr || rest_hard_stopped_) return false;
  if (!order_key(l.order_id)) return false;
  const std::string target = CoinbaseOrderEncoder::order_path(l.order_id);
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET",
      target,
      rest_headers("GET", target, {}),
      {},
      [this, alive, l](const net::HttpResponse& r) {
        if (alive.expired() || !connected_) return;
        ++stats_.rest_requests;
        OrderRow o;
        if (!r.ok() || !decode_order(r.body, o).empty() || o.id != l.order_id) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET /orders/{} failed: status={} {}; its fill names no order yet",
                          cfg_.name,
                          l.order_id,
                          r.status,
                          error_message(r.body));
          exec_replay_.looked_up(l, LookupResult::Failed);
          return;
        }
        const auto cl = decode_client_oid(o.client_oid);
        if (!cl) {
          exec_replay_.looked_up(l, LookupResult::NotOurs);
          return;
        }
        remember_order(o.id, *cl);
        exec_replay_.looked_up(l, LookupResult::Named);
      });
}

// ---- control requests -----------------------------------------------------------------------

void CoinbaseExchangeVenue::request_server_time() {
  if (rest_ == nullptr || time_request_pending_ || rest_hard_stopped_) return;
  time_request_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued = rest_->request(
      "GET", "/time", kUserAgent, {}, [this, alive, gen](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        time_request_pending_ = false;
        ++stats_.rest_requests;
        std::int64_t server_ms = 0;
        if (!r.ok() || !decode_server_time(r.body, server_ms).empty()) {
          ++stats_.rest_errors;
          return;
        }
        const std::int64_t offset = server_ms - wall_now().ns / kNsPerMs;
        clock_offset_ms_.store(offset);
        clock_sync_ns_ = now_ns();
        clock_resync_wanted_ = false;
        stats_.clock_offset_ms = offset;
        if (offset > 1000 || offset < -1000)
          FASTMM_LOG_WARN(
              "{}: clock offset to venue is {} ms (the venue allows 30 s)", cfg_.name, offset);
      });
  if (!queued) time_request_pending_ = false;
}

// User-channel loss: DELETE /orders per product, asynchronously.
void CoinbaseExchangeVenue::cancel_all_async() {
  if (rest_ == nullptr || !signer_.usable() || symbols_ == nullptr) return;
  std::weak_ptr<int> alive = alive_;
  for (InstrumentId id : subscribed_) {
    const std::string product(symbols_->venue_symbol(id));
    const std::string target = CoinbaseOrderEncoder::cancel_all_path(product);
    static_cast<void>(rest_->request(
        "DELETE",
        target,
        rest_headers("DELETE", target, {}),
        {},
        [this, alive, product](const net::HttpResponse& r) {
          if (alive.expired() || r.error == net::NetError::Canceled) return;
          ++stats_.rest_requests;
          std::vector<std::string> ids;
          if (!r.ok() || !decode_ids(r.body, ids).empty()) {
            ++stats_.rest_errors;
            FASTMM_LOG_ERROR("{}: cancel-all {} failed: status={} {}",
                             cfg_.name,
                             product,
                             r.status,
                             error_message(r.body));
            return;
          }
          FASTMM_LOG_INFO("{}: cancel-all {}: {} orders", cfg_.name, product, ids.size());
        }));
  }
}

bool CoinbaseExchangeVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr) return true;
  BlockingControl control(cfg_, coinbase_blocking_retry());
  std::vector<std::string> products;
  products.reserve(subscribed_.size());
  for (InstrumentId id : subscribed_) products.emplace_back(symbols_->venue_symbol(id));
  bool all_ok = true;
  for (const std::string& product : products) {
    // "This may require you to make the request multiple times until all of the open orders are
    // deleted": again until a round cancels nothing.
    bool ok = false;
    std::size_t cancelled = 0;
    for (int round = 0; round < std::max(1, cfg_.cancel_all_rounds); ++round) {
      const HttpReply reply =
          control.send("kill-switch cancel-all " + product, [&](BlockingRequest& q) {
            q.method = "DELETE";
            q.target = CoinbaseOrderEncoder::cancel_all_path(product);
            q.headers = rest_headers("DELETE", q.target, {});
            return true;
          });
      std::vector<std::string> ids;
      if (!reply.ok() || !decode_ids(reply.body, ids).empty()) {
        FASTMM_LOG_ERROR(
            "{}: kill-switch cancel-all {} failed: {}", cfg_.name, product, reply_error(reply));
        ok = false;
        break;
      }
      ok = true;
      cancelled += ids.size();
      if (ids.empty()) break;
    }
    if (ok) {
      FASTMM_LOG_INFO(
          "{}: kill-switch cancel-all {} ok ({} orders)", cfg_.name, product, cancelled);
    }
    all_ok = all_ok && ok;
  }
  return all_ok;
}

// ---- housekeeping -----------------------------------------------------------------------------

void CoinbaseExchangeVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (clock_resync_wanted_ || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  if (balances_after_fill_) {
    balances_after_fill_ = false;
    reconcile_.request_balances();
  }
  reconcile_.on_timer(now);
  if (!cfg_.dry_run && signer_.usable()) exec_replay_.on_timer(now);
  shadow_overflow_.check(cfg_.name, shadows_.size(), decltype(shadows_)::kMaxSize);
  publish_status();
  raw_md_.flush();
  raw_user_.flush();
  if (reactor_ != nullptr && housekeeping_timer_ == net::kInvalidTimer) {
    std::weak_ptr<int> alive = alive_;
    housekeeping_timer_ = reactor_->add_timer_after(kHousekeepingNs, [this, alive] {
      if (alive.expired()) return;
      housekeeping_timer_ = net::kInvalidTimer;
      on_timer(now_ns());
    });
  }
}

void CoinbaseExchangeVenue::publish_status() noexcept {
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
}

VenueStatus CoinbaseExchangeVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

CoinbaseVenueConfig make_coinbase_config(const VenueSection& v, bool dry_run) {
  CoinbaseVenueConfig c;
  c.name = v.name;
  c.ws_url = v.ws_url;
  c.rest_url = v.rest_url;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.dry_run = dry_run;
  c.sandbox = v.testnet;
  c.credentials.api_key = v.api_key;
  c.credentials.secret.value = v.api_secret;
  c.credentials.passphrase.value = v.api_passphrase;
  const VenueExtras x(v.extra);
  auto bad = [&](const char* key, std::string_view why) {
    return std::invalid_argument(
        fmt::format("venues.{}.{}: {} (\"{}\")", v.name, key, why, x.get(key)));
  };
  if (v.supports_replace)
    throw std::invalid_argument(fmt::format(
        "venues.{}.supports_replace: the Coinbase Exchange has no replace for REST orders; set it "
        "to false (cancel and new)",
        v.name));
  c.ws_private_url = x.get("ws_private_url");
  if (c.ws_private_url.empty()) c.ws_private_url = c.ws_url;
  if (const std::string d = x.get("depth_channel"); d == "level2") {
    c.depth_channel = DepthChannel::Level2;
  } else if (!d.empty() && d != "level2_batch") {
    throw bad("depth_channel", "expected \"level2_batch\" or \"level2\"");
  }
  if (const std::string s = x.get("stp"); s == "co") {
    c.stp = Stp::Co;
  } else if (s == "cn") {
    c.stp = Stp::Cn;
  } else if (s == "cb") {
    c.stp = Stp::Cb;
  } else if (!s.empty() && s != "dc") {
    throw bad("stp", "expected \"dc\", \"co\", \"cn\" or \"cb\"");
  }
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("dead_ms", c.dead_ms)));
  check_liveness(v.name, c.stale_ms, c.dead_ms);
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("orders_per_second", c.orders_per_second)));
  c.cancel_all_rounds = static_cast<int>(
      std::clamp<std::int64_t>(x.integer("cancel_all_rounds", c.cancel_all_rounds), 1, 10));
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.emit_ack_from_response = x.flag("emit_ack_from_response", true);
  if (!dry_run && !c.credentials.api_key.empty()) {
    if (c.credentials.passphrase.value.empty())
      throw std::invalid_argument(fmt::format(
          "venues.{}.api_passphrase: required with api_key (the passphrase set when the API key "
          "was created, written as \"${{VARIABLE}}\")",
          v.name));
    if (Signer(c.credentials).secret_malformed())
      throw std::invalid_argument(fmt::format(
          "venues.{}.api_secret: not base64 (the secret is shown once, when the key is created)",
          v.name));
  }
  if (c.depth_channel == DepthChannel::Level2 && !Signer(c.credentials).usable()) {
    if (!dry_run) throw bad("depth_channel", "level2 needs api_key, api_secret, api_passphrase");
    FASTMM_LOG_WARN("{}: level2 needs a signature; a dry run without keys subscribes level2_batch",
                    v.name);
    c.depth_channel = DepthChannel::Level2Batch;
  }
  // Sandbox keys work only against the sandbox hosts and the reverse: refuse the mix here.
  for (const std::string* url : {&c.ws_url, &c.rest_url}) {
    const auto u = net::Url::parse(*url);
    if (!u) continue;
    const bool sandbox_host = u->host.find("sandbox") != std::string_view::npos;
    const bool live_host = !sandbox_host && u->host.ends_with(".exchange.coinbase.com");
    if (sandbox_host && !c.sandbox)
      throw std::invalid_argument(fmt::format(
          "venues.{}.testnet: false with the sandbox host {}; the sandbox needs testnet = true",
          v.name,
          u->host));
    if (live_host && c.sandbox)
      throw std::invalid_argument(fmt::format(
          "venues.{}.testnet: true (the default) with the production host {}: set testnet = "
          "false for production, or use the sandbox hosts",
          v.name,
          u->host));
  }
  return c;
}

}  // namespace fastmm::venues::coinbase
