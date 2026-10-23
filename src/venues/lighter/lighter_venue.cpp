#include "fastmm/venues/lighter/lighter_venue.hpp"

#include "fastmm/net/url.hpp"
#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/error_action.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fastmm::venues::lighter {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kPendingTimeoutNs = 30'000'000'000;
// An order the reply accepted that the stream has not named after this long: reconcile.
constexpr std::int64_t kUnseenNs = 10'000'000'000;
constexpr std::string_view kFormHeaders = "Content-Type: application/x-www-form-urlencoded\r\n";
constexpr std::string_view kPingFrame = R"({"type":"ping"})";
constexpr std::size_t kMaxRequestBytes = 4096;
// The private subscriptions: account_all_orders, account_all_trades.
constexpr std::uint32_t kPrivateChannels = 2;
constexpr std::int64_t kPow10[9] = {
    1, 10, 100, 1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000};
constexpr int kScaleDigits = 8;
// Nonces kept ready for the order path (some 100 us each to make): the pool, and how many the
// housekeeping tick adds.
constexpr std::size_t kPreparedNonces = 256;
constexpr std::size_t kRefillPerTick = 16;

std::int64_t percentile(std::vector<std::int64_t>& v, int pct) {
  if (v.empty()) return 0;
  const std::size_t k = std::min(v.size() - 1, v.size() * static_cast<std::size_t>(pct) / 100);
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  if (out.starts_with("0x")) out.erase(0, 2);
  return out;
}

// tx_type=..&tx_info=.. for POST /api/v1/sendTx.
std::string send_tx_form(std::uint8_t tx_type, std::string_view tx_info) {
  std::string body = fmt::format("tx_type={}&tx_info=", tx_type);
  std::string enc(tx_info.size() * 3, '\0');
  const std::size_t n = net::percent_encode(tx_info, enc);
  body.append(enc.data(), n);
  return body;
}

Timestamp ts_from_us(std::int64_t us) noexcept {
  return us > 0 ? Timestamp{us * 1000} : Timestamp{};
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

LighterVenue::LighterVenue(VenueId id, LighterVenueConfig cfg)
    : id_(id),
      cfg_(std::move(cfg)),
      signer_(cfg_.private_key),
      dms_(cfg_.dead_mans_switch_s * 1000) {
  cfg_.private_key.assign(cfg_.private_key.size(), '\0');  // the signer holds the key now
  cfg_.private_key.clear();
  if (cfg_.tx_per_minute > 0) rate_.add_weight_bucket(cfg_.tx_per_minute, 60'000'000'000);
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  request_buf_ = std::make_unique<char[]>(kMaxRequestBytes);
  info_buf_ = std::make_unique<char[]>(kMaxTxInfoBytes);
  private_parser_ = std::make_unique<LighterPrivateParser>(cfg_.account_index);
  trade_parser_ = std::make_unique<LighterPrivateParser>(cfg_.account_index, 1U << 16);
  reply_rtt_ms_.reserve(4096);
  open_lag_ms_.reserve(4096);
}

LighterVenue::~LighterVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps LighterVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = cfg_.ws_order_api;
  c.user_stream = keys();
  return c;
}

void LighterVenue::set_market(InstrumentId id,
                              std::uint32_t market,
                              int size_decimals,
                              int price_decimals) {
  if (id.value >= kMaxInstruments) return;
  markets_[id.value] = MarketSpec{market,
                                  std::clamp(size_decimals, 0, kScaleDigits),
                                  std::clamp(price_decimals, 0, kScaleDigits),
                                  true};
  market_map_.add(market, id);
}

std::uint32_t LighterVenue::price_units(Price px, int price_decimals, bool down) noexcept {
  if (px.raw <= 0) return 0;
  const std::int64_t step = kPow10[kScaleDigits - std::clamp(price_decimals, 0, kScaleDigits)];
  std::int64_t units = px.raw / step;
  if (!down && px.raw % step != 0) ++units;
  if (units <= 0 || units > 0xFFFFFFFFLL) return 0;
  return static_cast<std::uint32_t>(units);
}

std::int64_t LighterVenue::size_units(Qty q, int size_decimals) noexcept {
  if (q.raw <= 0) return 0;
  return q.raw / kPow10[kScaleDigits - std::clamp(size_decimals, 0, kScaleDigits)];
}

net::ConnectionConfig LighterVenue::ws_config(bool manual_subscribe) const {
  net::ConnectionConfig c;
  c.url = cfg_.ws_url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.source = cfg_.source;
  c.stale_ms = cfg_.stale_ms;
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, cfg_.ping_interval_ms * 2 + 5000);
  c.backoff = cfg_.backoff;
  c.manual_subscribe = manual_subscribe;
  return c;
}

InstrumentId LighterVenue::instrument_of_market(std::int64_t market) const noexcept {
  return market_map_.find(market);
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> LighterVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  BlockingHttpOptions opts = blocking_options(cfg_);
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    const HttpReply r = http.get("/api/v1/orderBookDetails");
    std::vector<MarketDetail> details;
    std::string err =
        r.status == 0 ? fmt::format("HTTP {}", r.error) : decode_market_details(r.body, details);
    if (!err.empty()) {
      if (!cfg_.allow_offline_reference_data)
        return fail(fmt::format("{}: orderBookDetails failed: {}", cfg_.name, err));
      FASTMM_LOG_WARN(
          "{}: orderBookDetails failed ({}); orders need the markets, none will be "
          "sent",
          cfg_.name,
          err);
    }
    for (Instrument* inst : mine) {
      if (details.empty()) break;
      const auto it = std::find_if(details.begin(), details.end(), [&](const MarketDetail& d) {
        return iequals_symbol(d.symbol, inst->symbol.view()) &&
               (d.market_type.empty() || d.market_type == "perp");
      });
      if (it == details.end())
        return fail(
            fmt::format("{}: {} is not a listed perp market", cfg_.name, inst->symbol.view()));
      const MarketDetail& m = *it;
      if (m.size_decimals > kScaleDigits || m.price_decimals > kScaleDigits)
        return fail(
            fmt::format("{}: {} has more decimals than FastMM's fixed point", cfg_.name, m.symbol));
      set_market(
          inst->id, static_cast<std::uint32_t>(m.market_id), m.size_decimals, m.price_decimals);
      symbols_by_id_[inst->id.value] = m.symbol;
      const Price tick = Price::from_raw(kPow10[kScaleDigits - m.price_decimals]);
      const Qty lot = Qty::from_raw(kPow10[kScaleDigits - m.size_decimals]);
      if (inst->tick != tick || inst->lot != lot)
        FASTMM_LOG_INFO("{}: {} (market {}) tick/lot from the venue: {} / {}",
                        cfg_.name,
                        m.symbol,
                        m.market_id,
                        tick,
                        lot);
      inst->tick = tick;
      inst->lot = lot;
      inst->min_qty = m.min_base.is_positive() ? m.min_base : lot;
      inst->min_notional = m.min_quote;
      inst->contract_multiplier = Qty::from_int(1);
      inst->expiry_ns = 0;
      inst->asset_class = AssetClass::Perpetual;
      inst->flags = static_cast<std::uint8_t>((inst->flags | Instrument::kReduceOnlySupported) &
                                              ~Instrument::kInverse);
      if (!inst->quote.assign("USDC"))
        return fail(fmt::format("{}: {} quote too long", cfg_.name, m.symbol));
      static_cast<void>(
          inst->base.assign(std::string_view(m.symbol).substr(0, inst->base.kCapacity)));
      if (!m.active) {
        FASTMM_LOG_ERROR("{}: {} is not active: disabled", cfg_.name, m.symbol);
        inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kEnabled);
      }
    }
    if (keys()) {
      // The key must be the one the venue holds in that slot.
      const HttpReply k = http.get(fmt::format("/api/v1/apikeys?account_index={}&api_key_index={}",
                                               cfg_.account_index,
                                               cfg_.api_key_index));
      std::vector<ApiKeyRow> rows;
      std::string kerr =
          k.status == 0 ? fmt::format("HTTP {}", k.error) : decode_api_keys(k.body, rows);
      if (!kerr.empty()) return fail(fmt::format("{}: apikeys failed: {}", cfg_.name, kerr));
      const std::string mine_pk = signer_.public_key_hex();
      const auto slot = std::find_if(rows.begin(), rows.end(), [&](const ApiKeyRow& a) {
        return a.api_key_index == cfg_.api_key_index;
      });
      if (slot == rows.end() || lower(slot->public_key) != mine_pk) {
        refused_account_settings_ = true;
        return fail(fmt::format(
            "{}: account {} holds a different public key in API key slot {} ({}); register this "
            "key ({}) first (tools/lighter_api_key.py)",
            cfg_.name,
            cfg_.account_index,
            cfg_.api_key_index,
            slot == rows.end() ? std::string("empty") : slot->public_key,
            mine_pk));
      }
      const HttpReply n =
          http.get(fmt::format("/api/v1/nextNonce?account_index={}&api_key_index={}",
                               cfg_.account_index,
                               cfg_.api_key_index));
      std::int64_t next = -1;
      std::string nerr =
          n.status == 0 ? fmt::format("HTTP {}", n.error) : decode_next_nonce(n.body, next);
      if (!nerr.empty()) return fail(fmt::format("{}: nextNonce failed: {}", cfg_.name, nerr));
      nonce_ = next;
      FASTMM_LOG_INFO("{}: account {} API key {} ({}...): next nonce {}",
                      cfg_.name,
                      cfg_.account_index,
                      cfg_.api_key_index,
                      mine_pk.substr(0, 12),
                      next);
    }
  } catch (const std::exception& e) {
    if (!cfg_.allow_offline_reference_data)
      return fail(
          fmt::format("{}: reference data failed: {}", cfg_.name, std::string_view(e.what())));
    FASTMM_LOG_WARN("{}: reference data failed ({}); keeping the configured values",
                    cfg_.name,
                    std::string_view(e.what()));
  }
  FASTMM_LOG_INFO("{}: reference data loaded for {} market(s)", cfg_.name, mine.size());
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void LighterVenue::attach(const SymbolTable& symbols,
                          const InstrumentTable& instruments,
                          EventSink& md_sink,
                          EventSink& order_sink,
                          MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ = std::make_unique<LighterMdFeed>(
      symbols,
      id_,
      md_sink,
      InstrumentCallback{&LighterVenue::resubscribe_requester, this},
      cfg_.channels);
  md_feed_->set_log_name(cfg_.name);
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments);
}

void LighterVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    if (id.value >= kMaxInstruments) continue;
    if (!markets_[id.value].known) {
      FASTMM_LOG_ERROR("{}: {} has no market index (no reference data): not subscribed",
                       cfg_.name,
                       instruments_->get(id).symbol.view());
      continue;
    }
    if (symbols_by_id_[id.value].empty())
      symbols_by_id_[id.value] = std::string(instruments_->get(id).symbol.view());
    subscribed_.push_back(id);
    if (md_feed_) md_feed_->add_instrument(id, markets_[id.value].market, symbols_by_id_[id.value]);
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (connected_ && md_conn_.opened()) {
    md_conn_.close();
    md_conn_.open(*reactor_, ws_config(false), md_handler_);
    md_conn_.connect();
  }
}

void LighterVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("LighterVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  const bool k = keys();
  reconcile_.open(k);
  if (k) signer_.prepare(kPreparedNonces);
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    if (k) {
      raw_private_.open(cfg_.record_raw_dir, cfg_.name, "private");
      raw_trade_.open(cfg_.record_raw_dir, cfg_.name, "trade");
    }
  }
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
  md_conn_.open(*reactor_, ws_config(false), md_handler_);
  md_conn_.connect();
  if (k) {
    private_conn_.open(*reactor_, ws_config(true), private_handler_);
    private_conn_.connect();
    private_opened_ns_ = now_ns();
    if (cfg_.ws_order_api) {
      trade_conn_.open(*reactor_, ws_config(false), trade_handler_);
      trade_conn_.connect();
    }
  }
  last_ping_ns_ = now_ns();
  last_latency_log_ns_ = now_ns();
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting (chain {}, account {}, API key {}, dry_run={}, ws_orders={})",
                  cfg_.name,
                  cfg_.chain_id,
                  cfg_.account_index,
                  cfg_.api_key_index,
                  cfg_.dry_run,
                  cfg_.ws_order_api);
}

void LighterVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  reconcile_.close();
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  if (dms_.needs_stop() && keys() && !dms_refused_) {
    // Remove the scheduled cancel: a clean stop has cancelled what it wanted to.
    const std::string info = signed_cancel_all(kCancelAllAbort, 0);
    if (!info.empty()) {
      BlockingControl control(cfg_);
      const HttpReply reply =
          control.send("aborting the scheduled cancel", [&](BlockingRequest& q) {
            q.method = "POST";
            q.target = "/api/v1/sendTx";
            q.headers = std::string(kFormHeaders);
            q.body = send_tx_form(kTxCancelAll, info);
            return true;
          });
      TxReply tr;
      const bool ok = reply.status != 0 && decode_send_tx(reply.body, tr).empty() && !tr.refused;
      control.report("aborting the scheduled cancel", reply, ok, reply.body.substr(0, 160));
    }
  }
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

// ---- market data ------------------------------------------------------------------------------

void LighterVenue::on_md_state(net::ConnState s) {
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
        if (LighterBookSync* sync = md_feed_->sync(id))
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

void LighterVenue::on_md_open() {
  for (const std::string& p : md_feed_->subscription_payloads()) {
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void LighterVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (LighterBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void LighterVenue::on_md_text(std::string_view t, std::int64_t ts) {
  if (raw_md_.enabled()) raw_md_.record(ts, t);
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Error)
    FASTMM_LOG_ERROR("{}: market-data error: {}", cfg_.name, md_feed_->last_error());
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  }
}

void LighterVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;
  for (const std::string& p : md_feed_->resubscribe_payloads(id))
    static_cast<void>(md_conn_.send_text(p));
  FASTMM_LOG_INFO("{}: resubscribing market {} for a fresh book", cfg_.name, market_of(id));
}

// ---- account stream -----------------------------------------------------------------------------

const std::string& LighterVenue::auth() noexcept {
  const std::int64_t now_s = wall_ms() / 1000;
  // A fresh token once half its life is gone.
  if (auth_.empty() || now_s >= auth_deadline_s_ - cfg_.auth_ttl_s / 2) {
    auth_deadline_s_ = now_s + cfg_.auth_ttl_s;
    auth_ = auth_token(signer_,
                       auth_deadline_s_,
                       cfg_.account_index,
                       static_cast<std::uint8_t>(cfg_.api_key_index));
  }
  return auth_;
}

void LighterVenue::on_private_state(net::ConnState s) {
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

void LighterVenue::on_private_open() {
  private_subscribed_ = 0;
  const std::string& token = auth();
  if (token.empty()) {
    apply_fatal("could not sign the auth token");
    return;
  }
  const std::string orders =
      subscription_frame(true, "account_all_orders", cfg_.account_index, token);
  const std::string trades = subscription_frame(true, "account_all_trades", cfg_.account_index);
  if (!private_conn_.send_text(orders) || !private_conn_.send_text(trades))
    FASTMM_LOG_ERROR("{}: could not subscribe the account channels", cfg_.name);
}

ClientOrderId LighterVenue::current_id(std::int64_t client_index) const noexcept {
  const ClientOrderId key{static_cast<std::uint64_t>(client_index)};
  if (const ClientOrderId* cur = aliases_.find(key)) return *cur;
  return key;
}

void LighterVenue::forget_order(ClientOrderId id) noexcept {
  if (const Shadow* s = shadows_.find(id)) {
    const ClientOrderId chain{static_cast<std::uint64_t>(s->client_index)};
    if (chain != id) aliases_.erase(chain);
  }
  shadows_.erase(id);
}

void LighterVenue::push_order_event(EventHeader& h) {
  sent_.answered(h);
  static_cast<void>(order_sink_->push(h));
  ++stats_.order_events;
}

void LighterVenue::ack(ClientOrderId id, Shadow& s, Timestamp venue_ts) {
  s.acked = true;
  s.answered = true;
  sent_.answered(id);
  emit_order_ack(*order_sink_,
                 id_,
                 s.instrument,
                 id,
                 s.order_index != 0 ? IdText(s.order_index).view() : std::string_view{},
                 0,
                 venue_ts);
  ++stats_.order_events;
}

void LighterVenue::flush_terminal(ClientOrderId id, Shadow& s) {
  const IdText oid(s.order_index);
  const std::string_view vid = s.order_index != 0 ? oid.view() : std::string_view{};
  switch (s.terminal) {
    case Terminal::CancelAck:
      emit_cancel_ack(*order_sink_,
                      id_,
                      s.instrument,
                      id,
                      vid,
                      s.terminal_cum,
                      ts_from_us(s.terminal_venue_us));
      ++stats_.order_events;
      break;
    case Terminal::Expired: {
      OrderExpiredMsg m{};
      init_header(m, EventType::OrderExpired, s.instrument, id_);
      m.cl_ord_id = id;
      static_cast<void>(m.venue_order_id.assign(vid));
      m.cum_qty = s.terminal_cum;
      m.hdr.exch_ts = ts_from_us(s.terminal_venue_us);
      m.hdr.recv_ts = wall_now();
      m.hdr.t0_cycles = rdtscp();
      push_order_event(m.hdr);
      break;
    }
    case Terminal::Reject:
      emit_order_reject(
          *order_sink_, id_, s.instrument, id, s.terminal_reason, 0, "ended by the venue");
      ++stats_.order_events;
      break;
    case Terminal::None:
      return;
  }
  sent_.answered(id);
  forget_order(id);
}

void LighterVenue::on_order_row(const OrderRow& row) {
  if (row.client_order_index <= 0) return;  // not placed by a client index: not ours
  const ClientOrderId id = current_id(row.client_order_index);
  Shadow* s = shadows_.find(id);
  if (s == nullptr) return;  // forgotten, or another client's: the reconciliation decides
  if (row.order_index != 0) s->order_index = row.order_index;
  s->seen = true;
  const StatusClass cls = classify_status(row.status);
  const Timestamp venue_ts = ts_from_us(row.time_us);
  switch (cls) {
    case StatusClass::Ignore:
      return;
    case StatusClass::Open:
      if (!s->acked) {
        if (s->sent_wall_ns > 0 && row.time_us > 0 && open_lag_ms_.size() < 100'000)
          open_lag_ms_.push_back((row.time_us * 1000 - s->sent_wall_ns) / kNsPerMs);
        ack(id, *s, venue_ts);
      }
      return;
    case StatusClass::Filled:
      if (!s->acked) ack(id, *s, venue_ts);
      return;  // the fills finish it
    case StatusClass::Canceled:
    case StatusClass::Ended:
      break;
  }
  if (s->terminal != Terminal::None) return;
  if (cls == StatusClass::Ended && !s->acked && !row.filled.is_positive() &&
      !s->cum_qty.is_positive()) {
    // Never rested: a post-only that would have crossed, an IOC with nothing to take, no margin.
    s->terminal = Terminal::Reject;
    s->terminal_reason = ended_reason(row.status);
    FASTMM_LOG_INFO("{}: order {} ended by the venue: {}", cfg_.name, id.value, row.status);
    flush_terminal(id, *s);
    return;
  }
  if (!s->acked) ack(id, *s, venue_ts);
  s->terminal = cls == StatusClass::Canceled ? Terminal::CancelAck : Terminal::Expired;
  s->terminal_cum = row.filled;
  s->terminal_ns = now_ns();
  s->terminal_venue_us = row.time_us;
  if (s->cum_qty >= s->terminal_cum) flush_terminal(id, *s);
}

void LighterVenue::on_fill_row(const FillRow& row) {
  const ClientOrderId id =
      row.client_order_index > 0 ? current_id(row.client_order_index) : ClientOrderId{};
  Shadow* s = id.valid() ? shadows_.find(id) : nullptr;
  const InstrumentId inst = s != nullptr ? s->instrument : instrument_of_market(row.market);
  if (!inst.valid()) return;
  if (s != nullptr) {
    if (row.order_index != 0) s->order_index = row.order_index;
    if (!s->acked) ack(id, *s, ts_from_ms(row.time_ms));
  }
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill, inst, id_);
  m.cl_ord_id = id;
  static_cast<void>(m.venue_order_id.assign(IdText(row.order_index).view()));
  ExecId exec;
  exec.append(IdText(static_cast<std::int64_t>(row.trade_id)).view());
  if (row.self_trade_other_leg) exec.push_back('b');
  m.exec_id = exec;
  m.price = row.price;
  m.qty = row.size;
  m.fee = fee_of(row.usd, row.fee_rate);
  m.side = row.side;
  m.liquidity = row.maker ? Liquidity::Maker : Liquidity::Taker;
  m.fee_asset = FeeAsset::Quote;
  m.flags = 0;
  m.hdr.venue_seq = row.trade_id;
  m.hdr.exch_ts = ts_from_ms(row.time_ms);
  m.hdr.recv_ts = wall_now();
  m.hdr.t0_cycles = rdtscp();
  if (s != nullptr) {
    s->cum_qty = s->cum_qty + row.size;
    m.cum_qty = s->cum_qty;
    m.leaves_qty = s->orig_qty > s->cum_qty ? s->orig_qty - s->cum_qty : Qty{};
  } else {
    m.cum_qty = row.size;
    m.leaves_qty = Qty{};
  }
  push_order_event(m.hdr);
  if (s == nullptr) return;
  if (s->terminal != Terminal::None) {
    if (s->cum_qty >= s->terminal_cum) flush_terminal(id, *s);
  } else if (!m.leaves_qty.is_positive()) {
    forget_order(id);
  }
}

void LighterVenue::on_private_text(std::string_view t, std::int64_t ts) {
  if (raw_private_.enabled()) raw_private_.record(ts, t);
  const PrivateFrame f = private_parser_->decode(t);
  switch (f.kind) {
    case FrameKind::Orders:
      for (const OrderRow& row : private_parser_->orders()) on_order_row(row);
      return;
    case FrameKind::Trades:
      for (const FillRow& row : private_parser_->fills()) on_fill_row(row);
      return;
    case FrameKind::Ignored:
      if (f.type.starts_with("subscribed/") && ++private_subscribed_ >= kPrivateChannels)
        private_conn_.subscribe_done();
      return;
    case FrameKind::Error:
    case FrameKind::TxReply:
      FASTMM_LOG_ERROR(
          "{}: private channel error {}: {}", cfg_.name, f.reply.code, f.reply.message);
      if (contains_ci(f.reply.message, "auth")) apply_fatal(f.reply.message);
      return;
    case FrameKind::Malformed:
      FASTMM_LOG_WARN("{}: malformed private frame: {}", cfg_.name, t.substr(0, 200));
      return;
  }
}

// ---- trade channel --------------------------------------------------------------------------

void LighterVenue::on_trade_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.order = private_channel_state(s);
  if (mapped == trade_state_) return;
  const ConnState prev = trade_state_;
  trade_state_ = mapped;
  if (mapped != ConnState::Live && mapped != ConnState::Stale) sent_.connection_lost();
  if (mapped == ConnState::Live) {
    const bool reconnected = trade_was_live_ && prev != ConnState::Stale;
    trade_was_live_ = true;
    if (prev != ConnState::Stale) FASTMM_LOG_INFO("{}: trade channel -> Live", cfg_.name);
    drain_outbound();
    if (reconnected && !cfg_.dry_run) {
      refresh_nonce();
      request_open_orders();
    }
    return;
  }
  if (mapped == ConnState::Stale) return;
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    // The transactions in flight on it will not be answered there: the reconciliation decides.
    std::size_t lost = 0;
    for (Pending& p : pending_) {
      if (p.id != 0) {
        p.id = 0;
        ++lost;
      }
    }
    FASTMM_LOG_WARN("{}: trade channel lost ({} transaction(s) unanswered)", cfg_.name, lost);
    if (lost > 0) reconcile_.request();
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

LighterVenue::Pending* LighterVenue::new_pending(std::uint64_t id) {
  Pending& p = pending_[id % kPendingSlots];
  if (p.id != 0) {
    FASTMM_LOG_WARN(
        "{}: transaction {} still unanswered when its slot was reused", cfg_.name, p.id);
    reconcile_.request();
  }
  p = Pending{};
  p.id = id;
  return &p;
}

LighterVenue::Pending* LighterVenue::find_pending(std::uint64_t id) {
  Pending& p = pending_[id % kPendingSlots];
  return p.id == id && id != 0 ? &p : nullptr;
}

void LighterVenue::on_trade_text(std::string_view t, std::int64_t ts) {
  if (raw_trade_.enabled()) raw_trade_.record(ts, t);
  const PrivateFrame f = trade_parser_->decode(t);
  if (f.kind == FrameKind::Ignored) return;  // connected, pong
  if (f.kind != FrameKind::TxReply && f.kind != FrameKind::Error) {
    FASTMM_LOG_WARN("{}: unreadable trade frame: {}", cfg_.name, t.substr(0, 200));
    return;
  }
  const auto id = parse_int64(f.reply.id);
  if (!id || *id <= 0) {
    // Nothing names the transaction: its order is decided by a reconciliation.
    FASTMM_LOG_ERROR(
        "{}: the venue refused a request: {} {}", cfg_.name, f.reply.code, f.reply.message);
    reconcile_.request();
    return;
  }
  Pending* p = find_pending(static_cast<std::uint64_t>(*id));
  if (p == nullptr) return;  // timed out, or not ours
  const Pending copy = *p;
  p->id = 0;
  if (copy.control) {
    handle_control_reply(copy, f.reply);
  } else {
    handle_reply(copy, f.reply);
  }
}

void LighterVenue::handle_control_reply(const Pending& p, const TxReply& r) {
  if (r.quota_remaining >= 0) quota_ = r.quota_remaining;
  if (!r.refused) {
    dms_.confirmed(p.round);
    return;
  }
  const TxErrorMapping m = map_tx_error(r.code, r.message);
  if (m.nonce) refresh_nonce();
  if (m.rate_limited || m.nonce) {
    FASTMM_LOG_WARN(
        "{}: the scheduled cancel was refused ({} {}); trying again", cfg_.name, r.code, r.message);
    return;
  }
  if (!dms_refused_)
    FASTMM_LOG_WARN("{}: the scheduled cancel was refused: {} {}; going on without it",
                    cfg_.name,
                    r.code,
                    r.message);
  dms_refused_ = true;
}

void LighterVenue::handle_reply(const Pending& p, const TxReply& r) {
  const std::int64_t rtt_ms = (now_ns() - p.sent_ns) / kNsPerMs;
  if (reply_rtt_ms_.size() < 100'000) reply_rtt_ms_.push_back(rtt_ms);
  if (r.quota_remaining >= 0) quota_ = r.quota_remaining;
  const Entry& e = p.entry;
  if (!r.refused) {
    switch (e.kind) {
      case OrderCommandKind::New:
        if (Shadow* s = shadows_.find(e.cl_ord_id)) {
          s->accepted_ns = now_ns();
          if (cfg_.emit_ack_from_response && !s->acked) ack(e.cl_ord_id, *s, Timestamp{});
        }
        break;
      case OrderCommandKind::Replace:
        if (Shadow* s = shadows_.find(e.cl_ord_id)) {
          // Applied in place: the chain's later events are the new id's.
          s->accepted_ns = now_ns();
          s->acked = true;
          s->answered = true;
          const ClientOrderId chain{static_cast<std::uint64_t>(s->client_index)};
          if (chain != e.cl_ord_id) aliases_.assign(chain, e.cl_ord_id);
          const IdText oid(s->order_index);
          shadows_.erase(e.orig_cl_ord_id);
          sent_.answered(e.cl_ord_id);
          emit_order_ack(*order_sink_,
                         id_,
                         e.instrument,
                         e.cl_ord_id,
                         s->order_index != 0 ? oid.view() : std::string_view{},
                         OrderAckMsg::kAmendedInPlace);
          ++stats_.order_events;
        }
        break;
      case OrderCommandKind::Cancel:
        if (!private_conn_.is_live()) {
          // Without the stream the reply is all that will come.
          if (Shadow* s = shadows_.find(e.cl_ord_id)) {
            s->terminal = Terminal::CancelAck;
            s->terminal_cum = s->cum_qty;
            flush_terminal(e.cl_ord_id, *s);
          }
        }
        break;
    }
    return;
  }
  const TxErrorMapping m = map_tx_error(r.code, r.message);
  const std::string why = fmt::format("{} {}", r.code, r.message);
  FASTMM_LOG_WARN("{}: transaction {} (nonce {}) refused: {}", cfg_.name, p.id, e.nonce, why);
  if (e.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_,
                       id_,
                       e.instrument,
                       e.cl_ord_id,
                       m.unknown_order ? RejectReason::VenueUnknownOrder : m.reason,
                       static_cast<int>(r.code),
                       why);
    ++stats_.order_events;
  } else {
    sent_.answered(e.cl_ord_id);
    const Shadow* s = shadows_.find(e.cl_ord_id);
    if (s == nullptr || !s->answered) {
      emit_order_reject(
          *order_sink_, id_, e.instrument, e.cl_ord_id, m.reason, static_cast<int>(r.code), why);
      ++stats_.order_events;
    }
    if (e.kind == OrderCommandKind::Replace) {
      shadows_.erase(e.cl_ord_id);  // the original keeps working
    } else {
      forget_order(e.cl_ord_id);
    }
  }
  if (m.nonce) refresh_nonce();
  if (m.rate_limited) {
    rate_.cooldown(1'000'000'000, now_ns());
    ++stats_.rate_limit_cooldowns;
  }
  if (m.fatal) apply_fatal(why);
}

// ---- outbound ---------------------------------------------------------------------------------

void LighterVenue::on_wake() {
  drain_outbound();
}

void LighterVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void LighterVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

template <class Ring>
void LighterVenue::write_orders(Ring& ring) {
  drain_outbound_coalesced(
      ring,
      wire_,
      [this] {
        trade_conn_.cork();
        n_corked_ = 0;
      },
      [this](const EventHeader& h) {
        if (const auto cmd = OrderCommand::from(h)) {
          sent_.note(*cmd, now_ns());
          stage(*cmd);
        } else if (is_reconcile_request(h)) {
          request_open_orders();
        }
      },
      [this] {
        if (trade_conn_.uncork()) return true;
        // Nothing written reached the venue: the engine hears so, the replies will not come.
        for (std::size_t i = 0; i < n_corked_; ++i) {
          if (Pending* p = find_pending(corked_ids_[i])) p->id = 0;
          reject_entry(corked_[i], RejectReason::TransportFull, "order batch not written");
        }
        ++stats_.order_send_failures;
        return false;
      });
}

void LighterVenue::refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why) {
  sent_.answered(cmd.cl_ord_id);
  if (cmd.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  } else {
    emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  }
  ++stats_.order_events;
}

void LighterVenue::reject_entry(const Entry& e, RejectReason reason, std::string_view why) {
  sent_.answered(e.cl_ord_id);
  if (e.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, e.instrument, e.cl_ord_id, reason, 0, why);
  } else {
    emit_order_reject(*order_sink_, id_, e.instrument, e.cl_ord_id, reason, 0, why);
    if (e.kind == OrderCommandKind::Replace) {
      shadows_.erase(e.cl_ord_id);
    } else {
      forget_order(e.cl_ord_id);
    }
  }
  ++stats_.order_events;
}

bool LighterVenue::quota_allows() const noexcept {
  return quota_ < 0 || quota_ > cfg_.quota_reserve;
}

TxHeader LighterVenue::tx_header() noexcept {
  TxHeader h;
  h.account_index = cfg_.account_index;
  h.api_key_index = static_cast<std::uint8_t>(cfg_.api_key_index);
  h.expired_at = wall_ms() + kDefaultTxExpiryMs;
  h.nonce = nonce_++;
  if (cfg_.skip_nonce) static_cast<void>(h.attributes.add(kAttrSkipNonce, 1));
  return h;
}

void LighterVenue::stage(const OrderCommand& cmd) {
  note_taken(cmd);
  const std::int64_t now = now_ns();
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  if (cfg_.dry_run) return refuse(cmd, RejectReason::VenueKilled, "dry-run: orders disabled");
  if (fatal_ && !is_cancel) return refuse(cmd, RejectReason::VenueKilled, "venue fatal");
  if (!keys()) return refuse(cmd, RejectReason::VenueReject, "no API key");
  if (nonce_ < 0) return refuse(cmd, RejectReason::VenueReject, "the API key's nonce is unknown");
  if (!is_cancel) {
    if (cmd.kind == OrderCommandKind::New && cmd.tif == TimeInForce::Fok)
      return refuse(cmd, RejectReason::VenueReject, "FOK is not supported (use IOC)");
    if (cmd.kind == OrderCommandKind::Replace && !cfg_.supports_replace)
      return refuse(cmd, RejectReason::VenueReject, "replace not enabled (supports_replace)");
    const RateCheck c = rate_.check(1, now, true);
    if (!c.ok()) {
      count_refusal(stats_, c);
      ++stats_.rate_limit_cooldowns;
      return refuse(cmd, RejectReason::VenueRateLimit, refusal_text(c));
    }
    if (!quota_allows()) {
      ++stats_.refused_orders_1d;
      return refuse(cmd, RejectReason::VenueRateLimit, "local rate limit: volume quota");
    }
  } else {
    const RateCheck c = rate_.check(1, now, false);
    if (!c.ok()) {
      count_refusal(stats_, c);
      ++stats_.rate_limit_cooldowns;
      return refuse(cmd, RejectReason::VenueRateLimit, refusal_text(c));
    }
  }
  Entry e{cmd.kind, cmd.instrument, cmd.cl_ord_id, cmd.orig_cl_ord_id, 0, false};
  SigBytes sig{};
  switch (cmd.kind) {
    case OrderCommandKind::New: {
      if (cmd.instrument.value >= kMaxInstruments || !markets_[cmd.instrument.value].known)
        return refuse(cmd, RejectReason::InvalidLot, "no market for the instrument");
      const MarketSpec& m = markets_[cmd.instrument.value];
      const auto index = static_cast<std::int64_t>(cmd.cl_ord_id.value);
      if (index < 1 || index > kMaxClientOrderIndex)
        return refuse(cmd, RejectReason::VenueReject, "client order id beyond 48 bits");
      const bool buy = cmd.side == Side::Buy;
      CreateOrderTx tx;
      tx.market_index = static_cast<std::int16_t>(m.market);
      tx.client_order_index = index;
      tx.base_amount = size_units(cmd.qty, m.size_decimals);
      tx.price = price_units(cmd.price, m.price_decimals, buy);
      if (tx.base_amount <= 0 || tx.price == 0)
        return refuse(cmd, RejectReason::InvalidLot, "no price or size for the venue");
      tx.is_ask = buy ? 0 : 1;
      tx.type = kOrderLimit;
      tx.reduce_only = cmd.reduce_only ? 1 : 0;
      if (cmd.type == OrderType::PostOnly) {
        tx.time_in_force = kTifPostOnly;
        tx.order_expiry = wall_ms() + cfg_.order_expiry_s * 1000;
      } else if (cmd.type == OrderType::Market || cmd.tif == TimeInForce::Ioc) {
        tx.time_in_force = kTifIoc;
        tx.order_expiry = 0;
      } else {
        tx.time_in_force = kTifGoodTillTime;
        tx.order_expiry = wall_ms() + cfg_.order_expiry_s * 1000;
      }
      Shadow s{};
      s.instrument = cmd.instrument;
      s.side = cmd.side;
      s.type = cmd.type;
      s.tif = cmd.tif;
      s.client_index = index;
      s.sent_seq = sent_.last_seq();
      s.orig_qty = Qty::from_raw(tx.base_amount * kPow10[kScaleDigits - m.size_decimals]);
      s.sent_wall_ns = wall_now().ns;
      if (shadows_.assign(cmd.cl_ord_id, s) == nullptr) {
        shadow_overflow_.refused(cfg_.name, shadows_.size());
        return refuse(cmd, RejectReason::OrderTableFull, "order table full");
      }
      const Cycles before = rdtscp();
      tx.h = tx_header();
      e.nonce = tx.h.nonce;
      e.quota = true;
      if (!signer_.sign(tx_hash(tx, cfg_.chain_id), sig)) {
        forget_order(cmd.cl_ord_id);
        return refuse(cmd, RejectReason::VenueReject, "signing failed");
      }
      const std::size_t n = write_tx_info(tx, sig, {info_buf_.get(), kMaxTxInfoBytes});
      wire_.record(cmd.t0_cycles(), before, rdtscp(), rdtscp());
      if (n == 0 || !send_tx(kTxCreateOrder, {info_buf_.get(), n}, e))
        return reject_entry(e, RejectReason::TransportFull, "not sent");
      rate_.on_sent(1, now, true);
      ++stats_.orders_sent;
      return;
    }
    case OrderCommandKind::Replace: {
      const Shadow* orig = shadows_.find(cmd.orig_cl_ord_id);
      if (orig == nullptr)
        return refuse(cmd, RejectReason::UnknownOrder, "modify: original unknown");
      const MarketSpec& m = markets_[orig->instrument.value];
      ModifyOrderTx tx;
      tx.market_index = static_cast<std::int16_t>(m.market);
      tx.index = orig->client_index;
      tx.base_amount = size_units(cmd.qty, m.size_decimals);
      tx.price = price_units(cmd.price, m.price_decimals, orig->side == Side::Buy);
      if (tx.base_amount <= 0 || tx.price == 0)
        return refuse(cmd, RejectReason::InvalidLot, "no price or size for the venue");
      Shadow copy = *orig;
      copy.replaces = cmd.orig_cl_ord_id;
      copy.sent_seq = sent_.last_seq();
      copy.orig_qty = Qty::from_raw(tx.base_amount * kPow10[kScaleDigits - m.size_decimals]);
      copy.acked = false;
      copy.answered = false;
      copy.sent_wall_ns = wall_now().ns;
      e.instrument = orig->instrument;
      if (shadows_.assign(cmd.cl_ord_id, copy) == nullptr) {
        shadow_overflow_.refused(cfg_.name, shadows_.size());
        return refuse(cmd, RejectReason::OrderTableFull, "order table full");
      }
      const Cycles before = rdtscp();
      tx.h = tx_header();
      e.nonce = tx.h.nonce;
      e.quota = true;
      if (!signer_.sign(tx_hash(tx, cfg_.chain_id), sig)) {
        shadows_.erase(cmd.cl_ord_id);
        return refuse(cmd, RejectReason::VenueReject, "signing failed");
      }
      const std::size_t n = write_tx_info(tx, sig, {info_buf_.get(), kMaxTxInfoBytes});
      wire_.record(cmd.t0_cycles(), before, rdtscp(), rdtscp());
      if (n == 0 || !send_tx(kTxModifyOrder, {info_buf_.get(), n}, e))
        return reject_entry(e, RejectReason::TransportFull, "not sent");
      rate_.on_sent(1, now, true);
      ++stats_.replaces_sent;
      return;
    }
    case OrderCommandKind::Cancel: {
      const Shadow* s = shadows_.find(cmd.cl_ord_id);
      const InstrumentId inst = s != nullptr ? s->instrument : cmd.instrument;
      if (inst.value >= kMaxInstruments || !markets_[inst.value].known)
        return refuse(cmd, RejectReason::UnknownOrder, "cancel: no market for the instrument");
      CancelOrderTx tx;
      tx.market_index = static_cast<std::int16_t>(markets_[inst.value].market);
      if (s != nullptr) {
        tx.index = s->client_index;
      } else if (cmd.venue_order_id != nullptr && parse_int64(cmd.venue_order_id->view())) {
        tx.index = *parse_int64(cmd.venue_order_id->view());  // the venue's order index
      } else {
        tx.index = static_cast<std::int64_t>(cmd.cl_ord_id.value);
      }
      e.instrument = inst;
      tx.h = tx_header();
      e.nonce = tx.h.nonce;
      if (!signer_.sign(tx_hash(tx, cfg_.chain_id), sig))
        return refuse(cmd, RejectReason::VenueReject, "signing failed");
      const std::size_t n = write_tx_info(tx, sig, {info_buf_.get(), kMaxTxInfoBytes});
      if (n == 0 || !send_tx(kTxCancelOrder, {info_buf_.get(), n}, e))
        return reject_entry(e, RejectReason::TransportFull, "not sent");
      rate_.on_sent(1, now, false);
      ++stats_.cancels_sent;
      return;
    }
  }
}

// One transaction: a sendtx frame on the trade connection, else POST /api/v1/sendTx.
bool LighterVenue::send_tx(std::uint8_t tx_type, std::string_view tx_info, const Entry& e) {
  const std::uint64_t id = ++next_tx_id_;
  const std::int64_t now = now_ns();
  if (e.quota && quota_ > 0) --quota_;
  if (cfg_.ws_order_api && trade_conn_.is_live()) {
    const auto r = fmt::format_to_n(
        request_buf_.get(),
        kMaxRequestBytes,
        R"({{"type":"jsonapi/sendtx","data":{{"id":"{}","tx_type":{},"tx_info":{}}}}})",
        id,
        tx_type,
        tx_info);
    if (r.size <= kMaxRequestBytes &&
        trade_conn_.send_text(std::string_view(request_buf_.get(), r.size))) {
      Pending* p = new_pending(id);
      p->sent_ns = now;
      p->entry = e;
      if (n_corked_ < corked_.size()) {
        corked_[n_corked_] = e;
        corked_ids_[n_corked_++] = id;
      }
      return true;
    }
    ++stats_.order_send_failures;
  }
  if (rest_ == nullptr) return false;
  auto pending = std::make_shared<Pending>();
  pending->id = id;
  pending->sent_ns = now;
  pending->entry = e;
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "POST",
      "/api/v1/sendTx",
      kFormHeaders,
      send_tx_form(tx_type, tx_info),
      [this, alive, pending](const net::HttpResponse& resp) {
        if (alive.expired()) return;
        ++stats_.rest_requests;
        TxReply r;
        if (resp.error != net::NetError::None || !decode_send_tx(resp.body, r).empty()) {
          // Unknown whether it went through: reconcile.
          ++stats_.rest_errors;
          reconcile_.request();
          return;
        }
        handle_reply(*pending, r);
      },
      RestLane::Orders);
  if (queued && e.kind != OrderCommandKind::Cancel) sent_.sent_over_rest(e.cl_ord_id);
  return queued;
}

void LighterVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void LighterVenue::apply_fatal(std::string_view why) {
  if (fatal_) return;
  fatal_ = true;
  FASTMM_LOG_ERROR(
      "{}: fatal venue error ({}); order entry disabled", cfg_.name, why.substr(0, 200));
  trip_venue_kill(KillReason::VenueFatal);
}

// ---- control path -----------------------------------------------------------------------------

bool LighterVenue::get(const std::string& target,
                       bool authenticated,
                       std::function<void(bool, std::string_view)> done) {
  if (rest_ == nullptr) return false;
  std::string headers;
  if (authenticated) headers = fmt::format("Authorization: {}\r\n", auth());
  std::weak_ptr<int> alive = alive_;
  return rest_->request("GET",
                        target,
                        headers,
                        {},
                        [this, alive, done = std::move(done)](const net::HttpResponse& r) {
                          if (alive.expired()) return;
                          ++stats_.rest_requests;
                          if (r.error != net::NetError::None) {
                            ++stats_.rest_errors;
                            done(false, r.body);
                            return;
                          }
                          done(true, r.body);
                        });
}

void LighterVenue::refresh_nonce() {
  if (nonce_refresh_pending_ || !keys()) return;
  nonce_refresh_pending_ = true;
  const bool queued =
      get(fmt::format("/api/v1/nextNonce?account_index={}&api_key_index={}",
                      cfg_.account_index,
                      cfg_.api_key_index),
          false,
          [this](bool ok, std::string_view body) {
            nonce_refresh_pending_ = false;
            std::int64_t next = -1;
            if (!ok || !decode_next_nonce(body, next).empty()) return;
            // With skip_nonce any nonce above the last used one does; never go
            // back below one sent since.
            if (cfg_.skip_nonce ? next > nonce_ : next != nonce_) {
              FASTMM_LOG_INFO("{}: next nonce {} (was {})", cfg_.name, next, nonce_);
              nonce_ = next;
            }
          });
  if (!queued) nonce_refresh_pending_ = false;
}

void LighterVenue::request_open_orders() {
  reconcile_.request();
}

bool LighterVenue::fetch_snapshot(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || !keys()) return false;
  return get(fmt::format("/api/v1/accountActiveOrders?account_index={}", cfg_.account_index),
             true,
             [this, generation](bool ok, std::string_view body) {
               if (!reconcile_.current(generation)) return;
               std::vector<OrderRow> rows;
               std::string err = ok ? decode_active_orders(body, rows) : std::string("HTTP error");
               if (!err.empty()) {
                 FASTMM_LOG_WARN("{}: open orders failed ({})", cfg_.name, err);
                 reconcile_.fetched(generation, false);
                 return;
               }
               for (const OrderRow& o : rows) {
                 const InstrumentId inst = instrument_of_market(o.market);
                 if (!inst.valid()) continue;
                 ReconcileMsg& m = reconcile_.add_order(inst);
                 m.side = o.is_ask ? Side::Sell : Side::Buy;
                 m.state = o.filled.is_positive() ? OrderState::PartiallyFilled : OrderState::Live;
                 if (o.client_order_index > 0) {
                   m.cl_ord_id = current_id(o.client_order_index);
                   if (Shadow* s = shadows_.find(m.cl_ord_id)) {
                     s->order_index = o.order_index;
                     s->seen = true;
                   }
                 }
                 static_cast<void>(m.venue_order_id.assign(IdText(o.order_index).view()));
                 m.price = o.price;
                 m.orig_qty = o.initial;
                 m.cum_qty = o.filled;
               }
               if (!fetch_positions(generation)) reconcile_.fetched(generation, false);
             });
}

bool LighterVenue::fetch_positions(std::uint64_t generation) {
  return get(fmt::format("/api/v1/account?by=index&value={}", cfg_.account_index),
             false,
             [this, generation](bool ok, std::string_view body) {
               if (!reconcile_.current(generation)) return;
               AccountState st;
               std::string err = ok ? decode_account(body, st) : std::string("HTTP error");
               if (!err.empty()) {
                 FASTMM_LOG_WARN("{}: positions failed ({})", cfg_.name, err);
                 reconcile_.fetched(generation, false);
                 return;
               }
               for (InstrumentId id : subscribed_) {
                 Qty qty{};
                 Price avg{};
                 for (const PositionRow& p : st.positions) {
                   if (instrument_of_market(p.market) != id) continue;
                   qty = p.qty;
                   avg = p.avg_entry;
                 }
                 reconcile_.add_position(id, qty, avg);
                 FASTMM_LOG_INFO(
                     "{}: {} position {} @ {}", cfg_.name, symbols_by_id_[id.value], qty, avg);
               }
               reconcile_.fetched(generation, true);
             });
}

// The balance leg: the account's USDC collateral, what of it is available, and its value.
bool LighterVenue::fetch_balances(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || cfg_.account_index < 0) return false;
  const bool queued =
      get(fmt::format("/api/v1/account?by=index&value={}", cfg_.account_index),
          false,
          [this, generation](bool ok, std::string_view body) {
            if (!reconcile_.balances_current(generation)) return;
            AccountState st;
            std::string err = ok ? decode_account(body, st) : "HTTP error";
            if (!err.empty()) {
              FASTMM_LOG_WARN("{}: balances failed ({})", cfg_.name, err);
              reconcile_.balances_fetched(generation, false, 0);
              return;
            }
            BalanceFields b;
            b.free = st.available;
            b.total = st.collateral;
            b.locked = st.collateral > st.available ? st.collateral - st.available : Notional{};
            b.equity = st.equity.is_zero() ? st.collateral : st.equity;
            reconcile_.add_balance("USDC", b);
            reconcile_.balances_fetched(generation, true, wall_ms());
          });
  if (!queued) reconcile_.balances_fetched(generation, false, 0);
  return true;
}

void LighterVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const Shadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void LighterVenue::drop_shadow(ClientOrderId id) {
  forget_order(id);
}

// ---- cancel-all and the scheduled cancel ------------------------------------------------------

std::string LighterVenue::signed_cancel_all(std::uint8_t tif, std::int64_t time_ms) {
  if (!keys() || nonce_ < 0) return {};
  CancelAllTx tx;
  tx.h = tx_header();
  tx.time_in_force = tif;
  tx.time = time_ms;
  SigBytes sig{};
  if (!signer_.sign(tx_hash(tx, cfg_.chain_id), sig)) return {};
  std::string info(kMaxTxInfoBytes, '\0');
  const std::size_t n = write_tx_info(tx, sig, info);
  info.resize(n);
  return info;
}

void LighterVenue::cancel_all_async() {
  if (rest_ == nullptr) return;
  const std::string info = signed_cancel_all(kCancelAllNow, 0);
  if (info.empty()) return;
  std::weak_ptr<int> alive = alive_;
  static_cast<void>(rest_->request(
      "POST",
      "/api/v1/sendTx",
      kFormHeaders,
      send_tx_form(kTxCancelAll, info),
      [this, alive](const net::HttpResponse& r) {
        if (alive.expired()) return;
        TxReply tr;
        if (r.error != net::NetError::None || !decode_send_tx(r.body, tr).empty() || tr.refused)
          FASTMM_LOG_ERROR("{}: cancel-all failed: {}", cfg_.name, r.body.substr(0, 160));
      },
      RestLane::Orders));
}

bool LighterVenue::cancel_all() {
  if (cfg_.dry_run || !keys() || symbols_ == nullptr) return true;
  try {
    BlockingHttp http(cfg_.rest_url, blocking_options(cfg_));
    if (nonce_ < 0) {
      const HttpReply n =
          http.get(fmt::format("/api/v1/nextNonce?account_index={}&api_key_index={}",
                               cfg_.account_index,
                               cfg_.api_key_index));
      std::int64_t next = -1;
      if (n.status == 0 || !decode_next_nonce(n.body, next).empty()) {
        FASTMM_LOG_ERROR("{}: kill-switch cancel-all: no nonce (HTTP {})", cfg_.name, n.status);
        return false;
      }
      nonce_ = next;
    }
    const std::string info = signed_cancel_all(kCancelAllNow, 0);
    if (info.empty()) return false;
    const HttpReply c =
        http.request("POST", "/api/v1/sendTx", kFormHeaders, send_tx_form(kTxCancelAll, info));
    TxReply tr;
    const bool ok = c.status != 0 && decode_send_tx(c.body, tr).empty() && !tr.refused;
    if (!ok) {
      FASTMM_LOG_ERROR("{}: kill-switch cancel-all failed: HTTP {} {}",
                       cfg_.name,
                       c.status,
                       c.body.substr(0, 160));
      return false;
    }
    FASTMM_LOG_INFO("{}: kill-switch cancel-all accepted", cfg_.name);
    return true;
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR(
        "{}: kill-switch cancel-all failed: {}", cfg_.name, std::string_view(e.what()));
    return false;
  }
}

// A scheduled CancelAll at `time_ms`, on the trade connection (the order of the nonces is that
// of the frames), or over REST without it.
void LighterVenue::send_schedule_cancel(std::int64_t time_ms) {
  if (!keys() || nonce_ < 0) return;
  const std::string info = signed_cancel_all(kCancelAllScheduled, time_ms);
  if (info.empty()) return;
  const std::uint32_t round = dms_.begin_round(now_ns(), 1);
  const std::uint64_t id = ++next_tx_id_;
  if (cfg_.ws_order_api && trade_conn_.is_live()) {
    const std::string frame =
        fmt::format(R"({{"type":"jsonapi/sendtx","data":{{"id":"{}","tx_type":{},"tx_info":{}}}}})",
                    id,
                    static_cast<int>(kTxCancelAll),
                    info);
    if (trade_conn_.send_text(frame)) {
      Pending* p = new_pending(id);
      p->sent_ns = now_ns();
      p->control = true;
      p->round = round;
      dms_.went_out();
      return;
    }
  }
  if (rest_ == nullptr) return;
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "POST",
      "/api/v1/sendTx",
      kFormHeaders,
      send_tx_form(kTxCancelAll, info),
      [this, alive, round](const net::HttpResponse& r) {
        if (alive.expired() || r.error == net::NetError::Canceled) return;
        TxReply tr;
        if (r.error != net::NetError::None || !decode_send_tx(r.body, tr).empty()) return;
        Pending p;
        p.control = true;
        p.round = round;
        handle_control_reply(p, tr);
      },
      RestLane::Orders);
  if (queued) dms_.went_out();
}

// ---- housekeeping -----------------------------------------------------------------------------

void LighterVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  // Signatures on the order path take a nonce prepared here (k * G is most of a signature's
  // cost); a few a tick, so the reactor never stalls for long. An empty pool signs inline.
  if (keys() && signer_.prepared() < kPreparedNonces)
    signer_.prepare(std::min(kPreparedNonces, signer_.prepared() + kRefillPerTick));
  if (now - last_ping_ns_ >= static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs) {
    last_ping_ns_ = now;
    if (md_conn_.is_live()) static_cast<void>(md_conn_.send_text(kPingFrame));
    if (private_conn_.is_live()) static_cast<void>(private_conn_.send_text(kPingFrame));
    if (trade_conn_.is_live()) static_cast<void>(trade_conn_.send_text(kPingFrame));
  }
  // The account channel's subscription took a token that lives auth_ttl_s: a new connection,
  // with a new token, before it runs out (the reconnect reconciles).
  if (keys() && private_conn_.opened() && cfg_.auth_ttl_s > 0 &&
      now - private_opened_ns_ >= cfg_.auth_ttl_s * 900'000'000) {
    FASTMM_LOG_INFO("{}: renewing the account channel's auth token", cfg_.name);
    private_opened_ns_ = now;
    auth_.clear();
    private_conn_.close();
    private_conn_.open(*reactor_, ws_config(true), private_handler_);
    private_conn_.connect();
  }
  bool stale = false;
  for (Pending& p : pending_) {
    if (p.id != 0 && now - p.sent_ns >= kPendingTimeoutNs) {
      FASTMM_LOG_WARN("{}: transaction {} unanswered after {} s",
                      cfg_.name,
                      p.id,
                      kPendingTimeoutNs / 1'000'000'000);
      p.id = 0;
      stale = true;
    }
  }
  // Held cancels and expiries whose fills did not come; accepted orders the stream never named.
  std::vector<ClientOrderId> flush;
  std::vector<ClientOrderId> unseen;
  shadows_.for_each([&](ClientOrderId id, const Shadow& s) {
    if (s.terminal != Terminal::None && now - s.terminal_ns >= cfg_.terminal_wait_ms * kNsPerMs) {
      flush.push_back(id);
    } else if (s.terminal == Terminal::None && !s.seen && s.accepted_ns > 0 && !s.late_logged &&
               now - s.accepted_ns >= kUnseenNs) {
      unseen.push_back(id);
    }
  });
  for (ClientOrderId id : unseen) {
    if (Shadow* s = shadows_.find(id)) s->late_logged = true;
    FASTMM_LOG_WARN("{}: order {} accepted but not on the account stream after {} s",
                    cfg_.name,
                    id.value,
                    kUnseenNs / 1'000'000'000);
    stale = true;
  }
  for (ClientOrderId id : flush) {
    if (Shadow* s = shadows_.find(id)) {
      FASTMM_LOG_WARN("{}: order {} ended with {} filled but only {} came as fills",
                      cfg_.name,
                      id.value,
                      s->terminal_cum,
                      s->cum_qty);
      flush_terminal(id, *s);
      stale = true;
    }
  }
  if (stale) reconcile_.request();
  reconcile_.on_timer(now);
  if (keys() && !dms_refused_) {
    switch (dms_.poll(now)) {
      case CountdownDriver::Step::Lapsed:
        FASTMM_LOG_ERROR(
            "{}: the scheduled cancel was not refreshed within {} ms; the venue has cancelled the "
            "account's orders: killing this venue",
            cfg_.name,
            dms_.window_ms());
        trip_venue_kill(KillReason::DeadMansSwitchLost);
        break;
      case CountdownDriver::Step::Refresh:
        // On the trade connection once it is up, so its nonce is in order with the orders'.
        if (!cfg_.ws_order_api || trade_conn_.is_live())
          send_schedule_cancel(wall_ms() + dms_.window_ms());
        break;
      case CountdownDriver::Step::None:
        break;
    }
  }
  if (cfg_.latency_log_s > 0 && now - last_latency_log_ns_ >= cfg_.latency_log_s * 1'000'000'000)
    log_latency(now);
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

void LighterVenue::log_latency(std::int64_t now) {
  last_latency_log_ns_ = now;
  if (reply_rtt_ms_.empty() && open_lag_ms_.empty()) return;
  const std::size_t n_rtt = reply_rtt_ms_.size();
  const std::size_t n_lag = open_lag_ms_.size();
  const std::int64_t rtt50 = percentile(reply_rtt_ms_, 50);
  const std::int64_t rtt90 = percentile(reply_rtt_ms_, 90);
  const std::int64_t rtt_max =
      reply_rtt_ms_.empty() ? 0 : *std::max_element(reply_rtt_ms_.begin(), reply_rtt_ms_.end());
  const std::int64_t lag50 = percentile(open_lag_ms_, 50);
  const std::int64_t lag90 = percentile(open_lag_ms_, 90);
  const std::int64_t lag_max =
      open_lag_ms_.empty() ? 0 : *std::max_element(open_lag_ms_.begin(), open_lag_ms_.end());
  FASTMM_LOG_INFO(
      "{}: latency: tx reply {} sample(s) p50 {} p90 {} max {} ms; order open (venue time - "
      "send) {} sample(s) p50 {} p90 {} max {} ms; volume quota {}",
      cfg_.name,
      n_rtt,
      rtt50,
      rtt90,
      rtt_max,
      n_lag,
      lag50,
      lag90,
      lag_max,
      quota_);
  reply_rtt_ms_.clear();
  open_lag_ms_.clear();
}

void LighterVenue::publish_status() noexcept {
  stats_.shadows = shadows_.size();
  stats_.shadows_refused = shadow_overflow_.count();
  stats_.shadows_swept = reconcile_.shadows_swept();
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  wire_.summarize(tsc_calibration(),
                  now_ns(),
                  stats_.wire_tick_to_trade,
                  stats_.order_encode,
                  stats_.order_send,
                  stats_.wire_1m,
                  stats_.wire_1h);
  published_.store(stats_);
  OrderBudget b = budget_of(rate_, now_ns(), orders_taken_);
  if (quota_ >= 0) {
    // The volume quota as a day-long window: what is left of it above the reserve.
    b.orders_1d.window_ms = 86'400'000;
    b.orders_1d.limit = quota_;
    b.orders_1d.used = 0;
    b.orders_1d.cap = std::max<std::int64_t>(0, quota_ - cfg_.quota_reserve);
  }
  budget_pub_.store(b);
}

VenueStatus LighterVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

LighterVenueConfig make_lighter_config(const VenueSection& v, bool dry_run) {
  LighterVenueConfig c;
  c.name = v.name;
  c.chain_id = v.testnet ? kTestnetChainId : kMainnetChainId;
  c.ws_url = !v.ws_url.empty() ? v.ws_url
             : v.testnet       ? std::string("wss://testnet.zklighter.elliot.ai/stream")
                               : std::string("wss://mainnet.zklighter.elliot.ai/stream");
  c.rest_url = !v.rest_url.empty() ? v.rest_url
               : v.testnet         ? std::string("https://testnet.zklighter.elliot.ai")
                                   : std::string("https://mainnet.zklighter.elliot.ai");
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.supports_replace = v.supports_replace;
  c.dry_run = dry_run;
  c.private_key = v.api_secret;
  const VenueExtras x(v.extra);
  if (!v.api_key.empty()) {
    const auto a = parse_int64(v.api_key);
    if (!a || *a < 0)
      throw std::invalid_argument(
          fmt::format("venues.{}.api_key: the account index, a non-negative integer", v.name));
    c.account_index = *a;
  }
  const std::int64_t key_index = x.integer("api_key_index", -1);
  if (key_index != -1 && (key_index < 0 || key_index > 254))
    throw std::invalid_argument(
        fmt::format("venues.{}.api_key_index: 0 to 254 (4 and up for API keys)", v.name));
  c.api_key_index = key_index;
  if (!c.private_key.empty() && (c.account_index < 0 || c.api_key_index < 0))
    throw std::invalid_argument(fmt::format(
        "venues.{}: a private key needs api_key (the account index) and api_key_index", v.name));
  if (x.get("order_api") == "rest") c.ws_order_api = false;
  c.channels.order_book = x.flag("order_book", true);
  c.channels.ticker = x.flag("ticker", true);
  c.channels.trades = x.flag("trades", true);
  c.skip_nonce = x.flag("skip_nonce", true);
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("dead_ms", c.dead_ms)));
  c.ping_interval_ms = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(x.integer("ping_interval_ms", c.ping_interval_ms), 1000, 110'000));
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("orders_per_second", c.orders_per_second)));
  c.tx_per_minute = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("tx_per_minute", c.tx_per_minute)));
  c.quota_reserve = std::max<std::int64_t>(0, x.integer("quota_reserve", c.quota_reserve));
  c.order_expiry_s = std::clamp<std::int64_t>(
      x.integer("order_expiry_s", c.order_expiry_s), 300, 30LL * 24 * 3600);
  c.auth_ttl_s =
      std::clamp<std::int64_t>(x.integer("auth_ttl_s", c.auth_ttl_s), 600, 8 * 3600 - 60);
  c.terminal_wait_ms =
      std::clamp<std::int64_t>(x.integer("terminal_wait_ms", c.terminal_wait_ms), 0, 60'000);
  c.emit_ack_from_response = x.flag("emit_ack_from_response", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  const std::int64_t dms = std::max<std::int64_t>(0, x.integer("dead_mans_switch_s", 0));
  if (dms != 0 && (dms < 310 || dms > kMaxCancelAllWindowMs / 1000))
    throw std::invalid_argument(fmt::format(
        "venues.{}.dead_mans_switch_s: 310 to 1296000 (the venue wants the cancel time at least "
        "5 minutes and at most 15 days ahead)",
        v.name));
  c.dead_mans_switch_s = dms;
  c.latency_log_s = std::max<std::int64_t>(0, x.integer("latency_log_s", c.latency_log_s));
  check_liveness(v.name, c.stale_ms, c.dead_ms);
  return c;
}

}  // namespace fastmm::venues::lighter
