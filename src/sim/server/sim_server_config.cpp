#include "fastmm/sim/server/sim_server_config.hpp"

#include "fastmm/config/config.hpp"

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace fastmm::sim::server {

namespace {

template <class Tag>
Fixed<Tag> parse_fixed(std::string_view text, std::string_view what) {
  const auto v = Fixed<Tag>::from_decimal(text);
  if (!v)
    throw std::invalid_argument("sim config: bad decimal for " + std::string(what) + ": '" +
                                std::string(text) + "'");
  return *v;
}

std::uint32_t ms_from_seconds(const GenericSection& s, std::string_view key, std::uint32_t def) {
  if (!s.has(key)) return def;
  const double v = s.get_double(key, 0.0);
  if (v < 0.0) throw std::invalid_argument("sim config: " + std::string(key) + " must be >= 0");
  return static_cast<std::uint32_t>(std::llround(v * 1000.0));
}

std::uint32_t get_u32(const GenericSection& s, std::string_view key, std::uint32_t def) {
  const std::int64_t v = s.get_int(key, static_cast<std::int64_t>(def));
  if (v < 0 || v > 0xFFFF'FFFFLL)
    throw std::invalid_argument("sim config: " + std::string(key) + " out of range");
  return static_cast<std::uint32_t>(v);
}

// "${VAR}" -> getenv(VAR) (empty when unset); anything else verbatim.
std::string resolve_env(std::string value) {
  if (value.size() > 3 && value.starts_with("${") && value.back() == '}') {
    const std::string name = value.substr(2, value.size() - 3);
    const char* env = std::getenv(name.c_str());
    return env == nullptr ? std::string{} : std::string(env);
  }
  return value;
}

}  // namespace

SimServerConfig SimServerConfig::defaults() {
  SimServerConfig c;
  SimSymbolConfig s;
  s.symbol = "BTCUSDT";
  s.base_asset = "BTC";
  s.quote_asset = "USDT";
  s.tick = Price::from_raw(1'000'000);  // 0.01
  s.lot = Qty::from_raw(1'000);         // 0.00001
  s.min_qty = s.lot;
  s.max_qty = Qty::from_int(100);
  s.min_notional = Notional::from_int(5);
  s.start_mid = Price::from_int(60'000);
  c.symbols.push_back(s);
  // The generator's touch sits base_spread_ticks from its latent mid, so BasicMM's 5 bps quotes
  // (30 USDT = 3000 ticks at 60000) rest a few ticks behind the generator's best levels and
  // are reached by the larger market orders.
  MarketGeneratorParams& g = c.generator;
  g.limit_rate_per_s = 100.0;
  g.cancel_rate_per_order_s = 0.5;
  g.market_rate_per_s = 4.0;
  g.mid_step_rate_per_s = 2.0;
  g.offset_p = 0.01;
  g.base_spread_ticks = 2990;
  g.limit_qty_median_lots = 200.0;
  g.limit_qty_sigma = 0.8;
  g.market_qty_median_lots = 1000.0;
  g.market_qty_sigma = 1.0;
  g.regimes = true;
  g.regime_switch_rate_per_s = 0.02;
  g.volatile_mult = 4.0;
  g.max_resting = 4000;
  return c;
}

SimServerConfig SimServerConfig::from_config(const Config& cfg) {
  SimServerConfig c = defaults();
  const GenericSection& s = cfg.sim;
  c.symbols.clear();
  const std::string venue = s.get_string("venue", "");
  const Price default_mid = parse_fixed<PriceTag>(s.get_string("start_mid", "60000"), "start_mid");
  for (const InstrumentSection& inst : cfg.instruments) {
    if (!inst.enabled) continue;
    if (!venue.empty() && inst.venue != venue) continue;
    SimSymbolConfig sym;
    sym.symbol = inst.symbol;
    sym.base_asset = inst.base;
    sym.quote_asset = inst.quote;
    if (sym.base_asset.empty() || sym.quote_asset.empty())
      throw std::invalid_argument("sim config: instrument " + inst.symbol + " needs base/quote");
    sym.tick = parse_fixed<PriceTag>(inst.tick, inst.symbol + ".tick");
    sym.lot = parse_fixed<QtyTag>(inst.lot, inst.symbol + ".lot");
    sym.min_qty = inst.min_qty.empty()
                      ? sym.lot
                      : parse_fixed<QtyTag>(inst.min_qty, inst.symbol + ".min_qty");
    sym.max_qty =
        inst.max_qty.empty() ? Qty{} : parse_fixed<QtyTag>(inst.max_qty, inst.symbol + ".max_qty");
    sym.min_notional =
        inst.min_notional.empty()
            ? Notional{}
            : parse_fixed<NotionalTag>(inst.min_notional, inst.symbol + ".min_notional");
    const std::string mid_key = "symbols." + inst.symbol + ".start_mid";
    sym.start_mid =
        s.has(mid_key) ? parse_fixed<PriceTag>(s.get_string(mid_key), mid_key) : default_mid;
    const std::string max_notional_key = "symbols." + inst.symbol + ".max_notional";
    if (s.has(max_notional_key))
      sym.max_notional = parse_fixed<NotionalTag>(s.get_string(max_notional_key), max_notional_key);
    if (!sym.tick.is_positive() || !sym.lot.is_positive() || !sym.start_mid.is_positive())
      throw std::invalid_argument("sim config: " + inst.symbol + " needs tick, lot, start_mid > 0");
    c.symbols.push_back(std::move(sym));
  }
  if (c.symbols.empty()) throw std::invalid_argument("sim config: no [[instruments]] to simulate");

  static_cast<void>(net::parse_reactor_backend(cfg.engine.net_backend, c.net_backend));
  c.bind_host = s.get_string("bind", c.bind_host);
  c.port = static_cast<int>(s.get_int("port", c.port));
  c.tls_port = static_cast<int>(s.get_int("tls_port", c.tls_port));
  c.tls_cert = s.get_string("tls_cert", c.tls_cert);
  c.tls_key = s.get_string("tls_key", c.tls_key);
  c.seed = static_cast<std::uint64_t>(s.get_int("seed", static_cast<std::int64_t>(c.seed)));
  c.api_key = resolve_env(s.get_string("account.api_key", c.api_key));
  c.api_secret = resolve_env(s.get_string("account.api_secret", c.api_secret));
  if (const char* k = std::getenv("FASTMM_SIM_API_KEY"); k != nullptr && *k != '\0') c.api_key = k;
  if (const char* k = std::getenv("FASTMM_SIM_API_SECRET"); k != nullptr && *k != '\0')
    c.api_secret = k;
  if (c.api_key.empty() || c.api_secret.empty())
    throw std::invalid_argument("sim config: [sim.account] api_key/api_secret are empty");
  for (const auto& [key, value] : s.values) {
    static constexpr std::string_view kPrefix = "account.balances.";
    if (!key.starts_with(kPrefix)) continue;
    c.balances.push_back(
        SimBalanceConfig{key.substr(kPrefix.size()), parse_fixed<QtyTag>(value, key)});
  }

  c.maker_bps = s.get_double("fees.maker_bps", c.maker_bps);
  c.taker_bps = s.get_double("fees.taker_bps", c.taker_bps);
  c.depth_update_ms = get_u32(s, "depth_update_ms", c.depth_update_ms);
  if (const std::string mode = s.get_string("depth_snapshot", "live"); mode == "flushed") {
    c.snapshot_at_flush = true;
  } else if (mode != "live") {
    throw std::invalid_argument("sim config: depth_snapshot must be \"live\" or \"flushed\"");
  }
  c.driver_tick_ms = get_u32(s, "driver_tick_ms", c.driver_tick_ms);
  c.start_time_ms = s.get_int("start_time_ms", c.start_time_ms);
  c.clock_offset_ms = s.get_int("clock_offset_ms", c.clock_offset_ms);
  c.weight_limit_per_minute = get_u32(s, "limits.weight_per_minute", c.weight_limit_per_minute);
  c.orders_limit_per_10s = get_u32(s, "limits.orders_per_10s", c.orders_limit_per_10s);
  c.orders_limit_per_day = get_u32(s, "limits.orders_per_day", c.orders_limit_per_day);
  c.max_recv_window_ms = get_u32(s, "limits.max_recv_window_ms", c.max_recv_window_ms);
  c.ping_interval_ms = get_u32(s, "ping_interval_ms", c.ping_interval_ms);
  c.pong_timeout_ms = get_u32(s, "pong_timeout_ms", c.pong_timeout_ms);
  c.listen_key_validity_ms = get_u32(s, "listen_key_validity_ms", c.listen_key_validity_ms);
  if (c.depth_update_ms == 0 || c.driver_tick_ms == 0)
    throw std::invalid_argument("sim config: depth_update_ms and driver_tick_ms must be > 0");

  MarketGeneratorParams& g = c.generator;
  c.generator_enabled = s.get_bool("generator.enabled", c.generator_enabled);
  c.seed_levels = static_cast<int>(s.get_int("generator.seed_levels", c.seed_levels));
  g.limit_rate_per_s = s.get_double("generator.limit_rate_per_s", g.limit_rate_per_s);
  g.cancel_rate_per_order_s =
      s.get_double("generator.cancel_rate_per_order_s", g.cancel_rate_per_order_s);
  g.market_rate_per_s = s.get_double("generator.market_rate_per_s", g.market_rate_per_s);
  g.mid_step_rate_per_s = s.get_double("generator.mid_step_rate_per_s", g.mid_step_rate_per_s);
  g.offset_p = s.get_double("generator.offset_p", g.offset_p);
  g.base_spread_ticks =
      static_cast<int>(s.get_int("generator.base_spread_ticks", g.base_spread_ticks));
  g.limit_qty_median_lots =
      s.get_double("generator.limit_qty_median_lots", g.limit_qty_median_lots);
  g.limit_qty_sigma = s.get_double("generator.limit_qty_sigma", g.limit_qty_sigma);
  g.market_qty_median_lots =
      s.get_double("generator.market_qty_median_lots", g.market_qty_median_lots);
  g.market_qty_sigma = s.get_double("generator.market_qty_sigma", g.market_qty_sigma);
  g.regimes = s.get_bool("generator.regimes", g.regimes);
  g.regime_switch_rate_per_s =
      s.get_double("generator.regime_switch_rate_per_s", g.regime_switch_rate_per_s);
  g.volatile_mult = s.get_double("generator.volatile_mult", g.volatile_mult);
  g.max_resting = static_cast<std::size_t>(
      s.get_int("generator.max_resting", static_cast<std::int64_t>(g.max_resting)));
  if (g.offset_p <= 0.0 || g.offset_p > 1.0)
    throw std::invalid_argument("sim config: generator.offset_p must be in (0, 1]");

  FaultConfig& f = c.faults;
  f.drop_md_after_ms = ms_from_seconds(s, "faults.drop_md_after_s", f.drop_md_after_ms);
  f.drop_ws_api_after_ms = ms_from_seconds(s, "faults.drop_ws_api_after_s", f.drop_ws_api_after_ms);
  f.skip_depth_update_after_ms =
      ms_from_seconds(s, "faults.skip_depth_update_after_s", f.skip_depth_update_after_ms);
  f.timestamp_error_after_ms =
      ms_from_seconds(s, "faults.timestamp_error_after_s", f.timestamp_error_after_ms);
  f.rest_unresponsive_after_ms =
      ms_from_seconds(s, "faults.rest_unresponsive_after_s", f.rest_unresponsive_after_ms);
  f.rest_unresponsive_for_ms =
      ms_from_seconds(s, "faults.rest_unresponsive_for_s", f.rest_unresponsive_for_ms);
  f.delay_ack_ms = get_u32(s, "faults.delay_ack_ms", f.delay_ack_ms);
  f.reject_next_orders = get_u32(s, "faults.reject_next_orders", f.reject_next_orders);
  f.rate_limit_next = get_u32(s, "faults.rate_limit_next", f.rate_limit_next);
  return c;
}

SimServerConfig SimServerConfig::load(const std::string& path) {
  Config::LoadOptions opts;
  opts.substitute_env = false;
  return from_config(Config::load(path, opts));
}

}  // namespace fastmm::sim::server
