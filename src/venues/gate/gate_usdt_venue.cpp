#include "fastmm/venues/gate/gate_usdt_venue.hpp"

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

namespace fastmm::venues::gate {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kClockResyncNs = 30LL * 60 * 1'000'000'000;
constexpr std::int64_t kDefaultCooldownNs = 10'000'000'000;
// GET /orders?status=open: 100 rows a page (the documented maximum per request).
constexpr int kOpenOrdersPage = 100;
constexpr std::size_t kMaxReconcilePages = 40;
// GET /my_trades_timerange: 1000 rows a page; one day a window, 30 days of history asked for.
constexpr int kTradesPage = 1000;
constexpr std::int64_t kTradesWindowMs = 24LL * 3600 * 1000;
constexpr std::int64_t kTradesHistoryMs = 30LL * 24 * 3600 * 1000;
constexpr std::size_t kMaxTradePagesPerWindow = 20;
constexpr std::size_t kMaxTradeRequests = 100;
constexpr std::int64_t kPositionSettleNs = 1'000'000'000;
constexpr std::string_view kLoginPrivate = "login-p";
constexpr std::string_view kLoginTrade = "login-t";
constexpr std::string_view kUpgradeHeaders = "X-Gate-Size-Decimal: 1\r\n";

// "NVDA_USDT" -> "NVDA"
std::string_view base_of(std::string_view contract) noexcept {
  const std::size_t us = contract.find('_');
  return us == std::string_view::npos ? contract : contract.substr(0, us);
}

std::string upper(std::string s) {
  for (char& c : s) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
  return s;
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

GateUsdtVenue::GateUsdtVenue(VenueId id, GateUsdtVenueConfig cfg)
    : id_(id),
      cfg_(std::move(cfg)),
      signer_(cfg_.credentials),
      rate_(cfg_.rate_threshold),
      dms_(cfg_.dead_mans_switch_s * 1000) {
  rate_.add_weight_bucket(0, 1'000'000'000);  // limits come from X-Gate-RateLimit headers
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  std::memset(scratch_, 0, sizeof scratch_);
  ReplayLimits limits;
  limits.window_ms = kTradesWindowMs;
  limits.history_ms = kTradesHistoryMs;
  limits.newest_first = true;
  limits.page_rows = kTradesPage;
  limits.window_pages = kMaxTradePagesPerWindow;
  limits.max_pages = kMaxTradeRequests;
  trade_replay_.setup(cfg_.name,
                      "fill(s)",
                      limits,
                      {[this] { return replay_ready(); },
                       [this] { return venue_time_ms(); },
                       [this](const ReplayQuery& q) { return query_trades(q); },
                       [this](bool complete) { reconcile_.replay_done(complete); },
                       {},
                       [this] { return rate_.can_send(1, now_ns(), false); }},
                      [this](std::size_t, const TradeRow& t) { return emit_trade(t); });
  trade_replay_.set_streams(1);
}

GateUsdtVenue::~GateUsdtVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps GateUsdtVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = cfg_.ws_order_api;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

std::int64_t GateUsdtVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_.load(std::memory_order_relaxed);
}

net::ConnectionConfig GateUsdtVenue::ws_config(bool manual_auth, bool manual_subscribe) const {
  net::ConnectionConfig c;
  c.url = cfg_.ws_url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  // Our own futures.ping every ping_interval_ms is answered, so the dead threshold must exceed
  // it for quiet private/trade channels.
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, cfg_.ping_interval_ms * 2 + 5000);
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = 0;
  c.manual_auth = manual_auth;
  c.manual_subscribe = manual_subscribe;
  c.extra_headers = std::string(kUpgradeHeaders);
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> GateUsdtVenue::load_reference_data(InstrumentTable& instruments) {
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
      // fx-api.gateio.ws has no time endpoint: the order book's `current` is the venue's clock.
      const HttpReply t = http.get(fmt::format("/api/v4/futures/{}/order_book?contract={}&limit=1",
                                               cfg_.settle,
                                               mine.front()->symbol.view()));
      std::int64_t server_ms = 0;
      if (t.ok() && decode_server_time(t.body, server_ms).empty()) {
        const std::int64_t recv_ms = wall_now().ns / kNsPerMs;
        clock_offset_ms_.store(server_ms - recv_ms);
        clock_sync_ns_ = now_ns();
      }
    }
    // GET /contracts/{name} per instrument: the list endpoint pages by 100 and the venue lists
    // several hundred contracts.
    std::vector<ContractInfo> infos;
    for (Instrument* inst : mine) {
      const HttpReply reply = http.get(
          fmt::format("/api/v4/futures/{}/contracts/{}", cfg_.settle, inst->symbol.view()));
      if (!reply.ok()) {
        const std::string why =
            reply.error.empty() ? fmt::format("HTTP {} {}", reply.status, reply.body.substr(0, 200))
                                : reply.error;
        if (!cfg_.allow_offline_reference_data)
          return fail(
              fmt::format("{}: contract {} failed: {}", cfg_.name, inst->symbol.view(), why));
        FASTMM_LOG_WARN("{}: contract {} failed ({}); keeping configured tick/lot",
                        cfg_.name,
                        inst->symbol.view(),
                        why);
        continue;
      }
      ContractInfo info;
      if (const std::string err = decode_contract(reply.body, info); !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
      infos.push_back(std::move(info));
    }
    for (Instrument* inst : mine) {
      const ContractInfo* f = nullptr;
      for (const ContractInfo& i : infos) {
        if (iequals_symbol(i.name, inst->symbol.view())) f = &i;
      }
      if (f == nullptr) {
        if (infos.empty() && cfg_.allow_offline_reference_data) continue;
        return fail(fmt::format("{}: contract {} not listed", cfg_.name, inst->symbol.view()));
      }
      if (f->type != "direct")
        return fail(fmt::format("{}: {} is an {} contract; only direct (linear) is supported",
                                cfg_.name,
                                inst->symbol.view(),
                                f->type.empty() ? "unknown" : f->type));
      if (!f->tick.is_positive() || !f->quanto_multiplier.is_positive())
        return fail(fmt::format("{}: {} has an invalid order_price_round/quanto_multiplier",
                                cfg_.name,
                                inst->symbol.view()));
      const Qty lot = Qty::from_int(1);
      if (inst->tick != f->tick || inst->lot != lot ||
          inst->contract_multiplier != f->quanto_multiplier) {
        FASTMM_LOG_WARN(
            "{}: {} tick/lot/multiplier from the contract table override config ({} / {} / {} -> "
            "{} / {} / {})",
            cfg_.name,
            inst->symbol.view(),
            inst->tick,
            inst->lot,
            inst->contract_multiplier,
            f->tick,
            lot,
            f->quanto_multiplier);
      }
      inst->tick = f->tick;
      inst->lot = lot;
      inst->min_qty = f->min_size.is_positive() ? f->min_size : lot;
      inst->max_qty = f->max_size;
      inst->min_notional = Notional{};
      inst->contract_multiplier = f->quanto_multiplier;
      inst->expiry_ns = 0;
      inst->flags = static_cast<std::uint8_t>((inst->flags | Instrument::kReduceOnlySupported) &
                                              ~Instrument::kInverse);
      if (inst->asset_class != AssetClass::Perpetual) {
        FASTMM_LOG_WARN(
            "{}: {} is a perpetual; asset_class set to perpetual", cfg_.name, inst->symbol.view());
        inst->asset_class = AssetClass::Perpetual;
      }
      const std::string settle = upper(cfg_.settle);
      if (!inst->quote.empty() && !iequals_symbol(inst->quote.view(), settle))
        FASTMM_LOG_WARN("{}: {} settles in {}, not the configured quote {}; using {}",
                        cfg_.name,
                        inst->symbol.view(),
                        settle,
                        inst->quote.view(),
                        settle);
      if (!inst->quote.assign(settle))
        return fail(fmt::format("{}: {} coin names too long", cfg_.name, inst->symbol.view()));
      if (const std::string_view base = base_of(f->name); !inst->base.assign(base)) {
        // The base is a label here (a USDT-settled contract's PnL is in the quote): keep what
        // fits rather than refuse a contract named like ANTHROPIC_USDT or SAMSUNGEM_USDT.
        static_cast<void>(inst->base.assign(base.substr(0, inst->base.kCapacity)));
        FASTMM_LOG_WARN("{}: {} base coin label shortened to {}",
                        cfg_.name,
                        inst->symbol.view(),
                        inst->base.view());
      }
      if (inst->id.value < kMaxInstruments) {
        funding_interval_[inst->id.value] = Duration{f->funding_interval_s * 1'000'000'000LL};
        funding_next_ms_[inst->id.value] = f->funding_next_apply_s * 1000;
      }
      if (f->status != "trading" || f->in_delisting) {
        FASTMM_LOG_ERROR("{}: {} status is {}{}: disabled",
                         cfg_.name,
                         inst->symbol.view(),
                         f->status,
                         f->in_delisting ? " (delisting)" : "");
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
  FASTMM_LOG_INFO("{}: reference data loaded for {} contracts ({} settled)",
                  cfg_.name,
                  mine.size(),
                  cfg_.settle);
  if (!cfg_.dry_run && signer_.usable()) {
    AccountInfo acct;
    if (std::string err = check_account(acct); !err.empty()) return fail(std::move(err));
  }
  return {};
}

// GET /accounts: the user id the private channels are subscribed with, and the position mode.
std::string GateUsdtVenue::check_account(AccountInfo& out) {
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    const std::string path = fmt::format("/api/v4/futures/{}/accounts", cfg_.settle);
    const std::string headers = signer_.rest_headers("GET", path, {}, {}, venue_time_s());
    const HttpReply reply = http.request("GET", path, headers);
    if (!reply.ok()) {
      std::string label;
      std::string msg;
      if (decode_error(reply.body, label, msg)) {
        if (map_label(label).action == VenueAction::Fatal) refused_account_settings_ = true;
        return fmt::format("{}: GET accounts refused: {} {}", cfg_.name, label, msg);
      }
      return fmt::format("{}: GET accounts failed: HTTP {} {}",
                         cfg_.name,
                         reply.status,
                         reply.error.empty() ? reply.body.substr(0, 200) : reply.error);
    }
    if (const std::string err = decode_account(reply.body, out); !err.empty())
      return fmt::format("{}: {}", cfg_.name, err);
    if (out.in_dual_mode) {
      refused_account_settings_ = true;
      return fmt::format(
          "{}: the futures account is in dual position mode; the gate_usdt connector trades "
          "single mode only: switch it with POST /futures/{}/dual_mode {{\"dual_mode\":false}}",
          cfg_.name,
          cfg_.settle);
    }
    user_id_ = out.user;
    FASTMM_LOG_INFO("{}: account user {} ({}), total {} available {}",
                    cfg_.name,
                    user_id_.empty() ? "?" : user_id_,
                    out.currency,
                    out.fields.total,
                    out.fields.free);
  } catch (const std::exception& e) {
    return fmt::format("{}: account check failed: {}", cfg_.name, std::string_view(e.what()));
  }
  return {};
}

Result<std::vector<VenueFee>, std::string> GateUsdtVenue::account_fees(
    const InstrumentTable& instruments) {
  std::vector<VenueFee> out;
  if (!cfg_.fetch_fees || cfg_.dry_run || !signer_.usable()) return out;
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    const std::string path = fmt::format("/api/v4/futures/{}/fee", cfg_.settle);
    const std::string headers = signer_.rest_headers("GET", path, {}, {}, venue_time_s());
    const HttpReply reply = http.request("GET", path, headers);
    if (!reply.ok())
      return fail(fmt::format("{}: GET fee failed: HTTP {} {}",
                              cfg_.name,
                              reply.status,
                              reply.error.empty() ? reply.body.substr(0, 200) : reply.error));
    std::vector<std::pair<std::string, FeeRates>> fees;
    if (const std::string err = decode_fees(reply.body, fees); !err.empty())
      return fail(fmt::format("{}: {}", cfg_.name, err));
    for (const Instrument& inst : instruments) {
      if (inst.venue != id_) continue;
      for (const auto& [contract, f] : fees) {
        if (!iequals_symbol(contract, inst.symbol.view())) continue;
        out.push_back(
            {inst.id, fastmm::FeeRates::from_bps(f.maker * 10'000.0, f.taker * 10'000.0)});
        break;
      }
    }
  } catch (const std::exception& e) {
    return fail(fmt::format("{}: fee query failed: {}", cfg_.name, std::string_view(e.what())));
  }
  return out;
}

// ---- wiring ---------------------------------------------------------------------------------

void GateUsdtVenue::attach(const SymbolTable& symbols,
                           const InstrumentTable& instruments,
                           EventSink& md_sink,
                           EventSink& order_sink,
                           MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ = std::make_unique<GateMdFeed>(
      symbols,
      id_,
      md_sink,
      ResubscribeRequester{&GateUsdtVenue::resubscribe_requester, this},
      cfg_.book_level);
  md_feed_->set_log_name(cfg_.name);
  private_parser_ = std::make_unique<GatePrivateParser>(symbols, instruments, id_);
  encoder_ = std::make_unique<GateOrderEncoder>(signer_, symbols, cfg_.settle);
  decoder_ = std::make_unique<GateResponseDecoder>();
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments);
}

void GateUsdtVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (md_feed_ && id.value < kMaxInstruments) {
      md_feed_->add_instrument(id);
      md_feed_->set_funding(id, funding_interval_[id.value], funding_next_ms_[id.value]);
    }
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (connected_ && md_conn_.opened()) {
    md_conn_.close();
    open_md();
  }
}

void GateUsdtVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("GateUsdtVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  reconcile_.open(!cfg_.dry_run && signer_.usable());
  trade_replay_.start_at(venue_time_ms());
  trade_replay_.open(!cfg_.dry_run && signer_.usable());
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
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, private={}, ws_orders={}, book obu.{})",
                  cfg_.name,
                  cfg_.dry_run,
                  !cfg_.dry_run && signer_.usable(),
                  cfg_.ws_order_api,
                  cfg_.book_level);
}

void GateUsdtVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  reconcile_.close();
  trade_replay_.close();
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  if (dms_.needs_stop()) stop_countdown_blocking();
  dms_.reset();
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

void GateUsdtVenue::open_rest() {
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
}

void GateUsdtVenue::open_md() {
  md_conn_.open(*reactor_, ws_config(false, false), md_handler_);
  md_conn_.connect();
}

void GateUsdtVenue::open_private() {
  private_conn_.open(*reactor_, ws_config(true, true), private_handler_);
  private_conn_.connect();
}

void GateUsdtVenue::open_trade() {
  trade_conn_.open(*reactor_, ws_config(true, false), trade_handler_);
  trade_conn_.connect();
}

void GateUsdtVenue::send_login(bool trade) {
  const std::size_t n =
      encoder_->encode_ws_login(venue_time_s(), trade ? kLoginTrade : kLoginPrivate, request_buf_);
  const std::string_view frame(request_buf_, n);
  const bool ok = n > 0 && (trade ? trade_conn_.send_text(frame) : private_conn_.send_text(frame));
  if (!ok) FASTMM_LOG_ERROR("{}: could not send the login request", cfg_.name);
}

// ---- market data ------------------------------------------------------------------------------

void GateUsdtVenue::on_md_state(net::ConnState s) {
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
      for (InstrumentId id : subscribed_) {
        if (GateBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
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

void GateUsdtVenue::on_md_open() {
  for (const std::string& p : md_feed_->subscription_payloads()) {
    std::string frame = p;
    GateMdFeed::fill_time(frame, venue_time_s());
    if (!md_conn_.send_text(frame))
      FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void GateUsdtVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (GateBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void GateUsdtVenue::on_md_text(std::string_view t, std::int64_t ts) {
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

void GateUsdtVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;  // the reconnect subscribes everything again
  for (std::string p : md_feed_->resubscribe_payloads(id)) {
    GateMdFeed::fill_time(p, venue_time_s());
    static_cast<void>(md_conn_.send_text(p));
  }
  FASTMM_LOG_INFO(
      "{}: resubscribing {} for a fresh snapshot", cfg_.name, symbols_->venue_symbol(id));
}

// ---- private stream -------------------------------------------------------------------------

void GateUsdtVenue::on_private_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating) send_login(false);
  const ConnState mapped = map_conn_state(s);
  stats_.user = private_channel_state(s);
  if (mapped == private_state_) return;
  const ConnState prev = private_state_;
  private_state_ = mapped;
  if (mapped == ConnState::Live) {
    if (prev != ConnState::Stale) {
      emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
      FASTMM_LOG_INFO("{}: private channel -> Live", cfg_.name);
    }
    if (private_was_live_ && prev != ConnState::Stale) {
      reconcile_.request();
    } else if (!private_was_live_) {
      reconcile_.sweep();
    }
    private_was_live_ = true;
  } else if ((mapped == ConnState::Disconnected || mapped == ConnState::Connecting) &&
             (prev == ConnState::Live || prev == ConnState::Stale)) {
    private_subscribed_ = 0;
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: private channel lost", cfg_.name);
  }
}

// After the login: the four private channels, each signed, with the account's user id.
void GateUsdtVenue::on_private_open() {
  private_subscribed_ = 0;
  if (user_id_.empty()) {
    FASTMM_LOG_ERROR("{}: no user id for the private channels", cfg_.name);
    apply_action(VenueAction::Fatal, "NO_USER_ID", "login reply named no uid", 0);
    return;
  }
  const std::string_view uid = user_id_;
  const std::string_view all[2] = {uid, "!all"};
  const std::string_view one[1] = {uid};
  const std::int64_t t = venue_time_s();
  struct Sub {
    std::string_view channel;
    std::span<const std::string_view> payload;
  };
  const Sub subs[kPrivateChannels] = {{"futures.orders", all},
                                      {"futures.usertrades", all},
                                      {"futures.positions", all},
                                      {"futures.balances", one}};
  for (const Sub& sub : subs) {
    const std::size_t n = encoder_->encode_subscribe(sub.channel, sub.payload, t, request_buf_);
    if (n == 0 || !private_conn_.send_text(std::string_view(request_buf_, n)))
      FASTMM_LOG_ERROR("{}: could not subscribe {}", cfg_.name, sub.channel);
  }
}

ClientOrderId GateUsdtVenue::current_id(ClientOrderId link) const noexcept {
  if (const ClientOrderId* cur = aliases_.find(link)) return *cur;
  return link;
}

void GateUsdtVenue::forget_order(ClientOrderId id) noexcept {
  if (const OrderShadow* s = shadows_.find(id)) {
    if (s->link_id.valid() && s->link_id != id) aliases_.erase(s->link_id);
  }
  shadows_.erase(id);
}

void GateUsdtVenue::complete_fill(OrderFillMsg& f) noexcept {
  OrderShadow* s = shadows_.find(f.cl_ord_id);
  if (s == nullptr) {
    f.cum_qty = f.qty;
    f.leaves_qty = Qty{};
    return;
  }
  s->cum_qty = s->cum_qty + f.qty;
  if (!f.venue_order_id.empty() && s->venue_id.empty()) s->venue_id = f.venue_order_id;
  f.cum_qty = s->cum_qty;
  f.leaves_qty = s->orig_qty > s->cum_qty ? s->orig_qty - s->cum_qty : Qty{};
  if (f.leaves_qty.is_zero()) forget_order(f.cl_ord_id);
}

void GateUsdtVenue::on_private_text(std::string_view t, std::int64_t ts) {
  if (raw_private_.enabled()) raw_private_.record(ts, t);
  // A WebSocket API reply on this connection is the login's.
  ApiResponse api;
  if (decoder_->decode_ws(t, api) == ParseStatus::Ok) {
    if (api.request_id != kLoginPrivate || api.ack) return;
    if (api.success) {
      if (user_id_.empty() && !api.uid.empty()) user_id_ = std::string(api.uid);
      FASTMM_LOG_INFO("{}: private stream logged in (uid {})", cfg_.name, api.uid);
      private_conn_.auth_done();
    } else {
      FASTMM_LOG_ERROR("{}: private login failed: {} {}", cfg_.name, api.label, api.message);
      apply_action(VenueAction::Fatal, api.label, api.message, 0);
    }
    return;
  }
  const Cycles t0 = rdtscp();
  const PrivateDecodeResult r = private_parser_->decode(t, wall_now(), t0, scratch_);
  if (r.balance_changed) reconcile_.request_balances();
  if (r.status == ParseStatus::Ok) {
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      switch (h->type) {
        case EventType::PositionUpdate: {
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
          m->cl_ord_id = current_id(m->cl_ord_id);
          OrderShadow* s = shadows_.find(m->cl_ord_id);
          if (s != nullptr) {
            if (s->venue_id.empty()) s->venue_id = m->venue_order_id;
            // The orders channel repeats "open" after an amend: one ack per order.
            if (s->acked) continue;
            s->acked = true;
          }
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
          complete_fill(*m);
          note_fill(*m);
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
  switch (r.control) {
    case ControlOp::Subscribe:
      if (r.control_success) {
        if (++private_subscribed_ >= kPrivateChannels) private_conn_.subscribe_done();
      } else {
        FASTMM_LOG_ERROR(
            "{}: private subscribe {} failed: {} {}", cfg_.name, r.channel, r.error_code, r.error);
        apply_action(VenueAction::Fatal, "SUBSCRIBE", r.error, 0);
      }
      return;
    case ControlOp::System:
      FASTMM_LOG_WARN("{}: system notice on the private stream: {}", cfg_.name, t.substr(0, 200));
      return;
    default:
      break;
  }
  if (private_parser_->stats().dual_positions != 0 && !fatal_) {
    FASTMM_LOG_ERROR("{}: the positions channel reports a dual-mode position", cfg_.name);
    apply_action(VenueAction::Fatal, "POSITION_DUAL_MODE", "dual-mode position", 0);
  }
  if (r.status == ParseStatus::Malformed) FASTMM_LOG_WARN("{}: malformed private frame", cfg_.name);
}

// ---- trade channel --------------------------------------------------------------------------

void GateUsdtVenue::on_trade_state(net::ConnState s) {
  if (s == net::ConnState::Authenticating) send_login(true);
  const ConnState mapped = map_conn_state(s);
  stats_.order = private_channel_state(s);
  if (mapped == trade_state_) return;
  const ConnState prev = trade_state_;
  trade_state_ = mapped;
  if (mapped != ConnState::Live && mapped != ConnState::Stale) sent_.connection_lost();
  if (mapped == ConnState::Live) {
    const bool reconnected = trade_was_live_ && prev != ConnState::Stale;
    trade_was_live_ = true;
    if (prev != ConnState::Stale) {
      emit_connection_state(*order_sink_, id_, 1, ConnState::Live);
      FASTMM_LOG_INFO("{}: trade channel -> Live", cfg_.name);
    }
    drain_outbound();
    if (reconnected && !cfg_.dry_run) request_open_orders();
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: trade channel lost", cfg_.name);
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void GateUsdtVenue::on_trade_text(std::string_view t, std::int64_t ts) {
  if (raw_trade_.enabled()) raw_trade_.record(ts, t);
  ApiResponse r;
  const ParseStatus st = decoder_->decode_ws(t, r);
  if (st == ParseStatus::Malformed) {
    FASTMM_LOG_WARN("{}: malformed trade frame", cfg_.name);
    return;
  }
  if (st != ParseStatus::Ok) return;  // pong, system notices
  if (r.request_id == kLoginTrade) {
    if (r.ack) return;
    if (r.success) {
      FASTMM_LOG_INFO("{}: trade stream logged in", cfg_.name);
      trade_conn_.auth_done();
    } else {
      FASTMM_LOG_ERROR("{}: trade login failed: {} {}", cfg_.name, r.label, r.message);
      apply_action(VenueAction::Fatal, r.label, r.message, 0);
    }
    return;
  }
  if (r.ack) return;  // the venue took the request; its result follows
  if (const auto req = parse_request_id(r.request_id)) {
    handle_order_response(req->first, req->second, r);
    return;
  }
  if (!r.success)
    FASTMM_LOG_WARN("{}: trade error for '{}': {} {}", cfg_.name, r.request_id, r.label, r.message);
}

void GateUsdtVenue::handle_order_response(RequestKind kind,
                                          ClientOrderId id,
                                          const ApiResponse& r) {
  if (kind != RequestKind::Cancel) sent_.answered(id);
  rate_.on_remaining(r.limit, r.remain, now_ns());
  OrderShadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : InstrumentId::invalid();
  const ErrorMapping m = r.success ? ErrorMapping{RejectReason::None, VenueAction::None, true}
                                   : map_label(r.label, r.message);
  const Timestamp venue_ts = ts_from_ms(r.response_time_ms);
  switch (kind) {
    case RequestKind::New:
      if (r.success) {
        if (shadow != nullptr && !r.order_id.empty()) shadow->venue_id.assign(r.order_id);
        if (cfg_.emit_ack_from_response && (shadow == nullptr || !shadow->acked)) {
          if (shadow != nullptr) shadow->acked = true;
          emit_order_ack(*order_sink_, id_, inst, id, r.order_id, 0, venue_ts);
        }
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, r.status, r.message);
        forget_order(id);
        apply_action(m.action, r.label, r.message, reset_ms_of(r.reset_ms));
      }
      ++stats_.order_events;
      return;
    case RequestKind::Cancel:
      if (r.success) {
        if (!private_conn_.is_live()) {
          emit_cancel_ack(*order_sink_, id_, inst, id, r.order_id, r.size - r.left, venue_ts);
          forget_order(id);
          ++stats_.order_events;
        }
      } else {
        emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, r.status, r.message);
        apply_action(m.action, r.label, r.message, reset_ms_of(r.reset_ms));
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
          shadow->acked = true;
          if (!r.order_id.empty()) shadow->venue_id.assign(r.order_id);
        }
        emit_order_ack(
            *order_sink_, id_, inst, id, r.order_id, OrderAckMsg::kAmendedInPlace, venue_ts);
      } else {
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, r.status, r.message);
        shadows_.erase(id);
        apply_action(m.action, r.label, r.message, reset_ms_of(r.reset_ms));
      }
      ++stats_.order_events;
      return;
    case RequestKind::Amend:
    case RequestKind::Other:
      return;
  }
}

// ---- outbound ---------------------------------------------------------------------------------

void GateUsdtVenue::on_wake() {
  drain_outbound();
}

template <class Ring>
void GateUsdtVenue::write_orders(Ring& ring) {
  drain_outbound_coalesced(
      ring,
      wire_,
      [this] {
        trade_conn_.cork();
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
        if (trade_conn_.uncork()) return true;
        fail_batch();
        return false;
      });
}

void GateUsdtVenue::fail_batch() {
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

void GateUsdtVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void GateUsdtVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

void GateUsdtVenue::refuse_untracked(const OrderCommand& cmd) {
  shadow_overflow_.refused(cfg_.name, shadows_.size());
  sent_.answered(cmd.cl_ord_id);
  emit_order_reject(*order_sink_,
                    id_,
                    cmd.instrument,
                    cmd.cl_ord_id,
                    RejectReason::OrderTableFull,
                    0,
                    "order table full");
  ++stats_.order_events;
}

void GateUsdtVenue::send_command(const OrderCommand& cmd) {
  note_taken(cmd);
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
  if (fatal_ && !is_cancel) return refuse(RejectReason::VenueKilled, "venue fatal");
  const OrderShadow* shadow = nullptr;
  if (cmd.kind == OrderCommandKind::New) {
    if (!rate_.can_send(1, now, true)) {
      ++stats_.rate_limit_cooldowns;
      return refuse(RejectReason::VenueRateLimit, "local rate limit");
    }
    OrderShadow s{};
    s.instrument = cmd.instrument;
    s.side = cmd.side;
    s.type = cmd.type;
    s.tif = cmd.tif;
    s.link_id = cmd.cl_ord_id;
    s.sent_seq = sent_.last_seq();
    s.orig_qty = cmd.qty;
    shadow = shadows_.assign(cmd.cl_ord_id, s);
    if (FASTMM_UNLIKELY(shadow == nullptr)) return refuse_untracked(cmd);
  } else if (cmd.kind == OrderCommandKind::Replace) {
    const OrderShadow* orig = shadows_.find(cmd.orig_cl_ord_id);
    if (orig == nullptr) return refuse(RejectReason::UnknownOrder, "amend: original unknown");
    if (!rate_.can_send(1, now, true)) {
      ++stats_.rate_limit_cooldowns;
      return refuse(RejectReason::VenueRateLimit, "local rate limit");
    }
    OrderShadow copy = *orig;
    copy.replaces = cmd.orig_cl_ord_id;
    copy.sent_seq = sent_.last_seq();
    copy.orig_qty = cmd.qty;  // the amend's size is the new total
    copy.acked = false;
    shadow = shadows_.assign(cmd.cl_ord_id, copy);
    if (FASTMM_UNLIKELY(shadow == nullptr)) return refuse_untracked(cmd);
  } else {
    shadow = shadows_.find(cmd.cl_ord_id);
  }
  if (cfg_.ws_order_api && trade_conn_.is_live()) {
    const Cycles before_encode = rdtscp();
    const std::size_t n = encoder_->encode_ws(cmd, shadow, venue_time_s(), request_buf_);
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
    ++stats_.order_send_failures;
  }
  send_command_rest(cmd, shadow);
}

void GateUsdtVenue::send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow) {
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  RestRequest rr;
  const Cycles before_encode = rdtscp();
  if (rest_ == nullptr || (rest_hard_stopped_ && !is_cancel) ||
      !encoder_->encode_rest(cmd, shadow, rr)) {
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
  const std::string headers = encoder_->rest_headers(rr, venue_time_s());
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
      shadows_.erase(cmd.cl_ord_id);
    }
    ++stats_.order_send_failures;
    return;
  }
  wire_.record(cmd.t0_cycles(), before_encode, after_encode, rdtscp());
  if (cmd.kind != OrderCommandKind::Cancel) sent_.sent_over_rest(cmd.cl_ord_id);
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

void GateUsdtVenue::handle_rest_order_response(const OrderCommand& cmd,
                                               const net::HttpResponse& r) {
  ++stats_.rest_requests;
  note_rate_headers(r);
  const RequestKind kind = cmd.kind == OrderCommandKind::New      ? RequestKind::New
                           : cmd.kind == OrderCommandKind::Cancel ? RequestKind::Cancel
                                                                  : RequestKind::Replace;
  ApiResponse ar;
  if (r.error != net::NetError::None) {
    ++stats_.rest_errors;
    ar.status = 0;
    ar.label = "TRANSPORT";
    ar.message = net::to_string(r.error);
    handle_order_response(kind, cmd.cl_ord_id, ar);
    request_open_orders();  // send status unknown: reconcile rather than guess
    return;
  }
  const PaddedJson padded(r.body);
  if (decoder_->decode_rest(padded.view(), r.status, ar) != ParseStatus::Ok) {
    ++stats_.rest_errors;
    ar = ApiResponse{};
    ar.status = r.status;
    ar.label = "UNPARSEABLE";
    ar.message = "unparseable REST reply";
    ar.success = false;
    handle_order_response(kind, cmd.cl_ord_id, ar);
    apply_action(map_http_status(r.status).action, ar.label, ar.message, 0);
    return;
  }
  if (!ar.success && ar.label.empty()) ar.label = "HTTP";
  ar.limit = header_int(r, "X-Gate-RateLimit-Limit");
  ar.remain = header_int(r, "X-Gate-RateLimit-Requests-Remain");
  ar.reset_ms = reset_ms_of(header_int(r, "X-Gate-RateLimit-Reset-Timestamp"));
  handle_order_response(kind, cmd.cl_ord_id, ar);
}

// X-Gate-RateLimit-Reset-Timestamp is Unix seconds on REST and milliseconds on the WebSocket API
// (x_gat_ratelimit_reset_timestamp); either becomes ms.
std::int64_t GateUsdtVenue::reset_ms_of(std::int64_t v) noexcept {
  if (v <= 0) return 0;
  return v < 100'000'000'000LL ? v * 1000 : v;
}

void GateUsdtVenue::note_rate_headers(const net::HttpResponse& r) {
  rate_.on_remaining(header_int(r, "X-Gate-RateLimit-Limit"),
                     header_int(r, "X-Gate-RateLimit-Requests-Remain"),
                     now_ns());
  if (r.status == 429) {
    std::int64_t wait = kDefaultCooldownNs;
    const std::int64_t reset = reset_ms_of(header_int(r, "X-Gate-RateLimit-Reset-Timestamp"));
    if (reset > 0) wait = std::max<std::int64_t>(0, reset - venue_time_ms()) * kNsPerMs;
    rate_.cooldown(wait, now_ns());
    ++stats_.rate_limit_cooldowns;
    FASTMM_LOG_WARN("{}: HTTP 429: REST paused for {} ms", cfg_.name, wait / kNsPerMs);
  }
}

void GateUsdtVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void GateUsdtVenue::apply_action(VenueAction action,
                                 std::string_view label,
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
          "{}: rate limited ({} {}); cooling down {} ms", cfg_.name, label, msg, wait / kNsPerMs);
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
      FASTMM_LOG_ERROR("{}: venue rejected a contract rule ({} {}); check the configuration",
                       cfg_.name,
                       label,
                       msg);
      break;
    case VenueAction::HardStop:
      rate_.hard_stop();
      rest_hard_stopped_ = true;
      FASTMM_LOG_ERROR("{}: REST hard stop ({} {})", cfg_.name, label, msg);
      trip_venue_kill(KillReason::VenueHardStop);
      break;
    case VenueAction::Fatal:
      fatal_ = true;
      FASTMM_LOG_ERROR(
          "{}: fatal venue error ({} {}); order entry disabled", cfg_.name, label, msg);
      trip_venue_kill(KillReason::VenueFatal);
      break;
  }
}

// ---- reconciliation ---------------------------------------------------------------------------

void GateUsdtVenue::request_open_orders() {
  reconcile_.request();
}

bool GateUsdtVenue::replay_executions() {
  return trade_replay_.run();
}

bool GateUsdtVenue::fetch_snapshot(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  reconcile_pages_ = 0;
  reconcile_positions_.clear();
  if (subscribed_.empty()) return request_positions(generation);
  return request_open_orders_page(generation, 0, 0);
}

// GET /orders?status=open&contract=C&limit=100&offset=N for subscribed_[index]; the last page of
// the last contract goes on to the positions.
bool GateUsdtVenue::request_open_orders_page(std::uint64_t generation,
                                             std::size_t index,
                                             int offset) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_ || index >= subscribed_.size())
    return false;
  RestRequest rr;
  encoder_->encode_rest_open_orders(
      symbols_->venue_symbol(subscribed_[index]), kOpenOrdersPage, offset, rr);
  const std::string headers = encoder_->rest_headers(rr, venue_time_s());
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET",
      rr.target(),
      headers,
      {},
      [this, alive, generation, index, offset](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.current(generation)) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET orders?status=open failed: status={} err={} {}",
                          cfg_.name,
                          r.status,
                          net::to_string(r.error),
                          r.body.substr(0, 120));
          reconcile_.fetched(generation, false);
          return;
        }
        std::vector<OpenOrderRecord> rows;
        if (const std::string err = decode_open_orders(r.body, rows); !err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: open orders reply rejected ({})", cfg_.name, err);
          reconcile_.fetched(generation, false);
          return;
        }
        for (const OpenOrderRecord& o : rows) {
          const InstrumentId inst = symbols_->find(id_, o.contract);
          if (!inst.valid()) continue;
          ReconcileMsg& m = reconcile_.add_order(inst);
          const bool bid = o.size.raw >= 0;
          m.side = bid ? Side::Buy : Side::Sell;
          const Qty size = bid ? o.size : -o.size;
          const Qty left = o.left.raw >= 0 ? o.left : -o.left;
          m.state = left < size ? OrderState::PartiallyFilled : OrderState::Live;
          if (const auto cl = cl_ord_id_of_text(o.text)) m.cl_ord_id = current_id(*cl);
          m.venue_order_id.assign(o.id);
          m.price = o.price;
          m.orig_qty = size;
          m.cum_qty = size - left;
        }
        bool sent = false;
        if (rows.size() >= static_cast<std::size_t>(kOpenOrdersPage)) {
          if (++reconcile_pages_ >= kMaxReconcilePages) {
            FASTMM_LOG_WARN("{}: more than {} pages of open orders", cfg_.name, kMaxReconcilePages);
          } else {
            sent = request_open_orders_page(generation, index, offset + kOpenOrdersPage);
          }
        } else if (index + 1 < subscribed_.size()) {
          sent = request_open_orders_page(generation, index + 1, 0);
        } else {
          sent = request_positions(generation);
        }
        if (!sent) reconcile_.fetched(generation, false);
      });
}

bool GateUsdtVenue::request_positions(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  RestRequest rr;
  encoder_->encode_rest_positions(rr);
  const std::string headers = encoder_->rest_headers(rr, venue_time_s());
  std::weak_ptr<int> alive = alive_;
  return rest_->request(
      "GET", rr.target(), headers, {}, [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.current(generation)) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        std::string err;
        if (!r.ok()) {
          err = fmt::format(
              "status={} err={} {}", r.status, net::to_string(r.error), r.body.substr(0, 120));
        } else {
          err = decode_positions(r.body, reconcile_positions_);
        }
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET positions failed ({})", cfg_.name, err);
          reconcile_.fetched(generation, false);
          return;
        }
        finish_snapshot(generation);
      });
}

void GateUsdtVenue::finish_snapshot(std::uint64_t generation) {
  bool dual = false;
  // holding=true lists the non-zero positions only: an absent contract is flat.
  for (InstrumentId id : subscribed_) {
    const std::string_view sym = symbols_->venue_symbol(id);
    Qty qty{};
    Price avg{};
    for (const PositionRecord& p : reconcile_positions_) {
      if (!iequals_symbol(p.contract, sym)) continue;
      if (!p.mode.empty() && p.mode != "single") {
        dual = dual || !p.size.is_zero();
        continue;
      }
      qty = p.size;
      avg = p.entry_price;
    }
    reconcile_.add_position(id, qty, avg);
    PositionCheck& pc = positions_[id.value];
    pc.tracked = qty;
    pc.venue = qty;
    pc.venue_avg = avg;
    pc.pending = false;
    FASTMM_LOG_INFO("{}: {} position {} @ {}", cfg_.name, sym, qty, avg);
  }
  reconcile_positions_.clear();
  reconcile_.fetched(generation, true);
  if (dual) {
    FASTMM_LOG_ERROR("{}: positions report a dual-mode position", cfg_.name);
    apply_action(VenueAction::Fatal, "POSITION_DUAL_MODE", "dual-mode position", 0);
  }
}

// The balance leg: GET /accounts, one row for the settle currency.
bool GateUsdtVenue::fetch_balances(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  RestRequest rr;
  encoder_->encode_rest_accounts(rr);
  const std::string headers = encoder_->rest_headers(rr, venue_time_s());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", rr.target(), headers, {}, [this, alive, generation](const net::HttpResponse& r) {
        if (alive.expired() || !reconcile_.balances_current(generation)) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        AccountInfo a;
        std::string err;
        if (!r.ok()) {
          err = fmt::format("status={} err={}", r.status, net::to_string(r.error));
        } else {
          err = decode_account(r.body, a);
        }
        if (!err.empty()) {
          ++stats_.rest_errors;
          FASTMM_LOG_WARN("{}: GET accounts failed ({})", cfg_.name, err);
          reconcile_.balances_fetched(generation, false, 0);
          return;
        }
        if (user_id_.empty()) user_id_ = a.user;
        reconcile_.add_balance(a.currency.empty() ? upper(cfg_.settle) : a.currency, a.fields);
        reconcile_.balances_fetched(generation, true, venue_time_ms());
      });
  if (!queued) reconcile_.balances_fetched(generation, false, 0);
  return true;
}

void GateUsdtVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const OrderShadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void GateUsdtVenue::drop_shadow(ClientOrderId id) {
  forget_order(id);
}

// ---- fill replay ------------------------------------------------------------------------------

void GateUsdtVenue::resume_executions(std::int64_t since_venue_ms,
                                      const std::vector<std::string>& known) {
  trade_replay_.resume(since_venue_ms, known);
}

bool GateUsdtVenue::request_executions(std::int64_t since_venue_ms) {
  if (since_venue_ms > 0 && !trade_replay_.active()) trade_replay_.restart_from(since_venue_ms);
  return trade_replay_.run();
}

bool GateUsdtVenue::replay_ready() const noexcept {
  return !cfg_.dry_run && connected_ && signer_.usable() && rest_ != nullptr &&
         !rest_hard_stopped_ && !subscribed_.empty();
}

// GET /my_trades_timerange?from=&to=&limit=1000&offset=: the account's fills, newest first; the
// page token is the offset of the next page.
bool GateUsdtVenue::query_trades(const ReplayQuery& q) {
  if (rest_ == nullptr || rest_hard_stopped_ || q.start_ms <= 0) return false;
  int offset = 0;
  if (!q.page.empty()) {
    if (const auto n = parse_int64(q.page)) offset = static_cast<int>(*n);
  }
  RestRequest rr;
  encoder_->encode_rest_my_trades(
      {}, q.start_ms / 1000, q.end_ms > 0 ? (q.end_ms + 999) / 1000 : 0, kTradesPage, offset, rr);
  const std::string headers = encoder_->rest_headers(rr, venue_time_s());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", rr.target(), headers, {}, [this, alive, q, offset](const net::HttpResponse& r) {
        if (alive.expired() || !trade_replay_.expects(q)) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        std::vector<TradeRecord> rows;
        std::string err;
        if (!r.ok()) {
          err = fmt::format(
              "status={} err={} {}", r.status, net::to_string(r.error), r.body.substr(0, 120));
        } else {
          err = decode_my_trades(r.body, rows);
        }
        if (!err.empty()) {
          ++stats_.rest_errors;
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR(
              "{}: GET my_trades_timerange failed ({}); this reconciliation cannot book the fills "
              "the private stream missed",
              cfg_.name,
              err);
          trade_replay_.failed(q);
          return;
        }
        ReplayPage<TradeRow> page;
        for (TradeRecord& t : rows) {
          TradeRow row{symbols_->find(id_, t.contract),
                       std::move(t.id),
                       std::move(t.order_id),
                       std::move(t.text),
                       t.size,
                       t.price,
                       t.fee,
                       t.maker,
                       t.time_ms};
          std::string key = row.id;
          page.rows.push_back({row.time_ms, 0, std::move(key), std::move(row)});
        }
        page.more = rows.size() >= static_cast<std::size_t>(kTradesPage);
        if (page.more) page.next = std::to_string(offset + kTradesPage);
        trade_replay_.answer(q, std::move(page));
      });
  if (!queued) {
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: no room to ask for the account's fills", cfg_.name);
  }
  return queued;
}

bool GateUsdtVenue::emit_trade(const TradeRow& t) {
  if (!t.inst.valid()) return false;
  const bool bid = t.size.raw >= 0;
  ClientOrderId cl{};
  if (const auto id = cl_ord_id_of_text(t.text)) cl = current_id(*id);
  emit_replayed_fill(*order_sink_,
                     id_,
                     t.inst,
                     cl,
                     t.order_id,
                     t.id,
                     bid ? Side::Buy : Side::Sell,
                     t.price,
                     bid ? t.size : -t.size,
                     t.fee,
                     FeeAsset::Quote,
                     t.maker ? Liquidity::Maker : Liquidity::Taker,
                     t.time_ms);
  ++stats_.order_events;
  ++stats_.executions_fetched;
  return true;
}

// ---- control requests -------------------------------------------------------------------------

void GateUsdtVenue::request_server_time() {
  if (rest_ == nullptr || time_request_pending_ || rest_hard_stopped_) return;
  time_request_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const std::string target =
      fmt::format("/api/v4/futures/{}/order_book?contract={}&limit=1",
                  cfg_.settle,
                  subscribed_.empty() ? std::string_view("BTC_USDT")
                                      : symbols_->venue_symbol(subscribed_.front()));
  const bool queued =
      rest_->request("GET", target, {}, {}, [this, alive](const net::HttpResponse& r) {
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
        const std::int64_t offset = server_ms - local_recv_ms;
        clock_offset_ms_.store(offset);
        clock_sync_ns_ = now_ns();
        clock_resync_wanted_ = false;
        stats_.clock_offset_ms = offset;
        if (offset > 1000 || offset < -1000)
          FASTMM_LOG_WARN("{}: clock offset to venue is {} ms", cfg_.name, offset);
      });
  if (!queued) time_request_pending_ = false;
}

void GateUsdtVenue::cancel_all_async() {
  if (rest_ == nullptr || !signer_.usable()) return;
  for (InstrumentId id : subscribed_) {
    RestRequest rr;
    if (!encoder_->encode_rest_cancel_all(symbols_->venue_symbol(id), rr)) continue;
    const std::string headers = encoder_->rest_headers(rr, venue_time_s());
    std::weak_ptr<int> alive = alive_;
    static_cast<void>(rest_->request(
        rr.method, rr.target(), headers, {}, [this, alive, id](const net::HttpResponse& r) {
          if (alive.expired() || r.error == net::NetError::Canceled) return;
          ++stats_.rest_requests;
          note_rate_headers(r);
          if (!r.ok()) {
            ++stats_.rest_errors;
            FASTMM_LOG_ERROR("{}: cancel-all for {} failed: status={} {}",
                             cfg_.name,
                             symbols_->venue_symbol(id),
                             r.status,
                             r.body.substr(0, 120));
          }
        }));
  }
}

bool GateUsdtVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr || encoder_ == nullptr) return true;
  BlockingRetry retry;
  BlockingControl control(cfg_, retry);
  return control.per_target(
      "kill-switch cancel-all",
      std::span<const InstrumentId>(subscribed_),
      [&](InstrumentId id) { return symbols_->venue_symbol(id); },
      [&](InstrumentId id, BlockingRequest& q) {
        RestRequest rr;
        if (!encoder_->encode_rest_cancel_all(symbols_->venue_symbol(id), rr)) return false;
        q.method = std::string(rr.method);
        q.target = rr.target();
        q.headers = encoder_->rest_headers(rr, venue_time_s());
        q.body = rr.body;
        return true;
      },
      // A 2xx with the list of cancelled orders (empty when nothing was open).
      [](const HttpReply& reply, std::string& why) {
        if (reply.ok()) return true;
        std::string label;
        std::string msg;
        if (decode_error(reply.body, label, msg)) why = label + " " + msg;
        return false;
      });
}

// POST /countdown_cancel_all {"timeout":N}: the venue cancels every order of the account when no
// refresh arrives within N seconds; 0 stops it.
void GateUsdtVenue::send_countdown(std::int64_t timeout_s) {
  if (rest_ == nullptr || !signer_.usable() || cfg_.dry_run) return;
  RestRequest rr;
  if (!encoder_->encode_rest_countdown(timeout_s, {}, rr)) return;
  const std::uint32_t round = dms_.begin_round(now_ns(), 1);
  const std::string headers = encoder_->rest_headers(rr, venue_time_s());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "POST", rr.target(), headers, rr.body, [this, alive, round](const net::HttpResponse& r) {
        if (alive.expired() || r.error == net::NetError::Canceled) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (r.ok()) {
          dms_.confirmed(round);
          return;
        }
        ++stats_.rest_errors;
        FASTMM_LOG_ERROR("{}: countdown_cancel_all failed: status={} err={} {}",
                         cfg_.name,
                         r.status,
                         net::to_string(r.error),
                         r.body.substr(0, 120));
      });
  if (queued) dms_.went_out();
}

void GateUsdtVenue::stop_countdown_blocking() {
  if (!signer_.usable() || encoder_ == nullptr) return;
  BlockingControl control(cfg_);
  const HttpReply reply = control.send("stopping countdown_cancel_all", [&](BlockingRequest& q) {
    RestRequest rr;
    if (!encoder_->encode_rest_countdown(0, {}, rr)) return false;
    q.method = "POST";
    q.target = rr.target();
    q.headers = encoder_->rest_headers(rr, venue_time_s());
    q.body = rr.body;
    return true;
  });
  control.report("stopping countdown_cancel_all", reply, reply.ok(), reply.body.substr(0, 120));
}

// ---- housekeeping -----------------------------------------------------------------------------

void GateUsdtVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (now - last_ping_ns_ >= static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs) {
    last_ping_ns_ = now;
    const std::size_t n = GateOrderEncoder::encode_ping(venue_time_s(), request_buf_);
    const std::string_view ping(request_buf_, n);
    if (md_conn_.is_live()) static_cast<void>(md_conn_.send_text(ping));
    if (private_conn_.is_live()) static_cast<void>(private_conn_.send_text(ping));
    if (trade_conn_.is_live()) static_cast<void>(trade_conn_.send_text(ping));
  }
  if (clock_resync_wanted_ || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  reconcile_.on_timer(now);
  trade_replay_.on_timer(now);
  check_positions(now);
  if (!cfg_.dry_run && signer_.usable()) {
    switch (dms_.poll(now)) {
      case CountdownDriver::Step::Lapsed:
        FASTMM_LOG_ERROR(
            "{}: countdown_cancel_all not refreshed within {} ms; the venue has cancelled the "
            "account's orders: killing this venue",
            cfg_.name,
            dms_.window_ms());
        trip_venue_kill(KillReason::DeadMansSwitchLost);
        break;
      case CountdownDriver::Step::Refresh:
        send_countdown(dms_.window_ms() / 1000);
        break;
      case CountdownDriver::Step::None:
        break;
    }
  }
  shadow_overflow_.check(cfg_.name, shadows_.size(), decltype(shadows_)::kMaxSize);
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

void GateUsdtVenue::note_fill(const OrderFillMsg& f) noexcept {
  if (f.hdr.instrument.value >= kMaxInstruments) return;
  PositionCheck& p = positions_[f.hdr.instrument.value];
  p.tracked = f.side == Side::Buy ? p.tracked + f.qty : p.tracked - f.qty;
  p.last_event_ns = now_ns();
}

void GateUsdtVenue::check_positions(std::int64_t now) {
  if (order_sink_ == nullptr || reconcile_.busy() || trade_replay_.active()) return;
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

void GateUsdtVenue::publish_status() noexcept {
  stats_.shadows = shadows_.size();
  stats_.shadows_refused = shadow_overflow_.count();
  stats_.shadows_swept = reconcile_.shadows_swept();
  stats_.execution_queries = trade_replay_.replays();
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_.load(std::memory_order_relaxed);
  wire_.summarize(
      tsc_calibration(), stats_.wire_tick_to_trade, stats_.order_encode, stats_.order_send);
  published_.store(stats_);
}

VenueStatus GateUsdtVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

GateUsdtVenueConfig make_gate_usdt_config(const VenueSection& v, bool dry_run) {
  GateUsdtVenueConfig c;
  c.name = v.name;
  c.ws_url = v.ws_url.empty() ? std::string("wss://fx-ws.gateio.ws/v4/ws/usdt") : v.ws_url;
  c.rest_url = v.rest_url.empty() ? std::string("https://fx-api.gateio.ws") : v.rest_url;
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.supports_replace = v.supports_replace;
  c.dry_run = dry_run;
  c.credentials.api_key = v.api_key;
  c.credentials.secret.value = v.api_secret;
  const VenueExtras x(v.extra);
  auto extra = [&](const char* key) { return x.get(key); };
  auto extra_bool = [&](const char* key, bool def) { return x.flag(key, def); };
  auto extra_int = [&](const char* key, std::int64_t def) { return x.integer(key, def); };
  if (const std::string s = extra("settle"); !s.empty()) {
    std::string lower = s;
    for (char& ch : lower) ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch;
    if (lower != "usdt" && lower != "usd1" && lower != "btc")
      throw std::invalid_argument(
          fmt::format("venues.{}.settle: expected usdt, usd1 or btc, got \"{}\"", v.name, s));
    c.settle = lower;
  }
  if (extra("order_api") == "rest") c.ws_order_api = false;
  const std::int64_t level = extra_int("book_level", c.book_level);
  if (level != 50 && level != 400)
    throw std::invalid_argument(
        fmt::format("venues.{}.book_level: expected 50 or 400, got {}", v.name, level));
  c.book_level = static_cast<int>(level);
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, extra_int("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, extra_int("dead_ms", c.dead_ms)));
  c.ping_interval_ms = static_cast<std::uint32_t>(
      std::max<std::int64_t>(1000, extra_int("ping_interval_ms", c.ping_interval_ms)));
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, extra_int("orders_per_second", c.orders_per_second)));
  c.position_from_stream = extra_bool("position_from_stream", true);
  c.allow_offline_reference_data = extra_bool("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = extra_bool("cancel_on_order_channel_loss", true);
  c.emit_ack_from_response = extra_bool("emit_ack_from_response", true);
  c.fetch_fees = extra_bool("fetch_fees", false);
  c.dead_mans_switch_s = std::max<std::int64_t>(0, extra_int("dead_mans_switch_s", 0));
  return c;
}

}  // namespace fastmm::venues::gate
