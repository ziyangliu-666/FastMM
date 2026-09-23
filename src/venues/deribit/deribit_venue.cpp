#include "fastmm/venues/deribit/deribit_venue.hpp"

#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/deribit/deribit_rest_decoder.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

namespace fastmm::venues::deribit {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kNsPerSec = 1'000'000'000;
constexpr std::int64_t kHousekeepingNs = kNsPerSec;

std::string_view kind_of(AssetClass a) noexcept {
  switch (a) {
    case AssetClass::Option:
      return "option";
    case AssetClass::Future:
    case AssetClass::Perpetual:
      return "future";
    case AssetClass::Spot:
      return "spot";
    default:
      return {};
  }
}

std::uint8_t decimals_of(Price tick) noexcept {
  if (!tick.is_positive()) return 8;
  std::int64_t raw = tick.raw;
  std::uint8_t d = 8;
  while (d > 0 && raw % 10 == 0) {
    raw /= 10;
    --d;
  }
  return d;
}

}  // namespace

// ---- reference data mapping ---------------------------------------------------------------------

std::string apply_instrument_info(const InstrumentInfo& f, Instrument& inst, TickSchedule& ticks) {
  if (!f.tick.is_positive() || !f.min_trade_amount.is_positive() || !f.contract_size.is_positive())
    return fmt::format("{} has a non-positive tick_size, min_trade_amount or contract_size",
                       f.name);
  if (f.kind == "option") {
    inst.asset_class = AssetClass::Option;
    if (f.option_type == "call") {
      inst.option_type = OptionType::Call;
    } else if (f.option_type == "put") {
      inst.option_type = OptionType::Put;
    } else {
      return fmt::format("{} has option_type '{}'", f.name, f.option_type);
    }
    if (!f.strike.is_positive()) return fmt::format("{} has no strike", f.name);
    inst.strike = f.strike;
    inst.expiry_ns = f.expiration_ms * kNsPerMs;
  } else if (f.kind == "future") {
    const bool perpetual = f.settlement_period == "perpetual";
    inst.asset_class = perpetual ? AssetClass::Perpetual : AssetClass::Future;
    inst.option_type = OptionType::None;
    inst.strike = Price{};
    inst.expiry_ns = perpetual ? 0 : f.expiration_ms * kNsPerMs;
  } else if (f.kind == "spot") {
    inst.asset_class = AssetClass::Spot;
    inst.option_type = OptionType::None;
    inst.expiry_ns = 0;
  } else {
    return fmt::format("{} has unsupported kind '{}'", f.name, f.kind);
  }
  const Qty lot = amount_to_contracts(f.min_trade_amount, f.contract_size);
  if (!lot.is_positive())
    return fmt::format("{}: min_trade_amount / contract_size is not positive", f.name);
  inst.tick = f.tick;
  inst.price_decimals = decimals_of(f.tick);
  inst.contract_multiplier = f.contract_size;
  inst.lot = lot;
  inst.min_qty = lot;
  inst.max_qty = Qty{};
  inst.min_notional = Notional{};
  inst.max_notional = Notional{};
  if (f.inverse()) {
    inst.flags = static_cast<std::uint8_t>(inst.flags | Instrument::kInverse);
  } else {
    inst.flags = static_cast<std::uint8_t>(inst.flags & ~Instrument::kInverse);
  }
  if (!f.is_active || (!f.state.empty() && f.state != "open"))
    inst.flags = static_cast<std::uint8_t>(inst.flags & ~Instrument::kEnabled);
  if (!f.base_currency.empty()) static_cast<void>(inst.base.assign(f.base_currency));
  if (!f.quote_currency.empty()) static_cast<void>(inst.quote.assign(f.quote_currency));
  ticks.base = f.tick;
  ticks.steps.clear();
  std::vector<TickStep> steps = f.tick_steps;
  std::sort(steps.begin(), steps.end(), [](const TickStep& a, const TickStep& b) {
    return a.above_price < b.above_price;
  });
  for (const TickStep& s : steps) {
    if (!s.tick.is_positive() || !ticks.steps.push_back(s))
      return fmt::format("{}: unsupported tick_size_steps", f.name);
  }
  return {};
}

// ---- construction ---------------------------------------------------------------------------

DeribitVenue::DeribitVenue(VenueId id, DeribitVenueConfig cfg)
    : id_(id),
      cfg_(std::move(cfg)),
      matching_credits_(
          CreditBucket::from_rate(cfg_.matching_engine_rate, cfg_.matching_engine_burst)) {
  if (cfg_.ws_private_url.empty()) cfg_.ws_private_url = cfg_.ws_url;
  std::memset(scratch_, 0, sizeof scratch_);
  std::memset(request_buf_, 0, sizeof request_buf_);
}

DeribitVenue::~DeribitVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps DeribitVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = true;
  c.user_stream = !cfg_.dry_run && cfg_.credentials.usable();
  return c;
}

net::ConnectionConfig DeribitVenue::ws_config(const std::string& url, bool authenticated) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  // With heartbeats on, the server sends a test_request about every interval, so a healthy but
  // quiet connection is never silent for much longer than that.
  const auto hb_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(10, cfg_.heartbeat_interval_s) * 1000);
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, hb_ms * 3);
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = 0;  // no documented WebSocket lifetime (connection-management article)
  c.manual_auth = authenticated;
  c.manual_subscribe = authenticated;
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> DeribitVenue::load_reference_data(InstrumentTable& instruments) {
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
      const HttpReply t = http.get("/public/get_time");
      std::int64_t server_ms = 0;
      if (t.ok() && decode_server_time(t.body, server_ms).empty())
        clock_offset_ms_.store(server_ms - wall_now().ns / kNsPerMs);
    }
    std::vector<InstrumentInfo> infos;
    bool complete = true;
    for (const std::string& currency : cfg_.currencies) {
      for (const char* kind : {"option", "future"}) {
        // public/get_instruments: 1 request/s sustained, burst 50 (rate-limits article).
        const HttpReply reply = http.get(fmt::format(
            "/public/get_instruments?currency={}&kind={}&expired=false", currency, kind));
        if (!reply.ok()) {
          const std::string why =
              reply.error.empty()
                  ? fmt::format("HTTP {} {}", reply.status, reply.body.substr(0, 200))
                  : reply.error;
          if (!cfg_.allow_offline_reference_data)
            return fail(fmt::format(
                "{}: get_instruments {} {} failed: {}", cfg_.name, currency, kind, why));
          FASTMM_LOG_WARN("{}: get_instruments {} {} failed ({})", cfg_.name, currency, kind, why);
          complete = false;
          continue;
        }
        if (const std::string err = decode_instruments(reply.body, infos); !err.empty())
          return fail(fmt::format("{}: {}", cfg_.name, err));
      }
    }
    for (Instrument* inst : mine) {
      const InstrumentInfo* f = nullptr;
      for (const InstrumentInfo& i : infos) {
        if (iequals_symbol(i.name, inst->symbol.view())) f = &i;
      }
      if (f == nullptr) {
        if (!complete && cfg_.allow_offline_reference_data) {
          FASTMM_LOG_WARN("{}: {} not in reference data; keeping the configured values",
                          cfg_.name,
                          inst->symbol.view());
          continue;
        }
        return fail(
            fmt::format("{}: {} is not an active instrument of currencies [{}] "
                        "(expired, or the currency is not configured)",
                        cfg_.name,
                        inst->symbol.view(),
                        fmt::join(cfg_.currencies, ",")));
      }
      if (const std::string err = apply_instrument_info(*f, *inst, ticks_[inst->id.value]);
          !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
      if (!inst->enabled())
        FASTMM_LOG_ERROR("{}: {} is not open for trading (state {}): disabled",
                         cfg_.name,
                         inst->symbol.view(),
                         f->state);
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
  FASTMM_LOG_INFO("{}: reference data loaded for {} instruments", cfg_.name, mine.size());
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void DeribitVenue::attach(const SymbolTable& symbols,
                          const InstrumentTable& instruments,
                          EventSink& md_sink,
                          EventSink& order_sink,
                          MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  for (const Instrument& inst : instruments) {
    // Instruments without reference data (offline start, or tests) keep their configured tick.
    if (inst.venue == id_ && !ticks_[inst.id.value].base.is_positive())
      ticks_[inst.id.value].base = inst.tick;
  }
  md_feed_ = std::make_unique<DeribitMdFeed>(
      symbols,
      instruments,
      id_,
      md_sink,
      ResubscribeRequester{&DeribitVenue::resubscribe_requester, this},
      cfg_.intervals);
  private_parser_ = std::make_unique<DeribitPrivateParser>(symbols, instruments, id_);
  encoder_ = std::make_unique<DeribitOrderEncoder>(
      symbols, instruments, std::span<const TickSchedule>(ticks_), cfg_.reject_post_only);
}

std::vector<std::string> DeribitVenue::private_channels() const {
  std::vector<std::string_view> kinds;
  for (InstrumentId id : subscribed_) {
    if (instruments_ == nullptr || !instruments_->contains(id)) continue;
    const std::string_view k = kind_of(instruments_->get(id).asset_class);
    if (!k.empty() && std::find(kinds.begin(), kinds.end(), k) == kinds.end()) kinds.push_back(k);
  }
  if (kinds.empty()) kinds = {"option", "future"};
  std::vector<std::string> out;
  for (const std::string& currency : cfg_.currencies) {
    for (std::string_view k : kinds) {
      out.push_back(fmt::format("user.orders.{}.{}.raw", k, currency));
      out.push_back(fmt::format("user.trades.{}.{}.raw", k, currency));
    }
  }
  return out;
}

void DeribitVenue::subscribe(std::span<const InstrumentId> instruments) {
  bool added = false;
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id);
    added = true;
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (added && connected_) {
    if (md_conn_.opened()) {
      md_conn_.close();
      open_md();
    }
    if (private_conn_.opened()) {
      private_conn_.close();
      open_private();
    }
  }
}

void DeribitVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("DeribitVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  const bool with_private = !cfg_.dry_run && cfg_.credentials.usable();
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    if (with_private) raw_private_.open(cfg_.record_raw_dir, cfg_.name, "private");
  }
  if (!cfg_.rest_url.empty()) {
    RestChannelConfig rc;
    rc.base_url = cfg_.rest_url;
    rc.ca_file = cfg_.ca_file;
    rc.insecure_tls = cfg_.insecure_tls;
    rc.timeout_ms = cfg_.http_timeout_ms;
    rc.max_queue = kMaxInstruments + 8;  // one cancel_all_by_instrument per instrument
    rest_ = std::make_unique<RestChannel>(reactor, rc);
  }
  open_md();
  if (with_private) open_private();
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, private={})", cfg_.name, cfg_.dry_run, with_private);
}

void DeribitVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  md_conn_.close();
  private_conn_.close();
  if (rest_) rest_->reset();
  md_feed_->on_disconnected();
  access_token_.clear();
  refresh_token_.clear();
  raw_md_.flush();
  raw_private_.flush();
  publish_status();
}

void DeribitVenue::open_md() {
  md_conn_.open(*reactor_, ws_config(cfg_.ws_url, false), md_handler_);
  md_conn_.connect();
}

void DeribitVenue::open_private() {
  private_conn_.open(*reactor_, ws_config(cfg_.ws_private_url, true), private_handler_);
  private_conn_.connect();
}

// ---- market data ------------------------------------------------------------------------------

void DeribitVenue::on_md_state(net::ConnState s) {
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
        if (DeribitBookSync* sync = md_feed_->sync(id))
          sync->resync(SyncReason::Explicit, now_ns());
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

void DeribitVenue::on_md_open() {
  std::size_t n = DeribitOrderEncoder::encode_set_heartbeat(
      kIdHeartbeat, cfg_.heartbeat_interval_s, request_buf_);
  if (n == 0 || !md_conn_.send_text(std::string_view(request_buf_, n)))
    FASTMM_LOG_ERROR("{}: could not enable heartbeats on the md channel", cfg_.name);
  for (const std::string& p : md_feed_->subscription_payloads()) {
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void DeribitVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const std::uint64_t sub_errors = md_feed_->stats().subscribe_errors;
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  const MdDecodeResult& last = md_feed_->last();
  if (last.frame == FrameKind::TestRequest) {
    const std::size_t n = DeribitOrderEncoder::encode_test(kIdTest, request_buf_);
    static_cast<void>(md_conn_.send_text(std::string_view(request_buf_, n)));
    return;
  }
  if (md_feed_->stats().subscribe_errors != sub_errors)
    FASTMM_LOG_ERROR(
        "{}: market-data subscription incomplete or rejected: {}", cfg_.name, t.substr(0, 300));
  if (st == ParseStatus::Error && md_feed_->stats().subscribe_errors == sub_errors)
    FASTMM_LOG_WARN("{}: md request {} failed: {} {}",
                    cfg_.name,
                    last.rpc.id,
                    last.rpc.error_code,
                    last.rpc.error_message);
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  }
}

void DeribitVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;  // the reconnect subscribes everything again
  for (const std::string& p : md_feed_->resubscribe_payloads(id))
    static_cast<void>(md_conn_.send_text(p));
  FASTMM_LOG_INFO(
      "{}: resubscribing {} for a fresh snapshot", cfg_.name, symbols_->venue_symbol(id));
}

// ---- private connection ---------------------------------------------------------------------

void DeribitVenue::send_auth(std::int64_t id) {
  if (!cfg_.credentials.usable()) return;
  const std::size_t n = DeribitOrderEncoder::encode_auth(
      id, cfg_.credentials.client_id, cfg_.credentials.client_secret.value, request_buf_);
  auth_in_flight_ = true;
  const bool ok = n > 0 && private_conn_.send_text(std::string_view(request_buf_, n));
  std::memset(request_buf_, 0, n);  // the request carries the client secret
  if (!ok) {
    auth_in_flight_ = false;
    FASTMM_LOG_ERROR("{}: could not send public/auth", cfg_.name);
  }
}

void DeribitVenue::send_private(std::string_view frame, std::string_view what) {
  if (frame.empty() || !private_conn_.send_text(frame))
    FASTMM_LOG_ERROR("{}: could not send {}", cfg_.name, what);
}

void DeribitVenue::on_private_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating) send_auth(kIdAuth);
  const ConnState mapped = map_conn_state(s);
  stats_.user = channel_state(s);
  stats_.order = stats_.user;
  if (mapped == private_state_) return;
  const ConnState prev = private_state_;
  private_state_ = mapped;
  if (mapped == ConnState::Live) {
    const bool reconnected = private_was_live_ && prev != ConnState::Stale;
    private_was_live_ = true;
    // Stale is not reported for this channel, so neither is the return from it.
    if (prev != ConnState::Stale) emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
    FASTMM_LOG_INFO("{}: private channel -> Live", cfg_.name);
    drain_outbound();
    // 6.7: reconcile after a reconnect (orders may have been cancelled meanwhile).
    if (reconnected) request_open_orders();
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: private channel lost", cfg_.name);
    access_token_.clear();
    user_channels_ok_ = false;
    auth_in_flight_ = false;
    refresh_at_ns_ = 0;
    reconcile_records_.clear();
    reconcile_pending_ = 0;
    // disconnect() clears connected_ before closing: a requested shutdown runs cancel_all().
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void DeribitVenue::on_private_open() {
  // Authenticated: heartbeats, cancel-on-disconnect for this connection, user channels.
  std::size_t n = DeribitOrderEncoder::encode_set_heartbeat(
      kIdHeartbeat, cfg_.heartbeat_interval_s, request_buf_);
  send_private(std::string_view(request_buf_, n), "public/set_heartbeat");
  if (cfg_.cancel_on_disconnect) {
    n = DeribitOrderEncoder::encode_enable_cancel_on_disconnect(
        kIdCancelOnDisconnect, access_token_, request_buf_);
    send_private(std::string_view(request_buf_, n), "private/enable_cancel_on_disconnect");
  }
  const std::vector<std::string> channels = private_channels();
  private_channels_requested_ = static_cast<std::uint32_t>(channels.size());
  n = DeribitOrderEncoder::encode_subscribe(
      kIdPrivateSubscribe, true, channels, access_token_, request_buf_);
  send_private(std::string_view(request_buf_, n), "private/subscribe");
}

ClientOrderId DeribitVenue::current_id(ClientOrderId label) const noexcept {
  if (const ClientOrderId* cur = aliases_.find(label)) return *cur;
  return label;
}

void DeribitVenue::forget_order(ClientOrderId id) noexcept {
  if (const OrderShadow* s = shadows_.find(id)) {
    if (s->label.valid() && s->label != id) aliases_.erase(s->label);
  }
  shadows_.erase(id);
}

void DeribitVenue::on_private_text(std::string_view t, std::int64_t ts) {
  if (raw_private_.enabled()) {
    // public/auth responses carry the access and refresh tokens: never write them to disk.
    constexpr std::string_view kRedacted = R"({"redacted":"public/auth response"})";
    raw_private_.record(ts, t.find("\"access_token\"") == std::string_view::npos ? t : kRedacted);
  }
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
          auto* m = reinterpret_cast<OrderAckMsg*>(h);
          m->cl_ord_id = current_id(m->cl_ord_id);
          if (OrderShadow* s = shadows_.find(m->cl_ord_id)) {
            s->venue_order_id = m->venue_order_id;
            if (s->edit_pending) continue;  // the edit response acks the new id
          }
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
        case EventType::OrderFill: {
          auto* m = reinterpret_cast<OrderFillMsg*>(h);
          m->cl_ord_id = current_id(m->cl_ord_id);
          if (OrderShadow* s = shadows_.find(m->cl_ord_id)) {
            s->filled = s->filled + m->qty;
            m->cum_qty = s->filled < s->qty ? s->filled : s->qty;
            m->leaves_qty = s->qty > s->filled ? s->qty - s->filled : Qty{};
            if (!m->leaves_qty.is_positive()) forget_order(m->cl_ord_id);
          }
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
  switch (r.frame) {
    case FrameKind::TestRequest: {
      const std::size_t n = DeribitOrderEncoder::encode_test(kIdTest, request_buf_);
      static_cast<void>(private_conn_.send_text(std::string_view(request_buf_, n)));
      return;
    }
    case FrameKind::Response: {
      if (!r.rpc.id_text.empty()) {
        if (const auto req = parse_request_id(r.rpc.id_text)) {
          handle_order_response(req->first, req->second, r);
        } else {
          FASTMM_LOG_WARN("{}: response for unknown request '{}'", cfg_.name, r.rpc.id_text);
        }
        return;
      }
      const std::int64_t id = r.rpc.id;
      if (id >= kIdOpenOrdersBase &&
          id < kIdOpenOrdersBase + static_cast<std::int64_t>(cfg_.currencies.size())) {
        if (r.rpc.is_error)
          FASTMM_LOG_WARN("{}: get_open_orders_by_currency failed: {} {}",
                          cfg_.name,
                          r.rpc.error_code,
                          r.rpc.error_message);
        handle_open_orders_response(
            static_cast<std::size_t>(id - kIdOpenOrdersBase), t, r.rpc.is_error);
        return;
      }
      handle_control_response(r);
      return;
    }
    default:
      break;
  }
  if (r.status == ParseStatus::Malformed) FASTMM_LOG_WARN("{}: malformed private frame", cfg_.name);
}

void DeribitVenue::handle_control_response(const PrivateDecodeResult& r) {
  const int code = static_cast<int>(r.rpc.error_code);
  switch (r.rpc.id) {
    case kIdAuth:
    case kIdReauth: {
      auth_in_flight_ = false;
      if (r.rpc.is_error || !r.auth.present || r.auth.access_token.empty()) {
        FASTMM_LOG_ERROR("{}: public/auth failed: {} {}", cfg_.name, code, r.rpc.error_message);
        const ErrorMapping m = map_error(code, r.rpc.error_message);
        apply_action(m.action == VenueAction::None ? VenueAction::Fatal : m.action,
                     code,
                     r.rpc.error_message);
        return;
      }
      access_token_.assign(r.auth.access_token);
      refresh_token_.assign(r.auth.refresh_token);
      refresh_at_ns_ =
          r.auth.expires_in > 0
              ? now_ns() + static_cast<std::int64_t>(static_cast<double>(r.auth.expires_in) *
                                                     cfg_.token_refresh_fraction *
                                                     static_cast<double>(kNsPerSec))
              : 0;
      FASTMM_LOG_INFO("{}: authenticated (token lifetime {} s)", cfg_.name, r.auth.expires_in);
      if (r.rpc.id == kIdAuth) private_conn_.auth_done();
      return;
    }
    case kIdRefresh:
      auth_in_flight_ = false;
      if (r.rpc.is_error || !r.auth.present || r.auth.access_token.empty()) {
        FASTMM_LOG_WARN("{}: token refresh failed ({} {}); re-authenticating",
                        cfg_.name,
                        code,
                        r.rpc.error_message);
        send_auth(kIdReauth);
        return;
      }
      access_token_.assign(r.auth.access_token);
      refresh_token_.assign(r.auth.refresh_token);
      refresh_at_ns_ =
          r.auth.expires_in > 0
              ? now_ns() + static_cast<std::int64_t>(static_cast<double>(r.auth.expires_in) *
                                                     cfg_.token_refresh_fraction *
                                                     static_cast<double>(kNsPerSec))
              : 0;
      return;
    case kIdPrivateSubscribe:
      if (r.rpc.is_error) {
        FASTMM_LOG_ERROR(
            "{}: private/subscribe failed: {} {}", cfg_.name, code, r.rpc.error_message);
        apply_action(map_error(code, r.rpc.error_message).action, code, r.rpc.error_message);
        return;
      }
      user_channels_ok_ = r.result_items >= private_channels_requested_;
      if (!user_channels_ok_)
        FASTMM_LOG_WARN(
            "{}: only {} of {} user channels subscribed; cancels are acknowledged "
            "from request responses",
            cfg_.name,
            r.result_items,
            private_channels_requested_);
      private_conn_.subscribe_done();
      return;
    case kIdCancelOnDisconnect:
      if (r.rpc.is_error) {
        FASTMM_LOG_WARN(
            "{}: enable_cancel_on_disconnect failed: {} {}", cfg_.name, code, r.rpc.error_message);
      }
      return;
    case kIdHeartbeat:
    case kIdTest:
    default:
      if (r.rpc.is_error)
        FASTMM_LOG_WARN(
            "{}: request {} failed: {} {}", cfg_.name, r.rpc.id, code, r.rpc.error_message);
      return;
  }
}

void DeribitVenue::handle_order_response(RequestKind kind,
                                         ClientOrderId id,
                                         const PrivateDecodeResult& r) {
  OrderShadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  const int code = static_cast<int>(r.rpc.error_code);
  const ErrorMapping m = map_error(code, r.rpc.error_message);
  if (r.rpc.is_error) {
    if (needs_reauth(code) && !auth_in_flight_) send_auth(kIdReauth);
    if (m.action == VenueAction::RateLimit) matching_credits_.drain(now_ns());
  }
  switch (kind) {
    case RequestKind::New:
      if (!r.rpc.is_error) {
        if (shadow != nullptr && r.order.present) shadow->venue_order_id.assign(r.order.order_id);
        if (cfg_.emit_ack_from_response)
          emit_order_ack(*order_sink_, id_, inst, id, r.order.order_id);
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, code, r.rpc.error_message);
        forget_order(id);
        apply_action(m.action, code, r.rpc.error_message);
      }
      ++stats_.order_events;
      return;
    case RequestKind::Cancel:
      if (!r.rpc.is_error) {
        if (r.result_int == 0) {
          // private/cancel_by_label answers the number of cancelled orders.
          emit_cancel_reject(*order_sink_,
                             id_,
                             inst,
                             id,
                             RejectReason::VenueUnknownOrder,
                             0,
                             "no open order with this label");
          ++stats_.order_events;
        } else if (!user_channels_ok_) {
          // Without the user.orders channel the response is all we will get.
          const Qty cum = shadow != nullptr ? shadow->filled : Qty{};
          emit_cancel_ack(*order_sink_, id_, inst, id, r.order.order_id, cum);
          forget_order(id);
          ++stats_.order_events;
        }
      } else {
        emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, code, r.rpc.error_message);
        apply_action(m.action, code, r.rpc.error_message);
        ++stats_.order_events;
      }
      return;
    case RequestKind::Replace: {
      const ClientOrderId orig = shadow != nullptr ? shadow->replaces : ClientOrderId{};
      if (OrderShadow* o = orig.valid() ? shadows_.find(orig) : nullptr) o->edit_pending = false;
      if (!r.rpc.is_error) {
        if (shadow != nullptr) {
          const ClientOrderId label = shadow->label;
          if (r.order.present) shadow->venue_order_id.assign(r.order.order_id);
          if (label.valid() && label != id) aliases_.assign(label, id);
          if (orig.valid() && orig != id) shadows_.erase(orig);
        }
        emit_order_ack(*order_sink_, id_, inst, id, r.order.order_id);
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, code, r.rpc.error_message);
        shadows_.erase(id);
        apply_action(m.action, code, r.rpc.error_message);
      }
      ++stats_.order_events;
      return;
    }
    case RequestKind::Other:
      return;
  }
}

// ---- outbound ---------------------------------------------------------------------------------

void DeribitVenue::on_wake() {
  drain_outbound();
}

// The orders the ring holds go out as one write of all their WebSocket frames.
template <class Ring>
void DeribitVenue::write_orders(Ring& ring) {
  drain_outbound_coalesced(
      ring,
      wire_,
      [this] {
        private_conn_.cork();
        batch_.clear();
      },
      [this](const EventHeader& h) {
        if (const auto cmd = OrderCommand::from(h)) {
          sent_.note(*cmd);
          send_command(*cmd);
        }
      },
      [this] {
        if (private_conn_.uncork()) return true;
        fail_batch();
        return false;
      });
}

void DeribitVenue::fail_batch() {
  for (const BatchedOrders::Entry& e : batch_.entries()) {
    ++stats_.order_send_failures;
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
      if (e.kind == OrderCommandKind::Replace) {
        if (OrderShadow* orig = shadows_.find(e.orig_cl_ord_id)) orig->edit_pending = false;
      }
    }
  }
  batch_.clear();
}

void DeribitVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void DeribitVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

void DeribitVenue::send_command(const OrderCommand& cmd) {
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
  if (cfg_.dry_run) {
    refuse(RejectReason::VenueKilled, "dry-run: orders disabled");
    return;
  }
  // A venue-fatal error stops new orders, never cancels: the kill path's whole remedy is to
  // cancel, so a cancel goes out as long as the private connection carries it.
  if (fatal_ && !is_cancel) {
    refuse(RejectReason::VenueKilled, "venue fatal");
    return;
  }
  if (!private_conn_.is_live() || access_token_.empty()) {
    refuse(RejectReason::VenueReject, "private channel down");
    return;
  }
  OrderShadow* shadow = nullptr;
  OrderShadow* orig = nullptr;
  switch (cmd.kind) {
    case OrderCommandKind::New:
      if (!matching_credits_.try_consume(CreditBucket::kRequestCost, now)) {
        ++stats_.rate_limit_cooldowns;
        refuse(RejectReason::VenueRateLimit, "local rate limit");
        return;
      }
      shadows_.assign(cmd.cl_ord_id,
                      OrderShadow{cmd.instrument,
                                  cmd.side,
                                  cmd.type,
                                  cmd.tif,
                                  cmd.cl_ord_id,
                                  {},
                                  {},
                                  cmd.qty,
                                  {},
                                  false});
      shadow = shadows_.find(cmd.cl_ord_id);
      break;
    case OrderCommandKind::Replace: {
      orig = shadows_.find(cmd.orig_cl_ord_id);
      if (orig == nullptr) {
        refuse(RejectReason::UnknownOrder, "edit: original unknown");
        return;
      }
      const bool have_id = (cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty()) ||
                           !orig->venue_order_id.empty();
      if (!have_id) {
        refuse(RejectReason::VenueReject, "edit before the order was acknowledged");
        return;
      }
      if (!matching_credits_.try_consume(CreditBucket::kRequestCost, now)) {
        ++stats_.rate_limit_cooldowns;
        refuse(RejectReason::VenueRateLimit, "local rate limit");
        return;
      }
      OrderShadow copy = *orig;
      copy.replaces = cmd.orig_cl_ord_id;
      copy.qty = cmd.qty;
      copy.edit_pending = false;
      shadows_.assign(cmd.cl_ord_id, copy);
      orig = shadows_.find(cmd.orig_cl_ord_id);  // assign may have moved entries
      if (orig != nullptr) orig->edit_pending = true;
      shadow = shadows_.find(cmd.cl_ord_id);
      break;
    }
    case OrderCommandKind::Cancel:
      matching_credits_.consume(CreditBucket::kRequestCost, now);  // never refused locally
      shadow = shadows_.find(cmd.cl_ord_id);
      break;
  }
  const Cycles before_encode = rdtscp();
  const std::size_t n = encoder_->encode(cmd, shadow, access_token_, request_buf_);
  const Cycles after_encode = rdtscp();
  auto undo = [&] {
    if (cmd.kind == OrderCommandKind::New) shadows_.erase(cmd.cl_ord_id);
    if (cmd.kind == OrderCommandKind::Replace) {
      shadows_.erase(cmd.cl_ord_id);
      if (OrderShadow* o = shadows_.find(cmd.orig_cl_ord_id)) o->edit_pending = false;
    }
  };
  if (n == 0) {
    undo();
    ++stats_.order_send_failures;
    refuse(RejectReason::VenueReject, "request could not be encoded");
    return;
  }
  if (!private_conn_.send_text(std::string_view(request_buf_, n))) {
    undo();
    ++stats_.order_send_failures;
    refuse(RejectReason::TransportFull, "send failed");
    return;
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  batch_.note(cmd);
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

// First HardStop / Fatal error: the engine trips this venue's kill switch (quotes pulled,
// new orders refused by risk); the other venues keep trading.
void DeribitVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void DeribitVenue::apply_action(VenueAction action, int code, std::string_view msg) {
  switch (action) {
    case VenueAction::None:
    case VenueAction::ResyncClock:  // client_credentials auth carries no timestamp
      break;
    case VenueAction::Backoff:
      matching_credits_.drain(now_ns());
      break;
    case VenueAction::RateLimit:
      matching_credits_.drain(now_ns());
      ++stats_.rate_limit_cooldowns;
      FASTMM_LOG_WARN("{}: rate limited ({} {})", cfg_.name, code, msg);
      break;
    case VenueAction::Reconcile:
      request_open_orders();
      break;
    case VenueAction::DisableInstrument:
      FASTMM_LOG_ERROR("{}: venue rejected an instrument rule ({} {}); check the instrument",
                       cfg_.name,
                       code,
                       msg);
      break;
    case VenueAction::HardStop:
    case VenueAction::Fatal:
      fatal_ = true;
      FASTMM_LOG_ERROR("{}: fatal venue error ({} {}); order entry disabled", cfg_.name, code, msg);
      trip_venue_kill(action == VenueAction::HardStop ? KillReason::VenueHardStop
                                                      : KillReason::VenueFatal);
      break;
  }
}

// ---- control requests -----------------------------------------------------------------------

void DeribitVenue::request_open_orders() {
  if (cfg_.dry_run || !connected_ || !private_conn_.is_live() || access_token_.empty()) return;
  if (reconcile_pending_ > 0) return;  // one reconciliation at a time
  reconcile_watermark_ = sent_.value();
  reconcile_records_.clear();
  reconcile_failed_ = false;
  for (std::size_t i = 0; i < cfg_.currencies.size(); ++i) {
    const std::size_t n =
        DeribitOrderEncoder::encode_open_orders(kIdOpenOrdersBase + static_cast<std::int64_t>(i),
                                                cfg_.currencies[i],
                                                access_token_,
                                                request_buf_);
    if (n > 0 && private_conn_.send_text(std::string_view(request_buf_, n))) {
      ++reconcile_pending_;
    } else {
      reconcile_failed_ = true;
    }
  }
  if (reconcile_pending_ == 0) FASTMM_LOG_WARN("{}: could not request open orders", cfg_.name);
}

void DeribitVenue::handle_open_orders_response(std::size_t currency_index,
                                               std::string_view json,
                                               bool error) {
  if (reconcile_pending_ == 0) return;
  --reconcile_pending_;
  if (error) {
    reconcile_failed_ = true;
  } else {
    const ParseStatus st = private_parser_->decode_open_orders(json, [&](const OpenOrderRecord& o) {
      const InstrumentId inst = symbols_->find(id_, o.instrument_name);
      if (!inst.valid() || !instruments_->contains(inst)) return;
      const Qty csize = instruments_->get(inst).contract_multiplier;
      ReconcileMsg m{};
      init_header(m, EventType::Reconcile, inst, id_);
      m.kind = ReconcileMsg::Kind::OpenOrder;
      m.side = o.direction == "sell" ? Side::Sell : Side::Buy;
      m.cum_qty = amount_to_contracts(o.filled_amount, csize);
      m.state = m.cum_qty.is_positive() ? OrderState::PartiallyFilled : OrderState::Live;
      if (const auto cl = decode_cl_ord_id(o.label)) m.cl_ord_id = current_id(*cl);
      m.venue_order_id.assign(o.order_id);
      m.price = o.price;
      m.orig_qty = amount_to_contracts(o.amount, csize);
      m.hdr.recv_ts = wall_now();
      reconcile_records_.push_back(m);
    });
    if (st != ParseStatus::Ok) reconcile_failed_ = true;
  }
  static_cast<void>(currency_index);
  if (reconcile_pending_ > 0) return;
  if (reconcile_failed_) {
    FASTMM_LOG_WARN(
        "{}: open orders could not be fetched for every currency; reconciliation skipped",
        cfg_.name);
    reconcile_records_.clear();
    return;
  }
  ReconcileMsg begin{};
  init_header(begin, EventType::Reconcile, InstrumentId::invalid(), id_);
  begin.kind = ReconcileMsg::Kind::Begin;
  SentWatermark::stamp(begin, reconcile_watermark_);
  begin.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(begin.hdr));
  for (const ReconcileMsg& m : reconcile_records_) static_cast<void>(order_sink_->push(m.hdr));
  ReconcileMsg end{};
  init_header(end, EventType::Reconcile, InstrumentId::invalid(), id_);
  end.kind = ReconcileMsg::Kind::End;
  end.hdr.recv_ts = wall_now();
  static_cast<void>(order_sink_->push(end.hdr));
  FASTMM_LOG_INFO("{}: reconciled {} open orders", cfg_.name, reconcile_records_.size());
  reconcile_records_.clear();
}

void DeribitVenue::cancel_all_async() {
  if (rest_ == nullptr || !cfg_.credentials.usable()) return;
  const std::string auth = DeribitOrderEncoder::basic_auth_header(cfg_.credentials);
  for (InstrumentId id : subscribed_) {
    const std::string symbol(symbols_->venue_symbol(id));
    std::weak_ptr<int> alive = alive_;
    const bool queued = rest_->request(
        "GET",
        DeribitOrderEncoder::rest_cancel_all_target(symbol),
        auth,
        {},
        [this, alive, symbol](const net::HttpResponse& r) {
          if (alive.expired() || r.error == net::NetError::Canceled) return;
          ++stats_.rest_requests;
          RpcEnvelope env;
          if (r.error != net::NetError::None || !decode_envelope(r.body, env) || !env.has_result) {
            ++stats_.rest_errors;
            FASTMM_LOG_ERROR("{}: cancel_all_by_instrument {} failed: status={} code={} {}",
                             cfg_.name,
                             symbol,
                             r.status,
                             env.error_code,
                             env.message);
          }
        });
    if (!queued)
      FASTMM_LOG_ERROR("{}: could not queue cancel_all_by_instrument {}", cfg_.name, symbol);
  }
}

bool DeribitVenue::cancel_all() {
  if (cfg_.dry_run || !cfg_.credentials.usable() || symbols_ == nullptr) return true;
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  bool all_ok = true;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    const std::string auth = DeribitOrderEncoder::basic_auth_header(cfg_.credentials);
    for (InstrumentId id : subscribed_) {
      const std::string_view symbol = symbols_->venue_symbol(id);
      const HttpReply reply = http.get(DeribitOrderEncoder::rest_cancel_all_target(symbol), auth);
      RpcEnvelope env;
      if (!reply.error.empty() || !decode_envelope(reply.body, env) || !env.has_result) {
        all_ok = false;
        FASTMM_LOG_ERROR("{}: kill-switch cancel_all_by_instrument {} failed: status={} code={} {}",
                         cfg_.name,
                         symbol,
                         reply.status,
                         env.error_code,
                         reply.error.empty() ? env.message : reply.error);
      } else {
        FASTMM_LOG_INFO("{}: kill-switch cancel_all_by_instrument {} ok", cfg_.name, symbol);
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

void DeribitVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (refresh_at_ns_ != 0 && now >= refresh_at_ns_ && private_conn_.is_live() && !auth_in_flight_ &&
      !refresh_token_.empty()) {
    refresh_at_ns_ = 0;
    const std::size_t n =
        DeribitOrderEncoder::encode_auth_refresh(kIdRefresh, refresh_token_, request_buf_);
    auth_in_flight_ = n > 0 && private_conn_.send_text(std::string_view(request_buf_, n));
    std::memset(request_buf_, 0, n);
  }
  publish_status();
  raw_md_.flush();
  raw_private_.flush();
  if (reactor_ != nullptr && housekeeping_timer_ == net::kInvalidTimer) {
    std::weak_ptr<int> alive = alive_;
    housekeeping_timer_ = reactor_->add_timer_after(kHousekeepingNs, [this, alive] {
      if (alive.expired()) return;
      housekeeping_timer_ = net::kInvalidTimer;
      on_timer(now_ns());
    });
  }
}

void DeribitVenue::publish_status() noexcept {
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.clock_offset_ms = clock_offset_ms_.load(std::memory_order_relaxed);
  wire_.summarize(
      tsc_calibration(), stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
}

VenueStatus DeribitVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

DeribitVenueConfig make_deribit_config(const VenueSection& v, bool dry_run) {
  DeribitVenueConfig c;
  c.name = v.name;
  c.ws_url = v.ws_url;
  c.rest_url = v.rest_url;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.supports_replace = v.supports_replace;
  c.dry_run = dry_run;
  c.credentials.client_id = v.api_key;
  c.credentials.client_secret.value = v.api_secret;
  const VenueExtras x(v.extra);
  auto extra = [&](const char* key) { return x.get(key); };
  auto extra_bool = [&](const char* key, bool def) { return x.flag(key, def); };
  auto extra_int = [&](const char* key, std::int64_t def) { return x.integer(key, def); };
  c.ws_private_url = extra("ws_private_url");
  if (c.ws_private_url.empty()) c.ws_private_url = v.ws_api_url.empty() ? v.ws_url : v.ws_api_url;
  // "BTC", "BTC,ETH" or a stringified TOML array ["BTC", "ETH"].
  if (const std::string list = extra("currencies"); !list.empty()) {
    std::vector<std::string> currencies;
    std::string cur;
    for (const char ch : list + ",") {
      if (ch == ',') {
        if (!cur.empty()) currencies.push_back(cur);
        cur.clear();
      } else if (ch != '[' && ch != ']' && ch != '"' && ch != '\'' && ch != ' ') {
        cur += ascii_upper(ch);
      }
    }
    if (!currencies.empty()) c.currencies = std::move(currencies);
  }
  if (const std::string s = extra("book_interval"); !s.empty()) c.intervals.book = s;
  if (const std::string s = extra("ticker_interval"); !s.empty()) c.intervals.ticker = s;
  if (const std::string s = extra("trades_interval"); !s.empty()) c.intervals.trades = s;
  c.heartbeat_interval_s =
      std::max<std::int64_t>(10, extra_int("heartbeat_interval_s", c.heartbeat_interval_s));
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, extra_int("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, extra_int("dead_ms", c.dead_ms)));
  c.reject_post_only = extra_bool("reject_post_only", c.reject_post_only);
  c.cancel_on_disconnect = extra_bool("cancel_on_disconnect", c.cancel_on_disconnect);
  c.cancel_on_order_channel_loss =
      extra_bool("cancel_on_order_channel_loss", c.cancel_on_order_channel_loss);
  c.allow_offline_reference_data = extra_bool("allow_offline_reference_data", false);
  c.emit_ack_from_response = extra_bool("emit_ack_from_response", true);
  c.matching_engine_rate =
      std::max<std::int64_t>(1, extra_int("matching_engine_rate", c.matching_engine_rate));
  c.matching_engine_burst =
      std::max<std::int64_t>(1, extra_int("matching_engine_burst", c.matching_engine_burst));
  return c;
}

}  // namespace fastmm::venues::deribit
