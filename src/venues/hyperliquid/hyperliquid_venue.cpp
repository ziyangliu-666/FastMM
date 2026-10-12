#include "fastmm/venues/hyperliquid/hyperliquid_venue.hpp"

#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/hyperliquid/hl_error_map.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fastmm::venues::hyperliquid {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kHousekeepingNs = 1'000'000'000;
constexpr std::int64_t kPendingTimeoutNs = 30'000'000'000;
constexpr std::string_view kJsonHeaders = "Content-Type: application/json\r\n";
// userFillsByTime: at most 2000 fills a reply, the 10000 most recent available.
constexpr std::size_t kFillsPage = 2000;
constexpr std::int64_t kFillsHistoryMs = 7LL * 24 * 3600 * 1000;
// The venue's minimum notional for an order, USDC.
constexpr std::int64_t kMinNotional = 10;
// The private subscriptions: orderUpdates, userFills, userFundings.
constexpr std::uint32_t kPrivateChannels = 3;

std::string_view dex_of(std::string_view coin) noexcept {
  const std::size_t c = coin.find(':');
  return c == std::string_view::npos ? std::string_view{} : coin.substr(0, c);
}

std::string lower(std::string s) {
  for (char& c : s) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  return s;
}

std::string info_body(std::string_view type, std::string_view user, std::string_view dex) {
  std::string b = fmt::format(R"({{"type":"{}")", type);
  if (!user.empty()) b += fmt::format(R"(,"user":"{}")", user);
  if (!dex.empty()) b += fmt::format(R"(,"dex":"{}")", dex);
  b += '}';
  return b;
}

std::int64_t percentile(std::vector<std::int64_t>& v, int pct) {
  if (v.empty()) return 0;
  const std::size_t k = std::min(v.size() - 1, v.size() * static_cast<std::size_t>(pct) / 100);
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

HyperliquidVenue::HyperliquidVenue(VenueId id, HyperliquidVenueConfig cfg)
    : id_(id),
      cfg_(std::move(cfg)),
      signer_(cfg_.private_key),
      action_signer_(signer_, cfg_.mainnet, lower(cfg_.vault_address)),
      dms_(cfg_.dead_mans_switch_s * 1000) {
  cfg_.private_key.assign(cfg_.private_key.size(), '\0');  // the signer holds the key now
  cfg_.private_key.clear();
  cfg_.account_address = lower(cfg_.account_address);
  if (cfg_.account_address.empty() && signer_.usable()) cfg_.account_address = signer_.address();
  if (cfg_.posts_per_minute > 0) rate_.add_weight_bucket(cfg_.posts_per_minute, 60'000'000'000);
  if (cfg_.orders_per_second > 0) rate_.add_order_bucket(cfg_.orders_per_second, 1'000'000'000);
  action_ = std::make_unique<ActionBuilder>();
  request_buf_ = std::make_unique<char[]>(kMaxRequestBytes);
  std::memset(scratch_, 0, sizeof scratch_);
  post_rtt_ms_.reserve(4096);
  open_lag_ms_.reserve(4096);
  ReplayLimits limits;
  limits.window_ms = 0;
  limits.history_ms = kFillsHistoryMs;
  limits.page_rows = kFillsPage;
  limits.newest_first = false;
  fill_replay_.setup(cfg_.name,
                     "fill(s)",
                     limits,
                     {[this] {
                        return !cfg_.dry_run && connected_ && signer_.usable() &&
                               rest_ != nullptr && !subscribed_.empty();
                      },
                      [] { return wall_ms(); },
                      [this](const ReplayQuery& q) { return query_fills(q); },
                      [this](bool complete) { reconcile_.replay_done(complete); },
                      {},
                      {}},
                     [this](std::size_t, const FillRow& f) { return emit_fill_row(f); });
  fill_replay_.set_streams(1);
}

HyperliquidVenue::~HyperliquidVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps HyperliquidVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = cfg_.ws_order_api;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

void HyperliquidVenue::set_asset(InstrumentId id,
                                 std::string coin,
                                 std::uint32_t asset,
                                 int sz_decimals) {
  if (id.value >= kMaxInstruments) return;
  assets_[id.value] = AssetSpec{asset, sz_decimals, true};
  coins_[id.value] = std::move(coin);
}

net::ConnectionConfig HyperliquidVenue::ws_config(bool manual_subscribe) const {
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

InstrumentId HyperliquidVenue::instrument_of(std::string_view coin) const noexcept {
  if (symbols_ == nullptr) return InstrumentId::invalid();
  const InstrumentId id = symbols_->find(id_, coin);
  if (id.valid() || cfg_.dex.empty()) return id;
  if (dex_of(coin) == cfg_.dex) return symbols_->find(id_, coin.substr(cfg_.dex.size() + 1));
  return id;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

Result<void, std::string> HyperliquidVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  for (const Instrument& inst : instruments) {
    if (inst.venue == id_) mine.push_back(&instruments.get(inst.id));
  }
  if (mine.empty()) return {};
  // The coin each instrument names, and the dexes they belong to.
  std::vector<std::string> wanted;
  std::vector<std::string> dexes;
  for (Instrument* inst : mine) {
    std::string coin(inst->symbol.view());
    if (dex_of(coin).empty() && !cfg_.dex.empty()) coin = fmt::format("{}:{}", cfg_.dex, coin);
    const std::string dex(dex_of(coin));
    if (std::find(dexes.begin(), dexes.end(), dex) == dexes.end()) dexes.push_back(dex);
    wanted.push_back(std::move(coin));
  }
  BlockingHttpOptions opts = blocking_options(cfg_);
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    auto post = [&](const std::string& body) {
      return http.request("POST", "/info", kJsonHeaders, body);
    };
    std::vector<std::string> dex_names;  // by perp dex index
    if (std::any_of(dexes.begin(), dexes.end(), [](const std::string& d) { return !d.empty(); })) {
      const HttpReply r = post(info_body("perpDexs", {}, {}));
      if (!r.ok())
        return fail(fmt::format("{}: perpDexs failed: HTTP {} {}", cfg_.name, r.status, r.error));
      if (std::string err = decode_perp_dexs(r.body, dex_names); !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
    }
    for (const std::string& dex : dexes) {
      std::uint32_t offset = 0;
      if (!dex.empty()) {
        const auto it = std::find(dex_names.begin(), dex_names.end(), dex);
        if (it == dex_names.end() || it == dex_names.begin())
          return fail(fmt::format("{}: perp dex '{}' is not listed (perpDexs)", cfg_.name, dex));
        offset = 100'000 + static_cast<std::uint32_t>(it - dex_names.begin()) * 10'000;
      }
      const HttpReply r = post(info_body("metaAndAssetCtxs", {}, dex));
      if (!r.ok()) {
        if (cfg_.allow_offline_reference_data) {
          FASTMM_LOG_WARN(
              "{}: metaAndAssetCtxs for dex '{}' failed (HTTP {}); keeping the "
              "configured tick and lot",
              cfg_.name,
              dex,
              r.status);
          continue;
        }
        return fail(fmt::format("{}: metaAndAssetCtxs for dex '{}' failed: HTTP {} {}",
                                cfg_.name,
                                dex,
                                r.status,
                                r.error));
      }
      std::vector<AssetMeta> universe;
      if (std::string err = decode_meta(r.body, universe); !err.empty())
        return fail(fmt::format("{}: {}", cfg_.name, err));
      for (std::size_t k = 0; k < mine.size(); ++k) {
        if (dex_of(wanted[k]) != dex) continue;
        Instrument* inst = mine[k];
        std::size_t idx = universe.size();
        for (std::size_t u = 0; u < universe.size(); ++u) {
          if (iequals_symbol(universe[u].name, wanted[k])) idx = u;
        }
        if (idx == universe.size())
          return fail(fmt::format("{}: {} is not listed on dex '{}'", cfg_.name, wanted[k], dex));
        const AssetMeta& m = universe[idx];
        set_asset(inst->id, m.name, offset + static_cast<std::uint32_t>(idx), m.sz_decimals);
        const int dec = std::clamp(m.sz_decimals, 0, 8);
        const Qty lot = Qty::from_raw(Qty::from_int(1).raw / [dec] {
          std::int64_t p = 1;
          for (int i = 0; i < dec; ++i) p *= 10;
          return p;
        }());
        const Price tick = m.mark.is_positive()
                               ? tick_at(m.mark, m.sz_decimals)
                               : tick_at(Price::from_int(1), m.sz_decimals);  // the decimal cap
        if (inst->tick != tick || inst->lot != lot)
          FASTMM_LOG_INFO("{}: {} tick/lot from the venue: {} / {} (mark {}, szDecimals {})",
                          cfg_.name,
                          m.name,
                          tick,
                          lot,
                          m.mark,
                          m.sz_decimals);
        inst->tick = tick;
        inst->lot = lot;
        inst->min_qty = lot;
        inst->min_notional = Notional::from_int(kMinNotional);
        inst->contract_multiplier = Qty::from_int(1);
        inst->expiry_ns = 0;
        inst->asset_class = AssetClass::Perpetual;
        inst->flags = static_cast<std::uint8_t>((inst->flags | Instrument::kReduceOnlySupported) &
                                                ~Instrument::kInverse);
        if (!inst->quote.assign("USDC"))
          return fail(fmt::format("{}: {} quote too long", cfg_.name, m.name));
        std::string_view base = m.name;
        if (const std::size_t c = base.find(':'); c != std::string_view::npos)
          base.remove_prefix(c + 1);
        static_cast<void>(inst->base.assign(base.substr(0, inst->base.kCapacity)));
        if (m.delisted) {
          FASTMM_LOG_ERROR("{}: {} is delisted: disabled", cfg_.name, m.name);
          inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kEnabled);
        }
        if (m.only_isolated)
          FASTMM_LOG_INFO("{}: {} trades isolated margin only (max leverage {})",
                          cfg_.name,
                          m.name,
                          m.max_leverage);
      }
    }
    if (!cfg_.dry_run && signer_.usable()) {
      // The signing key must be the account's or an agent it approved.
      if (signer_.address() != cfg_.account_address) {
        const HttpReply r = post(info_body("extraAgents", cfg_.account_address, {}));
        if (r.ok() && r.body.find(signer_.address()) == std::string::npos &&
            lower(r.body).find(signer_.address()) == std::string::npos) {
          refused_account_settings_ = true;
          return fail(fmt::format(
              "{}: the signing key {} is not an agent of account {} (extraAgents); approve it "
              "first",
              cfg_.name,
              signer_.address(),
              cfg_.account_address));
        }
      }
      const HttpReply rl = post(info_body("userRateLimit", cfg_.account_address, {}));
      RateLimitState st;
      if (rl.ok() && decode_rate_limit(rl.body, st).empty()) {
        set_address_budget(st.used, st.cap + st.surplus);
        FASTMM_LOG_INFO("{}: account {} (signer {}): request budget {} of {} used, volume {:.2f}",
                        cfg_.name,
                        cfg_.account_address,
                        signer_.address(),
                        st.used,
                        st.cap,
                        st.cum_volume);
      } else {
        FASTMM_LOG_WARN(
            "{}: userRateLimit failed (HTTP {}): the address budget is unknown until "
            "the first refresh",
            cfg_.name,
            rl.status);
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
  FASTMM_LOG_INFO(
      "{}: reference data loaded for {} coins ({} dex(es))", cfg_.name, mine.size(), dexes.size());
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void HyperliquidVenue::attach(const SymbolTable& symbols,
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
      std::make_unique<HlMdFeed>(symbols,
                                 id_,
                                 md_sink,
                                 InstrumentCallback{&HyperliquidVenue::resubscribe_requester, this},
                                 cfg_.channels,
                                 cfg_.dex);
  md_feed_->set_log_name(cfg_.name);
  private_parser_ = std::make_unique<HlPrivateParser>(symbols, id_, cfg_.dex);
  decoder_ = std::make_unique<HlResponseDecoder>();
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments);
}

void HyperliquidVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != id_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    if (id.value >= kMaxInstruments) continue;
    if (coins_[id.value].empty()) {  // no reference data (offline): the configured symbol
      const Instrument& inst = instruments_->get(id);
      std::string coin(inst.symbol.view());
      if (dex_of(coin).empty() && !cfg_.dex.empty()) coin = fmt::format("{}:{}", cfg_.dex, coin);
      coins_[id.value] = std::move(coin);
    }
    subscribed_.push_back(id);
    const std::string dex(dex_of(coins_[id.value]));
    if (std::find(dexes_.begin(), dexes_.end(), dex) == dexes_.end()) dexes_.push_back(dex);
    if (md_feed_) md_feed_->add_instrument(id, coins_[id.value]);
  }
  stats_.books_total = static_cast<std::uint32_t>(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (connected_ && md_conn_.opened()) {
    md_conn_.close();
    md_conn_.open(*reactor_, ws_config(false), md_handler_);
    md_conn_.connect();
  }
}

void HyperliquidVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("HyperliquidVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  const bool keys = !cfg_.dry_run && signer_.usable();
  reconcile_.open(keys);
  fill_replay_.start_at(wall_ms());
  fill_replay_.open(keys);
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    if (keys) {
      raw_private_.open(cfg_.record_raw_dir, cfg_.name, "private");
      raw_trade_.open(cfg_.record_raw_dir, cfg_.name, "trade");
    }
  }
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
  md_conn_.open(*reactor_, ws_config(false), md_handler_);
  md_conn_.connect();
  if (keys) {
    private_conn_.open(*reactor_, ws_config(true), private_handler_);
    private_conn_.connect();
    if (cfg_.ws_order_api) {
      trade_conn_.open(*reactor_, ws_config(false), trade_handler_);
      trade_conn_.connect();
    }
  }
  last_ping_ns_ = now_ns();
  last_rate_refresh_ns_ = now_ns();
  last_latency_log_ns_ = now_ns();
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting ({}, account {}, signer {}, dry_run={}, ws_orders={})",
                  cfg_.name,
                  cfg_.mainnet ? "mainnet" : "testnet",
                  cfg_.account_address.empty() ? "?" : cfg_.account_address,
                  signer_.usable() ? signer_.address() : std::string("none"),
                  cfg_.dry_run,
                  cfg_.ws_order_api);
}

void HyperliquidVenue::disconnect() {
  if (!connected_) return;
  connected_ = false;
  reconcile_.close();
  fill_replay_.close();
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  if (dms_.needs_stop() && signer_.usable() && !dms_refused_) {
    // Remove the scheduled cancel: a clean stop has cancelled what it wanted to.
    ActionBuilder a;
    if (a.schedule_cancel(0)) {
      std::string body(kMaxRequestBytes, '\0');
      const std::size_t n =
          action_signer_.write_payload(a, action_signer_.next_nonce(wall_ms()), body);
      if (n > 0) {
        body.resize(n);
        BlockingControl control(cfg_);
        const HttpReply reply =
            control.send("removing the scheduled cancel", [&](BlockingRequest& q) {
              q.method = "POST";
              q.target = "/exchange";
              q.headers = std::string(kJsonHeaders);
              q.body = body;
              return true;
            });
        control.report(
            "removing the scheduled cancel", reply, reply.ok(), reply.body.substr(0, 120));
      }
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

void HyperliquidVenue::on_md_state(net::ConnState s) {
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
        if (HlBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
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

void HyperliquidVenue::on_md_open() {
  for (const std::string& p : md_feed_->subscription_payloads()) {
    if (!md_conn_.send_text(p)) FASTMM_LOG_ERROR("{}: could not send a subscription", cfg_.name);
  }
  md_feed_->on_connected();
}

void HyperliquidVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (HlBookSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

void HyperliquidVenue::on_md_text(std::string_view t, std::int64_t ts) {
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

void HyperliquidVenue::request_resubscribe(InstrumentId id) {
  if (!md_conn_.is_live()) return;
  for (const std::string& p : md_feed_->resubscribe_payloads(id))
    static_cast<void>(md_conn_.send_text(p));
  FASTMM_LOG_INFO("{}: resubscribing {} for a fresh book", cfg_.name, coin_of(id));
}

// ---- private stream -------------------------------------------------------------------------

void HyperliquidVenue::on_private_state(net::ConnState s) {
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

void HyperliquidVenue::on_private_open() {
  private_subscribed_ = 0;
  if (cfg_.account_address.empty()) {
    FASTMM_LOG_ERROR("{}: no account address for the private channels", cfg_.name);
    apply_action(VenueAction::Fatal, "no account address");
    return;
  }
  for (const std::string_view type : {"orderUpdates", "userFills", "userFundings"}) {
    char buf[256];
    const std::size_t n = write_subscription(true, type, "user", cfg_.account_address, buf);
    if (n == 0 || !private_conn_.send_text(std::string_view(buf, n)))
      FASTMM_LOG_ERROR("{}: could not subscribe {}", cfg_.name, type);
  }
}

void HyperliquidVenue::forget_order(ClientOrderId id) noexcept {
  shadows_.erase(id);
}

void HyperliquidVenue::complete_fill(OrderFillMsg& f) noexcept {
  Shadow* s = shadows_.find(f.cl_ord_id);
  if (s == nullptr) {
    f.cum_qty = f.qty;
    f.leaves_qty = Qty{};
    return;
  }
  s->cum_qty = s->cum_qty + f.qty;
  f.cum_qty = s->cum_qty;
  f.leaves_qty = s->orig_qty > s->cum_qty ? s->orig_qty - s->cum_qty : Qty{};
  if (f.leaves_qty.is_zero()) forget_order(f.cl_ord_id);
}

void HyperliquidVenue::on_private_text(std::string_view t, std::int64_t ts) {
  if (raw_private_.enabled()) raw_private_.record(ts, t);
  const Cycles t0 = rdtscp();
  const PrivateDecodeResult r = private_parser_->decode(t, wall_now(), t0, scratch_);
  if (r.count > 0 && (r.status == ParseStatus::Ok || r.status == ParseStatus::Overflow)) {
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
      off += h->len;
      h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
      switch (h->type) {
        case EventType::OrderAck: {
          auto* m = reinterpret_cast<OrderAckMsg*>(h);
          Shadow* s = shadows_.find(m->cl_ord_id);
          if (s != nullptr) {
            if (s->retired) continue;
            if (const auto oid = parse_int64(m->venue_order_id.view()))
              s->oid = static_cast<std::uint64_t>(*oid);
            if (s->sent_wall_ns > 0 && m->hdr.exch_ts.ns > 0 && open_lag_ms_.size() < 100'000)
              open_lag_ms_.push_back((m->hdr.exch_ts.ns - s->sent_wall_ns) / kNsPerMs);
            s->answered = true;
            if (s->replaces.valid()) {
              if (Shadow* orig = shadows_.find(s->replaces)) orig->retired = true;
            }
            if (s->acked) continue;  // the reply acknowledged it already
            s->acked = true;
          }
          break;
        }
        case EventType::OrderCancelAck:
        case EventType::OrderExpired: {
          const ClientOrderId cl = h->type == EventType::OrderCancelAck
                                       ? reinterpret_cast<OrderCancelAckMsg*>(h)->cl_ord_id
                                       : reinterpret_cast<OrderExpiredMsg*>(h)->cl_ord_id;
          const Shadow* s = shadows_.find(cl);
          const bool retired = s != nullptr && s->retired;
          forget_order(cl);
          if (retired) continue;  // the order a modify replaced
          break;
        }
        case EventType::OrderReject: {
          auto* m = reinterpret_cast<OrderRejectMsg*>(h);
          Shadow* s = shadows_.find(m->cl_ord_id);
          if (s == nullptr || s->answered) continue;  // the reply said it already
          s->answered = true;
          if (s->replaces.valid()) {
            shadows_.erase(m->cl_ord_id);  // a failed modify: the original keeps working
          } else {
            forget_order(m->cl_ord_id);
          }
          break;
        }
        case EventType::OrderFill: {
          auto* m = reinterpret_cast<OrderFillMsg*>(h);
          complete_fill(*m);
          break;
        }
        default:
          break;
      }
      sent_.answered(*h);
      static_cast<void>(order_sink_->push(*h));
      ++stats_.order_events;
    }
    if (r.status == ParseStatus::Overflow) {
      FASTMM_LOG_WARN("{}: a private frame held more events than fit; reconciling", cfg_.name);
      reconcile_.request();
    }
    return;
  }
  if (r.control == ControlOp::Subscribe) {
    if (++private_subscribed_ >= kPrivateChannels) private_conn_.subscribe_done();
    return;
  }
  if (r.status == ParseStatus::Error) {
    FASTMM_LOG_ERROR("{}: private stream error: {}", cfg_.name, r.error);
    return;
  }
  if (r.status == ParseStatus::Malformed) FASTMM_LOG_WARN("{}: malformed private frame", cfg_.name);
}

// ---- trade channel --------------------------------------------------------------------------

void HyperliquidVenue::on_trade_state(net::ConnState s) {
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
    // The posts in flight on it will not be answered there: the reconciliation decides them.
    std::size_t lost = 0;
    for (Pending& p : pending_) {
      if (p.id != 0) {
        p.id = 0;
        ++lost;
      }
    }
    emit_connection_state(*order_sink_, id_, 1, ConnState::Disconnected);
    FASTMM_LOG_WARN("{}: trade channel lost ({} post(s) unanswered)", cfg_.name, lost);
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

HyperliquidVenue::Pending* HyperliquidVenue::new_pending(std::uint64_t id) {
  Pending& p = pending_[id % kPendingSlots];
  if (p.id != 0) {
    FASTMM_LOG_WARN("{}: post {} still unanswered when its slot was reused", cfg_.name, p.id);
    reconcile_.request();
  }
  p = Pending{};
  p.id = id;
  return &p;
}

HyperliquidVenue::Pending* HyperliquidVenue::find_pending(std::uint64_t id) {
  Pending& p = pending_[id % kPendingSlots];
  return p.id == id && id != 0 ? &p : nullptr;
}

void HyperliquidVenue::on_trade_text(std::string_view t, std::int64_t ts) {
  if (raw_trade_.enabled()) raw_trade_.record(ts, t);
  PostReply r;
  const ParseStatus st = decoder_->decode_frame(t, r);
  if (st == ParseStatus::Ignored) return;  // pong
  if (st != ParseStatus::Ok) {
    FASTMM_LOG_WARN("{}: unreadable trade frame: {}", cfg_.name, t.substr(0, 200));
    return;
  }
  if (r.kind == PostReply::Kind::Error && r.id == 0) {
    // A request the venue could not parse: nothing names it, so its orders are decided by a
    // reconciliation.
    FASTMM_LOG_ERROR("{}: the venue refused a request: {}", cfg_.name, r.error.substr(0, 200));
    reconcile_.request();
    return;
  }
  Pending* p = find_pending(r.id);
  if (p == nullptr) return;  // answered over REST, timed out, or not ours
  const Pending copy = *p;
  p->id = 0;
  handle_reply(copy, r);
}

void HyperliquidVenue::handle_reply(const Pending& p, const PostReply& r) {
  const std::int64_t rtt_ms = (now_ns() - p.sent_ns) / kNsPerMs;
  if (post_rtt_ms_.size() < 100'000) post_rtt_ms_.push_back(rtt_ms);
  const bool whole_failed = r.kind != PostReply::Kind::Action || !r.ok;
  const ErrorMapping whole = whole_failed ? map_error_text(r.error) : ErrorMapping{};
  if (whole_failed)
    FASTMM_LOG_WARN(
        "{}: action of {} order(s) refused: {}", cfg_.name, p.count, r.error.substr(0, 160));
  bool unknown = false;
  for (std::uint32_t i = 0; i < p.count; ++i) {
    const Entry& e = p.entries[i];
    OrderStatus st;
    if (whole_failed) {
      st.kind = OrderStatus::Kind::Error;
      st.error = r.error;
    } else if (i < r.count) {
      st = r.statuses[i];
    } else {
      unknown = true;
      continue;
    }
    const ErrorMapping m = st.kind == OrderStatus::Kind::Error ? map_error_text(st.error) : whole;
    const bool success = st.kind == OrderStatus::Kind::Resting ||
                         st.kind == OrderStatus::Kind::Filled ||
                         st.kind == OrderStatus::Kind::Success;
    if (e.kind == OrderCommandKind::Cancel) {
      if (success) {
        if (!private_conn_.is_live()) {
          const Shadow* s = shadows_.find(e.cl_ord_id);
          emit_cancel_ack(*order_sink_,
                          id_,
                          e.instrument,
                          e.cl_ord_id,
                          s != nullptr ? IdText(static_cast<std::int64_t>(s->oid)).view() : "",
                          s != nullptr ? s->cum_qty : Qty{});
          forget_order(e.cl_ord_id);
          ++stats_.order_events;
        }
      } else {
        emit_cancel_reject(*order_sink_,
                           id_,
                           e.instrument,
                           e.cl_ord_id,
                           m.reason == RejectReason::VenueUnknownOrder
                               ? RejectReason::VenueUnknownOrder
                               : m.reason,
                           0,
                           st.error);
        ++stats_.order_events;
        if (!whole_failed) apply_action(m.action, st.error);
      }
      continue;
    }
    // New or Replace.
    sent_.answered(e.cl_ord_id);
    Shadow* s = shadows_.find(e.cl_ord_id);
    if (success) {
      if (s != nullptr) {
        if (st.oid != 0) s->oid = st.oid;
        s->answered = true;
      }
      if (e.kind == OrderCommandKind::Replace) {
        if (Shadow* orig = shadows_.find(e.orig_cl_ord_id)) orig->retired = true;
      }
      const bool ack = cfg_.emit_ack_from_response && (s == nullptr || !s->acked);
      if (ack) {
        if (s != nullptr) s->acked = true;
        emit_order_ack(*order_sink_,
                       id_,
                       e.instrument,
                       e.cl_ord_id,
                       IdText(static_cast<std::int64_t>(st.oid)).view(),
                       0);
        ++stats_.order_events;
      }
      continue;
    }
    if (s != nullptr && s->answered) continue;  // the stream rejected it already
    emit_order_reject(*order_sink_, id_, e.instrument, e.cl_ord_id, m.reason, 0, st.error);
    ++stats_.order_events;
    if (e.kind == OrderCommandKind::Replace) {
      shadows_.erase(e.cl_ord_id);
    } else {
      forget_order(e.cl_ord_id);
    }
    if (!whole_failed) apply_action(m.action, st.error);
  }
  if (whole_failed) apply_action(whole.action, r.error);
  if (unknown) {
    FASTMM_LOG_WARN("{}: a reply named fewer statuses ({}) than its action had orders ({})",
                    cfg_.name,
                    r.count,
                    p.count);
    reconcile_.request();
  }
}

// ---- outbound ---------------------------------------------------------------------------------

void HyperliquidVenue::on_wake() {
  drain_outbound();
}

void HyperliquidVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void HyperliquidVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

template <class Ring>
void HyperliquidVenue::write_orders(Ring& ring) {
  drain_outbound_coalesced(
      ring,
      wire_,
      [this] {
        trade_conn_.cork();
        batch_.clear();
        n_orders_ = n_cancels_ = n_cloid_cancels_ = n_modifies_ = 0;
        n_corked_ = 0;
        n_corked_posts_ = 0;
      },
      [this](const EventHeader& h) {
        if (const auto cmd = OrderCommand::from(h)) {
          sent_.note(*cmd, now_ns());
          stage(*cmd);
        } else if (is_reconcile_request(h)) {
          request_open_orders();
        }
      },
      [this] { return flush_staged(); });
}

void HyperliquidVenue::refuse(const OrderCommand& cmd, RejectReason reason, std::string_view why) {
  sent_.answered(cmd.cl_ord_id);
  if (cmd.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  } else {
    emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, why);
  }
  ++stats_.order_events;
}

bool HyperliquidVenue::budget_allows() const noexcept {
  return !budget_.known || budget_.remaining() > cfg_.address_reserve;
}

bool HyperliquidVenue::wire_of(InstrumentId inst,
                               Side side,
                               OrderType type,
                               TimeInForce tif,
                               bool reduce_only,
                               Price px,
                               Qty qty,
                               ClientOrderId id,
                               OrderWire& out) const noexcept {
  if (inst.value >= kMaxInstruments || !assets_[inst.value].known || !px.is_positive())
    return false;
  const AssetSpec& a = assets_[inst.value];
  const bool buy = side == Side::Buy;
  const Price p = round_price(px, a.sz_decimals, buy);
  const Qty q = round_size(qty, a.sz_decimals);
  if (!p.is_positive() || !q.is_positive()) return false;
  out.asset = a.asset;
  out.buy = buy;
  out.px = wire_text(p);
  out.sz = wire_text(q);
  out.reduce_only = reduce_only;
  if (type == OrderType::PostOnly) {
    out.tif = "Alo";
  } else if (type == OrderType::Market || tif == TimeInForce::Ioc) {
    out.tif = "Ioc";
  } else {
    out.tif = "Gtc";
  }
  out.cloid = cloid_of(id);
  return true;
}

void HyperliquidVenue::stage(const OrderCommand& cmd) {
  note_taken(cmd);
  const std::int64_t now = now_ns();
  const bool is_cancel = cmd.kind == OrderCommandKind::Cancel;
  if (cfg_.dry_run) return refuse(cmd, RejectReason::VenueKilled, "dry-run: orders disabled");
  if (fatal_ && !is_cancel) return refuse(cmd, RejectReason::VenueKilled, "venue fatal");
  if (!signer_.usable()) return refuse(cmd, RejectReason::VenueReject, "no signing key");
  if (!is_cancel) {
    if (cmd.kind == OrderCommandKind::New && cmd.tif == TimeInForce::Fok)
      return refuse(cmd, RejectReason::VenueReject, "FOK is not supported (use IOC)");
    if (cmd.kind == OrderCommandKind::Replace && !cfg_.supports_replace)
      return refuse(cmd, RejectReason::VenueReject, "replace not enabled (supports_replace)");
    const RateCheck c = rate_.check(0, now, true);
    if (!c.ok()) {
      count_refusal(stats_, c);
      ++stats_.rate_limit_cooldowns;
      return refuse(cmd, RejectReason::VenueRateLimit, refusal_text(c));
    }
    if (!budget_allows()) {
      ++stats_.refused_orders_1d;
      return refuse(cmd, RejectReason::VenueRateLimit, "local rate limit: address budget");
    }
  }
  const auto entry = [&] {
    return Entry{cmd.kind, cmd.instrument, cmd.cl_ord_id, cmd.orig_cl_ord_id};
  };
  switch (cmd.kind) {
    case OrderCommandKind::New: {
      OrderWire w;
      if (!wire_of(cmd.instrument,
                   cmd.side,
                   cmd.type,
                   cmd.tif,
                   cmd.reduce_only,
                   cmd.price,
                   cmd.qty,
                   cmd.cl_ord_id,
                   w))
        return refuse(cmd, RejectReason::InvalidLot, "no asset, price or size for the venue");
      Shadow s{};
      s.instrument = cmd.instrument;
      s.side = cmd.side;
      s.type = cmd.type;
      s.tif = cmd.tif;
      s.sent_seq = sent_.last_seq();
      s.orig_qty = round_size(cmd.qty, assets_[cmd.instrument.value].sz_decimals);
      s.sent_wall_ns = wall_now().ns;
      if (shadows_.assign(cmd.cl_ord_id, s) == nullptr) {
        shadow_overflow_.refused(cfg_.name, shadows_.size());
        return refuse(cmd, RejectReason::OrderTableFull, "order table full");
      }
      rate_.on_sent(0, now, true);
      stage_orders_[n_orders_] = w;
      stage_order_entries_[n_orders_++] = entry();
      return;
    }
    case OrderCommandKind::Replace: {
      const Shadow* orig = shadows_.find(cmd.orig_cl_ord_id);
      if (orig == nullptr)
        return refuse(cmd, RejectReason::UnknownOrder, "modify: original unknown");
      OrderWire w;
      if (!wire_of(orig->instrument,
                   orig->side,
                   orig->type,
                   orig->tif,
                   false,
                   cmd.price,
                   cmd.qty,
                   cmd.cl_ord_id,
                   w))
        return refuse(cmd, RejectReason::InvalidLot, "no asset, price or size for the venue");
      ModifyWire m;
      m.oid = orig->oid;
      if (m.oid == 0) m.by_cloid = cloid_of(cmd.orig_cl_ord_id);
      m.order = w;
      Shadow copy = *orig;
      copy.replaces = cmd.orig_cl_ord_id;
      copy.sent_seq = sent_.last_seq();
      copy.orig_qty = round_size(cmd.qty, assets_[orig->instrument.value].sz_decimals);
      copy.cum_qty = Qty{};
      copy.oid = 0;
      copy.acked = false;
      copy.answered = false;
      copy.retired = false;
      copy.sent_wall_ns = wall_now().ns;
      if (shadows_.assign(cmd.cl_ord_id, copy) == nullptr) {
        shadow_overflow_.refused(cfg_.name, shadows_.size());
        return refuse(cmd, RejectReason::OrderTableFull, "order table full");
      }
      rate_.on_sent(0, now, true);
      stage_modifies_[n_modifies_] = m;
      Entry e = entry();
      e.instrument = orig->instrument;
      stage_modify_entries_[n_modifies_++] = e;
      return;
    }
    case OrderCommandKind::Cancel: {
      const Shadow* s = shadows_.find(cmd.cl_ord_id);
      const InstrumentId inst = s != nullptr ? s->instrument : cmd.instrument;
      if (inst.value >= kMaxInstruments || !assets_[inst.value].known)
        return refuse(cmd, RejectReason::UnknownOrder, "cancel: no asset for the instrument");
      std::uint64_t oid = s != nullptr ? s->oid : 0;
      if (oid == 0 && cmd.venue_order_id != nullptr) {
        if (const auto v = parse_int64(cmd.venue_order_id->view()))
          oid = static_cast<std::uint64_t>(*v);
      }
      Entry e = entry();
      e.instrument = inst;
      if (oid != 0) {
        stage_cancels_[n_cancels_] = CancelWire{assets_[inst.value].asset, oid};
        stage_cancel_entries_[n_cancels_++] = e;
      } else {
        stage_cloid_cancels_[n_cloid_cancels_] =
            CancelCloidWire{assets_[inst.value].asset, cloid_of(cmd.cl_ord_id)};
        stage_cloid_entries_[n_cloid_cancels_++] = e;
      }
      return;
    }
  }
}

// The staged orders as actions, at most kMaxBatch orders each; false when the corked trade
// connection failed to write them.
bool HyperliquidVenue::flush_staged() {
  auto chunks = [&](std::size_t n, auto&& build, const Entry* entries, ActionKind kind) {
    for (std::size_t off = 0; off < n; off += kMaxBatch) {
      const std::size_t len = std::min(kMaxBatch, n - off);
      if (!build(off, len)) {
        reject_entries({entries + off, len}, RejectReason::VenueReject, "action not encodable");
        continue;
      }
      send_action(kind, {entries + off, len});
    }
  };
  chunks(
      n_cancels_,
      [&](std::size_t off, std::size_t len) {
        return action_->cancel({stage_cancels_.data() + off, len});
      },
      stage_cancel_entries_.data(),
      ActionKind::Cancel);
  chunks(
      n_cloid_cancels_,
      [&](std::size_t off, std::size_t len) {
        return action_->cancel_by_cloid({stage_cloid_cancels_.data() + off, len});
      },
      stage_cloid_entries_.data(),
      ActionKind::CancelByCloid);
  chunks(
      n_modifies_,
      [&](std::size_t off, std::size_t len) {
        return action_->batch_modify({stage_modifies_.data() + off, len});
      },
      stage_modify_entries_.data(),
      ActionKind::BatchModify);
  chunks(
      n_orders_,
      [&](std::size_t off, std::size_t len) {
        return action_->order({stage_orders_.data() + off, len});
      },
      stage_order_entries_.data(),
      ActionKind::Order);
  n_orders_ = n_cancels_ = n_cloid_cancels_ = n_modifies_ = 0;
  if (trade_conn_.uncork()) return true;
  // Nothing written reached the venue: the engine hears so, and the replies will not come.
  for (std::size_t i = 0; i < n_corked_posts_; ++i) {
    if (Pending* p = find_pending(corked_posts_[i])) p->id = 0;
  }
  reject_entries(
      {corked_.data(), n_corked_}, RejectReason::TransportFull, "order batch not written");
  ++stats_.order_send_failures;
  return false;
}

void HyperliquidVenue::reject_entries(std::span<const Entry> entries,
                                      RejectReason reason,
                                      std::string_view why) {
  for (const Entry& e : entries) {
    sent_.answered(e.cl_ord_id);
    if (e.kind == OrderCommandKind::Cancel) {
      emit_cancel_reject(*order_sink_, id_, e.instrument, e.cl_ord_id, reason, 0, why);
    } else {
      emit_order_reject(*order_sink_, id_, e.instrument, e.cl_ord_id, reason, 0, why);
      shadows_.erase(e.cl_ord_id);
    }
    ++stats_.order_events;
  }
}

void HyperliquidVenue::send_action(ActionKind kind, std::span<const Entry> entries) {
  const std::int64_t now = now_ns();
  const RateCheck c = rate_.check(1, now, false);
  if (!c.ok()) {
    count_refusal(stats_, c);
    ++stats_.rate_limit_cooldowns;
    return reject_entries(entries, RejectReason::VenueRateLimit, refusal_text(c));
  }
  std::size_t orders = 0;
  for (const Entry& e : entries) orders += e.kind != OrderCommandKind::Cancel ? 1 : 0;
  const std::uint64_t nonce = action_signer_.next_nonce(wall_ms());
  const std::uint64_t post_id = ++next_post_id_;
  const Cycles before_encode = rdtscp();
  auto count_sent = [&] {
    rate_.on_sent(1, now, false);
    if (budget_.known) budget_.used += static_cast<std::int64_t>(orders);
    for (const Entry& e : entries) {
      switch (e.kind) {
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
  };
  auto fill_pending = [&](Pending& p) {
    p.kind = kind;
    p.count = static_cast<std::uint32_t>(entries.size());
    p.sent_ns = now;
    std::copy(entries.begin(), entries.end(), p.entries.begin());
  };
  if (cfg_.ws_order_api && trade_conn_.is_live()) {
    const std::size_t n =
        action_signer_.write_post(*action_, nonce, post_id, {request_buf_.get(), kMaxRequestBytes});
    const Cycles after_encode = rdtscp();
    if (n > 0 && trade_conn_.send_text(std::string_view(request_buf_.get(), n))) {
      fill_pending(*new_pending(post_id));
      for (const Entry& e : entries) {
        if (n_corked_ < corked_.size()) corked_[n_corked_++] = e;
      }
      if (n_corked_posts_ < corked_posts_.size()) corked_posts_[n_corked_posts_++] = post_id;
      wire_.record(Cycles{}, before_encode, after_encode, rdtscp());
      count_sent();
      return;
    }
    ++stats_.order_send_failures;
  }
  // REST fallback: POST /exchange with the payload alone.
  const std::size_t n =
      action_signer_.write_payload(*action_, nonce, {request_buf_.get(), kMaxRequestBytes});
  if (n == 0 || rest_ == nullptr) {
    ++stats_.order_send_failures;
    return reject_entries(entries, RejectReason::VenueReject, "no order channel");
  }
  auto pending = std::make_shared<Pending>();
  pending->id = post_id;
  fill_pending(*pending);
  std::weak_ptr<int> alive = alive_;
  const bool queued =
      rest_->request("POST",
                     "/exchange",
                     kJsonHeaders,
                     std::string_view(request_buf_.get(), n),
                     [this, alive, pending](const net::HttpResponse& resp) {
                       if (alive.expired()) return;
                       ++stats_.rest_requests;
                       PostReply r;
                       if (resp.error != net::NetError::None) {
                         ++stats_.rest_errors;
                         r.kind = PostReply::Kind::Error;
                         r.error = "transport error";
                         handle_reply(*pending, r);
                         reconcile_.request();
                         return;
                       }
                       const PaddedJson padded(resp.body);
                       if (decoder_->decode_exchange(padded.view(), r) != ParseStatus::Ok) {
                         ++stats_.rest_errors;
                         r = PostReply{};
                         r.kind = PostReply::Kind::Error;
                         r.error = "unreadable reply";
                       }
                       handle_reply(*pending, r);
                     });
  if (!queued) {
    ++stats_.order_send_failures;
    return reject_entries(entries, RejectReason::TransportFull, "rest queue full");
  }
  for (const Entry& e : entries) {
    if (e.kind != OrderCommandKind::Cancel) sent_.sent_over_rest(e.cl_ord_id);
  }
  count_sent();
}

void HyperliquidVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void HyperliquidVenue::apply_action(VenueAction action, std::string_view why) {
  switch (action) {
    case VenueAction::None:
    case VenueAction::DisableInstrument:
    case VenueAction::ResyncClock:
      break;
    case VenueAction::Backoff:
      rate_.cooldown(1'000'000'000, now_ns());
      break;
    case VenueAction::RateLimit:
      // An address over its budget may send one request every 10 s.
      rate_.cooldown(10'000'000'000, now_ns());
      ++stats_.rate_limit_cooldowns;
      FASTMM_LOG_WARN("{}: rate limited ({}); cooling down 10 s", cfg_.name, why.substr(0, 160));
      refresh_rate_limit();
      break;
    case VenueAction::Reconcile:
      request_open_orders();
      break;
    case VenueAction::HardStop:
    case VenueAction::Fatal:
      fatal_ = true;
      FASTMM_LOG_ERROR(
          "{}: fatal venue error ({}); order entry disabled", cfg_.name, why.substr(0, 200));
      trip_venue_kill(KillReason::VenueFatal);
      break;
  }
}

// ---- control path -----------------------------------------------------------------------------

bool HyperliquidVenue::info(const std::string& body,
                            std::function<void(bool, std::string_view)> done) {
  if (rest_ == nullptr) return false;
  std::weak_ptr<int> alive = alive_;
  return rest_->request("POST",
                        "/info",
                        kJsonHeaders,
                        body,
                        [this, alive, done = std::move(done)](const net::HttpResponse& r) {
                          if (alive.expired()) return;
                          ++stats_.rest_requests;
                          if (!r.ok()) {
                            ++stats_.rest_errors;
                            done(false, r.body);
                            return;
                          }
                          done(true, r.body);
                        });
}

void HyperliquidVenue::refresh_rate_limit() {
  if (rate_refresh_pending_ || cfg_.account_address.empty() || cfg_.dry_run) return;
  rate_refresh_pending_ = true;
  last_rate_refresh_ns_ = now_ns();
  const bool queued = info(info_body("userRateLimit", cfg_.account_address, {}),
                           [this](bool ok, std::string_view body) {
                             rate_refresh_pending_ = false;
                             RateLimitState st;
                             if (!ok || !decode_rate_limit(body, st).empty()) return;
                             const bool first = !budget_.known;
                             set_address_budget(st.used, st.cap + st.surplus);
                             if (first || budget_.remaining() <= cfg_.address_reserve * 2)
                               FASTMM_LOG_INFO("{}: request budget {} of {} used ({} left)",
                                               cfg_.name,
                                               st.used,
                                               st.cap,
                                               budget_.remaining());
                           });
  if (!queued) rate_refresh_pending_ = false;
}

void HyperliquidVenue::request_open_orders() {
  reconcile_.request();
}

bool HyperliquidVenue::replay_executions() {
  return fill_replay_.run();
}

bool HyperliquidVenue::fetch_snapshot(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr) return false;
  if (dexes_.empty()) {
    reconcile_.fetched(generation, true);
    return true;
  }
  return fetch_orders(generation, 0);
}

bool HyperliquidVenue::fetch_orders(std::uint64_t generation, std::size_t dex_index) {
  return info(info_body("frontendOpenOrders", cfg_.account_address, dexes_[dex_index]),
              [this, generation, dex_index](bool ok, std::string_view body) {
                if (!reconcile_.current(generation)) return;
                std::vector<OpenOrderRow> rows;
                std::string err = ok ? decode_open_orders(body, rows) : std::string("HTTP error");
                if (!err.empty()) {
                  FASTMM_LOG_WARN("{}: open orders failed ({})", cfg_.name, err);
                  reconcile_.fetched(generation, false);
                  return;
                }
                for (const OpenOrderRow& o : rows) {
                  const InstrumentId inst = instrument_of(o.coin);
                  if (!inst.valid()) continue;
                  ReconcileMsg& m = reconcile_.add_order(inst);
                  m.side = o.buy ? Side::Buy : Side::Sell;
                  m.state = o.sz < o.orig_sz ? OrderState::PartiallyFilled : OrderState::Live;
                  if (const auto cl = cl_ord_id_of_cloid(o.cloid)) {
                    m.cl_ord_id = *cl;
                    if (Shadow* s = shadows_.find(*cl)) s->oid = o.oid;
                  }
                  static_cast<void>(
                      m.venue_order_id.assign(IdText(static_cast<std::int64_t>(o.oid)).view()));
                  m.price = o.px;
                  m.orig_qty = o.orig_sz;
                  m.cum_qty = o.orig_sz > o.sz ? o.orig_sz - o.sz : Qty{};
                }
                const bool sent = dex_index + 1 < dexes_.size()
                                      ? fetch_orders(generation, dex_index + 1)
                                      : fetch_positions(generation, 0);
                if (!sent) reconcile_.fetched(generation, false);
              });
}

bool HyperliquidVenue::fetch_positions(std::uint64_t generation, std::size_t dex_index) {
  return info(info_body("clearinghouseState", cfg_.account_address, dexes_[dex_index]),
              [this, generation, dex_index](bool ok, std::string_view body) {
                if (!reconcile_.current(generation)) return;
                ClearinghouseState st;
                std::string err = ok ? decode_clearinghouse(body, st) : std::string("HTTP error");
                if (!err.empty()) {
                  FASTMM_LOG_WARN("{}: positions failed ({})", cfg_.name, err);
                  reconcile_.fetched(generation, false);
                  return;
                }
                for (InstrumentId id : subscribed_) {
                  if (dex_of(coins_[id.value]) != dexes_[dex_index]) continue;
                  Qty qty{};
                  Price avg{};
                  for (const PositionRow& p : st.positions) {
                    if (instrument_of(p.coin) != id) continue;
                    qty = p.szi;
                    avg = p.entry;
                  }
                  reconcile_.add_position(id, qty, avg);
                  FASTMM_LOG_INFO("{}: {} position {} @ {}", cfg_.name, coins_[id.value], qty, avg);
                }
                if (dex_index + 1 < dexes_.size()) {
                  if (!fetch_positions(generation, dex_index + 1))
                    reconcile_.fetched(generation, false);
                  return;
                }
                reconcile_.fetched(generation, true);
              });
}

// The balance leg: spotClearinghouseState. A unified account keeps the collateral of every dex
// in its spot USDC; the perp clearinghouseState then shows accountValue 0.
bool HyperliquidVenue::fetch_balances(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || cfg_.account_address.empty()) return false;
  const bool queued = info(info_body("spotClearinghouseState", cfg_.account_address, {}),
                           [this, generation](bool ok, std::string_view body) {
                             if (!reconcile_.balances_current(generation)) return;
                             std::vector<SpotBalanceRow> rows;
                             std::string err = ok ? decode_spot_balances(body, rows) : "HTTP error";
                             if (!err.empty()) {
                               FASTMM_LOG_WARN("{}: balances failed ({})", cfg_.name, err);
                               reconcile_.balances_fetched(generation, false, 0);
                               return;
                             }
                             for (const SpotBalanceRow& b : rows) {
                               const Notional free = b.total - b.hold;
                               reconcile_.add_balance(b.coin, BalanceFields::spot(free, b.hold));
                             }
                             reconcile_.balances_fetched(generation, true, wall_ms());
                           });
  if (!queued) reconcile_.balances_fetched(generation, false, 0);
  return true;
}

void HyperliquidVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const Shadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void HyperliquidVenue::drop_shadow(ClientOrderId id) {
  forget_order(id);
}

// ---- fill replay ------------------------------------------------------------------------------

void HyperliquidVenue::resume_executions(std::int64_t since_venue_ms,
                                         const std::vector<std::string>& known) {
  fill_replay_.resume(since_venue_ms, known);
}

bool HyperliquidVenue::request_executions(std::int64_t since_venue_ms) {
  if (since_venue_ms > 0 && !fill_replay_.active()) fill_replay_.restart_from(since_venue_ms);
  return fill_replay_.run();
}

// userFillsByTime from start_ms, oldest first, at most 2000 a reply.
bool HyperliquidVenue::query_fills(const ReplayQuery& q) {
  if (rest_ == nullptr || q.start_ms <= 0) return false;
  std::string body = fmt::format(
      R"({{"type":"userFillsByTime","user":"{}","startTime":{})", cfg_.account_address, q.start_ms);
  if (q.end_ms > 0) body += fmt::format(R"(,"endTime":{})", q.end_ms);
  body += R"(,"aggregateByTime":false})";
  const bool queued = info(body, [this, q](bool ok, std::string_view reply) {
    if (!fill_replay_.expects(q)) return;
    std::vector<FillRow> rows;
    std::string err = ok ? decode_user_fills(reply, rows) : std::string("HTTP error");
    if (!err.empty()) {
      ++stats_.execution_query_errors;
      FASTMM_LOG_ERROR(
          "{}: userFillsByTime failed ({}); this reconciliation cannot book the "
          "fills the private stream missed",
          cfg_.name,
          err);
      fill_replay_.failed(q);
      return;
    }
    ReplayPage<FillRow> page;
    page.more = rows.size() >= kFillsPage;
    for (FillRow& f : rows) {
      const std::int64_t t = f.time_ms;
      std::string key = std::to_string(f.tid);
      page.rows.push_back({t, 0, std::move(key), std::move(f)});
    }
    fill_replay_.answer(q, std::move(page));
  });
  if (!queued) {
    ++stats_.execution_query_errors;
    FASTMM_LOG_ERROR("{}: no room to ask for the account's fills", cfg_.name);
  }
  return queued;
}

bool HyperliquidVenue::emit_fill_row(const FillRow& f) {
  const InstrumentId inst = instrument_of(f.coin);
  if (!inst.valid()) return false;
  ClientOrderId cl{};
  if (const auto id = cl_ord_id_of_cloid(f.cloid)) cl = *id;
  emit_replayed_fill(
      *order_sink_,
      id_,
      inst,
      cl,
      IdText(static_cast<std::int64_t>(f.oid)).view(),
      std::to_string(f.tid),
      f.buy ? Side::Buy : Side::Sell,
      f.px,
      f.sz,
      f.fee,
      f.fee_token.empty() || f.fee_token == "USDC" ? FeeAsset::Quote : FeeAsset::Other,
      f.crossed ? Liquidity::Taker : Liquidity::Maker,
      f.time_ms);
  ++stats_.order_events;
  ++stats_.executions_fetched;
  return true;
}

// ---- cancel-all and the scheduled cancel
// ---------------------------------------------------------

void HyperliquidVenue::cancel_all_async() {
  if (rest_ == nullptr || !signer_.usable()) return;
  for (const std::string& dex : dexes_) {
    static_cast<void>(
        info(info_body("frontendOpenOrders", cfg_.account_address, dex),
             [this](bool ok, std::string_view body) {
               std::vector<OpenOrderRow> rows;
               if (!ok || !decode_open_orders(body, rows).empty()) {
                 FASTMM_LOG_ERROR("{}: cancel-all could not read the open orders", cfg_.name);
                 return;
               }
               std::vector<CancelWire> cancels;
               for (const OpenOrderRow& o : rows) {
                 const InstrumentId inst = instrument_of(o.coin);
                 if (!inst.valid() || !assets_[inst.value].known) continue;
                 cancels.push_back({assets_[inst.value].asset, o.oid});
               }
               for (std::size_t off = 0; off < cancels.size(); off += kMaxBatch) {
                 ActionBuilder a;
                 if (!a.cancel({cancels.data() + off, std::min(kMaxBatch, cancels.size() - off)}))
                   continue;
                 std::string payload(kMaxRequestBytes, '\0');
                 const std::size_t n =
                     action_signer_.write_payload(a, action_signer_.next_nonce(wall_ms()), payload);
                 if (n == 0) continue;
                 payload.resize(n);
                 static_cast<void>(rest_->request(
                     "POST", "/exchange", kJsonHeaders, payload, [](const net::HttpResponse&) {}));
               }
             }));
  }
}

bool HyperliquidVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr) return true;
  BlockingHttpOptions opts = blocking_options(cfg_);
  bool all_ok = true;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    for (const std::string& dex : dexes_) {
      const HttpReply r = http.request("POST",
                                       "/info",
                                       kJsonHeaders,
                                       info_body("frontendOpenOrders", cfg_.account_address, dex));
      std::vector<OpenOrderRow> rows;
      if (!r.ok() || !decode_open_orders(r.body, rows).empty()) {
        FASTMM_LOG_ERROR("{}: kill-switch cancel-all: open orders of dex '{}' unreadable (HTTP {})",
                         cfg_.name,
                         dex,
                         r.status);
        all_ok = false;
        continue;
      }
      std::vector<CancelWire> cancels;
      for (const OpenOrderRow& o : rows) {
        const InstrumentId inst = instrument_of(o.coin);
        if (!inst.valid() || !assets_[inst.value].known) continue;
        cancels.push_back({assets_[inst.value].asset, o.oid});
      }
      for (std::size_t off = 0; off < cancels.size(); off += kMaxBatch) {
        ActionBuilder a;
        if (!a.cancel({cancels.data() + off, std::min(kMaxBatch, cancels.size() - off)})) continue;
        std::string payload(kMaxRequestBytes, '\0');
        const std::size_t n =
            action_signer_.write_payload(a, action_signer_.next_nonce(wall_ms()), payload);
        if (n == 0) {
          all_ok = false;
          continue;
        }
        payload.resize(n);
        const HttpReply c = http.request("POST", "/exchange", kJsonHeaders, payload);
        const bool ok = c.ok() && c.body.find(R"("status":"ok")") != std::string::npos;
        if (!ok) {
          all_ok = false;
          FASTMM_LOG_ERROR("{}: kill-switch cancel of {} order(s) failed: HTTP {} {}",
                           cfg_.name,
                           std::min(kMaxBatch, cancels.size() - off),
                           c.status,
                           c.body.substr(0, 160));
        }
      }
      FASTMM_LOG_INFO(
          "{}: kill-switch cancel-all: {} order(s) on dex '{}'", cfg_.name, cancels.size(), dex);
    }
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR(
        "{}: kill-switch cancel-all failed: {}", cfg_.name, std::string_view(e.what()));
    return false;
  }
  return all_ok;
}

// scheduleCancel at `time_ms` (0: remove it), over REST.
void HyperliquidVenue::send_schedule_cancel(std::int64_t time_ms) {
  if (rest_ == nullptr || !signer_.usable() || cfg_.dry_run) return;
  ActionBuilder a;
  if (!a.schedule_cancel(time_ms)) return;
  std::string payload(kMaxRequestBytes, '\0');
  const std::size_t n =
      action_signer_.write_payload(a, action_signer_.next_nonce(wall_ms()), payload);
  if (n == 0) return;
  payload.resize(n);
  const std::uint32_t round = dms_.begin_round(now_ns(), 1);
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "POST", "/exchange", kJsonHeaders, payload, [this, alive, round](const net::HttpResponse& r) {
        if (alive.expired() || r.error == net::NetError::Canceled) return;
        ++stats_.rest_requests;
        if (r.ok() && r.body.find(R"("status":"ok")") != std::string::npos) {
          dms_.confirmed(round);
          return;
        }
        ++stats_.rest_errors;
        if (r.ok()) {
          // Refused by the venue, not lost: the reason, once; no more refreshes (each would
          // spend a request of the address budget for the same refusal) and no lapse.
          if (!dms_refused_)
            FASTMM_LOG_WARN("{}: scheduleCancel refused: {}; going on without it",
                            cfg_.name,
                            std::string_view(r.body).substr(0, 400));
          dms_refused_ = true;
          return;
        }
        FASTMM_LOG_ERROR(
            "{}: scheduleCancel failed: HTTP {} {}", cfg_.name, r.status, r.body.substr(0, 160));
      });
  if (queued) {
    dms_.went_out();
    if (budget_.known) ++budget_.used;
  }
}

// ---- housekeeping -----------------------------------------------------------------------------

void HyperliquidVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (now - last_ping_ns_ >= static_cast<std::int64_t>(cfg_.ping_interval_ms) * kNsPerMs) {
    last_ping_ns_ = now;
    if (md_conn_.is_live()) static_cast<void>(md_conn_.send_text(kPingFrame));
    if (private_conn_.is_live()) static_cast<void>(private_conn_.send_text(kPingFrame));
    if (trade_conn_.is_live()) static_cast<void>(trade_conn_.send_text(kPingFrame));
  }
  if (!cfg_.dry_run && signer_.usable() &&
      now - last_rate_refresh_ns_ >= cfg_.rate_limit_refresh_s * 1'000'000'000)
    refresh_rate_limit();
  // A post with no reply: its orders are decided by the reconciliation.
  bool stale = false;
  for (Pending& p : pending_) {
    if (p.id != 0 && now - p.sent_ns >= kPendingTimeoutNs) {
      FASTMM_LOG_WARN(
          "{}: post {} unanswered after {} s", cfg_.name, p.id, kPendingTimeoutNs / 1'000'000'000);
      p.id = 0;
      stale = true;
    }
  }
  if (stale) reconcile_.request();
  reconcile_.on_timer(now);
  fill_replay_.on_timer(now);
  if (!cfg_.dry_run && signer_.usable() && !dms_refused_) {
    switch (dms_.poll(now)) {
      case CountdownDriver::Step::Lapsed:
        FASTMM_LOG_ERROR(
            "{}: scheduleCancel not refreshed within {} ms; the venue has cancelled the "
            "account's orders: killing this venue",
            cfg_.name,
            dms_.window_ms());
        trip_venue_kill(KillReason::DeadMansSwitchLost);
        break;
      case CountdownDriver::Step::Refresh:
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

void HyperliquidVenue::log_latency(std::int64_t now) {
  last_latency_log_ns_ = now;
  if (post_rtt_ms_.empty() && open_lag_ms_.empty()) return;
  const std::size_t n_rtt = post_rtt_ms_.size();
  const std::size_t n_lag = open_lag_ms_.size();
  const std::int64_t rtt50 = percentile(post_rtt_ms_, 50);
  const std::int64_t rtt90 = percentile(post_rtt_ms_, 90);
  const std::int64_t rtt_max =
      post_rtt_ms_.empty() ? 0 : *std::max_element(post_rtt_ms_.begin(), post_rtt_ms_.end());
  const std::int64_t lag50 = percentile(open_lag_ms_, 50);
  const std::int64_t lag90 = percentile(open_lag_ms_, 90);
  const std::int64_t lag_max =
      open_lag_ms_.empty() ? 0 : *std::max_element(open_lag_ms_.begin(), open_lag_ms_.end());
  FASTMM_LOG_INFO(
      "{}: latency: post reply {} sample(s) p50 {} p90 {} max {} ms; order open "
      "(block time - send) {} sample(s) p50 {} p90 {} max {} ms; budget {} of {}",
      cfg_.name,
      n_rtt,
      rtt50,
      rtt90,
      rtt_max,
      n_lag,
      lag50,
      lag90,
      lag_max,
      budget_.used,
      budget_.cap);
  post_rtt_ms_.clear();
  open_lag_ms_.clear();
}

void HyperliquidVenue::publish_status() noexcept {
  stats_.shadows = shadows_.size();
  stats_.shadows_refused = shadow_overflow_.count();
  stats_.shadows_swept = reconcile_.shadows_swept();
  stats_.execution_queries = fill_replay_.replays();
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
  if (budget_.known) {
    b.orders_1d.window_ms = 86'400'000;
    b.orders_1d.limit = budget_.cap;
    b.orders_1d.used = budget_.used;
    b.orders_1d.cap = std::max<std::int64_t>(0, budget_.cap - cfg_.address_reserve);
  }
  budget_pub_.store(b);
}

VenueStatus HyperliquidVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

HyperliquidVenueConfig make_hyperliquid_config(const VenueSection& v, bool dry_run) {
  HyperliquidVenueConfig c;
  c.name = v.name;
  c.mainnet = !v.testnet;
  c.ws_url = !v.ws_url.empty() ? v.ws_url
             : c.mainnet       ? std::string("wss://api.hyperliquid.xyz/ws")
                               : std::string("wss://api.hyperliquid-testnet.xyz/ws");
  c.rest_url = !v.rest_url.empty() ? v.rest_url
               : c.mainnet         ? std::string("https://api.hyperliquid.xyz")
                                   : std::string("https://api.hyperliquid-testnet.xyz");
  c.insecure_tls = v.insecure_tls;
  c.ca_file = v.ca_file;
  c.supports_replace = v.supports_replace;
  c.dry_run = dry_run;
  c.private_key = v.api_secret;
  const VenueExtras x(v.extra);
  c.account_address = v.api_key;
  c.vault_address = x.get("vault_address");
  c.dex = x.get("dex");
  auto address_ok = [](const std::string& a) {
    std::uint8_t bytes[20];
    return a.empty() || parse_hex(a, bytes);
  };
  if (!address_ok(c.account_address))
    throw std::invalid_argument(fmt::format(
        "venues.{}.api_key: the account address, expected 0x and 40 hex digits", v.name));
  if (!address_ok(c.vault_address))
    throw std::invalid_argument(
        fmt::format("venues.{}.vault_address: expected 0x and 40 hex digits", v.name));
  if (x.get("order_api") == "rest") c.ws_order_api = false;
  c.channels.l2_book = x.flag("l2_book", true);
  c.channels.bbo = x.flag("bbo", true);
  c.channels.trades = x.flag("trades", true);
  c.stale_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("stale_ms", c.stale_ms)));
  c.dead_ms =
      static_cast<std::uint32_t>(std::max<std::int64_t>(0, x.integer("dead_ms", c.dead_ms)));
  c.ping_interval_ms = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(x.integer("ping_interval_ms", c.ping_interval_ms), 1000, 50'000));
  c.orders_per_second = static_cast<std::uint32_t>(
      std::max<std::int64_t>(0, x.integer("orders_per_second", c.orders_per_second)));
  c.posts_per_minute = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(x.integer("posts_per_minute", c.posts_per_minute), 0, 2000));
  c.address_reserve = std::max<std::int64_t>(0, x.integer("address_reserve", c.address_reserve));
  c.rate_limit_refresh_s =
      std::max<std::int64_t>(10, x.integer("rate_limit_refresh_s", c.rate_limit_refresh_s));
  c.emit_ack_from_response = x.flag("emit_ack_from_response", true);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  const std::int64_t dms = std::max<std::int64_t>(0, x.integer("dead_mans_switch_s", 0));
  if (dms != 0 && dms < 10)
    throw std::invalid_argument(fmt::format(
        "venues.{}.dead_mans_switch_s: at least 10 (the venue wants the cancel time 5 s ahead)",
        v.name));
  c.dead_mans_switch_s = dms;
  c.latency_log_s = std::max<std::int64_t>(0, x.integer("latency_log_s", c.latency_log_s));
  check_liveness(v.name, c.stale_ms, c.dead_ms);
  return c;
}

}  // namespace fastmm::venues::hyperliquid
