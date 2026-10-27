#include "fastmm/venues/alpaca/alpaca_venue.hpp"

#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <stdexcept>

namespace fastmm::venues::alpaca {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kClockResyncNs = 30LL * 60 * 1'000'000'000;
constexpr std::int64_t kRateLimitCooldownNs = 10'000'000'000;
constexpr std::int64_t kMinuteNs = 60'000'000'000;
constexpr int kFillsPageRows = 100;
constexpr std::string_view kLiveTradingHost = "api.alpaca.markets";
constexpr std::string_view kPaperRest = "https://paper-api.alpaca.markets";
constexpr std::string_view kPaperStream = "wss://paper-api.alpaca.markets/stream";
constexpr std::string_view kIexStream = "wss://stream.data.alpaca.markets/v2/iex";

std::string reply_error(const HttpReply& r) {
  return r.error.empty() ? fmt::format("HTTP {} {}", r.status, r.body.substr(0, 200)) : r.error;
}

std::string http_error(const net::HttpResponse& r) {
  if (r.error != net::NetError::None) return std::string(net::to_string(r.error));
  const ApiError e = decode_error(r.body);
  return fmt::format("HTTP {} {}", r.status, e.message.empty() ? r.body.substr(0, 160) : e.message);
}

// JSON string escaping for the auth frame (a key or secret is plain ASCII; quotes are refused).
bool plain(std::string_view s) {
  return std::none_of(s.begin(), s.end(), [](char c) { return c == '"' || c == '\\' || c < 32; });
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

AlpacaVenue::AlpacaVenue(VenueId id, AlpacaVenueConfig cfg)
    : id_(id), cfg_(std::move(cfg)), rate_(cfg_.rate_threshold) {
  if (cfg_.requests_per_minute > 0) rate_.add_order_bucket(cfg_.requests_per_minute, kMinuteNs);
  ReplayLimits limits;
  limits.window_ms = 24LL * 3600 * 1000;
  limits.page_rows = kFillsPageRows;
  limits.newest_first = true;
  limits.window_pages = 20;
  limits.max_pages = 100;
  limits.settle_ms = 60'000;
  exec_replay_.setup(
      cfg_.name,
      "fill(s)",
      limits,
      {[this] { return replay_ready(); },
       [this] { return venue_time_ms(); },
       [this](const ReplayQuery& q) { return query_fills(q); },
       [this](bool complete) { reconcile_.replay_done(complete); },
       [this](const ReplayLookup& l) { return lookup_order(l); },
       {}},
      [this](std::size_t, const FillRow& f) { return emit_replayed(f); },
      [this](std::size_t, const FillRow& f) {
        return !f.order_id.empty() && !name_of(f.order_id).valid() ? f.order_id : std::string{};
      });
  exec_replay_.set_streams(1);
}

AlpacaVenue::~AlpacaVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

bool AlpacaVenue::private_on() const noexcept {
  return !cfg_.dry_run && !cfg_.api_key.empty() && !cfg_.api_secret.empty();
}

VenueCaps AlpacaVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = true;
  c.supports_post_only = false;
  c.ws_order_entry = false;
  c.user_stream = private_on();
  return c;
}

std::int64_t AlpacaVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_.load(std::memory_order_relaxed);
}

std::string AlpacaVenue::auth_headers(bool body) const {
  std::string h = "User-Agent: fastmm\r\nAPCA-API-KEY-ID: " + cfg_.api_key +
                  "\r\nAPCA-API-SECRET-KEY: " + cfg_.api_secret + "\r\n";
  if (body) h += "Content-Type: application/json\r\n";
  return h;
}

std::string AlpacaVenue::auth_frame() const {
  return R"({"action":"auth","key":")" + cfg_.api_key + R"(","secret":")" + cfg_.api_secret + "\"}";
}

net::ConnectionConfig AlpacaVenue::ws_config(const std::string& url) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.source = cfg_.source;
  c.stale_ms = cfg_.stale_ms;
  c.dead_ms = cfg_.dead_ms;
  c.heartbeat_interval_ms = cfg_.ping_ms;
  c.backoff = cfg_.backoff;
  c.manual_subscribe = true;
  c.extra_headers = "User-Agent: fastmm\r\n";
  return c;
}

std::vector<std::string> AlpacaVenue::symbols() const {
  std::vector<std::string> out;
  out.reserve(subscribed_.size());
  for (InstrumentId id : subscribed_) out.emplace_back(symbols_->venue_symbol(id));
  return out;
}

InstrumentId AlpacaVenue::subscribed_instrument(std::string_view symbol) const noexcept {
  if (symbols_ == nullptr) return InstrumentId::invalid();
  const InstrumentId id = symbols_->find(id_, symbol);
  if (!id.valid() || std::find(subscribed_.begin(), subscribed_.end(), id) == subscribed_.end())
    return InstrumentId::invalid();
  return id;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> AlpacaVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  if (cfg_.api_key.empty() && !cfg_.dry_run) return fail(fmt::format("{}: no api_key", cfg_.name));
  try {
    BlockingHttp http(cfg_.rest_url, blocking_options(cfg_));
    const std::string headers = auth_headers(false);
    {
      const HttpReply c = http.get(std::string(kPrefix) + "/clock", headers);
      std::int64_t ms = 0;
      bool open = false;
      if (c.ok() && decode_clock(c.body, ms, open).empty()) {
        clock_offset_ms_.store(ms - wall_now().ns / kNsPerMs);
        clock_sync_ns_ = now_ns();
        FASTMM_LOG_INFO("{}: market {}", cfg_.name, open ? "open" : "closed");
      } else if (c.status == 401 || c.status == 403) {
        return fail(fmt::format("{}: the keys were refused: {}", cfg_.name, reply_error(c)));
      }
    }
    for (Instrument* inst : mine) {
      const HttpReply reply =
          http.get(std::string(kPrefix) + "/assets/" + std::string(inst->symbol.view()), headers);
      AssetInfo a;
      const std::string err = reply.ok() ? decode_asset(reply.body, a) : reply_error(reply);
      if (!err.empty()) {
        if (!cfg_.allow_offline_reference_data)
          return fail(fmt::format("{}: asset {}: {}", cfg_.name, inst->symbol.view(), err));
        FASTMM_LOG_WARN("{}: asset {} failed ({}); keeping the configured values",
                        cfg_.name,
                        inst->symbol.view(),
                        err);
        continue;
      }
      // Rule 612: a cent at or above $1.00. Sub-dollar stocks are not handled.
      const Price cent = Price::from_raw(kFixedScale / 100);
      if (inst->tick != cent)
        FASTMM_LOG_WARN("{}: {} tick {} -> 0.01", cfg_.name, inst->symbol.view(), inst->tick);
      inst->tick = cent;
      // A fraction of a share only where the asset allows it and the configuration asks.
      if (!inst->lot.is_positive() || (!a.fractionable && inst->lot.raw % kFixedScale != 0)) {
        FASTMM_LOG_WARN("{}: {} lot {} -> 1", cfg_.name, inst->symbol.view(), inst->lot);
        inst->lot = Qty::from_int(1);
      }
      if (inst->min_qty < inst->lot) inst->min_qty = inst->lot;
      inst->contract_multiplier = Qty::from_int(1);
      inst->expiry_ns = 0;
      if (inst->asset_class != AssetClass::Equity) {
        FASTMM_LOG_WARN("{}: {} asset_class set to equity", cfg_.name, inst->symbol.view());
        inst->asset_class = AssetClass::Equity;
      }
      if (inst->quote.view().empty()) static_cast<void>(inst->quote.assign("USD"));
      if (inst->base.view().empty()) static_cast<void>(inst->base.assign(inst->symbol.view()));
      if (a.status != "active" || !a.tradable) {
        FASTMM_LOG_ERROR("{}: {} is {}{}: disabled",
                         cfg_.name,
                         inst->symbol.view(),
                         a.status,
                         a.tradable ? "" : ", not tradable");
        inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kEnabled);
      } else if (!a.shortable) {
        FASTMM_LOG_WARN("{}: {} cannot be sold short", cfg_.name, inst->symbol.view());
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
  stats_.clock_offset_ms = clock_offset_ms_.load();
  publish_status();
  FASTMM_LOG_INFO("{}: reference data loaded for {} symbols", cfg_.name, mine.size());
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void AlpacaVenue::attach(const SymbolTable& symbols,
                         const InstrumentTable& instruments,
                         EventSink& md_sink,
                         EventSink& order_sink,
                         MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ = std::make_unique<AlpacaMdFeed>(symbols, id_, md_sink, cfg_.trades);
  md_feed_->set_log_name(cfg_.name);
  encoder_ = std::make_unique<AlpacaOrderEncoder>(symbols, cfg_.orders);
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments);
}

void AlpacaVenue::subscribe(std::span<const InstrumentId> instruments) {
  bool added = false;
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id);
    added = true;
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (added && connected_ && md_conn_.is_live()) {
    for (const std::string& p : md_feed_->subscription_payloads())
      static_cast<void>(md_conn_.send_text(p));
  }
}

void AlpacaVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("AlpacaVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  reconcile_.open(private_on());
  exec_replay_.start_at(venue_time_ms());
  exec_replay_.open(private_on());
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    if (private_on()) raw_user_.open(cfg_.record_raw_dir, cfg_.name, "user");
  }
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
  if (private_on()) request_clock();
  open_md();
  if (private_on()) open_user();
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, orders={})", cfg_.name, cfg_.dry_run, private_on());
}

void AlpacaVenue::disconnect() {
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
  clock_pending_ = false;
  if (rest_) rest_->reset();
  md_feed_->on_disconnected();
  raw_md_.flush();
  raw_user_.flush();
  publish_status();
}

void AlpacaVenue::open_md() {
  md_conn_.open(*reactor_, ws_config(cfg_.ws_url), md_handler_);
  md_conn_.connect();
}

void AlpacaVenue::open_user() {
  user_conn_.open(*reactor_, ws_config(cfg_.ws_private_url), user_handler_);
  user_conn_.connect();
}

// ---- market data ------------------------------------------------------------------------------

void AlpacaVenue::on_md_open() {
  // The stream takes the subscription right behind the auth frame.
  if (!md_conn_.send_text(auth_frame()))
    FASTMM_LOG_ERROR("{}: could not authenticate the market-data stream", cfg_.name);
  for (const std::string& p : md_feed_->subscription_payloads())
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
}

void AlpacaVenue::on_md_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.md = channel_state(s);
  if (s == net::ConnState::Backoff) ++stats_.reconnects;
  if (mapped == md_state_) return;
  const ConnState prev = md_state_;
  md_state_ = mapped;
  switch (mapped) {
    case ConnState::Live:
      md_feed_->on_connected();
      emit_connection_state(*md_sink_, id_, 0, ConnState::Live);
      FASTMM_LOG_INFO("{}: md channel -> Live", cfg_.name);
      break;
    case ConnState::Stale:
      emit_connection_state(*md_sink_, id_, 0, ConnState::Stale);
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

void AlpacaVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Ok) return;
  const MdDecodeResult& r = md_feed_->last();
  switch (r.control) {
    case MdControl::Subscription:
      md_conn_.subscribe_done();
      return;
    case MdControl::Error:
      // 406: another connection holds the account's one market-data connection.
      FASTMM_LOG_ERROR("{}: market-data stream error {}: {}", cfg_.name, r.error_code, r.msg);
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

// ---- trade_updates --------------------------------------------------------------------------

void AlpacaVenue::on_user_open() {
  if (!user_conn_.send_text(auth_frame()))
    FASTMM_LOG_ERROR("{}: could not authenticate the trading stream", cfg_.name);
}

void AlpacaVenue::on_user_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.user = private_channel_state(s);
  stats_.order = stats_.user;
  if (s == net::ConnState::Backoff) ++stats_.reconnects;
  if (mapped == user_state_) return;
  const ConnState prev = user_state_;
  user_state_ = mapped;
  if (mapped == ConnState::Live) {
    if (prev != ConnState::Stale) {
      emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
      FASTMM_LOG_INFO("{}: trading stream -> Live", cfg_.name);
    }
    if (!user_was_live_) {
      reconcile_.sweep();
    } else if (prev != ConnState::Stale) {
      exec_replay_.read_all();
      reconcile_.request();
    }
    user_was_live_ = true;
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: trading stream lost", cfg_.name);
    if (cfg_.cancel_on_order_channel_loss && connected_) cancel_all_async();
  }
}

void AlpacaVenue::on_user_text(std::string_view t, std::int64_t ts) {
  if (raw_user_.enabled()) raw_user_.record(ts, t);
  TradeUpdate& u = update_;
  if (!updates_.parse(t, u)) {
    FASTMM_LOG_WARN("{}: malformed trading-stream frame", cfg_.name);
    return;
  }
  switch (u.stream) {
    case TradeUpdate::Stream::Authorization:
      if (u.status == "authorized") {
        if (!user_conn_.send_text(R"({"action":"listen","data":{"streams":["trade_updates"]}})"))
          FASTMM_LOG_ERROR("{}: could not listen to trade_updates", cfg_.name);
      } else {
        FASTMM_LOG_ERROR("{}: trading stream refused the keys ({})", cfg_.name, u.status);
        apply_action(VenueAction::Fatal, 401, "trading stream unauthorized");
      }
      return;
    case TradeUpdate::Stream::Listening:
      user_conn_.subscribe_done();
      return;
    case TradeUpdate::Stream::TradeUpdates:
      on_order_update(u);
      return;
    case TradeUpdate::Stream::Other:
      return;
  }
}

void AlpacaVenue::on_order_update(const TradeUpdate& u) {
  const OrderView& o = u.order;
  const auto named = decode_cl_ord_id(o.client_order_id);
  if (!named) return;  // an order FastMM did not send
  const ClientOrderId cl = *named;
  remember_order(o.id, cl);
  const Shadow* s = shadows_.find(cl);
  const InstrumentId inst = s != nullptr ? s->instrument : symbols_->find(id_, o.symbol);
  if (!inst.valid()) return;
  const Timestamp venue_ts{u.time_ns > 0 ? u.time_ns : 0};
  const std::string_view ev = u.event;
  if (ev == "new") {
    emit_order_ack(*order_sink_, id_, inst, cl, o.id, 0, venue_ts);
    sent_.answered(cl);
    learn_venue_id(cl, o.id);
  } else if (ev == "fill" || ev == "partial_fill") {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, inst, id_);
    m.cl_ord_id = cl;
    m.venue_order_id.assign(o.id);
    m.exec_id.assign(u.execution_id);
    m.price = u.price;
    m.qty = u.qty;
    m.cum_qty = o.filled_qty;
    m.leaves_qty = o.qty > o.filled_qty ? o.qty - o.filled_qty : Qty{};
    m.fee = Notional{};  // commission free; regulatory fees are charged at the end of the day
    m.fee_asset = FeeAsset::Quote;
    m.side = o.side == "sell" ? Side::Sell : Side::Buy;
    m.liquidity = Liquidity::Unknown;
    m.hdr.exch_ts = venue_ts;
    m.hdr.recv_ts = wall_now();
    m.hdr.t0_cycles = rdtscp();
    sent_.answered(cl);
    static_cast<void>(order_sink_->push(m.hdr));
    learn_venue_id(cl, o.id);
    if (ev == "fill") end_order(cl);
  } else if (ev == "canceled") {
    emit_cancel_ack(*order_sink_, id_, inst, cl, o.id, o.filled_qty, venue_ts);
    end_order(cl);
  } else if (ev == "expired" || ev == "done_for_day") {
    OrderExpiredMsg m{};
    init_header(m, EventType::OrderExpired, inst, id_);
    m.cl_ord_id = cl;
    m.venue_order_id.assign(o.id);
    m.cum_qty = o.filled_qty;
    m.hdr.exch_ts = venue_ts;
    m.hdr.recv_ts = wall_now();
    m.hdr.t0_cycles = rdtscp();
    static_cast<void>(order_sink_->push(m.hdr));
    end_order(cl);
  } else if (ev == "rejected") {
    sent_.answered(cl);
    emit_order_reject(*order_sink_, id_, inst, cl, RejectReason::VenueReject, 0, "rejected");
    shadows_.erase(cl);
  } else if (ev == "replaced") {
    // The old order of a replace; the PATCH reply acknowledged the new one.
    end_order(cl);
    return;
  } else if (ev == "order_cancel_rejected") {
    emit_cancel_reject(*order_sink_, id_, inst, cl, RejectReason::VenueReject, 0, ev);
  } else if (ev == "order_replace_rejected") {
    Shadow* sh = shadows_.find(cl);
    if (sh == nullptr || !sh->replace_to.valid()) return;
    const ClientOrderId to = sh->replace_to;
    sh->replace_to = ClientOrderId{};
    sent_.answered(to);
    emit_order_reject(*order_sink_, id_, inst, to, RejectReason::VenueReject, 0, ev);
    shadows_.erase(to);
  } else {
    return;  // pending_new, accepted, pending_cancel, pending_replace, calculated, ...
  }
  ++stats_.order_events;
}

void AlpacaVenue::learn_venue_id(ClientOrderId id, std::string_view venue_id) {
  if (venue_id.empty()) return;
  remember_order(venue_id, id);
  Shadow* s = shadows_.find(id);
  if (s == nullptr) return;
  if (s->venue_id.empty()) s->venue_id.assign(venue_id);
  if (s->cancel_parked) {
    s->cancel_parked = false;
    send_cancel(id, s->instrument, s->venue_id.view());
  }
}

void AlpacaVenue::remember_order(std::string_view order_id, ClientOrderId cl) noexcept {
  const auto key = coinbase::order_key(order_id);
  if (!key || !cl.valid()) return;
  if (order_names_.size() >= decltype(order_names_)::kMaxSize) order_names_.clear();
  static_cast<void>(order_names_.assign(*key, cl));
}

ClientOrderId AlpacaVenue::name_of(std::string_view order_id) const noexcept {
  const auto key = coinbase::order_key(order_id);
  if (!key) return ClientOrderId{};
  const ClientOrderId* cl = order_names_.find(*key);
  return cl != nullptr ? *cl : ClientOrderId{};
}

void AlpacaVenue::end_order(ClientOrderId id) noexcept {
  const Shadow* s = shadows_.find(id);
  // A replace in flight keeps the shadow: its reply still needs the venue's order id.
  if (s != nullptr && !s->replace_to.valid()) shadows_.erase(id);
}

// ---- outbound ---------------------------------------------------------------------------------

void AlpacaVenue::on_wake() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void AlpacaVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

template <class Ring>
void AlpacaVenue::write_orders(Ring& ring) {
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

void AlpacaVenue::refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why) {
  if (cmd.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  } else {
    sent_.answered(cmd.cl_ord_id);
    emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  }
  ++stats_.order_events;
}

void AlpacaVenue::send_command(const OrderCommand& cmd) {
  note_taken(cmd);
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  if (!private_on()) return refuse(cmd, RejectReason::VenueKilled, "dry-run: orders disabled");
  if ((fatal_ || rest_hard_stopped_) && !is_cancel)
    return refuse(cmd, RejectReason::VenueKilled, "venue fatal");
  if (rest_ == nullptr) return refuse(cmd, RejectReason::VenueReject, "no order channel");
  if (is_cancel) {
    Shadow* s = shadows_.find(cmd.cl_ord_id);
    if (s == nullptr) {
      // An order of an earlier session the reconciliation found: cancelled by its venue id.
      if (cmd.venue_order_id == nullptr || cmd.venue_order_id->empty())
        return refuse(cmd, RejectReason::UnknownOrder, "cancel: order unknown");
      return send_cancel(cmd.cl_ord_id, cmd.instrument, cmd.venue_order_id->view());
    }
    if (s->ended) return refuse(cmd, RejectReason::VenueUnknownOrder, "cancel: order ended");
    if (s->venue_id.empty()) {
      s->cancel_parked = true;  // follows the venue's order id
      return;
    }
    return send_cancel(cmd.cl_ord_id, s->instrument, s->venue_id.view());
  }
  const RateCheck rc = rate_.check(1, now_ns(), true);
  if (!rc.ok()) {
    count_refusal(stats_, rc);
    return refuse(cmd, RejectReason::VenueRateLimit, refusal_text(rc));
  }
  if (cmd.kind == OrderCommandKind::Replace) return send_replace(cmd);
  send_new(cmd);
}

void AlpacaVenue::send_new(const OrderCommand& cmd) {
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
  const Cycles after_encode = rdtscp();
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const ClientOrderId cl = cmd.cl_ord_id;
  const bool queued = rest_->request(
      "POST",
      rq.target(),
      auth_headers(true),
      rq.payload(),
      [this, alive, gen, cl](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        handle_new_reply(cl, r);
      },
      RestLane::Orders);
  if (!queued) {
    ++stats_.order_send_failures;
    shadows_.erase(cl);
    return refuse(cmd, RejectReason::TransportFull, "rest queue full");
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  rate_.on_sent(1, now_ns(), true);
  sent_.sent_over_rest(cl);
  ++stats_.orders_sent;
}

void AlpacaVenue::send_replace(const OrderCommand& cmd) {
  Shadow* orig = shadows_.find(cmd.orig_cl_ord_id);
  if (orig == nullptr || orig->ended)
    return refuse(cmd, RejectReason::VenueUnknownOrder, "replace: order unknown or ended");
  if (orig->venue_id.empty() || orig->replace_to.valid())
    return refuse(cmd, RejectReason::VenueReject, "replace: order not acknowledged yet");
  OrderRequest rq;
  const Cycles before_encode = rdtscp();
  if (!AlpacaOrderEncoder::encode_replace(cmd, orig->venue_id.view(), rq))
    return refuse(cmd, RejectReason::VenueReject, "replace could not be encoded");
  Shadow next;
  next.instrument = orig->instrument;
  next.side = orig->side;
  next.qty = cmd.qty;
  next.sent_seq = sent_.last_seq();
  orig->replace_to = cmd.cl_ord_id;
  if (shadows_.assign(cmd.cl_ord_id, next) == nullptr) {
    if (Shadow* o = shadows_.find(cmd.orig_cl_ord_id)) o->replace_to = ClientOrderId{};
    shadow_overflow_.refused(cfg_.name, shadows_.size());
    return refuse(cmd, RejectReason::OrderTableFull, "order table full");
  }
  const Cycles after_encode = rdtscp();
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const ClientOrderId from = cmd.orig_cl_ord_id;
  const ClientOrderId to = cmd.cl_ord_id;
  const bool queued = rest_->request(
      "PATCH",
      rq.target(),
      auth_headers(true),
      rq.payload(),
      [this, alive, gen, from, to](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        handle_replace_reply(from, to, r);
      },
      RestLane::Orders);
  if (!queued) {
    ++stats_.order_send_failures;
    shadows_.erase(to);
    if (Shadow* o = shadows_.find(from)) o->replace_to = ClientOrderId{};
    return refuse(cmd, RejectReason::TransportFull, "rest queue full");
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  rate_.on_sent(1, now_ns(), true);
  sent_.sent_over_rest(to);
  ++stats_.replaces_sent;
}

void AlpacaVenue::send_cancel(ClientOrderId id, InstrumentId inst, std::string_view venue_id) {
  const std::string target = AlpacaOrderEncoder::order_path(venue_id);
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const bool queued = rest_->request(
      "DELETE",
      target,
      auth_headers(false),
      {},
      [this, alive, gen, id, inst](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        handle_cancel_reply(id, inst, r);
      },
      RestLane::Orders);
  if (!queued) {
    emit_cancel_reject(
        *order_sink_, id_, inst, id, RejectReason::TransportFull, 0, "rest queue full");
    ++stats_.order_events;
    return;
  }
  ++stats_.cancels_sent;
}

void AlpacaVenue::handle_new_reply(ClientOrderId id, const net::HttpResponse& r) {
  ++stats_.rest_requests;
  sent_.answered(id);
  const Shadow* shadow = shadows_.find(id);
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
  OrderRow o;
  if (r.ok() && decode_order(r.body, o).empty()) {
    emit_order_ack(*order_sink_, id_, inst, id, o.id);
    ++stats_.order_events;
    learn_venue_id(id, o.id);
    return;
  }
  ++stats_.rest_errors;
  const ApiError e = decode_error(r.body);
  const ErrorMapping m = map_error(r.status, e.code, e.message);
  emit_order_reject(
      *order_sink_,
      id_,
      inst,
      id,
      m.reason,
      static_cast<int>(e.code != 0 ? e.code : r.status),
      e.message.empty() ? std::string_view(r.body).substr(0, 120) : std::string_view(e.message));
  ++stats_.order_events;
  shadows_.erase(id);
  if (!m.known) FASTMM_LOG_WARN("{}: order refused: {}", cfg_.name, http_error(r));
  apply_action(m.action, r.status, e.message);
}

void AlpacaVenue::handle_replace_reply(ClientOrderId orig,
                                       ClientOrderId id,
                                       const net::HttpResponse& r) {
  ++stats_.rest_requests;
  sent_.answered(id);
  Shadow* from = shadows_.find(orig);
  if (from != nullptr && from->replace_to == id) from->replace_to = ClientOrderId{};
  const Shadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  OrderRow o;
  if (r.error == net::NetError::None && r.ok() && decode_order(r.body, o).empty()) {
    emit_order_ack(*order_sink_, id_, inst, id, o.id);
    ++stats_.order_events;
    learn_venue_id(id, o.id);
    // The old order is over; its "replaced" event may come before or after this reply.
    if (Shadow* old = shadows_.find(orig)) old->ended = true;
    end_order(orig);
    return;
  }
  ++stats_.rest_errors;
  const ApiError e = decode_error(r.body);
  const ErrorMapping m = r.error != net::NetError::None
                             ? ErrorMapping{RejectReason::VenueReject, VenueAction::Reconcile, true}
                             : map_error(r.status, e.code, e.message);
  // The old order works on unchanged (or is over: "order is not open", which its own event says).
  emit_order_reject(
      *order_sink_,
      id_,
      inst,
      id,
      m.reason,
      static_cast<int>(e.code != 0 ? e.code : r.status),
      r.error != net::NetError::None ? net::to_string(r.error) : std::string_view(e.message));
  ++stats_.order_events;
  shadows_.erase(id);
  if (!m.known) FASTMM_LOG_WARN("{}: replace refused: {}", cfg_.name, http_error(r));
  apply_action(m.action, r.status, e.message);
}

void AlpacaVenue::handle_cancel_reply(ClientOrderId id,
                                      InstrumentId inst,
                                      const net::HttpResponse& r) {
  ++stats_.rest_requests;
  Shadow* s = shadows_.find(id);
  if (r.error == net::NetError::None && r.status >= 200 && r.status < 300) {
    // Accepted: trade_updates reports the cancel with what filled. Without the stream the reply
    // is all there will be.
    if (!user_conn_.is_live() && s != nullptr) {
      emit_cancel_ack(*order_sink_, id_, inst, id, s->venue_id.view(), Qty{});
      end_order(id);
      ++stats_.order_events;
    }
    return;
  }
  ++stats_.rest_errors;
  const ApiError e = decode_error(r.body);
  const ErrorMapping m = r.error != net::NetError::None
                             ? ErrorMapping{RejectReason::VenueReject, VenueAction::None, true}
                             : map_error(r.status, e.code, e.message);
  emit_cancel_reject(*order_sink_,
                     id_,
                     inst,
                     id,
                     m.reason,
                     static_cast<int>(e.code != 0 ? e.code : r.status),
                     http_error(r));
  ++stats_.order_events;
  if (m.action != VenueAction::DisableInstrument) apply_action(m.action, r.status, e.message);
}

void AlpacaVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void AlpacaVenue::apply_action(VenueAction action, int code, std::string_view msg) {
  switch (action) {
    case VenueAction::None:
      break;
    case VenueAction::Backoff:
      rate_.cooldown(1'000'000'000, now_ns());
      break;
    case VenueAction::RateLimit:
      rate_.cooldown(kRateLimitCooldownNs, now_ns());
      ++stats_.rate_limit_cooldowns;
      FASTMM_LOG_WARN("{}: rate limited ({} {}); cooling down {} s",
                      cfg_.name,
                      code,
                      msg,
                      kRateLimitCooldownNs / 1'000'000'000);
      break;
    case VenueAction::ResyncClock:
      request_clock();
      break;
    case VenueAction::Reconcile:
      request_open_orders();
      break;
    case VenueAction::DisableInstrument:
      FASTMM_LOG_ERROR("{}: venue rejected a price or quantity rule ({} {})", cfg_.name, code, msg);
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

void AlpacaVenue::request_open_orders() {
  reconcile_.request();
}

bool AlpacaVenue::replay_executions() {
  return exec_replay_.run();
}

bool AlpacaVenue::fetch_snapshot(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  const std::string target = AlpacaOrderEncoder::open_orders_path(symbols());
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET",
      target,
      auth_headers(false),
      {},
      [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.current(generation)) return;
        ++stats_.rest_requests;
        std::vector<OrderRow> rows;
        const std::string err = r.ok() ? decode_orders(r.body, rows) : http_error(r);
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: open orders failed ({})", cfg_.name, err);
          reconcile_.fetched(generation, false);
          return;
        }
        for (const OrderRow& o : rows) {
          const InstrumentId inst = subscribed_instrument(o.symbol);
          if (!inst.valid()) continue;
          ReconcileMsg& m = reconcile_.add_order(inst);
          m.side = o.side == "sell" ? Side::Sell : Side::Buy;
          m.state = o.filled_qty.is_positive() ? OrderState::PartiallyFilled : OrderState::Live;
          if (const auto cl = decode_cl_ord_id(o.client_order_id)) {
            m.cl_ord_id = *cl;
            learn_venue_id(*cl, o.id);
          }
          m.venue_order_id.assign(o.id);
          m.price = o.limit_price;
          m.orig_qty = o.qty;
          m.cum_qty = o.filled_qty;
        }
        fetch_positions(generation);
      });
}

void AlpacaVenue::fetch_positions(std::uint64_t generation) {
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET",
      std::string(kPrefix) + "/positions",
      auth_headers(false),
      {},
      [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.current(generation)) return;
        ++stats_.rest_requests;
        std::vector<PositionRow> rows;
        const std::string err = r.ok() ? decode_positions(r.body, rows) : http_error(r);
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: positions failed ({})", cfg_.name, err);
          reconcile_.fetched(generation, false);
          return;
        }
        // Every subscribed symbol, flat ones as zero.
        for (InstrumentId id : subscribed_) {
          const std::string_view sym = symbols_->venue_symbol(id);
          const auto it = std::find_if(
              rows.begin(), rows.end(), [&](const PositionRow& p) { return p.symbol == sym; });
          if (it == rows.end()) {
            reconcile_.add_position(id, Qty{}, Price{});
          } else {
            reconcile_.add_position(id, it->qty, it->avg_px);
          }
        }
        reconcile_.fetched(generation, true);
      });
  if (!queued) reconcile_.fetched(generation, false);
}

void AlpacaVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const Shadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void AlpacaVenue::drop_shadow(ClientOrderId id) {
  shadows_.erase(id);
}

// ---- execution replay -------------------------------------------------------------------------

void AlpacaVenue::resume_executions(std::int64_t since_venue_ms,
                                    const std::vector<std::string>& known) {
  exec_replay_.resume(since_venue_ms, known);
}

bool AlpacaVenue::request_executions(std::int64_t since_venue_ms) {
  if (since_venue_ms > 0 && !exec_replay_.active()) exec_replay_.restart_from(since_venue_ms);
  return exec_replay_.run();
}

bool AlpacaVenue::replay_ready() const noexcept {
  return private_on() && connected_ && rest_ != nullptr && !rest_hard_stopped_ &&
         !subscribed_.empty();
}

bool AlpacaVenue::query_fills(const ReplayQuery& q) {
  if (rest_ == nullptr || rest_hard_stopped_) return false;
  const std::string target =
      AlpacaOrderEncoder::fills_path(q.start_ms, q.end_ms, q.page, kFillsPageRows);
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, auth_headers(false), {}, [this, alive, q](const net::HttpResponse& r) {
        if (alive.expired() || !exec_replay_.expects(q)) return;
        ++stats_.rest_requests;
        std::vector<FillRow> rows;
        const std::string err = r.ok() ? decode_fills(r.body, rows) : http_error(r);
        if (!err.empty()) {
          ++stats_.rest_errors;
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR("{}: fill activities failed ({})", cfg_.name, err);
          exec_replay_.failed(q);
          return;
        }
        ReplayPage<FillRow> page;
        page.more = rows.size() >= static_cast<std::size_t>(kFillsPageRows);
        if (!rows.empty()) page.next = rows.back().page_id;
        page.rows.reserve(rows.size());
        for (FillRow& f : rows) {
          const std::int64_t t = f.time_ms;
          std::string key = f.exec_id;
          page.rows.push_back({t, 0, std::move(key), std::move(f)});
        }
        exec_replay_.answer(q, std::move(page));
      });
  if (!queued) ++stats_.execution_query_errors;
  return queued;
}

bool AlpacaVenue::emit_replayed(const FillRow& f) {
  const InstrumentId inst = subscribed_instrument(f.symbol);
  if (!inst.valid()) return false;
  emit_replayed_fill(*order_sink_,
                     id_,
                     inst,
                     name_of(f.order_id),
                     f.order_id,
                     f.exec_id,
                     f.side,
                     f.price,
                     f.qty,
                     Notional{},
                     FeeAsset::Quote,
                     Liquidity::Unknown,
                     f.time_ms,
                     exec_replay_.emitting_unresolved() ? OrderFillMsg::kUnresolved : 0);
  ++stats_.order_events;
  ++stats_.executions_fetched;
  return true;
}

bool AlpacaVenue::lookup_order(const ReplayLookup& l) {
  if (rest_ == nullptr || rest_hard_stopped_ || l.order_id.empty()) return false;
  std::weak_ptr<int> alive = alive_;
  return rest_->request("GET",
                        AlpacaOrderEncoder::order_path(l.order_id),
                        auth_headers(false),
                        {},
                        [this, alive, l](const net::HttpResponse& r) {
                          if (alive.expired() || !connected_) return;
                          ++stats_.rest_requests;
                          OrderRow o;
                          if (!r.ok() || !decode_order(r.body, o).empty() || o.id != l.order_id) {
                            ++stats_.rest_errors;
                            exec_replay_.looked_up(l, LookupResult::Failed);
                            return;
                          }
                          const auto cl = decode_cl_ord_id(o.client_order_id);
                          if (!cl) {
                            exec_replay_.looked_up(l, LookupResult::NotOurs);
                            return;
                          }
                          remember_order(o.id, *cl);
                          exec_replay_.looked_up(l, LookupResult::Named);
                        });
}

// ---- control requests -----------------------------------------------------------------------

void AlpacaVenue::request_clock() {
  if (rest_ == nullptr || clock_pending_ || rest_hard_stopped_) return;
  clock_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  const std::int64_t sent_ms = wall_now().ns / kNsPerMs;
  const bool queued =
      rest_->request("GET",
                     std::string(kPrefix) + "/clock",
                     auth_headers(false),
                     {},
                     [this, alive, gen, sent_ms](const net::HttpResponse& r) {
                       if (alive.expired() || gen != generation_) return;
                       clock_pending_ = false;
                       ++stats_.rest_requests;
                       std::int64_t ms = 0;
                       bool open = false;
                       if (!r.ok() || !decode_clock(r.body, ms, open).empty()) {
                         ++stats_.rest_errors;
                         return;
                       }
                       // The venue stamped it somewhere in the round trip: its middle.
                       const std::int64_t mid = (sent_ms + wall_now().ns / kNsPerMs) / 2;
                       const std::int64_t offset = ms - mid;
                       clock_offset_ms_.store(offset);
                       clock_sync_ns_ = now_ns();
                       stats_.clock_offset_ms = offset;
                       if (offset > 1000 || offset < -1000)
                         FASTMM_LOG_WARN("{}: clock offset to venue is {} ms", cfg_.name, offset);
                     });
  if (!queued) clock_pending_ = false;
}

// Trading-stream loss: the open orders of the session's symbols, each cancelled.
void AlpacaVenue::cancel_all_async() {
  if (rest_ == nullptr || !private_on() || symbols_ == nullptr || subscribed_.empty()) return;
  std::weak_ptr<int> alive = alive_;
  const std::uint64_t gen = generation_;
  static_cast<void>(rest_->request(
      "GET",
      AlpacaOrderEncoder::open_orders_path(symbols()),
      auth_headers(false),
      {},
      [this, alive, gen](const net::HttpResponse& r) {
        if (alive.expired() || gen != generation_) return;
        ++stats_.rest_requests;
        std::vector<OrderRow> rows;
        if (!r.ok() || !decode_orders(r.body, rows).empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_ERROR("{}: cancel-all: open orders failed: {}", cfg_.name, http_error(r));
          return;
        }
        for (const OrderRow& o : rows) {
          static_cast<void>(rest_->request(
              "DELETE",
              AlpacaOrderEncoder::order_path(o.id),
              auth_headers(false),
              {},
              [this, alive](const net::HttpResponse& c) {
                if (alive.expired() || c.error == net::NetError::Canceled) return;
                ++stats_.rest_requests;
                if (c.status >= 300 && c.status != 404) {
                  ++stats_.rest_errors;
                  FASTMM_LOG_ERROR("{}: cancel-all: {}", cfg_.name, http_error(c));
                }
              },
              RestLane::Orders));
        }
        FASTMM_LOG_INFO("{}: cancel-all: {} open orders", cfg_.name, rows.size());
      },
      RestLane::Orders));
}

bool AlpacaVenue::cancel_all() {
  if (!private_on() || symbols_ == nullptr || subscribed_.empty()) return true;
  BlockingControl control(cfg_);
  const std::string list = AlpacaOrderEncoder::open_orders_path(symbols());
  std::size_t cancelled = 0;
  // Listed and cancelled until the list is empty, at most three rounds.
  for (int round = 0; round < 3; ++round) {
    const HttpReply reply = control.send("kill-switch open orders", [&](BlockingRequest& q) {
      q.method = "GET";
      q.target = list;
      q.headers = auth_headers(false);
      return true;
    });
    std::vector<OrderRow> rows;
    const std::string err = reply.ok() ? decode_orders(reply.body, rows) : reply_error(reply);
    if (!err.empty()) {
      FASTMM_LOG_ERROR("{}: kill-switch cancel-all: open orders failed: {}", cfg_.name, err);
      return false;
    }
    // An order cancelled a moment ago is listed until the venue has finished cancelling it.
    std::erase_if(rows, [](const OrderRow& o) { return o.status == "pending_cancel"; });
    if (rows.empty()) {
      FASTMM_LOG_INFO("{}: kill-switch cancel-all ok ({} orders)", cfg_.name, cancelled);
      return true;
    }
    for (const OrderRow& o : rows) {
      const HttpReply c = control.send("kill-switch cancel", [&](BlockingRequest& q) {
        q.method = "DELETE";
        q.target = AlpacaOrderEncoder::order_path(o.id);
        q.headers = auth_headers(false);
        return true;
      });
      if ((c.status >= 200 && c.status < 300) || c.status == 404 || c.status == 422) {
        ++cancelled;
      } else {
        FASTMM_LOG_ERROR(
            "{}: kill-switch cancel of {} failed: {}", cfg_.name, o.id, reply_error(c));
      }
    }
  }
  FASTMM_LOG_ERROR("{}: kill-switch cancel-all: orders still open after 3 rounds", cfg_.name);
  return false;
}

// ---- housekeeping -----------------------------------------------------------------------------

void AlpacaVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (private_on() && now - clock_sync_ns_ >= kClockResyncNs) request_clock();
  reconcile_.on_timer(now);
  if (private_on()) exec_replay_.on_timer(now);
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

void AlpacaVenue::publish_status() noexcept {
  stats_.shadows = shadows_.size();
  stats_.shadows_refused = shadow_overflow_.count();
  stats_.shadows_swept = reconcile_.shadows_swept();
  stats_.execution_queries = exec_replay_.replays();
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_.load(std::memory_order_relaxed);
  wire_.summarize(tsc_calibration(),
                  now_ns(),
                  stats_.wire_tick_to_trade,
                  stats_.order_encode,
                  stats_.order_send,
                  stats_.wire_1m,
                  stats_.wire_1h);
  published_.store(stats_);
  budget_pub_.store(budget_of(rate_, now_ns(), orders_taken_));
}

VenueStatus AlpacaVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

AlpacaVenueConfig make_alpaca_config(const VenueSection& v, bool dry_run) {
  AlpacaVenueConfig c;
  c.name = v.name;
  c.ws_url = v.ws_url.empty() ? std::string(kIexStream) : v.ws_url;
  c.rest_url = v.rest_url.empty() ? std::string(kPaperRest) : v.rest_url;
  c.api_key = v.api_key;
  c.api_secret = v.api_secret;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.dry_run = dry_run;
  const VenueExtras x(v.extra);
  c.ws_private_url = x.get("ws_private_url");
  if (c.ws_private_url.empty()) c.ws_private_url = std::string(kPaperStream);
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("dead_ms", c.dead_ms)));
  check_liveness(v.name, c.stale_ms, c.dead_ms);
  c.requests_per_minute = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("requests_per_minute", c.requests_per_minute)));
  c.trades = x.flag("trades", true);
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.orders.extended_hours = x.flag("extended_hours", false);
  const std::string tif = x.get("time_in_force");
  if (tif.empty() || tif == "day") {
    c.orders.resting_tif = Tif::Day;
  } else if (tif == "gtc") {
    c.orders.resting_tif = Tif::Gtc;
  } else {
    throw std::invalid_argument(
        fmt::format("venues.{}.time_in_force: \"{}\" is not day or gtc", v.name, tif));
  }
  if (!plain(c.api_key) || !plain(c.api_secret))
    throw std::invalid_argument(
        fmt::format("venues.{}.api_key: unexpected characters in the key or secret", v.name));
  // The live trading host with testnet = true (the default) is a mistake: paper is the test
  // environment.
  if (const auto u = net::Url::parse(c.rest_url); u && v.testnet && u->host == kLiveTradingHost)
    throw std::invalid_argument(
        fmt::format("venues.{}.testnet: true (the default) with {}, the live trading host; use "
                    "https://paper-api.alpaca.markets or set testnet = false",
                    v.name,
                    u->host));
  if (const auto u = net::Url::parse(c.ws_private_url);
      u && v.testnet && u->host == kLiveTradingHost)
    throw std::invalid_argument(fmt::format(
        "venues.{}.ws_private_url: the live trading stream with testnet = true", v.name));
  return c;
}

}  // namespace fastmm::venues::alpaca
