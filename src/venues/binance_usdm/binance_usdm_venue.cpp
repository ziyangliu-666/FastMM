#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"

#include "fastmm/venues/binance/binance_rest_decoder.hpp"
#include "fastmm/venues/binance/binance_trade_history.hpp"
#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_rest_decoder.hpp"
#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace fastmm::venues::binance_usdm {

namespace {

constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr std::int64_t kSecNs = 1'000'000'000;
constexpr std::int64_t kListenKeyKeepaliveNs = 30LL * 60 * kSecNs;  // key valid 60 min
constexpr std::int64_t kListenKeyRetryNs = 5 * kSecNs;
constexpr std::int64_t kClockResyncNs = 30LL * 60 * kSecNs;
constexpr std::int64_t kHousekeepingNs = kSecNs;
constexpr std::int64_t kDefaultCooldownNs = 10 * kSecNs;
constexpr std::int64_t kFundingQueryDelayNs = 1'000'000'000;  // after a user-stream funding event
// "Account Trade List" (GET /fapi/v1/userTrades): limit max 1000; a time range of at most 7 days,
// and nothing older than 3 months.
constexpr int kUserTradesLimit = 1000;
constexpr std::int64_t kUserTradesWindowMs = 7LL * 24 * 3600 * 1000;
constexpr std::int64_t kUserTradesHistoryMs = 90LL * 24 * 3600 * 1000;
constexpr std::size_t kUserTradesMaxPages = 10;  // per symbol and replay
constexpr std::uint32_t kUserTradesWeight = 5;   // "Account Trade List"
constexpr std::uint32_t kIncomeWeight = 30;      // "Get Income History"
// How long a blocking start-up request waits for its weight (wait_for_weight): a window and some.
constexpr std::int64_t kStartupWaitNs = 65'000'000'000;
// "Get Income History": limit max 1000, a range of at most 7 days, the last 3 months.
constexpr std::size_t kIncomeMaxPages = 20;
constexpr std::int64_t kLogonRetryNs = 2 * kSecNs;
constexpr std::string_view kLogonId = "logon";
// The futures servers ping every 3 minutes ("Websocket Market Streams" / "WebSocket API General
// Info"), and pings count as receive activity: a quiet connection is dead only after that.
constexpr std::uint32_t kQuietDeadMs = 240'000;
constexpr std::uint32_t kMdDeadMs = 45'000;

// "Order Book" weight by limit: 5, 10, 20, 50: 2; 100: 5; 500: 10; 1000: 20.
std::uint32_t depth_weight(int limit) noexcept {
  if (limit <= 50) return 2;
  if (limit <= 100) return 5;
  if (limit <= 500) return 10;
  return 20;
}
// The smallest valid limit that is not below `n`.
int valid_depth_limit(std::int64_t n) noexcept {
  for (int v : {5, 10, 20, 50, 100, 500}) {
    if (n <= v) return v;
  }
  return 1000;
}

std::string url_root(const std::string& url) {
  return venues::url_root("binance_usdm", url);
}

Qty non_negative(Qty q) noexcept {
  return q.raw < 0 ? Qty{} : q;
}

}  // namespace

// ---- construction ---------------------------------------------------------------------------

BinanceUsdmVenue::BinanceUsdmVenue(VenueId id, BinanceUsdmVenueConfig cfg)
    : id_(id),
      cfg_(std::move(cfg)),
      md_venue_(cfg_.pool_of.valid() ? cfg_.pool_of : id),
      signer_(cfg_.credentials),
      rate_(cfg_.rate_threshold),
      dms_(cfg_.dry_run ? 0 : cfg_.dead_mans_switch_ms) {
  // Binance counts request weight per IP: the accounts of a pool spend one window.
  if (cfg_.share_ip_weight) rate_.share_ip(shared_rate("binance_usdm " + cfg_.rest_url));
  std::memset(scratch_, 0, sizeof scratch_);
  ReplayLimits limits;
  limits.window_ms = kUserTradesWindowMs;
  limits.history_ms = kUserTradesHistoryMs;
  limits.page_rows = kUserTradesLimit;
  limits.max_pages = kUserTradesMaxPages;
  exec_replay_.setup(
      cfg_.name,
      "execution(s)",
      limits,
      {[this] { return replay_ready(); },
       [this] { return venue_time_ms(); },
       [this](const ReplayQuery& q) { return query_executions(q); },
       [this](bool complete) { reconcile_.replay_done(complete); },
       [this](const ReplayLookup& l) { return lookup_order(l); },
       [this] {
         return rate_.can_send(kUserTradesWeight, now_ns(), false, RateLimiter::kBulkShare);
       },
       // A sweep reads the instruments with order activity since their watermark.
       [this](std::size_t stream, std::int64_t since_ms) {
         return stream >= subscribed_.size() ||
                activity_.since(subscribed_[stream], since_ms, venue_time_ms(), now_ns());
       }},
      [this](std::size_t stream, const binance::MyTradeRow& t) {
        return emit_execution(stream, t);
      },
      [this](std::size_t, const binance::MyTradeRow& t) {
        return t.order_id > 0 && order_ids_.find(static_cast<std::uint64_t>(t.order_id)) == nullptr
                   ? std::string(IdText(t.order_id).view())
                   : std::string{};
      });
  limits.max_pages = kIncomeMaxPages;
  funding_replay_.setup(
      cfg_.name,
      "funding payment(s)",
      limits,
      {[this] { return replay_ready(); },
       [this] { return venue_time_ms(); },
       [this](const ReplayQuery& q) { return query_funding(q); },
       [](bool) {},
       {},
       [this] { return rate_.can_send(kIncomeWeight, now_ns(), false, RateLimiter::kBulkShare); }},
      [this](std::size_t, const IncomeRecord& row) { return emit_funding_row(row); });
  funding_replay_.set_streams(1);
}

BinanceUsdmVenue::~BinanceUsdmVenue() {
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr)
    reactor_->cancel_timer(housekeeping_timer_);
}

VenueCaps BinanceUsdmVenue::caps() const noexcept {
  VenueCaps c;
  c.supports_replace = cfg_.supports_replace;
  c.supports_post_only = true;
  c.ws_order_entry = cfg_.ws_order_api;
  c.user_stream = !cfg_.dry_run && signer_.usable();
  return c;
}

std::int64_t BinanceUsdmVenue::venue_time_ms() const noexcept {
  return wall_now().ns / kNsPerMs + clock_offset_ms_;
}

std::string BinanceUsdmVenue::api_headers() const {
  return signer_.usable() ? binance::api_key_header(signer_.api_key()) : std::string{};
}

InstrumentId BinanceUsdmVenue::instrument_of(std::string_view symbol) const noexcept {
  return symbols_ != nullptr ? symbols_->find(md_venue_, symbol) : InstrumentId::invalid();
}

std::string BinanceUsdmVenue::stream_root() const {
  return url_root(cfg_.ws_url);
}

std::string BinanceUsdmVenue::private_root() const {
  return cfg_.ws_private_url.empty() ? stream_root() + "/private" : url_root(cfg_.ws_private_url);
}

net::ConnectionConfig BinanceUsdmVenue::ws_config(const std::string& url,
                                                  std::uint32_t min_dead_ms) const {
  net::ConnectionConfig c;
  c.url = url;
  c.tls.ca_file = cfg_.ca_file;
  c.tls.insecure = cfg_.insecure_tls;
  c.stale_ms = cfg_.stale_ms;
  c.dead_ms = std::max<std::uint32_t>(cfg_.dead_ms, min_dead_ms);
  c.backoff = cfg_.backoff;
  c.max_lifetime_ms = cfg_.max_lifetime_ms;
  c.manual_subscribe = false;
  return c;
}

// ---- reference data (blocking, main thread) -------------------------------------------------

// exchangeInfo's rules for the configured symbols, and the venue's rate limits.
Result<void, std::string> BinanceUsdmVenue::apply_exchange_info(
    const HttpReply& reply,
    const std::vector<Instrument*>& mine,
    const std::vector<std::string>& wanted) {
  ExchangeInfo info;
  if (const std::string err = decode_exchange_info(reply.body, info, wanted); !err.empty())
    return fail(fmt::format("{}: {}", cfg_.name, err));
  for (Instrument* inst : mine) {
    const SymbolInfo* f = nullptr;
    for (const SymbolInfo& s : info.symbols) {
      if (iequals_symbol(s.symbol, inst->symbol.view())) f = &s;
    }
    if (f == nullptr)
      return fail(fmt::format("{}: symbol {} not in exchangeInfo", cfg_.name, inst->symbol.view()));
    if (!f->tick.is_positive() || !f->step.is_positive())
      return fail(fmt::format("{}: {} has invalid tick/step", cfg_.name, inst->symbol.view()));
    // A TradFi perpetual (equity, commodity) has a perpetual's rules, streams and funding.
    if (f->contract_type != "PERPETUAL" && f->contract_type != "TRADIFI_PERPETUAL")
      return fail(fmt::format("{}: {} is a {} contract; only PERPETUAL is supported",
                              cfg_.name,
                              inst->symbol.view(),
                              f->contract_type));
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
    inst->max_notional = Notional{};
    inst->flags = static_cast<std::uint8_t>(inst->flags | Instrument::kReduceOnlySupported);
    if (inst->asset_class != AssetClass::Perpetual) {
      FASTMM_LOG_WARN(
          "{}: {} is a perpetual; asset_class set to perpetual", cfg_.name, inst->symbol.view());
      inst->asset_class = AssetClass::Perpetual;
    }
    if (f->status != "TRADING") {
      FASTMM_LOG_ERROR(
          "{}: {} status is {} (not TRADING): disabled", cfg_.name, inst->symbol.view(), f->status);
      inst->flags = static_cast<std::uint8_t>(inst->flags & ~Instrument::kEnabled);
    }
    if (!f->gtx_allowed)
      FASTMM_LOG_WARN("{}: {} does not allow GTX (post-only)", cfg_.name, inst->symbol.view());
  }
  for (const binance::RateLimitRule& r : info.rate_limits) {
    if (r.type == "REQUEST_WEIGHT") {
      rate_.add_weight_bucket(static_cast<std::uint32_t>(r.limit), r.window_ns());
    } else if (r.type == "ORDERS") {
      rate_.add_order_bucket(static_cast<std::uint32_t>(r.limit), r.window_ns());
    }
  }
  if (const auto used = parse_int64(reply.header("x-mbx-used-weight-1m")))
    rate_.on_headers(*used, -1, now_ns());
  return {};
}

Result<void, std::string> BinanceUsdmVenue::load_reference_data(InstrumentTable& instruments) {
  std::vector<Instrument*> mine;
  std::vector<std::string> wanted;
  for (const Instrument& inst : instruments) {
    if (inst.venue != md_venue_) continue;  // a pool member: the primary's instruments
    mine.push_back(&instruments.get(inst.id));
    wanted.emplace_back(inst.symbol.view());
  }
  if (mine.empty()) return {};
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  HttpReply reply;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    // "Exchange Information": no symbol filter, IP weight 1.
    reply = http.get("/fapi/v1/exchangeInfo");
  } catch (const std::exception& e) {
    reply.error = e.what();
  }
  if (!reply.ok()) {
    const std::string why = reply.error.empty()
                                ? fmt::format("HTTP {} {}", reply.status, reply.body.substr(0, 200))
                                : reply.error;
    if (!cfg_.allow_offline_reference_data)
      return fail(fmt::format("{}: exchangeInfo failed: {}", cfg_.name, why));
    // Only the symbol rules are missing: the clock and the account checks below still run.
    FASTMM_LOG_WARN("{}: exchangeInfo failed ({}); keeping configured tick/lot", cfg_.name, why);
  } else if (auto applied = apply_exchange_info(reply, mine, wanted); !applied) {
    return applied;
  }
  load_funding_intervals(mine, wanted);
  // exchangeInfo.serverTime is cached (5 days old on Demo, observed 2026-09-15): the clock offset
  // comes from GET /fapi/v1/time.
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    const HttpReply t = http.get("/fapi/v1/time");
    std::int64_t server_ms = 0;
    if (t.ok() && binance::decode_server_time(t.body, server_ms).empty()) {
      clock_offset_ms_ = server_ms - wall_now().ns / kNsPerMs;
      clock_sync_ns_ = now_ns();
      stats_.clock_offset_ms = clock_offset_ms_;
      publish_status();  // the offset is known before the venue connects (fastmm-gateway reads it)
      if (clock_offset_ms_ > 1000 || clock_offset_ms_ < -1000)
        FASTMM_LOG_WARN("{}: clock offset to venue is {} ms", cfg_.name, clock_offset_ms_.load());
    }
  } catch (const std::exception& e) {
    FASTMM_LOG_WARN("{}: server time failed: {}", cfg_.name, std::string_view(e.what()));
  }
  FASTMM_LOG_INFO("{}: reference data loaded for {} symbols", cfg_.name, mine.size());
  refused_account_settings_ = false;
  if (!cfg_.dry_run && signer_.usable()) {
    if (std::string err = account_checks(mine); !err.empty()) return fail(std::move(err));
  }
  return {};
}

// GET /fapi/v1/fundingInfo: the funding interval of each configured symbol it lists; the others
// keep the parser's 8 hours. Not fatal: the interval only scales the rate for the strategy.
void BinanceUsdmVenue::load_funding_intervals(const std::vector<Instrument*>& mine,
                                              const std::vector<std::string>& wanted) {
  funding_intervals_.clear();
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  HttpReply reply;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    reply = http.get("/fapi/v1/fundingInfo");
  } catch (const std::exception& e) {
    reply.error = e.what();
  }
  std::vector<FundingInfoRecord> rows;
  std::string err;
  if (!reply.ok()) {
    err = reply.error.empty() ? fmt::format("HTTP {} {}", reply.status, reply.body.substr(0, 200))
                              : reply.error;
  } else {
    err = decode_funding_info(reply.body, rows, wanted);
  }
  if (!err.empty()) {
    FASTMM_LOG_WARN(
        "{}: fundingInfo failed ({}); funding interval 8 h for every symbol", cfg_.name, err);
    return;
  }
  funding_intervals_.reserve(rows.size());
  for (const FundingInfoRecord& row : rows) {
    for (const Instrument* inst : mine) {
      if (!iequals_symbol(row.symbol, inst->symbol.view())) continue;
      funding_intervals_.emplace_back(inst->id, seconds(row.interval_hours * 3600));
      FASTMM_LOG_INFO(
          "{}: {} funding every {} h", cfg_.name, inst->symbol.view(), row.interval_hours);
    }
  }
}

Duration BinanceUsdmVenue::funding_interval_of(InstrumentId id) const noexcept {
  for (const auto& [inst, interval] : funding_intervals_) {
    if (inst == id) return interval;
  }
  return BinanceUsdmMdParser::kDefaultFundingInterval;
}

// Account settings: position mode (hedge mode is refused), leverage and margin type per symbol,
// balances. Nothing is changed unless one_way_mode or leverage asks for it.
std::string BinanceUsdmVenue::account_checks(const std::vector<Instrument*>& mine) {
  BlockingHttpOptions opts;
  opts.ca_file = cfg_.ca_file;
  opts.insecure_tls = cfg_.insecure_tls;
  opts.timeout_ms = cfg_.http_timeout_ms;
  try {
    BlockingHttp http(cfg_.rest_url, opts);
    auto send = [&](const RestRequest& rr) {
      const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
      // Before the connections open, within the bulk share of the weight: symbolConfig is one per
      // symbol, and the weight it spends is the snapshots' and replay's a moment later.
      wait_for_weight(rate_, rr.weight, now_ns(), kStartupWaitNs, [](std::int64_t ns) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
      });
      HttpReply reply = http.request(rr.method, target, api_headers());
      rate_.on_sent(rr.weight, now_ns());
      return reply;
    };
    auto signed_get = [&](std::string_view path, std::string_view symbol, std::uint32_t weight) {
      RestRequest rr;
      if (!BinanceUsdmOrderEncoder::encode_rest_signed_get(
              signer_, cfg_.recv_window_ms, path, symbol, venue_time_ms(), weight, rr))
        return HttpReply{};
      return send(rr);
    };
    auto refused = [&](const HttpReply& r) {
      int code = 0;
      std::string msg;
      if (!r.error.empty()) return r.error;
      static_cast<void>(binance::decode_rest_error(r.body, code, msg));
      return fmt::format("HTTP {} code {} {}", r.status, code, msg);
    };
    // A setting a retry does not fix: fastmm-live exits 3, not 4.
    auto refuse = [&](std::string why) {
      refused_account_settings_ = true;
      return why;
    };
    // one_way_mode: only an account with nothing to lose by it. The venue refuses the change with
    // a position or an open order anyway; asking first says which.
    auto switch_to_one_way = [&]() -> std::string {
      const std::string prefix = fmt::format("{}: the account is in hedge mode", cfg_.name);
      // "Position Information V3": IP weight 5.
      const HttpReply pr = signed_get("/fapi/v3/positionRisk", {}, 5);
      std::vector<PositionRecord> positions;
      if (!pr.ok() || !decode_position_risk(pr.body, positions).empty())
        return fmt::format(
            "{} and its positions could not be read ({}); not switched", prefix, refused(pr));
      std::string held;
      for (const PositionRecord& p : positions) {
        if (p.qty.is_zero()) continue;
        held += fmt::format("{}{} {} {}",
                            held.empty() ? "" : ", ",
                            p.symbol,
                            p.position_side,
                            DecimalText(p.qty).view());
      }
      if (!held.empty())
        return fmt::format(
            "{} with open positions ({}); one_way_mode switches only an account with no position "
            "and no open order: close them first",
            prefix,
            held);
      // "Current All Open Orders" without a symbol: IP weight 40.
      const HttpReply oo = signed_get("/fapi/v1/openOrders", {}, 40);
      std::size_t open_orders = 0;
      if (!oo.ok() || !decode_open_order_count(oo.body, open_orders).empty())
        return fmt::format(
            "{} and its open orders could not be read ({}); not switched", prefix, refused(oo));
      if (open_orders > 0)
        return fmt::format(
            "{} with {} open order(s); one_way_mode switches only an account with no position and "
            "no open order: cancel them first",
            prefix,
            open_orders);
      RestRequest rr;
      if (!BinanceUsdmOrderEncoder::encode_rest_position_mode(
              signer_, cfg_.recv_window_ms, false, venue_time_ms(), rr))
        return prefix + "; the position mode request could not be encoded";
      const HttpReply r = send(rr);
      if (!r.ok())
        return fmt::format("{}; switching to one-way mode failed: {}", prefix, refused(r));
      FASTMM_LOG_WARN("{}: switched the account from hedge mode to one-way mode (one_way_mode)",
                      cfg_.name);
      return {};
    };
    // "Get Current Position Mode": IP weight 30.
    const HttpReply mode = signed_get("/fapi/v1/positionSide/dual", {}, 30);
    if (mode.ok()) {
      bool dual = false;
      if (decode_position_mode(mode.body, dual).empty() && dual) {
        if (!cfg_.one_way_mode)
          return refuse(fmt::format(
              "{}: the account is in hedge mode (dualSidePosition=true); binance_usdm needs "
              "one-way mode. Switch it in the futures preferences (Position Mode: One-way), or set "
              "one_way_mode = true in [venues.{}] to switch it at start",
              cfg_.name,
              cfg_.name));
        if (std::string err = switch_to_one_way(); !err.empty()) return refuse(std::move(err));
      } else {
        FASTMM_LOG_INFO("{}: position mode one-way", cfg_.name);
      }
    } else if (mode.status == 401 || mode.status == 400 || mode.status == 403) {
      return fmt::format("{}: account check failed: {}", cfg_.name, refused(mode));
    } else {
      FASTMM_LOG_WARN("{}: position mode unknown (HTTP {} {})",
                      cfg_.name,
                      mode.status,
                      mode.error.empty() ? mode.body.substr(0, 120) : mode.error);
    }
    // "Get Current Multi-Assets Mode": IP weight 30.
    const HttpReply multi = signed_get("/fapi/v1/multiAssetsMargin", {}, 30);
    if (multi.ok() && decode_multi_assets_mode(multi.body, multi_assets_).empty()) {
      FASTMM_LOG_INFO("{}: {} mode", cfg_.name, multi_assets_ ? "multi-assets" : "single-asset");
    } else {
      FASTMM_LOG_WARN("{}: multi-assets mode unknown (HTTP {}); taken as single-asset",
                      cfg_.name,
                      multi.status);
    }
    for (const Instrument* inst : mine) {
      // An instrument this session cannot trade (read for its market data) is not asked about.
      if (!inst->enabled()) continue;
      // "Symbol Configuration": IP weight 5.
      const HttpReply sc = signed_get("/fapi/v1/symbolConfig", inst->symbol.view(), 5);
      std::vector<SymbolConfig> configs;
      const bool known =
          sc.ok() && decode_symbol_config(sc.body, configs).empty() && !configs.empty();
      if (cfg_.leverage > 0 && (!known || configs.front().leverage != cfg_.leverage)) {
        RestRequest rr;
        if (!BinanceUsdmOrderEncoder::encode_rest_leverage(signer_,
                                                           cfg_.recv_window_ms,
                                                           inst->symbol.view(),
                                                           cfg_.leverage,
                                                           venue_time_ms(),
                                                           rr))
          return fmt::format(
              "{}: {} leverage request could not be encoded", cfg_.name, inst->symbol.view());
        const HttpReply r = send(rr);
        if (!r.ok())
          return refuse(fmt::format("{}: setting {} leverage to {}x failed: {}",
                                    cfg_.name,
                                    inst->symbol.view(),
                                    cfg_.leverage,
                                    refused(r)));
        FASTMM_LOG_INFO("{}: {} leverage set to {}x{}",
                        cfg_.name,
                        inst->symbol.view(),
                        cfg_.leverage,
                        known ? fmt::format(" (was {}x)", configs.front().leverage) : "");
      } else if (known) {
        FASTMM_LOG_INFO("{}: {} leverage {}x, margin {} (account settings, not changed)",
                        cfg_.name,
                        inst->symbol.view(),
                        configs.front().leverage,
                        configs.front().margin_type);
      } else {
        FASTMM_LOG_INFO("{}: {} symbol configuration not available (HTTP {})",
                        cfg_.name,
                        inst->symbol.view(),
                        sc.status);
      }
    }
    // "Futures Account Balance V3": IP weight 5.
    const HttpReply bal = signed_get("/fapi/v3/balance", {}, 5);
    std::vector<BalanceRecord> balances;
    if (bal.ok() && decode_balance(bal.body, balances).empty()) {
      for (const Instrument* inst : mine) {
        Notional available{};
        bool found = false;
        for (const BalanceRecord& b : balances) {
          if (!iequals_symbol(b.asset, inst->quote.view())) continue;
          found = true;
          available = b.available;
          FASTMM_LOG_INFO(
              "{}: {} balance {}, available {}", cfg_.name, b.asset, b.balance, b.available);
        }
        if (!found || !available.is_positive())
          FASTMM_LOG_WARN("{}: no available {} margin balance: {} orders will be rejected",
                          cfg_.name,
                          inst->quote.view(),
                          inst->symbol.view());
      }
    } else {
      FASTMM_LOG_WARN("{}: balance not available (HTTP {})", cfg_.name, bal.status);
    }
  } catch (const std::exception& e) {
    FASTMM_LOG_WARN("{}: account checks failed: {}", cfg_.name, std::string_view(e.what()));
  }
  return {};
}

// ---- wiring ---------------------------------------------------------------------------------

void BinanceUsdmVenue::attach(const SymbolTable& symbols,
                              const InstrumentTable& instruments,
                              EventSink& md_sink,
                              EventSink& order_sink,
                              MsgRing* outbound) {
  symbols_ = &symbols;
  instruments_ = &instruments;
  md_sink_ = &md_sink;
  order_sink_ = &order_sink;
  outbound_ = outbound;
  md_feed_ = std::make_unique<BinanceUsdmMdFeed>(
      symbols,
      id_,
      md_sink,
      SnapshotRequester{&BinanceUsdmVenue::snapshot_requester, this},
      cfg_.min_snapshot_interval_ns);
  md_feed_->set_log_name(cfg_.name);
  user_parser_ = std::make_unique<BinanceUsdmUserParser>(symbols, instruments, id_);
  user_parser_->set_symbol_venue(md_venue_);
  encoder_ = std::make_unique<BinanceUsdmOrderEncoder>(signer_, symbols, cfg_.recv_window_ms);
  encoder_->set_post_only_rpi(cfg_.post_only_rpi);
  ws_api_decoder_ = std::make_unique<binance::BinanceWsApiDecoder>();
  reconcile_.attach(cfg_.name, id_, order_sink_, &instruments, md_venue_);
}

// A pool member subscribes the primary's instruments for its order path and nothing for market
// data: it has no books.
void BinanceUsdmVenue::subscribe(std::span<const InstrumentId> instruments) {
  for (InstrumentId id : instruments) {
    if (symbols_ == nullptr || symbols_->venue_of(id) != md_venue_) continue;
    if (std::find(subscribed_.begin(), subscribed_.end(), id) != subscribed_.end()) continue;
    subscribed_.push_back(id);
    if (pool_member()) continue;
    if (md_feed_ && md_feed_->add_instrument(id) && instruments_ != nullptr &&
        instruments_->get(id).asset_class == AssetClass::Perpetual)
      md_feed_->add_perpetual(id, funding_interval_of(id));
  }
  exec_replay_.set_streams(subscribed_.size());
  std::size_t tracked = 0;
  for (InstrumentId id : subscribed_) tracked = std::max<std::size_t>(tracked, id.value + 1U);
  activity_.track(tracked, now_ns());
  stats_.books_total = pool_member() ? 0 : static_cast<std::uint32_t>(subscribed_.size());
  depth_limit_ = cfg_.depth_limit > 0 ? cfg_.depth_limit : auto_depth_limit(subscribed_.size());
  if (rest_ != nullptr) rest_->set_max_queue(rest_queue_for(subscribed_.size()));
  if (connected_) {
    // The stream lists live in the URLs: reopen the market-data connections.
    md_conn_.close();
    trades_conn_.close();
    open_md();
    open_trades();
  }
}

void BinanceUsdmVenue::connect(net::Reactor& reactor) {
  if (connected_) return;
  if (md_feed_ == nullptr) throw std::logic_error("BinanceUsdmVenue::connect before attach");
  reactor_ = &reactor;
  connected_ = true;
  reconcile_.open(!cfg_.dry_run && signer_.usable());
  // Where the execution and funding replays start unless resume_executions() said otherwise: this
  // session can only have missed what happened after it connected.
  const bool replays = !cfg_.dry_run && signer_.usable();
  exec_replay_.set_streams(subscribed_.size());
  exec_replay_.start_at(venue_time_ms());
  // A restart's exact start: the trade after the last one the earlier session booked.
  for (const auto& [id, next] : resume_from_ids_)
    exec_replay_.set_cursor(subscribed_slot(id), next);
  resume_from_ids_.clear();
  for (auto& [id, ids] : resume_known_ids_)
    exec_replay_.set_known_ids(subscribed_slot(id), std::move(ids));
  resume_known_ids_.clear();
  exec_replay_.open(replays);
  funding_replay_.start_at(venue_time_ms());
  funding_replay_.open(replays);
  if (!cfg_.record_raw_dir.empty()) {
    raw_md_.open(cfg_.record_raw_dir, cfg_.name, "md");
    raw_trades_.open(cfg_.record_raw_dir, cfg_.name, "trades");
    raw_user_.open(cfg_.record_raw_dir, cfg_.name, "user");
    raw_order_.open(cfg_.record_raw_dir, cfg_.name, "order");
  }
  open_rest();
  request_server_time();
  if (!pool_member()) {  // a member reads no market data: the primary has the books
    open_md();
    open_trades();
  }
  if (!cfg_.dry_run) {
    if (cfg_.ws_order_api) open_order();
    if (signer_.usable()) request_listen_key();
  }
  std::weak_ptr<int> alive = alive_;
  housekeeping_timer_ = reactor.add_timer_after(kHousekeepingNs, [this, alive] {
    if (alive.expired()) return;
    housekeeping_timer_ = net::kInvalidTimer;
    on_timer(now_ns());
  });
  FASTMM_LOG_INFO("{}: connecting (dry_run={}, ws_orders={}, user_stream={}, pool_member={})",
                  cfg_.name,
                  cfg_.dry_run,
                  cfg_.ws_order_api,
                  !cfg_.dry_run && signer_.usable(),
                  pool_member());
}

void BinanceUsdmVenue::disconnect() {
  if (!connected_) return;
  // A requested shutdown cancels its own orders (Session::stop runs cancel_all()), so the
  // countdown has nothing left to protect: stop it, rather than leave a timer running against an
  // account nobody is quoting. On its own blocking connection, as cancel_all() does: queued on the
  // REST channel it could be dropped by the reset below.
  if (dms_.needs_stop()) stop_countdown_blocking();
  dms_.reset();
  connected_ = false;
  reconcile_.close();  // before the reset below: nothing it aborts asks again
  if (housekeeping_timer_ != net::kInvalidTimer && reactor_ != nullptr) {
    reactor_->cancel_timer(housekeeping_timer_);
    housekeeping_timer_ = net::kInvalidTimer;
  }
  md_conn_.close();
  trades_conn_.close();
  user_conn_.close();
  order_conn_.close();
  // Replies to requests aborted by the reset are ignored: a later connect() starts afresh.
  exec_replay_.close();
  funding_replay_.close();
  if (rest_) rest_->reset();
  listen_key_pending_ = false;
  time_request_pending_ = false;
  md_feed_->on_disconnected();
  raw_md_.flush();
  raw_trades_.flush();
  raw_user_.flush();
  raw_order_.flush();
  publish_status();
}

void BinanceUsdmVenue::open_rest() {
  rest_ = std::make_unique<RestChannel>(*reactor_, rest_channel_config(cfg_, subscribed_.size()));
}

void BinanceUsdmVenue::open_md() {
  const std::string url = stream_root() + md_feed_->public_target();
  md_conn_.open(*reactor_, ws_config(url, kMdDeadMs), md_handler_);
  md_conn_.connect();
}

void BinanceUsdmVenue::open_trades() {
  if (subscribed_.empty()) return;
  const std::string url = stream_root() + md_feed_->market_target();
  trades_conn_.open(*reactor_, ws_config(url, kQuietDeadMs), trades_handler_);
  trades_conn_.connect();
}

void BinanceUsdmVenue::open_user() {
  if (listen_key_.empty()) return;
  // "User Data Streams": wss://fstream.binance.com/private/ws/<listenKey>.
  const std::string url = private_root() + "/ws/" + listen_key_;
  user_conn_.open(*reactor_, ws_config(url, kQuietDeadMs), user_handler_);
  user_conn_.connect();
}

void BinanceUsdmVenue::open_order() {
  net::ConnectionConfig c = ws_config(cfg_.ws_api_url, kQuietDeadMs);
  // Ed25519 keys log on once (session.logon from on_connected_send_subscriptions) and send
  // unsigned requests after that; the channel goes Live on the logon reply.
  c.manual_subscribe = signer_.type() == binance::KeyType::Ed25519 && signer_.usable();
  order_conn_.open(*reactor_, c, order_handler_);
  order_conn_.connect();
}

void BinanceUsdmVenue::on_order_open() {
  if (signer_.type() == binance::KeyType::Ed25519 && signer_.usable()) send_logon();
}

void BinanceUsdmVenue::send_logon() {
  char buf[kMaxRequestBytes];
  const std::size_t n = encoder_->encode_ws_logon(kLogonId, venue_time_ms(), buf);
  if (n == 0 || !order_conn_.send_text(std::string_view(buf, n)))
    FASTMM_LOG_ERROR("{}: could not send session.logon", cfg_.name);
}

// ---- market data channels -----------------------------------------------------------------

void BinanceUsdmVenue::on_md_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.md = channel_state(s);
  if (s == net::ConnState::Backoff) ++stats_.reconnects;
  if (mapped == md_state_) return;
  const ConnState prev = md_state_;
  md_state_ = mapped;
  switch (mapped) {
    case ConnState::Live:
      report_channel_state(Channel::Md, ConnState::Live);
      break;
    case ConnState::Stale:
      // The engine clears the books on any non-Live state: resync once data flows again.
      report_channel_state(Channel::Md, ConnState::Stale);
      for (InstrumentId id : subscribed_) {
        if (UsdmDepthSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
      }
      break;
    case ConnState::Disconnected:
      if (prev != ConnState::Connecting) {
        md_feed_->on_disconnected();
        report_channel_state(Channel::Md, ConnState::Disconnected);
      }
      break;
    case ConnState::Connecting:
      if (prev == ConnState::Live || prev == ConnState::Stale) {
        md_feed_->on_disconnected();
        report_channel_state(Channel::Md, ConnState::Disconnected);
      }
      break;
    default:
      break;
  }
}

void BinanceUsdmVenue::on_md_open() {
  md_feed_->on_connected();  // start every syncer -> REST snapshots
}

// A new consumer (a strategy attached to fastmm-gateway) needs whole books: every syncer asks for
// a snapshot again, which the engine receives after a Resyncing state. Nothing to do before the
// channel is live: its first snapshots are on the way.
void BinanceUsdmVenue::resync_books() {
  if (md_feed_ == nullptr || md_state_ != ConnState::Live) return;
  for (InstrumentId id : subscribed_) {
    if (UsdmDepthSync* sync = md_feed_->sync(id)) sync->resync(SyncReason::Explicit, now_ns());
  }
}

// Trades and mark prices are informational for the engine: their connection state is logged, not
// reported, so a quiet aggTrade stream never clears the books. A mark that stops arriving goes
// stale in the engine's perp table (core/perp_book.hpp).
void BinanceUsdmVenue::on_trades_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  if (mapped == trades_state_ || mapped == ConnState::Stale) return;
  const ConnState prev = trades_state_;
  trades_state_ = mapped;
  if (mapped == ConnState::Live) {
    FASTMM_LOG_INFO("{}: trades channel -> Live", cfg_.name);
  } else if (mapped == ConnState::Disconnected && prev != ConnState::Connecting) {
    FASTMM_LOG_WARN("{}: trades channel -> Disconnected", cfg_.name);
  }
}

void BinanceUsdmVenue::on_md_text(std::string_view t, std::int64_t ts, Channel ch) {
  RawRecorder& raw = ch == Channel::Md ? raw_md_ : raw_trades_;
  if (raw.enabled()) raw.record(ts, t);
  const ParseStatus st = md_feed_->on_message(t, ts);
  ++stats_.md_messages;
  if (ch == Channel::Md) stats_.last_md_rx_ns = ts;
  if (st == ParseStatus::Malformed) {
    ++stats_.md_malformed;
    if (stats_.md_malformed <= 5 || stats_.md_malformed % 1000 == 0)
      FASTMM_LOG_WARN(
          "{}: malformed market-data frame ({} so far)", cfg_.name, stats_.md_malformed);
  } else if (st == ParseStatus::Overflow) {
    ++stats_.md_dropped;
  }
}

void BinanceUsdmVenue::request_snapshot(InstrumentId id) {
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
      fmt::format("/fapi/v1/depth?symbol={}&limit={}", symbols_->venue_symbol(id), depth_limit_);
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

void BinanceUsdmVenue::on_user_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.user = private_channel_state(s);
  if (mapped == user_state_) return;
  const ConnState prev = user_state_;
  user_state_ = mapped;
  if (mapped == ConnState::Live) {
    if (prev == ConnState::Stale) return;  // a quiet stream coming back is not news
    report_channel_state(Channel::User, ConnState::Live);
    // Open orders and positions may have changed while the stream was down. On the first connect,
    // the start-up sweep: orders a session that died left resting, which the engine cancels, and
    // nothing about this session's own (one sent before the stream was up may be in flight).
    if (user_was_live_) {
      reconcile_.request();
    } else {
      reconcile_.sweep();
    }
    user_was_live_ = true;
  } else if (mapped == ConnState::Disconnected && prev != ConnState::Connecting) {
    report_channel_state(Channel::User, ConnState::Disconnected);
  }
}

ClientOrderId BinanceUsdmVenue::current_id(ClientOrderId link) const noexcept {
  if (const ClientOrderId* cur = aliases_.find(link)) return *cur;
  return link;
}

void BinanceUsdmVenue::forget_order(ClientOrderId id) noexcept {
  if (const OrderShadow* s = shadows_.find(id)) {
    if (s->link_id.valid() && s->link_id != id) aliases_.erase(s->link_id);
  }
  shadows_.erase(id);
}

// The engine counts a modified order's fills from zero; the venue keeps counting.
Qty BinanceUsdmVenue::engine_cum(ClientOrderId id, Qty venue_cum) const noexcept {
  const OrderShadow* s = shadows_.find(id);
  return s == nullptr ? venue_cum : non_negative(venue_cum - s->base_cum);
}

void BinanceUsdmVenue::on_user_text(std::string_view t, std::int64_t ts) {
  if (raw_user_.enabled()) raw_user_.record(ts, t);
  const Cycles t0 = rdtscp();
  const UserDecodeResult r = user_parser_->decode(t, wall_now(), t0, scratch_);
  // A funding payment: booked from the income history, where it has an id, a moment later (every
  // symbol's event arrives at once, and one query covers them all).
  if (r.funding) funding_replay_.due_in(kFundingQueryDelayNs);
  if (r.multi_assets >= 0 && multi_assets_ != (r.multi_assets == 1)) {
    multi_assets_ = r.multi_assets == 1;
    FASTMM_LOG_INFO(
        "{}: switched to {} mode", cfg_.name, multi_assets_ ? "multi-assets" : "single-asset");
    reconcile_.request_balances();
  }
  // B[] has the wallet balance only: the account (available, margin) is asked for, at most once a
  // second however many updates arrive.
  if (r.balances) reconcile_.request_balances();
  if (r.listen_key_expired) {
    FASTMM_LOG_WARN("{}: listenKey expired; requesting a new one", cfg_.name);
    // Not from inside the connection's own callback (net contract): post the close.
    std::weak_ptr<int> alive = alive_;
    reactor_->post([this, alive] {
      if (alive.expired() || !connected_) return;
      listen_key_.clear();
      user_conn_.close();
      request_listen_key();
    });
    return;
  }
  if (r.status != ParseStatus::Ok) {
    if (r.status == ParseStatus::Malformed)
      FASTMM_LOG_WARN("{}: malformed user-stream frame", cfg_.name);
    return;
  }
  const std::int64_t now = r.count > 0 ? now_ns() : 0;
  std::uint32_t off = 0;
  for (std::uint32_t i = 0; i < r.count; ++i) {
    auto* h = reinterpret_cast<EventHeader*>(scratch_ + off);
    h->t1_delta = static_cast<std::uint32_t>(rdtscp() - t0);
    off += h->len;
    if (h->type != EventType::PositionUpdate) activity_.note(h->instrument, now);
    switch (h->type) {
      case EventType::PositionUpdate:
        on_account_position(*reinterpret_cast<const PositionUpdateMsg*>(h));
        continue;  // compared with the fills, not forwarded as is
      case EventType::OrderAck: {
        auto* m = reinterpret_cast<OrderAckMsg*>(h);
        m->cl_ord_id = current_id(m->cl_ord_id);
        break;
      }
      case EventType::OrderCancelAck: {
        auto* m = reinterpret_cast<OrderCancelAckMsg*>(h);
        m->cl_ord_id = current_id(m->cl_ord_id);
        m->cum_qty = engine_cum(m->cl_ord_id, m->cum_qty);
        forget_order(m->cl_ord_id);
        break;
      }
      case EventType::OrderExpired: {
        auto* m = reinterpret_cast<OrderExpiredMsg*>(h);
        m->cl_ord_id = current_id(m->cl_ord_id);
        m->cum_qty = engine_cum(m->cl_ord_id, m->cum_qty);
        forget_order(m->cl_ord_id);
        break;
      }
      case EventType::OrderFill: {
        auto* f = reinterpret_cast<OrderFillMsg*>(h);
        if (f->cl_ord_id.valid()) {
          f->cl_ord_id = current_id(f->cl_ord_id);
          if (const OrderShadow* s = shadows_.find(f->cl_ord_id)) {
            OrderShadow copy = *s;
            copy.cum = f->cum_qty;
            shadows_.assign(f->cl_ord_id, copy);
            f->cum_qty = non_negative(f->cum_qty - copy.base_cum);
          }
          if (f->leaves_qty.raw <= 0) forget_order(f->cl_ord_id);
        }
        if (f->hdr.instrument.value < kMaxInstruments) {
          PositionCheck& p = positions_[f->hdr.instrument.value];
          p.tracked = f->side == Side::Buy ? p.tracked + f->qty : p.tracked - f->qty;
          p.last_event_ns = now_ns();
        }
        break;
      }
      default:
        break;
    }
    sent_.answered(*h);
    static_cast<void>(order_sink_->push(*h));
    ++stats_.order_events;
  }
}

void BinanceUsdmVenue::on_account_position(const PositionUpdateMsg& m) {
  if (!cfg_.position_from_account_update || m.hdr.instrument.value >= kMaxInstruments) return;
  PositionCheck& p = positions_[m.hdr.instrument.value];
  p.venue = m.qty;
  p.venue_avg = m.avg_px;
  p.pending = true;
  p.last_event_ns = now_ns();
}

void BinanceUsdmVenue::check_positions(std::int64_t now) {
  if (order_sink_ == nullptr || reconcile_.busy()) return;
  const std::int64_t settle_ns = cfg_.position_settle_ms * kNsPerMs;
  for (InstrumentId id : subscribed_) {
    PositionCheck& p = positions_[id.value];
    if (!p.pending || now - p.last_event_ns < settle_ns) continue;
    p.pending = false;
    if (p.venue == p.tracked) continue;
    FASTMM_LOG_WARN(
        "{}: {} position {} from ACCOUNT_UPDATE differs from the fills ({}); "
        "correcting the engine",
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

// ---- order channel --------------------------------------------------------------------------

void BinanceUsdmVenue::on_order_state(net::ConnState s) {
  const ConnState mapped = map_conn_state(s);
  stats_.order = private_channel_state(s);
  if (mapped == order_state_) return;
  const ConnState prev = order_state_;
  order_state_ = mapped;
  // Requests in flight on a connection that is gone are never answered on it: they no longer
  // hold the snapshot watermark back (the reconnect's snapshot settles them).
  if (mapped != ConnState::Live && mapped != ConnState::Stale) sent_.connection_lost();
  if (mapped == ConnState::Live) {
    const bool reconnected = order_was_live_ && prev != ConnState::Stale;
    order_was_live_ = true;
    if (prev != ConnState::Stale) report_channel_state(Channel::Order, ConnState::Live);
    drain_outbound();
    if (reconnected && !cfg_.dry_run) request_open_orders();
    return;
  }
  if (mapped == ConnState::Stale) return;  // quiet order channels are normal
  if (prev == ConnState::Live || prev == ConnState::Stale) {
    session_logged_on_ = false;
    encoder_->set_session_authenticated(false);
    report_channel_state(Channel::Order, ConnState::Disconnected);
    // disconnect() clears connected_ first: a requested shutdown runs the blocking cancel_all().
    if (cfg_.cancel_on_order_channel_loss && !cfg_.dry_run && connected_) cancel_all_async();
  }
}

void BinanceUsdmVenue::on_order_text(std::string_view t, std::int64_t ts) {
  if (raw_order_.enabled()) raw_order_.record(ts, t);
  binance::WsApiResponse r;
  const ParseStatus st = ws_api_decoder_->decode(t, r);
  if (st == ParseStatus::Ok) {
    handle_ws_api_response(r);
  } else if (st == ParseStatus::Malformed) {
    FASTMM_LOG_WARN("{}: malformed WS API frame", cfg_.name);
  }
}

void BinanceUsdmVenue::handle_ws_api_response(const binance::WsApiResponse& r) {
  rate_.on_headers(r.rate.used_weight, r.rate.order_count, now_ns());
  budget_pub_.store(budget_of(rate_, now_ns(), orders_taken_));
  if (const auto req = parse_request_id(r.id)) {
    handle_order_response(req->first, req->second, r);
    return;
  }
  if (r.id == kLogonId) {
    if (!r.is_error) {
      session_logged_on_ = true;
      encoder_->set_session_authenticated(true);
      order_conn_.subscribe_done();
      FASTMM_LOG_INFO("{}: WS API session logged on", cfg_.name);
      return;
    }
    FASTMM_LOG_ERROR("{}: session.logon failed: {} {}", cfg_.name, r.code, r.msg);
    const ErrorMapping m = map_error(r.code, r.msg);
    const VenueAction action = m.action == VenueAction::None ? VenueAction::Fatal : m.action;
    apply_action(action, r.code, r.msg, r.retry_after_ms);
    // Transient (timestamp, rate limit, busy): log on again after the action had time to work.
    if (action != VenueAction::Fatal && action != VenueAction::HardStop && reactor_ != nullptr) {
      std::weak_ptr<int> alive = alive_;
      reactor_->add_timer_after(kLogonRetryNs, [this, alive] {
        if (alive.expired() || !connected_ || fatal_ || session_logged_on_) return;
        send_logon();
      });
    }
    return;
  }
  if (r.id.empty() && r.is_error && r.status == 401) {
    // Session revocation: the logged-on key became invalid (deleted, IP whitelist, permissions).
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

void BinanceUsdmVenue::handle_order_response(RequestKind kind,
                                             ClientOrderId id,
                                             const binance::WsApiResponse& r) {
  // The venue answered a placement: the order no longer holds the snapshot watermark back.
  if (kind != RequestKind::Cancel) sent_.answered(id);
  const OrderShadow* shadow = shadows_.find(id);
  const InstrumentId inst = shadow != nullptr ? shadow->instrument : instrument_of(r.symbol);
  if (r.status == 429 || r.status == 418) {
    const ErrorMapping hm = map_http_status(r.status);
    apply_action(hm.action, r.code, r.msg, r.retry_after_ms);
  }
  const auto venue_cum = [&]() {
    const auto q = parse_qty(r.executed_qty);
    return q ? *q : Qty{};
  };
  const IdText order_id(r.order_id);
  // The response's updateTime: the venue's time of the placement, cancel or modify.
  const Timestamp venue_ts = ts_from_ms(r.transact_time_ms);
  switch (kind) {
    case RequestKind::New:
      if (!r.is_error) {
        remember_order_id(r.order_id, id);
        if (cfg_.emit_ack_from_response) {
          emit_order_ack(*order_sink_, id_, inst, id, order_id.view(), 0, venue_ts);
          ++stats_.order_events;
        }
        return;
      }
      {
        const ErrorMapping m = map_error(r.code, r.msg);
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, r.code, r.msg);
        ++stats_.order_events;
        forget_order(id);
        apply_action(m.action, r.code, r.msg, r.retry_after_ms);
      }
      return;
    case RequestKind::Cancel:
      if (!r.is_error) {
        emit_cancel_ack(
            *order_sink_, id_, inst, id, order_id.view(), engine_cum(id, venue_cum()), venue_ts);
        ++stats_.order_events;
        forget_order(id);
        return;
      }
      {
        const ErrorMapping m = map_error(r.code, r.msg);
        emit_cancel_reject(*order_sink_, id_, inst, id, m.reason, r.code, r.msg);
        ++stats_.order_events;
        apply_action(m.action, r.code, r.msg, r.retry_after_ms);
      }
      return;
    case RequestKind::Replace: {
      const ClientOrderId orig = shadow != nullptr ? shadow->replaces : ClientOrderId{};
      if (r.is_error) {
        const ErrorMapping m = map_error(r.code, r.msg);
        emit_order_reject(*order_sink_, id_, inst, id, m.reason, r.code, r.msg);
        ++stats_.order_events;
        shadows_.erase(id);
        apply_action(m.action, r.code, r.msg, r.retry_after_ms);
        return;
      }
      if (r.order_status == "CANCELED" || r.order_status == "EXPIRED") {
        // "Modify Order": the venue cancels instead of modifying when the new quantity is not above
        // the executed quantity or a GTX order would take. The original is gone, the replacement
        // rejected.
        const bool post_only = shadow != nullptr && shadow->type == OrderType::PostOnly;
        if (orig.valid()) {
          emit_cancel_ack(*order_sink_,
                          id_,
                          inst,
                          orig,
                          order_id.view(),
                          engine_cum(orig, venue_cum()),
                          venue_ts);
          forget_order(orig);
        }
        emit_order_reject(*order_sink_,
                          id_,
                          inst,
                          id,
                          post_only ? RejectReason::PostOnlyWouldCross : RejectReason::VenueReject,
                          0,
                          "modify cancelled the order");
        stats_.order_events += 2;
        shadows_.erase(id);
        return;
      }
      if (shadow != nullptr) {
        OrderShadow copy = *shadow;
        copy.cum = venue_cum();
        copy.base_cum = copy.cum;
        copy.replaces = ClientOrderId{};
        shadows_.assign(id, copy);
        if (copy.link_id.valid() && copy.link_id != id) aliases_.assign(copy.link_id, id);
      }
      if (orig.valid() && orig != id) shadows_.erase(orig);
      remember_order_id(r.order_id, id);
      emit_order_ack(*order_sink_, id_, inst, id, order_id.view(), 0, venue_ts);
      ++stats_.order_events;
      return;
    }
    // No connector but Binance Spot sends an amend, so a response carrying that kind here is a
    // request id this venue never minted.
    case RequestKind::Amend:
    case RequestKind::Other:
      return;
  }
}

// ---- outbound -------------------------------------------------------------------------------

void BinanceUsdmVenue::on_wake() {
  drain_outbound();
}

// The orders the ring holds go out as one write of all their WebSocket frames.
template <class Ring>
void BinanceUsdmVenue::write_orders(Ring& ring) {
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

void BinanceUsdmVenue::fail_batch() {
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

void BinanceUsdmVenue::drain_outbound() {
  if (outbound_ != nullptr) write_orders(*outbound_);
}

void BinanceUsdmVenue::send_now(std::span<const EventHeader* const> batch) {
  OutboundBatch b(batch);
  write_orders(b);
}

void BinanceUsdmVenue::refuse(const OrderCommand& cmd, RejectReason reason, std::string_view text) {
  if (cmd.kind == OrderCommandKind::Cancel) {
    emit_cancel_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, text);
  } else {
    emit_order_reject(*order_sink_, id_, cmd.instrument, cmd.cl_ord_id, reason, 0, text);
  }
  ++stats_.order_events;
}

void BinanceUsdmVenue::refuse_untracked(const OrderCommand& cmd) {
  shadow_overflow_.refused(cfg_.name, shadows_.size());
  sent_.answered(cmd.cl_ord_id);  // the refusal is its answer: it holds no snapshot back
  refuse(cmd, RejectReason::OrderTableFull, "order table full");
}

void BinanceUsdmVenue::send_command(const OrderCommand& cmd) {
  note_taken(cmd);
  const std::int64_t now = now_ns();
  activity_.note(cmd.instrument, now);
  if (cfg_.dry_run) return refuse(cmd, RejectReason::VenueKilled, "dry-run: orders disabled");
  // A venue-fatal error and a REST hard stop stop new orders, never cancels: the kill path's
  // whole remedy is to cancel, so a cancel goes out on whatever transport is still usable.
  if (fatal_ && cmd.kind != OrderCommandKind::Cancel)
    return refuse(cmd, RejectReason::VenueKilled, "venue fatal");
  const OrderShadow* shadow = nullptr;  // New: unused; Cancel: the order; Replace: the original
  const bool is_order = cmd.kind != OrderCommandKind::Cancel;
  // The WS API charges what the REST endpoint charges. Placing and modifying cost nothing
  // against the IP weight budget and one against the 10 s / 1 min order limits; cancelling is
  // the other way round. Charging every quote 1 IP weight, as this used to, spends half the
  // 2400/minute budget on orders that cost none of it and throttles the quoter well below what
  // the venue allows.
  const std::uint32_t weight = cmd.kind == OrderCommandKind::Cancel ? 1U : 0U;
  switch (cmd.kind) {
    case OrderCommandKind::New:
      if (!rate_.can_send(weight, now, true)) {
        ++stats_.rate_limit_cooldowns;
        return refuse(cmd, RejectReason::VenueRateLimit, "local rate limit");
      }
      if (FASTMM_UNLIKELY(shadows_.assign(cmd.cl_ord_id,
                                          OrderShadow{cmd.instrument,
                                                      cmd.side,
                                                      cmd.type,
                                                      cmd.tif,
                                                      cmd.reduce_only,
                                                      cmd.cl_ord_id,
                                                      ClientOrderId{},
                                                      Qty{},
                                                      Qty{},
                                                      sent_.last_seq()}) == nullptr))
        return refuse_untracked(cmd);
      break;
    case OrderCommandKind::Cancel:
      shadow = shadows_.find(cmd.cl_ord_id);
      break;
    case OrderCommandKind::Replace: {
      const OrderShadow* orig = shadows_.find(cmd.orig_cl_ord_id);
      if (orig == nullptr)
        return refuse(cmd, RejectReason::UnknownOrder, "modify: original unknown");
      if (!rate_.can_send(weight, now, true)) {
        ++stats_.rate_limit_cooldowns;
        return refuse(cmd, RejectReason::VenueRateLimit, "local rate limit");
      }
      OrderShadow copy = *orig;
      copy.replaces = cmd.orig_cl_ord_id;
      copy.sent_seq = sent_.last_seq();
      if (FASTMM_UNLIKELY(shadows_.assign(cmd.cl_ord_id, copy) == nullptr))
        return refuse_untracked(cmd);
      shadow = shadows_.find(cmd.orig_cl_ord_id);
      break;
    }
  }
  if (cfg_.ws_order_api && order_conn_.is_live()) {
    const Cycles before_encode = rdtscp();
    const std::size_t n = encoder_->encode_ws(cmd, shadow, venue_time_ms(), request_buf_);
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
          break;
      }
      return;
    }
    ++stats_.order_send_failures;
  }
  send_command_rest(cmd, shadow);
}

void BinanceUsdmVenue::send_command_rest(const OrderCommand& cmd, const OrderShadow* shadow) {
  RestRequest rr;
  const Cycles before_encode = rdtscp();
  if (rest_ == nullptr || (rest_hard_stopped_ && cmd.kind != OrderCommandKind::Cancel) ||
      !encoder_->encode_rest(cmd, shadow, venue_time_ms(), rr)) {
    refuse(cmd, RejectReason::VenueReject, "no order channel");
    if (cmd.kind != OrderCommandKind::Cancel) shadows_.erase(cmd.cl_ord_id);
    ++stats_.order_send_failures;
    return;
  }
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  const std::string headers = api_headers();
  const Cycles after_encode = rdtscp();
  OrderCommand copy = cmd;
  copy.venue_order_id = nullptr;
  copy.header = nullptr;
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      rr.method, target, headers, {}, [this, alive, copy](const net::HttpResponse& r) {
        if (alive.expired()) return;
        handle_rest_order_response(copy, r);
      });
  if (!queued) {
    refuse(cmd, RejectReason::TransportFull, "rest queue full");
    if (cmd.kind != OrderCommandKind::Cancel) shadows_.erase(cmd.cl_ord_id);
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
      break;
  }
}

void BinanceUsdmVenue::handle_rest_order_response(const OrderCommand& cmd,
                                                  const net::HttpResponse& r) {
  ++stats_.rest_requests;
  note_rate_headers(r);
  const RequestKind kind = cmd.kind == OrderCommandKind::New      ? RequestKind::New
                           : cmd.kind == OrderCommandKind::Cancel ? RequestKind::Cancel
                                                                  : RequestKind::Replace;
  if (r.error != net::NetError::None) {
    ++stats_.rest_errors;
    // Send status unknown (timeout / closed): reconcile rather than guess.
    refuse(cmd, RejectReason::VenueReject, net::to_string(r.error));
    request_open_orders();
    return;
  }
  // The REST body becomes a WS API response so both paths share one handler.
  const RequestId rid = make_request_id(kind, cmd.cl_ord_id);
  const std::string wrapped = fmt::format(R"({{"id":"{}","status":{},"{}":{}}})",
                                          rid.view(),
                                          r.status,
                                          r.status >= 200 && r.status < 300 ? "result" : "error",
                                          r.body.empty() ? std::string("{}") : r.body);
  const PaddedJson padded(wrapped);
  binance::WsApiResponse resp;
  if (ws_api_decoder_->decode(padded.view(), resp) != ParseStatus::Ok) {
    refuse(cmd, RejectReason::VenueReject, "unparseable REST reply");
    return;
  }
  if (r.status >= 400 && !resp.is_error) resp.is_error = true;
  if (resp.is_error && resp.code == 0) {
    // No Binance error body (e.g. a 503 from a proxy): classify by the HTTP status.
    const ErrorMapping hm = map_http_status(r.status);
    if (hm.action == VenueAction::Reconcile) request_open_orders();
  }
  handle_order_response(kind, cmd.cl_ord_id, resp);
}

void BinanceUsdmVenue::note_rate_headers(const net::HttpResponse& r) {
  // "General Info" -> "IP Limits" / "Order Rate Limits": X-MBX-USED-WEIGHT-1M,
  // X-MBX-ORDER-COUNT-10S (and -1M).
  rate_.on_headers(
      header_int(r, "X-MBX-USED-WEIGHT-1M"), header_int(r, "X-MBX-ORDER-COUNT-10S"), now_ns());
  budget_pub_.store(budget_of(rate_, now_ns(), orders_taken_));
}

// First HardStop / Fatal error: the engine trips this venue's kill switch (quotes pulled,
// new orders refused by risk); the other venues keep trading.
void BinanceUsdmVenue::trip_venue_kill(KillReason reason) {
  if (trip_venue_kill_once(venue_kill_sent_, order_sink_, id_, reason))
    FASTMM_LOG_ERROR("{}: asking the engine to kill this venue ({})", cfg_.name, reason);
}

void BinanceUsdmVenue::apply_action(VenueAction action,
                                    int code,
                                    std::string_view msg,
                                    std::int64_t retry_after_ms) {
  switch (action) {
    case VenueAction::None:
      break;
    case VenueAction::Backoff:
      rate_.cooldown(kSecNs, now_ns());
      break;
    case VenueAction::RateLimit: {
      std::int64_t wait = kDefaultCooldownNs;
      if (retry_after_ms > 1'000'000'000'000LL) {  // absolute epoch ms
        wait = std::max<std::int64_t>(0, retry_after_ms - venue_time_ms()) * kNsPerMs;
      } else if (retry_after_ms > 0) {
        wait = retry_after_ms * kNsPerMs;
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

void BinanceUsdmVenue::report_channel_state(Channel ch, ConnState state, std::int32_t reason) {
  if (order_sink_ == nullptr) return;
  // Market-data state travels with the market data (ordering against deltas matters).
  EventSink& sink = ch == Channel::Md ? *md_sink_ : *order_sink_;
  venues::emit_connection_state(sink, id_, ch == Channel::Md ? 0 : 1, state, reason);
  FASTMM_LOG_INFO("{}: {} channel -> {}",
                  cfg_.name,
                  ch == Channel::Md     ? "md"
                  : ch == Channel::User ? "user"
                                        : "order",
                  state);
}

// ---- reconciliation -------------------------------------------------------------------------

void BinanceUsdmVenue::request_open_orders() {
  reconcile_.request();
}

// The executions first (request_executions), then the snapshot (ReconcileDriver).
bool BinanceUsdmVenue::replay_executions() {
  // The funding payments go alongside: they change no position, so the snapshot does not wait.
  static_cast<void>(funding_replay_.run());
  return exec_replay_.run();
}

bool BinanceUsdmVenue::fetch_snapshot(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  RestRequest oo;
  RestRequest pr;
  if (!encoder_->encode_rest_open_orders({}, venue_time_ms(), oo) ||
      !encoder_->encode_rest_position_risk({}, venue_time_ms(), pr))
    return false;
  snapshot_replies_ = 0;
  snapshot_failed_ = false;
  orders_body_.clear();
  positions_body_.clear();
  std::weak_ptr<int> alive = alive_;
  const std::string headers = api_headers();
  const bool q1 = rest_->request("GET",
                                 std::string(oo.path) + "?" + std::string(oo.query.view()),
                                 headers,
                                 {},
                                 [this, alive, generation](const net::HttpResponse& r) {
                                   if (!alive.expired()) on_reconcile_reply(generation, true, r);
                                 });
  const bool q2 = q1 && rest_->request("GET",
                                       std::string(pr.path) + "?" + std::string(pr.query.view()),
                                       headers,
                                       {},
                                       [this, alive, generation](const net::HttpResponse& r) {
                                         if (!alive.expired())
                                           on_reconcile_reply(generation, false, r);
                                       });
  if (q1) rate_.on_sent(oo.weight, now_ns());
  if (q2) rate_.on_sent(pr.weight, now_ns());
  // A queued request's reply is ignored once the driver's generation moves on.
  return q1 && q2;
}

void BinanceUsdmVenue::on_reconcile_reply(std::uint64_t generation,
                                          bool orders,
                                          const net::HttpResponse& r) {
  if (!reconcile_.current(generation)) return;
  ++stats_.rest_requests;
  note_rate_headers(r);
  ++snapshot_replies_;
  if (!r.ok()) {
    ++stats_.rest_errors;
    snapshot_failed_ = true;
    int code = 0;
    std::string msg;
    if (r.error == net::NetError::None && binance::decode_rest_error(r.body, code, msg)) {
      const ErrorMapping m = map_error(code, msg);
      if (m.action != VenueAction::Reconcile) apply_action(m.action, code, msg, -1);
    }
    FASTMM_LOG_WARN("{}: GET {} failed: status={} err={} {}",
                    cfg_.name,
                    orders ? "openOrders" : "positionRisk",
                    r.status,
                    net::to_string(r.error),
                    r.body.substr(0, 120));
  } else if (orders) {
    orders_body_ = r.body;
  } else {
    positions_body_ = r.body;
  }
  if (snapshot_replies_ < 2) return;
  reconcile_.fetched(generation, !snapshot_failed_ && snapshot_rows());
}

// The balance leg. The reply carries no server time (updateTime is per asset, when it last
// changed), so the snapshot is stamped with the venue clock when it arrived.
bool BinanceUsdmVenue::fetch_balances(std::uint64_t generation) {
  if (!connected_ || rest_ == nullptr || rest_hard_stopped_) return false;
  RestRequest rr;
  if (!encoder_->encode_rest_account(venue_time_ms(), rr)) return false;
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request("GET",
                                     std::string(rr.path) + "?" + std::string(rr.query.view()),
                                     api_headers(),
                                     {},
                                     [this, alive, generation](const net::HttpResponse& r) {
                                       if (!alive.expired()) on_account(generation, r);
                                     });
  if (queued) rate_.on_sent(rr.weight, now_ns());
  return queued;
}

void BinanceUsdmVenue::on_account(std::uint64_t generation, const net::HttpResponse& r) {
  if (!reconcile_.balances_current(generation)) return;
  ++stats_.rest_requests;
  note_rate_headers(r);
  FuturesAccount acct;
  std::string err;
  if (!r.ok()) {
    ++stats_.rest_errors;
    err = fmt::format("GET account failed: status={} err={} {}",
                      r.status,
                      net::to_string(r.error),
                      r.body.substr(0, 120));
  } else {
    err = decode_account(r.body, acct);
  }
  if (!err.empty()) {
    FASTMM_LOG_WARN("{}: {}", cfg_.name, err);
    reconcile_.balances_fetched(generation, false, 0);
    return;
  }
  // Available is the asset's availableBalance in single-asset mode. In multi-assets mode that
  // field is the account's available margin in USD on every asset, so an asset's row takes what
  // can leave it (maxWithdrawAmount) and the account row, from the totals, is the margin.
  const auto fields = [](const FuturesMargin& m, bool available) {
    BalanceFields f;
    f.free = available ? m.available : m.max_withdraw;
    f.locked = m.initial;
    f.total = m.wallet;
    f.equity = m.margin;
    f.maintenance = m.maintenance;
    return f;
  };
  for (const FuturesAssetMargin& a : acct.assets)
    reconcile_.add_balance(a.asset, fields(a.m, !multi_assets_));
  // The totals are the USDT row again in single-asset mode.
  if (multi_assets_) reconcile_.add_balance("USD", fields(acct.total, true), BalanceMsg::kAccount);
  reconcile_.balances_fetched(generation, true, venue_time_ms());
}

// Decodes the whole snapshot before anything reaches the engine: Oms::reconcile_end() cancels
// every order the snapshot does not name, so a reply that did not parse must not be emitted as an
// empty snapshot.
bool BinanceUsdmVenue::snapshot_rows() {
  std::vector<PositionRecord> records;
  if (const std::string err = decode_position_risk(positions_body_, records); !err.empty()) {
    FASTMM_LOG_WARN("{}: {}", cfg_.name, err);
    return false;
  }
  const PaddedJson padded(orders_body_);
  const ParseStatus st = ws_api_decoder_->decode_open_orders(
      padded.view(), /*rest_array=*/true, [&](const binance::OpenOrderRecord& o) {
        const InstrumentId inst = instrument_of(o.symbol);
        if (!inst.valid()) return;  // another symbol on this account
        ReconcileMsg& m = reconcile_.add_order(inst);
        m.side = o.side == "SELL" ? Side::Sell : Side::Buy;
        m.state = o.status == "PARTIALLY_FILLED" ? OrderState::PartiallyFilled : OrderState::Live;
        Qty base{};
        if (const auto link = decode_cl_ord_id(o.client_order_id)) {
          m.cl_ord_id = current_id(*link);
          if (const OrderShadow* s = shadows_.find(m.cl_ord_id)) base = s->base_cum;
          remember_order_id(o.order_id, m.cl_ord_id);
        }
        m.venue_order_id.assign(IdText(o.order_id).view());
        if (const auto p = parse_price(o.price)) m.price = *p;
        if (const auto q = parse_qty(o.orig_qty)) m.orig_qty = non_negative(*q - base);
        if (const auto q = parse_qty(o.executed_qty)) m.cum_qty = non_negative(*q - base);
      });
  if (st != ParseStatus::Ok) {
    FASTMM_LOG_WARN("{}: open orders reply could not be parsed", cfg_.name);
    return false;
  }
  std::string summary;
  for (InstrumentId id : subscribed_) {
    const std::string_view sym = symbols_->venue_symbol(id);
    Qty qty{};
    Price avg{};
    // positionRisk v3 lists only symbols with a position or open orders: absent means flat.
    for (const PositionRecord& p : records) {
      if (!iequals_symbol(p.symbol, sym)) continue;
      if (p.position_side != "BOTH") {
        if (!p.qty.is_zero())
          FASTMM_LOG_ERROR("{}: {} {} position {} (hedge mode is not supported)",
                           cfg_.name,
                           sym,
                           p.position_side,
                           p.qty);
        continue;
      }
      qty = p.qty;
      avg = p.entry_price;
    }
    reconcile_.add_position(id, qty, avg);
    PositionCheck& pc = positions_[id.value];
    pc.tracked = qty;
    pc.venue = qty;
    pc.venue_avg = avg;
    pc.pending = false;
    summary += fmt::format(" {}={}", sym, DecimalText(qty).view());
  }
  FASTMM_LOG_INFO("{}: positions{}", cfg_.name, summary);
  return true;
}

void BinanceUsdmVenue::shadow_ids(std::vector<SentShadow>& out) {
  shadows_.for_each(
      [&](ClientOrderId id, const OrderShadow& s) { out.push_back(SentShadow{id, s.sent_seq}); });
}

void BinanceUsdmVenue::drop_shadow(ClientOrderId id) {
  forget_order(id);
}

// ---- execution replay -------------------------------------------------------------------------
//
// GET /fapi/v1/userTrades per subscribed symbol, before every open-order snapshot and once a minute
// (ReplayScheduler), as Binance Spot does with myTrades: each execution is emitted as an ordinary
// fill carrying the venue's trade id (the `t` the user stream reports for the same execution), so
// the OMS keeps the ones it never saw and drops the rest. A fill that finished an order during a
// user-stream outage is recoverable only this way: the snapshot no longer names the order.
//
// Where the replay starts: the trade id after the last one read for that symbol (`fromId`, which
// the venue will not take with a time range), otherwise the time watermark (`startTime`), in
// windows of 7 days and never before the 3 months the venue keeps.

std::size_t BinanceUsdmVenue::subscribed_slot(InstrumentId id) const noexcept {
  for (std::size_t i = 0; i < subscribed_.size(); ++i)
    if (subscribed_[i] == id) return i;
  return subscribed_.size();
}

void BinanceUsdmVenue::remember_order_id(std::int64_t order_id, ClientOrderId id) noexcept {
  if (order_id <= 0 || !id.valid()) return;
  // Full: the oldest pairing makes room, being the one least likely to be named by a replay.
  static_cast<void>(order_ids_.assign(static_cast<std::uint64_t>(order_id), id));
}

void BinanceUsdmVenue::resume_executions(std::int64_t since_venue_ms,
                                         const std::vector<std::string>& known) {
  exec_replay_.resume(since_venue_ms, known);
  funding_replay_.resume(since_venue_ms, known);
}

void BinanceUsdmVenue::resume_trade_ids(
    const std::vector<std::pair<InstrumentId, std::int64_t>>& next_ids) {
  resume_from_ids_ = next_ids;
}

void BinanceUsdmVenue::resume_known_trade_ids(
    const std::vector<std::pair<InstrumentId, std::vector<std::int64_t>>>& known) {
  resume_known_ids_ = known;
}

bool BinanceUsdmVenue::request_executions(std::int64_t since_venue_ms) {
  // An explicit start overrides the per-symbol watermarks: the caller knows of executions this
  // connector never heard about.
  if (since_venue_ms > 0 && !exec_replay_.active()) exec_replay_.restart_from(since_venue_ms);
  if (since_venue_ms > 0 && !funding_replay_.active()) funding_replay_.restart_from(since_venue_ms);
  static_cast<void>(funding_replay_.run());
  return exec_replay_.run();
}

bool BinanceUsdmVenue::replay_ready() const noexcept {
  return !cfg_.dry_run && connected_ && signer_.usable() && rest_ != nullptr &&
         !rest_hard_stopped_ && !subscribed_.empty();
}

void BinanceUsdmVenue::replay_query_failed(std::string_view what,
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
  } else if (r.error == net::NetError::None && binance::decode_rest_error(r.body, code, msg)) {
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

bool BinanceUsdmVenue::query_executions(const ReplayQuery& q) {
  if (rest_ == nullptr || rest_hard_stopped_ || q.stream >= subscribed_.size()) return false;
  const std::string_view symbol =
      symbols_ == nullptr ? std::string_view{} : symbols_->venue_symbol(subscribed_[q.stream]);
  RestRequest rr;
  if (symbol.empty() ||
      !encoder_->encode_rest_user_trades(
          symbol, q.from_id, q.start_ms, q.end_ms, kUserTradesLimit, venue_time_ms(), rr))
    return false;
  // Not while the venue asked for a pause or the bulk share of the weight is spent: the replay
  // asks again (Hooks::can_query keeps most queries from getting this far).
  if (!rate_.can_send(rr.weight, now_ns(), false, RateLimiter::kBulkShare)) return false;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, q](const net::HttpResponse& r) {
        if (alive.expired() || !exec_replay_.expects(q)) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.execution_query_errors;
          replay_query_failed("userTrades", r);
          exec_replay_.failed(q);
          return;
        }
        // The rows own their text: a window waits for its lookups past this reply.
        const PaddedJson padded(r.body);
        ReplayPage<binance::MyTradeRow> page;
        const ParseStatus st = ws_api_decoder_->decode_user_trades(
            padded.view(), [&](const binance::MyTradeRecord& t) {
              page.rows.push_back(
                  {t.time_ms, t.id, std::string(IdText(t.id).view()), binance::MyTradeRow::of(t)});
            });
        if (st != ParseStatus::Ok) {
          ++stats_.execution_query_errors;
          FASTMM_LOG_ERROR("{}: userTrades reply could not be parsed; asked again", cfg_.name);
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

bool BinanceUsdmVenue::emit_execution(std::size_t stream, const binance::MyTradeRow& t) {
  if (stream >= subscribed_.size() || instruments_ == nullptr) return false;
  const InstrumentId id = subscribed_[stream];
  if (!instruments_->contains(id)) return false;
  const ClientOrderId* mapped = order_ids_.find(static_cast<std::uint64_t>(t.order_id));
  const ClientOrderId cl = mapped != nullptr ? current_id(*mapped) : ClientOrderId{};
  if (!binance::emit_trade_history_fill(
          *order_sink_,
          id_,
          instruments_->get(id),
          id,
          cl,
          t.view(),
          exec_replay_.emitting_unresolved() ? OrderFillMsg::kUnresolved : 0))
    return false;
  ++stats_.order_events;
  ++stats_.executions_fetched;
  return true;
}

// As Spot's: userTrades names an order by orderId only; one order_ids_ does not hold (placed by a
// session that ended before the answer, or before this process started) is asked for, so that its
// fill names its client order id. Bounded and retried by the execution replay (ReplayScheduler).
bool BinanceUsdmVenue::lookup_order(const ReplayLookup& l) {
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
        remember_order_id(oid, *cl);
        exec_replay_.looked_up(l, LookupResult::Named);
      });
  if (!queued) return false;
  rate_.on_sent(rr.weight, now_ns());
  return true;
}

// ---- funding ----------------------------------------------------------------------------------
//
// GET /fapi/v1/income?incomeType=FUNDING_FEE, every symbol of the account in one query, from its
// watermark in windows of at most 7 days and never before the 3 months the venue keeps (weight
// 30). Rows on a subscribed symbol become FundingMsg with the tranId as id; the engine books one
// under kFundingIdPrefix + tranId, which is also what a restart's store names.

bool BinanceUsdmVenue::query_funding(const ReplayQuery& q) {
  if (rest_ == nullptr || rest_hard_stopped_) return false;
  RestRequest rr;
  if (!encoder_->encode_rest_funding_income(
          q.start_ms, q.end_ms, kUserTradesLimit, venue_time_ms(), rr))
    return false;
  if (!rate_.can_send(rr.weight, now_ns(), false, RateLimiter::kBulkShare)) return false;
  const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "GET", target, api_headers(), {}, [this, alive, q](const net::HttpResponse& r) {
        if (alive.expired() || !funding_replay_.expects(q)) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          replay_query_failed("income", r);
          funding_replay_.failed(q);
          return;
        }
        std::vector<IncomeRecord> rows;
        if (const std::string err = decode_income(r.body, rows); !err.empty()) {
          FASTMM_LOG_ERROR("{}: {}; funding is asked again", cfg_.name, err);
          funding_replay_.failed(q);
          return;
        }
        std::stable_sort(
            rows.begin(), rows.end(), [](const IncomeRecord& a, const IncomeRecord& b) {
              return a.time_ms < b.time_ms || (a.time_ms == b.time_ms && a.tran_id < b.tran_id);
            });
        ReplayPage<IncomeRecord> page;
        page.rows.reserve(rows.size());
        for (IncomeRecord& row : rows) {
          const std::int64_t t = row.time_ms;
          std::string key = std::string(kFundingIdPrefix) + std::string(IdText(row.tran_id).view());
          page.rows.push_back({t, 0, std::move(key), std::move(row)});
        }
        funding_replay_.answer(q, std::move(page));
      });
  if (!queued) {
    FASTMM_LOG_ERROR("{}: no room to ask for the account's funding", cfg_.name);
    return false;
  }
  rate_.on_sent(rr.weight, now_ns());
  return true;
}

bool BinanceUsdmVenue::emit_funding_row(const IncomeRecord& row) {
  if (row.income_type != "FUNDING_FEE") return false;
  const InstrumentId inst = instrument_of(row.symbol);
  if (!inst.valid() || subscribed_slot(inst) >= subscribed_.size()) return false;  // not ours
  const IdText id(row.tran_id);
  emit_funding(*order_sink_, id_, inst, id.view(), row.income, row.asset, row.time_ms, true);
  ++stats_.order_events;
  ++stats_.funding_fetched;
  return true;
}

// ---- control requests -----------------------------------------------------------------------

void BinanceUsdmVenue::request_server_time() {
  if (rest_ == nullptr || time_request_pending_ || rest_hard_stopped_) return;
  time_request_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const bool queued =
      rest_->request("GET", "/fapi/v1/time", {}, {}, [this, alive](const net::HttpResponse& r) {
        if (alive.expired()) return;
        time_request_pending_ = false;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (!r.ok()) {
          ++stats_.rest_errors;
          return;
        }
        std::int64_t server_ms = 0;
        if (!binance::decode_server_time(r.body, server_ms).empty()) return;
        clock_offset_ms_ = server_ms - wall_now().ns / kNsPerMs;
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
  rate_.on_sent(1, now_ns());  // "Check Server Time": IP weight 1
}

void BinanceUsdmVenue::request_listen_key() {
  // "Start User Data Stream": POST /fapi/v1/listenKey, IP weight 1; returns the active key if one
  // exists and extends it for 60 minutes.
  if (rest_ == nullptr || !signer_.usable() || listen_key_pending_ || rest_hard_stopped_) return;
  listen_key_pending_ = true;
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "POST", "/fapi/v1/listenKey", api_headers(), {}, [this, alive](const net::HttpResponse& r) {
        if (alive.expired()) return;
        listen_key_pending_ = false;
        ++stats_.rest_requests;
        note_rate_headers(r);
        std::string key;
        if (!r.ok() || !binance::decode_listen_key(r.body, key).empty()) {
          ++stats_.rest_errors;
          int code = 0;
          std::string msg;
          if (r.error == net::NetError::None && binance::decode_rest_error(r.body, code, msg)) {
            const ErrorMapping m = map_error(code, msg);
            apply_action(m.action, code, msg, -1);
          }
          FASTMM_LOG_WARN("{}: listenKey request failed (status {} {}); retrying",
                          cfg_.name,
                          r.status,
                          r.body.substr(0, 120));
          listen_key_retry_ns_ = now_ns() + kListenKeyRetryNs;
          return;
        }
        listen_key_refresh_ns_ = now_ns();
        if (key == listen_key_ && user_conn_.opened()) return;
        listen_key_ = std::move(key);
        user_conn_.close();
        open_user();
      });
  if (!queued) {
    listen_key_pending_ = false;
    listen_key_retry_ns_ = now_ns() + kListenKeyRetryNs;
    return;
  }
  rate_.on_sent(1, now_ns());
}

void BinanceUsdmVenue::keepalive_listen_key() {
  // "Keepalive User Data Stream": PUT /fapi/v1/listenKey, IP weight 1; -1125 means the key is gone.
  if (rest_ == nullptr || listen_key_.empty() || rest_hard_stopped_) return;
  listen_key_refresh_ns_ = now_ns();  // one attempt per interval
  std::weak_ptr<int> alive = alive_;
  const bool queued = rest_->request(
      "PUT", "/fapi/v1/listenKey", api_headers(), {}, [this, alive](const net::HttpResponse& r) {
        if (alive.expired()) return;
        ++stats_.rest_requests;
        note_rate_headers(r);
        if (r.ok()) return;
        ++stats_.rest_errors;
        FASTMM_LOG_WARN("{}: listenKey keepalive failed (status {} {}); requesting a new key",
                        cfg_.name,
                        r.status,
                        r.body.substr(0, 120));
        listen_key_.clear();
        user_conn_.close();
        request_listen_key();
      });
  if (queued) rate_.on_sent(1, now_ns());
}

// countdownCancelAll with countdownTime=0 for every subscribed symbol, synchronously.
void BinanceUsdmVenue::stop_countdown_blocking() {
  if (!signer_.usable() || subscribed_.empty() || symbols_ == nullptr) return;
  BlockingControl control(cfg_);
  static_cast<void>(control.per_target(
      "stopping countdownCancelAll",
      std::span<const InstrumentId>(subscribed_),
      [&](InstrumentId id) { return symbols_->venue_symbol(id); },
      [&](InstrumentId id, BlockingRequest& q) {
        const std::string_view symbol = symbols_->venue_symbol(id);
        RestRequest rr;
        if (symbol.empty() ||
            !encoder_->encode_rest_countdown_cancel_all(symbol, 0, venue_time_ms(), rr))
          return false;
        q.method = rr.method;
        q.target = std::string(rr.path) + "?" + std::string(rr.query.view());
        q.headers = api_headers();
        return true;
      },
      [](const HttpReply& reply, std::string&) { return reply.ok(); }));
}

// POST /fapi/v1/countdownCancelAll, one request per subscribed symbol: the countdown is per
// symbol, and sending it again replaces the running one. The round counts only when every symbol
// confirmed it (CountdownDriver): a symbol whose refresh fails keeps its old countdown, which
// runs out. A request that does not go out is retried from the next housekeeping tick; if this
// process is what broke, the countdown runs out and the venue cancels, which is the whole point.
void BinanceUsdmVenue::send_countdown_cancel_all(std::int64_t countdown_ms) {
  if (rest_ == nullptr || !signer_.usable() || subscribed_.empty() || symbols_ == nullptr) return;
  const std::uint32_t round =
      dms_.begin_round(now_ns(), static_cast<std::uint32_t>(subscribed_.size()));
  bool any = false;
  for (InstrumentId id : subscribed_) {
    const std::string_view symbol = symbols_->venue_symbol(id);
    if (symbol.empty()) continue;
    RestRequest rr;
    if (!encoder_->encode_rest_countdown_cancel_all(symbol, countdown_ms, venue_time_ms(), rr))
      continue;
    const std::string target = std::string(rr.path) + "?" + std::string(rr.query.view());
    std::weak_ptr<int> alive = alive_;
    const bool queued = rest_->request(
        rr.method, target, api_headers(), {}, [this, alive, id, round](const net::HttpResponse& r) {
          if (alive.expired() || r.error == net::NetError::Canceled) return;
          ++stats_.rest_requests;
          note_rate_headers(r);
          if (r.ok()) {
            dms_.confirmed(round);
            return;
          }
          ++stats_.rest_errors;
          // Not fatal by itself: the switch is a backstop, and a venue that refuses the request
          // outright (no permission, symbol unknown) must not stop the session dead. It is loud,
          // and if the switch was up and the window then lapses, on_timer() kills the venue.
          FASTMM_LOG_ERROR("{}: countdownCancelAll for {} failed: status={} err={} body={}",
                           cfg_.name,
                           symbols_->venue_symbol(id),
                           r.status,
                           net::to_string(r.error),
                           r.body.substr(0, 160));
        });
    if (!queued) continue;
    any = true;
    rate_.on_sent(rr.weight, now_ns());
  }
  if (any) dms_.went_out();
}

void BinanceUsdmVenue::cancel_all_async() {
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
          if (!r.ok()) {
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

bool BinanceUsdmVenue::cancel_all() {
  if (cfg_.dry_run || !signer_.usable() || symbols_ == nullptr) return true;
  BlockingControl control(cfg_);
  BinanceUsdmOrderEncoder enc(signer_, *symbols_, cfg_.recv_window_ms);  // this thread's copy
  enc.set_post_only_rpi(cfg_.post_only_rpi);
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
      // DELETE allOpenOrders answers 200 with nothing open too.
      [](const HttpReply& reply, std::string&) { return reply.ok(); });
}

// ---- housekeeping ---------------------------------------------------------------------------

void BinanceUsdmVenue::on_timer(std::int64_t now) {
  if (!connected_) return;
  md_feed_->on_timer(now);
  if (!cfg_.dry_run && signer_.usable()) {
    if (listen_key_.empty()) {
      if (now >= listen_key_retry_ns_) request_listen_key();
    } else if (now - listen_key_refresh_ns_ >= kListenKeyKeepaliveNs) {
      keepalive_listen_key();
    }
    reconcile_.on_timer(now);
    // The replays: a retry 5 s after an incomplete one (the next reconnect may be hours away), one
    // a minute while all is well (the OMS deduplicates a bounded window, and the private stream can
    // drop an event without disconnecting), and funding a second after a user-stream event.
    exec_replay_.on_timer(now);
    funding_replay_.on_timer(now);
    // Venue-side dead man's switch. Refreshed from the housekeeping timer, which is the same
    // thread that would stop running if this process died, so there is nothing to keep the
    // countdown alive when the process is gone.
    //
    // If the window ran out anyway, the venue has cancelled everything of ours and this process
    // is still running: it must not quietly put the quotes back. Kill the venue and let an
    // operator look, exactly as a venue-fatal error does (CountdownDriver).
    switch (dms_.poll(now)) {
      case CountdownDriver::Step::Lapsed:
        FASTMM_LOG_ERROR(
            "{}: countdownCancelAll not refreshed within {} ms; the venue has "
            "cancelled this account's orders",
            cfg_.name,
            dms_.window_ms());
        fatal_ = true;
        trip_venue_kill(KillReason::DeadMansSwitchLost);
        break;
      case CountdownDriver::Step::Refresh:
        send_countdown_cancel_all(dms_.window_ms());
        break;
      case CountdownDriver::Step::None:
        break;
    }
    check_positions(now);
  }
  if (clock_resync_wanted_ || now - clock_sync_ns_ >= kClockResyncNs) request_server_time();
  shadow_overflow_.check(cfg_.name, shadows_.size(), decltype(shadows_)::kMaxSize);
  publish_status();
  raw_md_.flush();
  raw_trades_.flush();
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

void BinanceUsdmVenue::publish_status() noexcept {
  stats_.shadows = shadows_.size();
  stats_.shadows_refused = shadow_overflow_.count();
  stats_.shadows_swept = reconcile_.shadows_swept();
  stats_.execution_queries = exec_replay_.replays();
  stats_.books_synced = md_feed_ ? md_feed_->synced_count() : 0;
  stats_.resyncs = md_feed_ ? md_feed_->resync_count() : 0;
  stats_.md_dropped = md_feed_ ? md_feed_->stats().dropped : 0;
  stats_.rate_limit_cooldowns = rate_.cooldowns();
  stats_.clock_offset_ms = clock_offset_ms_;
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

VenueStatus BinanceUsdmVenue::status() const noexcept {
  return load_published_status(published_);
}

// ---- config ---------------------------------------------------------------------------------

BinanceUsdmVenueConfig make_binance_usdm_config(const VenueSection& v, bool dry_run) {
  BinanceUsdmVenueConfig c;
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
    c.credentials.type = binance::KeyType::Ed25519;
    c.credentials.private_key_pem.value = binance::load_ed25519_pem(
        v.name, extra("private_key_file"), extra("private_key_env"), dry_run);
  }
  c.ws_private_url = extra("ws_private_url");
  if (extra("order_api") == "rest") c.ws_order_api = false;
  if (const std::string d = extra("depth_limit"); !d.empty()) {
    if (const auto n = parse_int64(d)) c.depth_limit = valid_depth_limit(*n);
  }
  c.stale_ms = static_cast<std::uint32_t>(x.integer("stale_ms", c.stale_ms));
  c.dead_ms = static_cast<std::uint32_t>(x.integer("dead_ms", c.dead_ms));
  check_liveness(v.name, x.integer("stale_ms", c.stale_ms), x.integer("dead_ms", c.dead_ms));
  c.position_from_account_update = x.flag("position_from_account_update", true);
  c.allow_offline_reference_data = x.flag("allow_offline_reference_data", false);
  c.cancel_on_order_channel_loss = x.flag("cancel_on_order_channel_loss", true);
  c.dead_mans_switch_ms =
      std::max<std::int64_t>(0, x.integer("dead_mans_switch_ms", c.dead_mans_switch_ms));
  c.post_only_rpi = x.flag("post_only_rpi", false);
  c.emit_ack_from_response = x.flag("emit_ack_from_response", true);
  c.one_way_mode = x.flag("one_way_mode", false);
  const std::int64_t leverage = x.integer("leverage", 0);
  if (leverage < 0 || leverage > 125)
    throw std::invalid_argument("venue '" + v.name + "': leverage must be 0 to 125");
  c.leverage = static_cast<int>(leverage);
  if (c.ws_url.empty() || c.rest_url.empty())
    throw std::invalid_argument("venue '" + v.name + "': binance_usdm needs ws_url and rest_url");
  static_cast<void>(url_root(c.ws_url));  // throws on a bad URL
  return c;
}

}  // namespace fastmm::venues::binance_usdm
