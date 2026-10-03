#include "fastmm/venues/coinbase/advanced_venue.hpp"

#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"

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
constexpr std::size_t kMaxReconcilePages = 20;
// GET /accounts: 250 a page; a retail account lists one per currency it has held.
constexpr std::size_t kMaxAccountPages = 20;
constexpr int kFillsPageRows = 100;
constexpr std::size_t kMaxFillPagesPerWindow = 20;
constexpr std::size_t kMaxFillPages = 100;
constexpr std::int64_t kFillWindowMs = kDayMs;
constexpr std::int64_t kSettleMs = 60'000;
// An order's executions the fills endpoint does not show yet: asked again this often, this many
// times, then left to the next replay.
constexpr std::int64_t kFetchRetryNs = 500'000'000;
constexpr std::uint8_t kMaxFetchAttempts = 10;
// Trade ids forwarded from the per-order reads, remembered so none goes out twice.
constexpr std::size_t kLiveTradesKept = 8192;
constexpr std::string_view kUserAgent = "User-Agent: fastmm\r\n";
constexpr std::string_view kProductionRestHost = "api.coinbase.com";
constexpr std::string_view kProductionUserUrl = "wss://advanced-trade-ws-user.coinbase.com";

std::string reply_error(const HttpReply& r) {
  return r.error.empty() ? fmt::format("HTTP {} {}", r.status, r.body.substr(0, 200)) : r.error;
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

CoinbaseAdvancedVenue::CoinbaseAdvancedVenue(VenueId id, AdvancedVenueConfig cfg)
    : id_(id), cfg_(std::move(cfg)), signer_(cfg_.credentials), rate_(cfg_.rate_threshold) {
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  if (const auto u = net::Url::parse(cfg_.rest_url)) rest_host_ = std::string(u->host);
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
      [this](std::size_t, const AdvFillRow& f) { return emit_replayed(f); },
      [this](std::size_t, const AdvFillRow& f) {
        return !f.order_id.empty() && !name_of(f.order_id).valid() ? f.order_id : std::string{};
      });
  exec_replay_.set_streams(1);
}

CoinbaseAdvancedVenue::~CoinbaseAdvancedVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps CoinbaseAdvancedVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = false;
  c.supports_post_only = true;
  c.ws_order_entry = false;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

std::int64_t CoinbaseAdvancedVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_.load(std::memory_order_relaxed);
}

std::string CoinbaseAdvancedVenue::rest_headers(std::string_view method,
                                                std::string_view target,
                                                bool body) const {
  std::string h(kUserAgent);
  const std::string jwt =
      signer_.sign(venue_time_ms() / 1000, CdpJwtSigner::rest_uri(method, rest_host_, target));
  if (!jwt.empty()) h.append("Authorization: Bearer ").append(jwt).append("\r\n");
  if (body) h.append("Content-Type: application/json\r\n");
  return h;
}

net::ConnectionConfig CoinbaseAdvancedVenue::ws_config(const std::string& url,
                                                       bool manual_subscribe) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  c.dead_ms = cfg_.dead_ms;
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = 0;
  c.manual_subscribe = manual_subscribe;
  c.extra_headers = std::string(kUserAgent);
  // A level2 snapshot of the whole book: 4.6 MB for BTC-USD on 2026-09-30.
  c.ws.recv_capacity = std::size_t{32} << 20;
  return c;
}

std::vector<std::string> CoinbaseAdvancedVenue::products() const {
  std::vector<std::string> out;
  out.reserve(subscribed_.size());
  for (InstrumentId id : subscribed_) out.emplace_back(symbols_->venue_symbol(id));
  return out;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> CoinbaseAdvancedVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  try {
    BlockingHttp http(cfg_.rest_url, blocking_options(cfg_));
    {
      const HttpReply t = http.get(std::string(kAdvancedPrefix) + "/time", kUserAgent);
      std::int64_t server_ms = 0;
      if (t.ok() && decode_adv_time(t.body, server_ms).empty()) {
        clock_offset_ms_.store(server_ms - wall_now().ns / kNsPerMs);
        clock_sync_ns_ = now_ns();
      }
    }
    for (Instrument* inst : mine) {
      const HttpReply reply =
          http.get(AdvancedOrderEncoder::product_path(inst->symbol.view()), kUserAgent);
      if (!reply.ok()) {
        if (!cfg_.allow_offline_reference_data)
          return fail(fmt::format(
              "{}: product {} failed: {}", cfg_.name, inst->symbol.view(), reply_error(reply)));
        FASTMM_LOG_WARN("{}: product {} failed ({}); keeping configured tick/lot",
                        cfg_.name,
                        inst->symbol.view(),
                        reply_error(reply));
        continue;
      }
      ProductInfo p;
      if (std::string err = decode_adv_product(reply.body, p); !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
      if (inst->tick != p.tick || inst->lot != p.lot)
        FASTMM_LOG_WARN("{}: {} tick/lot from the venue override config ({} / {} -> {} / {})",
                        cfg_.name,
                        inst->symbol.view(),
                        inst->tick,
                        inst->lot,
                        p.tick,
                        p.lot);
      inst->tick = p.tick;
      inst->lot = p.lot;
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

void CoinbaseAdvancedVenue::attach(const SymbolTable& symbols,
                                   const InstrumentTable& instruments,
                                   EventSink& md_sink,
                                   EventSink& order_sink,
                                   MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ = std::make_unique<AdvancedMdFeed>(
      symbols,
      id_,
      md_sink,
      ResubscribeRequester{&CoinbaseAdvancedVenue::resubscribe_requester, this});
  md_feed_->set_log_name(cfg_.name);
  user_parser_ = std::make_unique<AdvancedUserParser>(symbols, id_);
  encoder_ = std::make_unique<AdvancedOrderEncoder>(symbols);
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments);
}

void CoinbaseAdvancedVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id);
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (connected_ && md_conn_.opened()) {
    md_conn_.close();
    open_md();
  }
  // "To add products, unsubscribe and open a new connection with the expanded list."
  if (connected_ && user_conn_.opened()) {
    user_conn_.close();
    open_user();
  }
}

InstrumentId CoinbaseAdvancedVenue::subscribed_instrument(std::string_view product) const noexcept {
  const InstrumentId id = symbols_->find(id_, product);
  if (!id.valid()) return id;
  if (std::find(subscribed_.begin(), subscribed_.end(), id) == subscribed_.end())
    return InstrumentId::invalid();
  return id;
}

void CoinbaseAdvancedVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("CoinbaseAdvancedVenue::connect before attach");
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
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, user={})", cfg_.name, cfg_.dry_run, private_on);
}

void CoinbaseAdvancedVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  reconcile_.close();
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

void CoinbaseAdvancedVenue::open_rest() {
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
}

void CoinbaseAdvancedVenue::open_md() {
  md_conn_.open(*reactor_, ws_config(cfg_.ws_url, false), md_handler_);
  md_conn_.connect();
}

void CoinbaseAdvancedVenue::open_user() {
  user_conn_.open(*reactor_, ws_config(cfg_.ws_private_url, true), user_handler_);
  user_conn_.connect();
}

// ---- market data ------------------------------------------------------------------------------

void CoinbaseAdvancedVenue::on_md_state(net::ConnState s) {
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

void CoinbaseAdvancedVenue::on_md_open() {
  for (const std::string& p : md_feed_->subscription_payloads()) {
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void CoinbaseAdvancedVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (CoinbaseBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void CoinbaseAdvancedVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Ok) return;
  const AdvancedMdResult& r = md_feed_->last();
  if (r.control == AdvancedControl::Error) {
    FASTMM_LOG_ERROR("{}: market-data request refused: {}", cfg_.name, r.msg);
    return;
  }
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  }
}

void CoinbaseAdvancedVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;
  for (const std::string& p : md_feed_->resubscribe_payloads(id))
    static_cast<void>(md_conn_.send_text(p));
  FASTMM_LOG_INFO(
      "{}: resubscribing {} for a fresh snapshot", cfg_.name, symbols_->venue_symbol(id));
}

// ---- user channel ---------------------------------------------------------------------------

void CoinbaseAdvancedVenue::on_user_open() {
  std::string ids;
  for (InstrumentId id : subscribed_) {
    if (!ids.empty()) ids += ',';
    ids += '"';
    ids += symbols_->venue_symbol(id);
    ids += '"';
  }
  user_sequence_known_ = false;
  const std::string jwt = signer_.sign(venue_time_ms() / 1000, {});
  const std::string user = R"({"type":"subscribe","product_ids":[)" + ids +
                           R"(],"channel":"user","jwt":")" + jwt + "\"}";
  // Heartbeats keep the connection talking when the account is quiet.
  if (!user_conn_.send_text(user) ||
      !user_conn_.send_text(R"({"type":"subscribe","channel":"heartbeats"})"))
    FASTMM_LOG_ERROR("{}: could not subscribe the user channel", cfg_.name);
}

void CoinbaseAdvancedVenue::on_user_state(net::ConnState s) {
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
    if (!user_was_live_) {
      reconcile_.sweep();
    } else if (prev != ConnState::Stale) {
      reconcile_.request();
    }
    user_was_live_ = true;
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: user channel lost", cfg_.name);
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void CoinbaseAdvancedVenue::on_user_text(std::string_view t, std::int64_t ts) {
  if (raw_user_.enabled()) raw_user_.record(ts, t);
  const Cycles t0 = rdtscp();
  AdvancedUserResult& r = user_result_;
  user_parser_->decode(t, wall_now(), t0, scratch_, r);
  if (r.has_sequence) {
    // A message of the user channel lost: an order's state may be behind. Reconcile.
    if (user_sequence_known_ && r.sequence != user_sequence_ + 1) {
      FASTMM_LOG_WARN("{}: user channel sequence_num went from {} to {}; reconciling",
                      cfg_.name,
                      user_sequence_,
                      r.sequence);
      reconcile_.request();
    }
    user_sequence_ = r.sequence;
    user_sequence_known_ = true;
  }
  if (r.status == ParseStatus::Ok) {
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      switch (h->type) {
        case EventType::OrderAck: {
          const auto* m = reinterpret_cast<const OrderAckMsg*>(h);
          learn_venue_id(m->cl_ord_id, m->venue_order_id.view());
          break;
        }
        case EventType::OrderCancelAck:
        case EventType::OrderExpired: {
          const ClientOrderId id = h->type == EventType::OrderExpired
                                       ? reinterpret_cast<const OrderExpiredMsg*>(h)->cl_ord_id
                                       : reinterpret_cast<const OrderCancelAckMsg*>(h)->cl_ord_id;
          if (Shadow* s = shadows_.find(id)) s->ended = true;
          maybe_forget(id);
          break;
        }
        case EventType::OrderReject:
          shadows_.erase(reinterpret_cast<const OrderRejectMsg*>(h)->cl_ord_id);
          break;
        default:
          break;
      }
      sent_.answered(*h);
      static_cast<void>(order_sink_->push(*h));
      ++stats_.order_events;
    }
  }
  // Executions to read: the cumulative quantity grew.
  for (std::uint32_t i = 0; i < r.due_count; ++i) {
    const FillDue& d = r.due[i];
    Shadow* s = shadows_.find(d.cl);
    if (s == nullptr) {
      // Not this session's order (an earlier session's, found by the sweep): the replay books
      // its executions.
      remember_order(d.order_id.view(), d.cl);
      exec_replay_.due_in(kFetchRetryNs);
      continue;
    }
    if (s->venue_id.empty()) s->venue_id.assign(d.order_id.view());
    if (d.cum > s->wanted) s->wanted = d.cum;
    fetch_fills(d.cl);
  }
  if (r.status == ParseStatus::Ok || r.due_count > 0) return;
  switch (r.control) {
    case UserControl::Subscriptions:
      if (t.find(R"("user")") != std::string_view::npos) user_conn_.subscribe_done();
      return;
    case UserControl::Error:
      FASTMM_LOG_ERROR("{}: user channel refused: {}", cfg_.name, r.msg);
      // An authentication failure before the channel is up: keys the venue refuses.
      if (user_state_ != ConnState::Live) apply_action(VenueAction::Fatal, 401, r.msg);
      return;
    default:
      break;
  }
  if (r.status == ParseStatus::Malformed) FASTMM_LOG_WARN("{}: malformed user frame", cfg_.name);
}

void CoinbaseAdvancedVenue::learn_venue_id(ClientOrderId id, std::string_view venue_id) {
  if (venue_id.empty()) return;
  remember_order(venue_id, id);
  Shadow* s = shadows_.find(id);
  if (s == nullptr) return;
  if (s->venue_id.empty()) s->venue_id.assign(venue_id);
  if (s->cancel_parked) {
    s->cancel_parked = false;
    send_cancel(id, *s);
  }
}

void CoinbaseAdvancedVenue::remember_order(std::string_view order_id, ClientOrderId cl) noexcept {
  const auto key = order_key(order_id);
  if (!key || !cl.valid()) return;
  if (order_names_.size() >= decltype(order_names_)::kMaxSize) order_names_.clear();
  static_cast<void>(order_names_.assign(*key, cl));
}

ClientOrderId CoinbaseAdvancedVenue::name_of(std::string_view order_id) const noexcept {
  const auto key = order_key(order_id);
  if (!key) return ClientOrderId{};
  const ClientOrderId* cl = order_names_.find(*key);
  return cl != nullptr ? *cl : ClientOrderId{};
}

// A shadow goes once the venue has ended the order and every execution it reported was read.
void CoinbaseAdvancedVenue::maybe_forget(ClientOrderId id) noexcept {
  const Shadow* s = shadows_.find(id);
  if (s != nullptr && s->ended && s->emitted >= s->wanted && !s->fetching) shadows_.erase(id);
}

// ---- executions of an order ------------------------------------------------------------------

void CoinbaseAdvancedVenue::fetch_fills(ClientOrderId id) {
  Shadow* s = shadows_.find(id);
  if (s == nullptr || rest_ == nullptr || rest_hard_stopped_ || s->venue_id.empty()) return;
  if (s->emitted >= s->wanted) return;
  if (s->fetching) {
    s->fetch_again = true;
    return;
  }
  s->fetching = true;
  s->next_fetch_ns = 0;
  const std::string target = AdvancedOrderEncoder::order_fills_path(s->venue_id.view());
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued = rest_->request(
      "GET",
      target,
      rest_headers("GET", target, false),
      {},
      [this, alive, gen, id](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        ++stats_.rest_requests;
        Shadow* sh = shadows_.find(id);
        if (sh == nullptr) return;
        sh->fetching = false;
        std::vector<AdvFillRow> rows;
        std::string cursor;
        const std::string err =
            r.ok() ? decode_adv_fills(r.body, rows, cursor)
                   : fmt::format("status={} {}", r.status, adv_error_message(r.body));
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN(
              "{}: executions of order {} failed ({})", cfg_.name, sh->venue_id.view(), err);
        } else {
          // Oldest first: each row's cumulative quantity is the order's after it.
          std::stable_sort(rows.begin(), rows.end(), [](const AdvFillRow& a, const AdvFillRow& b) {
            return a.time_ms < b.time_ms;
          });
          // A row goes out once, by its trade id, whatever order the venue lists them in.
          bool filled = false;
          for (const AdvFillRow& f : rows) {
            if (f.order_id != sh->venue_id.view() || !note_live_trade(f.trade_id)) continue;
            const Qty cum = sh->emitted + f.size;
            emit_fill(*sh, id, f, cum);
            sh->emitted = cum;
            filled = true;
          }
          // No channel reports balances: the executions moved them.
          if (filled) reconcile_.request_balances();
        }
        if (sh->emitted < sh->wanted) {
          // Not listed yet: again shortly, a few times, then the next replay.
          if (++sh->fetch_attempts < kMaxFetchAttempts) {
            sh->next_fetch_ns = now_ns() + kFetchRetryNs;
          } else {
            FASTMM_LOG_WARN(
                "{}: order {} executions short of {} after {} reads; left to the replay",
                cfg_.name,
                sh->venue_id.view(),
                sh->wanted,
                kMaxFetchAttempts);
            sh->emitted = sh->wanted;
            exec_replay_.due_in(kFetchRetryNs);
          }
        } else {
          sh->fetch_attempts = 0;
        }
        if (sh->fetch_again) {
          sh->fetch_again = false;
          fetch_fills(id);
        }
        if (Shadow* after = shadows_.find(id); after != nullptr && after->emitted >= after->qty)
          after->ended = true;  // filled: nothing more comes
        maybe_forget(id);
      });
  if (!queued) {
    s->fetching = false;
    s->next_fetch_ns = now_ns() + kFetchRetryNs;
  }
}

bool CoinbaseAdvancedVenue::note_live_trade(const std::string& trade_id) {
  if (!live_trades_.insert(trade_id).second) return false;
  live_trade_order_.push_back(trade_id);
  if (live_trade_order_.size() > kLiveTradesKept) {
    live_trades_.erase(live_trade_order_.front());
    live_trade_order_.pop_front();
  }
  return true;
}

void CoinbaseAdvancedVenue::emit_fill(const Shadow& s,
                                      ClientOrderId id,
                                      const AdvFillRow& f,
                                      Qty cum) {
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill, s.instrument, id_);
  m.cl_ord_id = id;
  m.venue_order_id.assign(f.order_id);
  m.exec_id.assign(f.trade_id);
  m.price = f.price;
  m.qty = f.size;
  m.cum_qty = cum;
  m.leaves_qty = s.qty > cum ? s.qty - cum : Qty{};
  m.fee = f.commission;
  m.fee_asset = FeeAsset::Quote;  // spot commissions are in the quote currency
  m.side = s.side;
  m.liquidity = f.liquidity == "MAKER" ? Liquidity::Maker : Liquidity::Taker;
  m.hdr.exch_ts = Timestamp{f.time_ms * kNsPerMs};
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  sent_.answered(id);
  static_cast<void>(order_sink_->push(m.hdr));
  ++stats_.order_events;
}

// ---- outbound ---------------------------------------------------------------------------------

void CoinbaseAdvancedVenue::on_wake() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void CoinbaseAdvancedVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

template <class Ring>
void CoinbaseAdvancedVenue::write_orders(Ring& ring) {
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

void CoinbaseAdvancedVenue::refuse(const OrderCommand& cmd,
                                   RejectReason reason,
                                   std::string_view why) {
  if (cmd.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  } else {
    sent_.answered(cmd.cl_ord_id);
    emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  }
  ++stats_.order_events;
}

void CoinbaseAdvancedVenue::send_command(const OrderCommand& cmd) {
  const std::int64_t now = now_ns();
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  if (cfg_.dry_run) return refuse(cmd, RejectReason::VenueKilled, "dry-run: orders disabled");
  if ((fatal_ || rest_hard_stopped_) && !is_cancel)
    return refuse(cmd, RejectReason::VenueKilled, "venue fatal");
  if (cmd.kind == OrderCommandKind::Replace)
    return refuse(cmd, RejectReason::VenueReject, "replace: not supported (cancel and new)");
  if (rest_ == nullptr) return refuse(cmd, RejectReason::VenueReject, "no order channel");
  if (is_cancel) {
    Shadow* s = shadows_.find(cmd.cl_ord_id);
    if (s == nullptr) return refuse(cmd, RejectReason::UnknownOrder, "cancel: order unknown");
    if (s->ended) return refuse(cmd, RejectReason::VenueUnknownOrder, "cancel: order ended");
    if (s->venue_id.empty()) {
      // The venue's order id comes with the reply or the user channel: the cancel follows it.
      s->cancel_parked = true;
      return;
    }
    send_cancel(cmd.cl_ord_id, *s);
    return;
  }
  if (!rate_.can_send(1, now, true)) {
    ++stats_.rate_limit_cooldowns;
    return refuse(cmd, RejectReason::VenueRateLimit, "local rate limit");
  }
  OrderRequest rq;
  const Cycles before_encode = rdtscp();
  if (!encoder_->encode_new(cmd, rq))
    return refuse(cmd, RejectReason::VenueReject, "order could not be encoded");
  Shadow shadow;
  shadow.instrument = cmd.instrument;
  shadow.side = cmd.side;
  shadow.qty = cmd.qty;
  shadow.sent_seq = sent_.last_seq();
  if (shadows_.assign(cmd.cl_ord_id, shadow) == nullptr) {
    shadow_overflow_.refused(cfg_.name, shadows_.size());
    return refuse(cmd, RejectReason::OrderTableFull, "order table full");
  }
  const std::string headers = rest_headers("POST", rq.target(), true);
  const Cycles after_encode = rdtscp();
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const ClientOrderId cl = cmd.cl_ord_id;
  const bool queued = rest_->request("POST",
                                     rq.target(),
                                     headers,
                                     rq.payload(),
                                     [this, alive, gen, cl](const net::HttpResponse& r) {
                                       if (alive.expired() || gen != generation_) return;
                                       handle_new_reply(cl, r);
                                     });
  if (!queued) {
    ++stats_.order_send_failures;
    shadows_.erase(cl);
    return refuse(cmd, RejectReason::TransportFull, "rest queue full");
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  rate_.on_sent(1, now, true);
  sent_.sent_over_rest(cl);
  ++stats_.orders_sent;
}

void CoinbaseAdvancedVenue::send_cancel(ClientOrderId id, Shadow& s) {
  const std::string target = AdvancedOrderEncoder::cancel_path();
  const std::string ids[] = {std::string(s.venue_id.view())};
  const std::string body = AdvancedOrderEncoder::cancel_body(ids);
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued = rest_->request("POST",
                                     target,
                                     rest_headers("POST", target, true),
                                     body,
                                     [this, alive, gen, id](const net::HttpResponse& r) {
                                       if (alive.expired() || gen != generation_) return;
                                       handle_cancel_reply(id, r);
                                     });
  if (!queued) {
    const Shadow* sh = shadows_.find(id);
    emit_cancel_reject(*order_sink_,
                       id_,
                       sh != nullptr ? sh->instrument : InstrumentId::invalid(),
                       id,
                       RejectReason::TransportFull,
                       0,
                       "rest queue full");
    ++stats_.order_events;
    return;
  }
  ++stats_.cancels_sent;
}

void CoinbaseAdvancedVenue::handle_new_reply(ClientOrderId id, const net::HttpResponse& r) {
  ++stats_.rest_requests;
  sent_.answered(id);
  Shadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  if (r.error != net::NetError::None) {
    ++stats_.rest_errors;
    // It may or may not be at the venue: reject it here and let the snapshot say.
    emit_order_reject(*order_sink_,
                      id_,
                      inst,
                      id,
                      RejectReason::VenueReject,
                      0,
                      fmt::format("POST orders: {}", net::to_string(r.error)));
    ++stats_.order_events;
    shadows_.erase(id);
    request_open_orders();
    return;
  }
  CreateReply c;
  const std::string err = decode_create_reply(r.body, c);
  if (r.ok() && err.empty()) {
    if (c.success) {
      if (cfg_.emit_ack_from_response) emit_order_ack(*order_sink_, id_, inst, id, c.order_id);
      ++stats_.order_events;
      learn_venue_id(id, c.order_id);
      return;
    }
    const ErrorMapping m = map_order_failure(c.failure_reason);
    emit_order_reject(*order_sink_,
                      id_,
                      inst,
                      id,
                      m.reason,
                      r.status,
                      c.failure_reason.empty() ? std::string_view(c.message)
                                               : std::string_view(c.failure_reason));
    ++stats_.order_events;
    shadows_.erase(id);
    if (!m.known)
      FASTMM_LOG_WARN("{}: order refused: {} {}", cfg_.name, c.failure_reason, c.message);
    apply_action(m.action, r.status, c.failure_reason);
    return;
  }
  ++stats_.rest_errors;
  const std::string msg = adv_error_message(r.body);
  const ErrorMapping m = map_http(r.status, msg);
  emit_order_reject(*order_sink_,
                    id_,
                    inst,
                    id,
                    m.reason,
                    r.status,
                    msg.empty() ? r.body.substr(0, 120) : std::string_view(msg));
  ++stats_.order_events;
  shadows_.erase(id);
  apply_action(m.action, r.status, msg);
}

void CoinbaseAdvancedVenue::handle_cancel_reply(ClientOrderId id, const net::HttpResponse& r) {
  ++stats_.rest_requests;
  Shadow* s = shadows_.find(id);
  const InstrumentId inst = s != nullptr ? s->instrument : InstrumentId::invalid();
  std::vector<CancelResult> results;
  const std::string err = r.ok() ? decode_cancel_results(r.body, results)
                                 : fmt::format("status={} {}", r.status, adv_error_message(r.body));
  if (err.empty() && !results.empty() && results[0].success) {
    // Accepted: the user channel reports the cancel with what filled. Without the channel the
    // reply is all there will be.
    if (!user_conn_.is_live() && s != nullptr) {
      emit_cancel_ack(*order_sink_, id_, inst, id, s->venue_id.view(), s->wanted);
      s->ended = true;
      maybe_forget(id);
      ++stats_.order_events;
    }
    return;
  }
  ++stats_.rest_errors;
  const ErrorMapping m = !err.empty() ? map_http(r.status, err)
                         : results.empty()
                             ? ErrorMapping{RejectReason::VenueReject, VenueAction::None, false}
                             : map_cancel_failure(results[0].failure_reason);
  const std::string why = !err.empty()      ? err
                          : results.empty() ? "no result"
                                            : results[0].failure_reason;
  emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, r.status, why);
  ++stats_.order_events;
  apply_action(m.action, r.status, why);
}

void CoinbaseAdvancedVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void CoinbaseAdvancedVenue::apply_action(VenueAction action, int code, std::string_view msg) {
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

void CoinbaseAdvancedVenue::request_open_orders() {
  reconcile_.request();
}

bool CoinbaseAdvancedVenue::replay_executions() {
  return exec_replay_.run();
}

bool CoinbaseAdvancedVenue::fetch_snapshot(std::uint64_t generation) {
  reconcile_pages_ = 0;
  snapshot_ids_.clear();
  return request_open_orders_page(generation, {});
}

bool CoinbaseAdvancedVenue::request_open_orders_page(std::uint64_t generation,
                                                     const std::string& cursor) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  const std::string target = AdvancedOrderEncoder::open_orders_path(products(), cursor);
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET",
      target,
      rest_headers("GET", target, false),
      {},
      [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.current(generation)) return;
        ++stats_.rest_requests;
        std::vector<AdvOrderRow> rows;
        std::string next;
        bool has_next = false;
        const std::string err = r.ok() ? decode_adv_orders(r.body, rows, next, has_next)
                                       : fmt::format("status={} err={} {}",
                                                     r.status,
                                                     net::to_string(r.error),
                                                     adv_error_message(r.body));
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: open orders failed ({})", cfg_.name, err);
          if (r.error == net::NetError::None) {
            const std::string msg = adv_error_message(r.body);
            const VenueAction a = map_http(r.status, msg).action;
            if (a != VenueAction::Reconcile) apply_action(a, r.status, msg);
          }
          reconcile_.fetched(generation, false);
          return;
        }
        for (const AdvOrderRow& o : rows) {
          const InstrumentId inst = subscribed_instrument(o.product_id);
          if (!inst.valid() || o.status != "OPEN") continue;
          ReconcileMsg& m = reconcile_.add_order(inst);
          m.side = o.side == "SELL" ? Side::Sell : Side::Buy;
          m.state = o.filled_size.is_positive() ? OrderState::PartiallyFilled : OrderState::Live;
          if (const auto cl = decode_cl_ord_id(o.client_order_id)) {
            m.cl_ord_id = *cl;
            snapshot_ids_.push_back(*cl);
            learn_venue_id(*cl, o.order_id);
          }
          m.venue_order_id.assign(o.order_id);
          m.price = o.price;
          m.orig_qty = o.size;
          m.cum_qty = o.filled_size;
        }
        if (has_next && !next.empty()) {
          if (++reconcile_pages_ >= kMaxReconcilePages) {
            FASTMM_LOG_WARN("{}: more than {} pages of open orders", cfg_.name, kMaxReconcilePages);
            reconcile_.fetched(generation, false);
          } else if (!request_open_orders_page(generation, next)) {
            reconcile_.fetched(generation, false);
          }
          return;
        }
        reconcile_.fetched(generation, true);
        std::sort(snapshot_ids_.begin(), snapshot_ids_.end());
        static_cast<void>(user_parser_->sweep([this](ClientOrderId cl) {
          return shadows_.find(cl) != nullptr ||
                 std::binary_search(snapshot_ids_.begin(), snapshot_ids_.end(), cl);
        }));
      });
}

void CoinbaseAdvancedVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const Shadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void CoinbaseAdvancedVenue::drop_shadow(ClientOrderId id) {
  shadows_.erase(id);
}

// ---- balances -----------------------------------------------------------------------------------

bool CoinbaseAdvancedVenue::fetch_balances(std::uint64_t generation) {
  account_pages_ = 0;
  balance_rows_.clear();
  return request_accounts_page(generation, {});
}

bool CoinbaseAdvancedVenue::request_accounts_page(std::uint64_t generation,
                                                  const std::string& cursor) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  const std::string target = AdvancedOrderEncoder::accounts_path(cursor);
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET",
      target,
      rest_headers("GET", target, false),
      {},
      [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.balances_current(generation)) return;
        ++stats_.rest_requests;
        std::vector<AdvBalanceRow> rows;
        std::string next;
        bool has_next = false;
        const std::string err = r.ok() ? decode_adv_balances(r.body, rows, next, has_next)
                                       : fmt::format("status={} err={} {}",
                                                     r.status,
                                                     net::to_string(r.error),
                                                     adv_error_message(r.body));
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: accounts failed ({})", cfg_.name, err);
          if (r.error == net::NetError::None) {
            const std::string msg = adv_error_message(r.body);
            const VenueAction a = map_http(r.status, msg).action;
            if (a != VenueAction::Reconcile) apply_action(a, r.status, msg);
          }
          reconcile_.balances_fetched(generation, false, 0);
          return;
        }
        // One row a currency: two spot accounts of one currency add up.
        for (AdvBalanceRow& b : rows) {
          const auto same =
              std::find_if(balance_rows_.begin(), balance_rows_.end(), [&](const AdvBalanceRow& x) {
                return x.currency == b.currency;
              });
          if (same == balance_rows_.end()) {
            balance_rows_.push_back(std::move(b));
          } else {
            same->available = same->available + b.available;
            same->hold = same->hold + b.hold;
          }
        }
        if (has_next && !next.empty()) {
          if (++account_pages_ >= kMaxAccountPages) {
            FASTMM_LOG_WARN("{}: more than {} pages of accounts", cfg_.name, kMaxAccountPages);
            reconcile_.balances_fetched(generation, false, 0);
          } else if (!request_accounts_page(generation, next)) {
            reconcile_.balances_fetched(generation, false, 0);
          }
          return;
        }
        for (const AdvBalanceRow& b : balance_rows_)
          reconcile_.add_balance(b.currency, BalanceFields::spot(b.available, b.hold));
        balance_rows_.clear();
        // The reply carries no time: the venue's clock as it arrived.
        reconcile_.balances_fetched(generation, true, venue_time_ms());
      });
}

// ---- execution replay -------------------------------------------------------------------------

void CoinbaseAdvancedVenue::resume_executions(std::int64_t since_venue_ms,
                                              const std::vector<std::string>& known) {
  exec_replay_.resume(since_venue_ms, known);
}

bool CoinbaseAdvancedVenue::request_executions(std::int64_t since_venue_ms) {
  if (since_venue_ms > 0 && !exec_replay_.active()) exec_replay_.restart_from(since_venue_ms);
  return exec_replay_.run();
}

bool CoinbaseAdvancedVenue::replay_ready() const noexcept {
  return !cfg_.dry_run && connected_ && signer_.usable() && rest_ != nullptr &&
         !rest_hard_stopped_ && !subscribed_.empty();
}

bool CoinbaseAdvancedVenue::query_fills(const ReplayQuery& q) {
  if (rest_ == nullptr || rest_hard_stopped_) return false;
  const std::string target =
      AdvancedOrderEncoder::fills_path(products(), q.start_ms, q.end_ms, q.page, kFillsPageRows);
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET",
      target,
      rest_headers("GET", target, false),
      {},
      [this, alive, q](const net::HttpResponse& r) {
        if (alive.expired() || !exec_replay_.expects(q)) return;
        ++stats_.rest_requests;
        std::vector<AdvFillRow> rows;
        std::string cursor;
        const std::string err = r.ok() ? decode_adv_fills(r.body, rows, cursor)
                                       : fmt::format("status={} err={} {}",
                                                     r.status,
                                                     net::to_string(r.error),
                                                     adv_error_message(r.body));
        if (!err.empty()) {
          ++stats_.rest_errors;
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR(
              "{}: GET fills failed ({}); this reconciliation cannot book the fills the user "
              "channel missed",
              cfg_.name,
              err);
          if (r.error == net::NetError::None) {
            const std::string msg = adv_error_message(r.body);
            const VenueAction a = map_http(r.status, msg).action;
            if (a != VenueAction::Reconcile) apply_action(a, r.status, msg);
          }
          exec_replay_.failed(q);
          return;
        }
        ReplayPage<AdvFillRow> page;
        page.more = rows.size() >= static_cast<std::size_t>(kFillsPageRows) && !cursor.empty();
        page.next = cursor;
        page.rows.reserve(rows.size());
        for (AdvFillRow& f : rows) {
          const std::int64_t t = f.time_ms;
          std::string key = f.trade_id;
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

bool CoinbaseAdvancedVenue::emit_replayed(const AdvFillRow& f) {
  const InstrumentId inst = subscribed_instrument(f.product_id);
  if (!inst.valid()) return false;
  emit_replayed_fill(*order_sink_,
                     id_,
                     inst,
                     name_of(f.order_id),
                     f.order_id,
                     f.trade_id,
                     f.side == "SELL" ? Side::Sell : Side::Buy,
                     f.price,
                     f.size,
                     f.commission,
                     FeeAsset::Quote,
                     f.liquidity == "MAKER" ? Liquidity::Maker : Liquidity::Taker,
                     f.time_ms,
                     exec_replay_.emitting_unresolved() ? OrderFillMsg::kUnresolved : 0);
  ++stats_.order_events;
  ++stats_.executions_fetched;
  reconcile_.request_balances();  // at most one fetch a second, however many rows
  return true;
}

bool CoinbaseAdvancedVenue::lookup_order(const ReplayLookup& l) {
  if (rest_ == nullptr || rest_hard_stopped_ || l.order_id.empty()) return false;
  const std::string target = AdvancedOrderEncoder::order_path(l.order_id);
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET",
      target,
      rest_headers("GET", target, false),
      {},
      [this, alive, l](const net::HttpResponse& r) {
        if (alive.expired() || !connected_) return;
        ++stats_.rest_requests;
        AdvOrderRow o;
        if (!r.ok() || !decode_adv_order(r.body, o).empty() || o.order_id != l.order_id) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: order {} lookup failed: status={}; its fill names no order yet",
                          cfg_.name,
                          l.order_id,
                          r.status);
          exec_replay_.looked_up(l, LookupResult::Failed);
          return;
        }
        const auto cl = decode_cl_ord_id(o.client_order_id);
        if (!cl) {
          exec_replay_.looked_up(l, LookupResult::NotOurs);
          return;
        }
        remember_order(o.order_id, *cl);
        exec_replay_.looked_up(l, LookupResult::Named);
      });
}

// ---- control requests -----------------------------------------------------------------------

void CoinbaseAdvancedVenue::request_server_time() {
  if (rest_ == nullptr || time_request_pending_ || rest_hard_stopped_) return;
  time_request_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued =
      rest_->request("GET",
                     std::string(kAdvancedPrefix) + "/time",
                     kUserAgent,
                     {},
                     [this, alive, gen](const net::HttpResponse& r) {
                       if (alive.expired() || gen != generation_) return;
                       time_request_pending_ = false;
                       ++stats_.rest_requests;
                       std::int64_t server_ms = 0;
                       if (!r.ok() || !decode_adv_time(r.body, server_ms).empty()) {
                         ++stats_.rest_errors;
                         return;
                       }
                       const std::int64_t offset = server_ms - wall_now().ns / kNsPerMs;
                       clock_offset_ms_.store(offset);
                       clock_sync_ns_ = now_ns();
                       clock_resync_wanted_ = false;
                       stats_.clock_offset_ms = offset;
                       if (offset > 1000 || offset < -1000)
                         FASTMM_LOG_WARN("{}: clock offset to venue is {} ms", cfg_.name, offset);
                     });
  if (!queued) time_request_pending_ = false;
}

// User-channel loss: the open orders, then batch_cancel, asynchronously.
void CoinbaseAdvancedVenue::cancel_all_async() {
  if (rest_ == nullptr || !signer_.usable() || symbols_ == nullptr) return;
  const std::string target = AdvancedOrderEncoder::open_orders_path(products(), {});
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  static_cast<void>(rest_->request(
      "GET",
      target,
      rest_headers("GET", target, false),
      {},
      [this, alive, gen](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        ++stats_.rest_requests;
        std::vector<AdvOrderRow> rows;
        std::string next;
        bool has_next = false;
        if (!r.ok() || !decode_adv_orders(r.body, rows, next, has_next).empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_ERROR("{}: cancel-all: open orders failed: status={}", cfg_.name, r.status);
          return;
        }
        std::vector<std::string> ids;
        for (const AdvOrderRow& o : rows) {
          if (subscribed_instrument(o.product_id).valid()) ids.push_back(o.order_id);
        }
        for (std::size_t i = 0; i < ids.size(); i += cfg_.cancel_batch) {
          const std::size_t n = std::min<std::size_t>(cfg_.cancel_batch, ids.size() - i);
          const std::string body =
              AdvancedOrderEncoder::cancel_body(std::span<const std::string>(ids.data() + i, n));
          const std::string path = AdvancedOrderEncoder::cancel_path();
          static_cast<void>(rest_->request(
              "POST",
              path,
              rest_headers("POST", path, true),
              body,
              [this, alive](const net::HttpResponse& c) {
                if (alive.expired() || c.error == net::NetError::Canceled) return;
                ++stats_.rest_requests;
                if (!c.ok()) {
                  ++stats_.rest_errors;
                  FASTMM_LOG_ERROR(
                      "{}: cancel-all: batch_cancel failed: status={}", cfg_.name, c.status);
                }
              }));
        }
        FASTMM_LOG_INFO("{}: cancel-all: {} open orders", cfg_.name, ids.size());
      }));
}

bool CoinbaseAdvancedVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr) return true;
  BlockingControl control(cfg_);
  const std::vector<std::string> prods = products();
  std::size_t cancelled = 0;
  // Listed and cancelled until the list is empty (an order being placed while this runs shows
  // up in the next round), at most three rounds.
  for (int round = 0; round < 3; ++round) {
    std::vector<std::string> ids;
    std::string cursor;
    bool has_next = true;
    for (std::size_t page = 0; has_next && page < kMaxReconcilePages; ++page) {
      const std::string target = AdvancedOrderEncoder::open_orders_path(prods, cursor);
      const HttpReply reply = control.send("kill-switch open orders", [&](BlockingRequest& q) {
        q.method = "GET";
        q.target = target;
        q.headers = rest_headers("GET", target, false);
        return true;
      });
      std::vector<AdvOrderRow> rows;
      const std::string err =
          reply.ok() ? decode_adv_orders(reply.body, rows, cursor, has_next) : reply_error(reply);
      if (!err.empty()) {
        FASTMM_LOG_ERROR("{}: kill-switch cancel-all: open orders failed: {}", cfg_.name, err);
        return false;
      }
      for (const AdvOrderRow& o : rows) {
        if (subscribed_instrument(o.product_id).valid()) ids.push_back(o.order_id);
      }
      if (cursor.empty()) has_next = false;
    }
    if (ids.empty()) {
      FASTMM_LOG_INFO("{}: kill-switch cancel-all ok ({} orders)", cfg_.name, cancelled);
      return true;
    }
    for (std::size_t i = 0; i < ids.size(); i += cfg_.cancel_batch) {
      const std::size_t n = std::min<std::size_t>(cfg_.cancel_batch, ids.size() - i);
      const std::string body =
          AdvancedOrderEncoder::cancel_body(std::span<const std::string>(ids.data() + i, n));
      const HttpReply c = control.send("kill-switch batch_cancel", [&](BlockingRequest& q) {
        q.method = "POST";
        q.target = AdvancedOrderEncoder::cancel_path();
        q.headers = rest_headers("POST", q.target, true);
        q.body = body;
        return true;
      });
      std::vector<CancelResult> results;
      const std::string e = c.ok() ? decode_cancel_results(c.body, results) : reply_error(c);
      if (!e.empty()) {
        FASTMM_LOG_ERROR("{}: kill-switch batch_cancel failed: {}", cfg_.name, e);
        return false;
      }
      for (const CancelResult& res : results) {
        if (res.success) {
          ++cancelled;
        } else if (map_cancel_failure(res.failure_reason).reason !=
                   RejectReason::VenueUnknownOrder) {
          FASTMM_LOG_ERROR("{}: kill-switch cancel of {} refused: {}",
                           cfg_.name,
                           res.order_id,
                           res.failure_reason);
        }
      }
    }
  }
  FASTMM_LOG_ERROR("{}: kill-switch cancel-all: orders still open after 3 rounds", cfg_.name);
  return false;
}

// ---- housekeeping -----------------------------------------------------------------------------

void CoinbaseAdvancedVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (clock_resync_wanted_ || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  reconcile_.on_timer(now);
  if (!cfg_.dry_run && signer_.usable()) {
    exec_replay_.on_timer(now);
    // Executions the fills endpoint did not list yet.
    std::vector<ClientOrderId> due;
    shadows_.for_each([&](ClientOrderId id, const Shadow& s) {
      if (s.next_fetch_ns != 0 && now >= s.next_fetch_ns && !s.fetching) due.push_back(id);
    });
    for (ClientOrderId id : due) fetch_fills(id);
  }
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

void CoinbaseAdvancedVenue::publish_status() noexcept {
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
  budget_pub_.store(budget_of(rate_, now_ns()));
}

VenueStatus CoinbaseAdvancedVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

AdvancedVenueConfig make_coinbase_advanced_config(const VenueSection& v, bool dry_run) {
  AdvancedVenueConfig c;
  c.name = v.name;
  c.ws_url = v.ws_url;
  c.rest_url = v.rest_url;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.dry_run = dry_run;
  c.credentials.key_name = v.api_key;
  c.credentials.private_key.value = v.api_secret;
  const VenueExtras x(v.extra);
  if (v.supports_replace)
    throw std::invalid_argument(fmt::format(
        "venues.{}.supports_replace: the connector does not edit orders; set it to false (cancel "
        "and new)",
        v.name));
  c.ws_private_url = x.get("ws_private_url");
  if (c.ws_private_url.empty()) c.ws_private_url = std::string(kProductionUserUrl);
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("dead_ms", c.dead_ms)));
  check_liveness(v.name, c.stale_ms, c.dead_ms);
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("orders_per_second", c.orders_per_second)));
  c.cancel_batch = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(x.integer("cancel_batch", c.cancel_batch), 1, 100));
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.emit_ack_from_response = x.flag("emit_ack_from_response", true);
  if (!dry_run && !c.credentials.key_name.empty() && CdpJwtSigner(c.credentials).key_malformed())
    throw std::invalid_argument(fmt::format(
        "venues.{}.api_secret: not an EC P-256 private key PEM (the CDP key's \"privateKey\"; "
        "Ed25519 keys are not accepted by Advanced Trade)",
        v.name));
  if (!dry_run && !c.credentials.key_name.empty() &&
      c.credentials.key_name.find("/apiKeys/") == std::string::npos)
    throw std::invalid_argument(fmt::format(
        "venues.{}.api_key: expected the CDP key name, organizations/<org>/apiKeys/<id>", v.name));
  // Advanced Trade has no test environment with matching: testnet = true with its hosts is a
  // mistake, not a sandbox.
  if (const auto u = net::Url::parse(c.rest_url); u && v.testnet && u->host == kProductionRestHost)
    throw std::invalid_argument(fmt::format(
        "venues.{}.testnet: true (the default) with {}: Coinbase Advanced Trade has no test "
        "environment; set testnet = false",
        v.name,
        u->host));
  return c;
}

}  // namespace fastmm::venues::coinbase
