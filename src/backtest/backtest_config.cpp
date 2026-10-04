#include "fastmm/backtest/backtest_config.hpp"

#include "fastmm/backtest/fill_model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::bt {

namespace {
std::int64_t non_negative(const GenericSection& s, std::string_view key, std::int64_t def) {
  const std::int64_t v = s.get_int(key, def);
  if (v < 0) throw ConfigError("backtest." + std::string(key) + " must be >= 0");
  return v;
}
double drop_probability(const GenericSection& s, std::string_view key, double def) {
  const double p = s.get_double(key, def);
  if (!(p >= 0.0 && p < 1.0))
    throw ConfigError("backtest." + std::string(key) + " must be in [0, 1)");
  return p;
}

// The latency keys of [backtest] and of [backtest.venues.<name>]: `prefix` is "" or
// "venues.<name>.", and a missing key keeps the value `v` has. The venue -> engine path follows
// the order path unless latency_ack_us / latency_ack_jitter_us are set, here or (for a venue) in
// [backtest]: a real venue's replies are often slower than its intake (Binance Spot from AWS
// Tokyo: ~0.4 ms to transactTime, ~1.1 ms more to the ack).
void read_latency(const GenericSection& bt, const std::string& prefix, sim::SimVenueConfig& v) {
  const std::int64_t fixed_def = v.order_out.fixed.ns / 1000;
  const std::int64_t jitter_def = v.order_out.jitter.ns / 1000;
  const Duration fixed = microseconds(non_negative(bt, prefix + "latency_fixed_us", fixed_def));
  const Duration jitter = microseconds(non_negative(bt, prefix + "latency_jitter_us", jitter_def));
  const double p_drop = drop_probability(bt, prefix + "p_drop", v.order_out.p_drop);
  v.order_out = sim::LatencyParams{fixed, jitter, p_drop};
  const auto ack = [&](std::string_view key, Duration order_path) {
    const std::string own = prefix + std::string(key);
    if (bt.has(own)) return microseconds(non_negative(bt, own, 0));
    if (!prefix.empty() && bt.has(key)) return microseconds(non_negative(bt, key, 0));
    return order_path;
  };
  v.ack_in =
      sim::LatencyParams{ack("latency_ack_us", fixed), ack("latency_ack_jitter_us", jitter), 0.0};
  v.md_in = sim::LatencyParams{
      microseconds(non_negative(bt, prefix + "latency_md_us", v.md_in.fixed.ns / 1000)),
      microseconds(non_negative(bt, prefix + "latency_md_jitter_us", v.md_in.jitter.ns / 1000)),
      0.0};
  const std::string arrival_key = prefix + "md_arrival";
  if (bt.has(arrival_key)) {
    const std::string a = bt.get_string(arrival_key, "venue");
    if (a != "recorded" && a != "venue")
      throw ConfigError("backtest." + arrival_key + ": '" + a + "' is not venue or recorded");
    v.md_recorded_arrival = a == "recorded";
  }
}

// Keys of [backtest.venues.<name>] (docs/reference/configuration.md#backtest).
constexpr std::array<std::string_view, 12> kVenueKeys = {"latency_fixed_us",
                                                         "latency_jitter_us",
                                                         "latency_ack_us",
                                                         "latency_ack_jitter_us",
                                                         "latency_md_us",
                                                         "latency_md_jitter_us",
                                                         "md_arrival",
                                                         "p_drop",
                                                         "supports_replace",
                                                         "stp",
                                                         "orders_10s",
                                                         "orders_1d"};
constexpr std::string_view kVenuesPrefix = "venues.";
constexpr std::string_view kBalancesPrefix = "balances.";

// One `<ASSET> = "<amount>"` of [backtest.balances] or [backtest.venues.<name>.balances].
sim::SimBalance read_balance(const GenericSection& bt,
                             const std::string& key,
                             std::string_view asset) {
  const auto line = bt.lines.find(key);
  const std::string where =
      line == bt.lines.end() ? std::string() : fmt::format(" (line {})", line->second);
  if (asset.empty() || asset.size() > 8 || asset.find('.') != std::string_view::npos) {
    throw ConfigError(
        fmt::format("backtest.{}: expected an asset name of 1 to 8 characters{}", key, where));
  }
  const std::string text = bt.get_string(key, "");
  const auto amount = Notional::from_decimal(text);
  if (!amount || amount->raw < 0) {
    throw ConfigError(fmt::format(
        "backtest.{}: '{}' is not a decimal amount >= 0 (write it as a string: \"0.5\"){}",
        key,
        text,
        where));
  }
  sim::SimBalance b;
  b.asset = FixedString<8>(asset);
  b.amount = *amount;
  return b;
}

// An instrument trades on the venue, or on the primary of the pool it is a member of.
bool traded_on(const Config& cfg, const std::string& name) {
  const VenueSection* v = cfg.venue(name);
  const std::string& primary = v != nullptr && !v->pool_of.empty() ? v->pool_of : name;
  return std::any_of(cfg.instruments.begin(),
                     cfg.instruments.end(),
                     [&](const InstrumentSection& i) { return i.venue == primary; });
}

// [backtest.venues.<name>]: one venue's own latency, cancel-replace and STP, over the [backtest]
// values. A name that is not in [venues] or a key that is not one of kVenueKeys is an error.
std::vector<sim::SimVenueConfig> read_venues(const Config& cfg,
                                             const GenericSection& bt,
                                             const sim::SimVenueConfig& defaults) {
  std::vector<std::string> names;
  for (const auto& [key, value] : bt.values) {
    if (!key.starts_with(kVenuesPrefix)) continue;
    const std::string rest = key.substr(kVenuesPrefix.size());
    const std::size_t dot = rest.find('.');
    const auto line = bt.lines.find(key);
    const std::string where =
        line == bt.lines.end() ? std::string() : fmt::format(" (line {})", line->second);
    if (dot == std::string::npos || dot == 0) {
      throw ConfigError(
          fmt::format("backtest.{}: expected [backtest.venues.<name>] with keys{}", key, where));
    }
    const std::string name = rest.substr(0, dot);
    const std::string field = rest.substr(dot + 1);
    if (!cfg.venue_id(name).valid()) {
      std::string known;
      for (const VenueSection& v : cfg.venues)
        known.append(known.empty() ? "" : ", ").append(v.name);
      throw ConfigError(fmt::format("backtest.venues.{0}: no venue '{0}' in [venues] ({1}){2}",
                                    name,
                                    known.empty() ? "none configured" : "known: " + known,
                                    where));
    }
    if (!traded_on(cfg, name)) {
      throw ConfigError(fmt::format(
          "backtest.venues.{0}: no [[instruments]] trade on venue '{0}'{1}", name, where));
    }
    if (field.starts_with(kBalancesPrefix)) {
      static_cast<void>(
          read_balance(bt, key, std::string_view(field).substr(kBalancesPrefix.size())));
    } else if (std::find(kVenueKeys.begin(), kVenueKeys.end(), field) == kVenueKeys.end()) {
      throw ConfigError(
          fmt::format("backtest.{}: unknown key (latency_fixed_us, latency_jitter_us, "
                      "latency_ack_us, latency_ack_jitter_us, latency_md_us, "
                      "latency_md_jitter_us, md_arrival, p_drop, supports_replace, stp, "
                      "orders_10s, orders_1d, balances.<ASSET>){}",
                      key,
                      where));
    }
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
  std::vector<sim::SimVenueConfig> out;
  for (const std::string& name : names) {
    const std::string prefix = std::string(kVenuesPrefix) + name + ".";
    sim::SimVenueConfig v = defaults;
    v.venue = cfg.venue_id(name);
    read_latency(bt, prefix, v);
    v.supports_replace = bt.get_bool(prefix + "supports_replace", defaults.supports_replace);
    if (bt.has(prefix + "stp")) {
      v.stp = bt.get_bool(prefix + "stp", false) ? sim::StpMode::CancelTaker : sim::StpMode::None;
    }
    v.orders_10s = non_negative(bt, prefix + "orders_10s", defaults.orders_10s);
    v.orders_1d = non_negative(bt, prefix + "orders_1d", defaults.orders_1d);
    out.push_back(v);
  }
  std::sort(out.begin(), out.end(), [](const sim::SimVenueConfig& a, const sim::SimVenueConfig& b) {
    return a.venue < b.venue;
  });
  return out;
}
// The strategy's account per simulated venue: [backtest.venues.<name>.balances] for that venue,
// else [backtest.balances] for every venue an instrument trades on. Neither: no accounts.
std::vector<sim::SimAccountConfig> read_accounts(const Config& cfg, const GenericSection& bt) {
  std::vector<sim::SimBalance> defaults;
  for (const auto& [key, value] : bt.values) {
    if (key.starts_with(kBalancesPrefix)) {
      defaults.push_back(
          read_balance(bt, key, std::string_view(key).substr(kBalancesPrefix.size())));
    }
  }
  std::vector<sim::SimAccountConfig> out;
  for (const VenueSection& venue : cfg.venues) {
    const std::string& name = venue.name;
    if (!traded_on(cfg, name)) continue;
    const std::string prefix =
        std::string(kVenuesPrefix) + name + "." + std::string(kBalancesPrefix);
    sim::SimAccountConfig a;
    a.venue = cfg.venue_id(name);
    bool own = false;
    for (const auto& [key, value] : bt.values) {
      if (!key.starts_with(prefix)) continue;
      own = true;
      a.balances.push_back(read_balance(bt, key, std::string_view(key).substr(prefix.size())));
    }
    if (!own) {
      if (defaults.empty()) continue;
      a.balances = defaults;
    }
    out.push_back(std::move(a));
  }
  return out;
}

std::int64_t positive(const GenericSection& s, std::string_view key, std::int64_t def) {
  const std::int64_t v = s.get_int(key, def);
  if (v <= 0) throw ConfigError(std::string(key) + " must be > 0");
  return v;
}

// "1,10,60" or "[1, 10, 60]" (a TOML array arrives stringified) -> markout horizons. An empty
// string turns markouts off; a value that is not a positive number of seconds is an error.
std::vector<Duration> parse_horizons(const std::string& text) {
  std::vector<Duration> out;
  std::string_view s = text;
  while (!s.empty() && (s.front() == '[' || s.front() == ' ')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ']' || s.back() == ' ')) s.remove_suffix(1);
  std::size_t pos = 0;
  while (pos <= s.size() && !s.empty()) {
    const std::size_t comma = s.find(',', pos);
    std::string item(s.substr(pos, comma == std::string_view::npos ? comma : comma - pos));
    const auto first = item.find_first_not_of(" \t");
    const auto last = item.find_last_not_of(" \t");
    item = first == std::string::npos ? std::string() : item.substr(first, last - first + 1);
    if (!item.empty()) {
      char* end = nullptr;
      const double v = std::strtod(item.c_str(), &end);
      if (end == item.c_str() || *end != '\0' || !(v > 0.0) || v > 86'400.0) {
        throw ConfigError("backtest.markout_horizons_s: '" + item +
                          "' is not a number of seconds in (0, 86400]");
      }
      out.push_back(Duration{std::llround(v * 1e9)});
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  std::sort(out.begin(), out.end(), [](Duration a, Duration b) { return a.ns < b.ns; });
  out.erase(
      std::unique(out.begin(), out.end(), [](Duration a, Duration b) { return a.ns == b.ns; }),
      out.end());
  return out;
}

// Every [backtest] key from_config reads (docs/reference/configuration.md#backtest).
constexpr std::array<std::string_view, 24> kBacktestKeys = {"markout_horizons_s",
                                                            "orders_10s",
                                                            "orders_1d",
                                                            "source",
                                                            "path",
                                                            "seed",
                                                            "output_dir",
                                                            "journal_out",
                                                            "equity_bar_s",
                                                            "duration_s",
                                                            "initial_capital",
                                                            "fill_model",
                                                            "queue_conservatism",
                                                            "p_drop",
                                                            "latency_fixed_us",
                                                            "latency_jitter_us",
                                                            "latency_ack_us",
                                                            "latency_ack_jitter_us",
                                                            "latency_md_us",
                                                            "latency_md_jitter_us",
                                                            "md_arrival",
                                                            "balances_from_journal",
                                                            "own_orders_in_feed",
                                                            "reorder_window_ms"};

void check_backtest_keys(const GenericSection& bt) {
  for (const auto& [key, value] : bt.values) {
    if (std::find(kBacktestKeys.begin(), kBacktestKeys.end(), key) != kBacktestKeys.end()) continue;
    if (key.starts_with(kVenuesPrefix)) continue;    // read_venues() checks those
    if (key.starts_with(kBalancesPrefix)) continue;  // read_accounts() checks those
    const auto line = bt.lines.find(key);
    throw ConfigError(
        "unknown key 'backtest." + key + "'", line == bt.lines.end() ? 0 : line->second, 0);
  }
}
}  // namespace

BacktestConfig BacktestConfig::from_config(const Config& cfg) {
  BacktestConfig b;
  b.engine.rng_seed = cfg.engine.rng_seed;
  b.engine.max_events_per_step = cfg.engine.max_events_per_step;
  b.engine.crossed_grace = milliseconds(cfg.engine.crossed_grace_ms);
  b.engine.max_param_age = milliseconds(cfg.strategy.max_param_age_ms);
  b.engine.state_file = cfg.strategy.state_file;
  b.engine.state_interval = seconds(cfg.strategy.state_interval_s);
  // A replay reruns the engine's own timers from the journal; they must find the settings the
  // recorded session ran with (the ack sweep's timeout, the flatten's period and deadline).
  b.engine.ack_timeout = milliseconds(cfg.engine.ack_timeout_ms);
  b.engine.flatten_interval = milliseconds(cfg.engine.flatten_interval_ms);
  b.engine.flatten_timeout = milliseconds(cfg.engine.flatten_timeout_ms);
  b.engine.flatten_slippage_bps = cfg.engine.flatten_slippage_bps;
  b.engine.latency_publish_interval = milliseconds(cfg.engine.latency_publish_ms);
  b.engine.spin_mode = SpinMode::Busy;
  b.engine.risk = cfg.risk_limits();
  b.engine.quotes = cfg.quote_params();
  b.instruments = load_instruments(cfg);
  if (b.instruments.size() == 0) throw ConfigError("backtest: no [[instruments]] configured");
  b.accounting = cfg.accounting;
  for (const VenueSection& v : cfg.venues) b.venue_names.push_back(v.name);
  b.strategy = cfg.strategy.name;
  b.params = cfg.strategy.params;
  b.config_toml = cfg.effective_toml();

  const GenericSection& bt = cfg.backtest;
  const GenericSection& sm = cfg.sim;
  b.warnings = cfg.warnings;
  check_backtest_keys(bt);
  b.engine.fx = b.fx_plan(b.instruments);
  b.underlying = cfg.risk.underlying;
  b.engine.underlying = b.underlying_plan(b.instruments);
  b.set_seed(static_cast<std::uint64_t>(bt.get_int("seed", sm.get_int("seed", 1))));
  b.source = bt.get_string("source", "");
  b.path = bt.get_string("path", "");
  b.output_dir = bt.get_string("output_dir", "runs/backtest");
  b.journal_out = bt.get_string("journal_out", "");
  b.equity_bar = seconds(positive(bt, "equity_bar_s", 1));
  b.duration = seconds(positive(bt, "duration_s", sm.get_int("duration_s", 60)));
  b.initial_capital = bt.get_double("initial_capital", 0.0);
  if (bt.has("markout_horizons_s"))
    b.markout_horizons = parse_horizons(bt.get_string("markout_horizons_s", ""));
  b.generator_seed_levels = static_cast<int>(positive(sm, "seed_levels", 20));

  sim::SimTransportConfig& t = b.transport;
  const auto fm = parse_fill_model(bt.get_string("fill_model", "matching"));
  if (!fm) throw ConfigError("backtest.fill_model must be matching | l2_queue");
  t.fill_model = *fm;
  // One value for the fill model and the strategy's estimate of it (ctx.queue_ahead).
  const double c = bt.get_double("queue_conservatism", cfg.engine.queue_conservatism);
  if (!(c >= 0.0 && c <= 1.0)) throw ConfigError("backtest.queue_conservatism must be in [0, 1]");
  t.queue_conservatism_bps = static_cast<std::int64_t>(std::llround(c * 10'000.0));
  sim::SimVenueConfig defaults;  // [backtest] latency_*, [engine] supports_replace, [risk] stp
  defaults.order_out = sim::LatencyParams{microseconds(200), microseconds(50), 0.0};
  defaults.md_in = sim::LatencyParams{};
  read_latency(bt, "", defaults);
  t.order_out = defaults.order_out;
  t.ack_in = defaults.ack_in;
  t.md_in = defaults.md_in;
  t.md_recorded_arrival = defaults.md_recorded_arrival;
  t.supports_replace = cfg.engine.supports_replace;
  t.stp = cfg.risk.stp ? sim::StpMode::CancelTaker : sim::StpMode::None;
  defaults.supports_replace = t.supports_replace;
  defaults.stp = t.stp;
  t.orders_10s = non_negative(bt, "orders_10s", 0);
  t.orders_1d = non_negative(bt, "orders_1d", 0);
  defaults.orders_10s = t.orders_10s;
  defaults.orders_1d = t.orders_1d;
  t.venues = read_venues(cfg, bt, defaults);
  t.accounts = read_accounts(cfg, bt);
  t.pools = cfg.pool_plan();
  b.engine.pools = t.pools;
  t.own_orders_in_feed = bt.get_bool("own_orders_in_feed", true);
  b.balances_from_journal = bt.get_bool("balances_from_journal", false);
  const std::int64_t window = bt.get_int("reorder_window_ms", 1000);
  if (window < 0) throw ConfigError("backtest.reorder_window_ms must be >= 0");
  b.reorder_window = milliseconds(window);
  t.md.interval = milliseconds(positive(sm, "depth_update_ms", 100));
  t.md.book_ticker = sm.get_bool("book_ticker", true);
  t.venue = VenueId{0};
  // Each instrument pays its own venue's schedule; [[instruments]] maker_bps / taker_bps
  // override one instrument. The default covers instruments added outside the config. The engine
  // reports the same table to the strategy (the runner copies transport.fees at the start).
  t.fees = fee_table(cfg);
  b.engine.fees = t.fees;
  b.engine.balance = balance_config(cfg);
  t.initial_margin.assign(
      b.engine.balance.initial_margin.begin(),
      b.engine.balance.initial_margin.begin() + static_cast<std::ptrdiff_t>(b.instruments.size()));
  b.engine.perp = perp_config(cfg);

  sim::MarketGeneratorParams& g = b.generator;
  const Instrument& inst = b.instruments.get(InstrumentId{0});
  g.tick = inst.tick;
  g.lot = inst.lot;
  if (const std::string s = sm.get_string("start_mid", ""); !s.empty()) {
    const auto p = Price::from_decimal(s);
    if (!p || !p->is_positive())
      throw ConfigError("sim.start_mid: '" + s + "' is not a positive decimal");
    g.start_mid = *p;
  }
  g.limit_rate_per_s = sm.get_double("limit_rate_per_s", g.limit_rate_per_s);
  g.market_rate_per_s = sm.get_double("market_rate_per_s", g.market_rate_per_s);
  g.mid_step_rate_per_s = sm.get_double("mid_step_rate_per_s", g.mid_step_rate_per_s);
  g.cancel_rate_per_order_s = sm.get_double("cancel_rate_per_order_s", g.cancel_rate_per_order_s);
  g.offset_p = sm.get_double("offset_p", g.offset_p);
  g.base_spread_ticks = static_cast<int>(sm.get_int("base_spread_ticks", g.base_spread_ticks));
  g.limit_qty_median_lots = sm.get_double("limit_qty_median_lots", g.limit_qty_median_lots);
  g.market_qty_median_lots = sm.get_double("market_qty_median_lots", g.market_qty_median_lots);
  g.regimes = sm.get_bool("regimes", g.regimes);
  g.volatile_mult = sm.get_double("volatile_mult", g.volatile_mult);
  if (!(g.offset_p > 0.0 && g.offset_p <= 1.0)) throw ConfigError("sim.offset_p must be in (0, 1]");
  if (g.base_spread_ticks < 1) throw ConfigError("sim.base_spread_ticks must be >= 1");
  return b;
}

FxPlan BacktestConfig::fx_plan(const InstrumentTable& table) {
  std::string warning;
  auto plan =
      session_fx_plan(table, accounting, venue_names, false, engine.risk.reads_totals(), &warning);
  if (!plan) throw ConfigError(plan.error());
  if (!warning.empty()) warnings.push_back(warning);
  return *plan;
}

UnderlyingPlan BacktestConfig::underlying_plan(const InstrumentTable& table) const {
  auto plan = build_underlying_plan(table, underlying, "risk");
  if (!plan) throw ConfigError(plan.error());
  return *plan;
}

BacktestConfig BacktestConfig::single_instrument(std::string_view symbol, Price tick, Qty lot) {
  BacktestConfig b;
  Instrument i{};
  i.symbol = symbol;
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = tick;
  i.lot = lot;
  i.min_qty = lot;
  if (!b.instruments.add(i)) throw ConfigError("single_instrument: invalid tick/lot/symbol");
  b.generator.tick = tick;
  b.generator.lot = lot;
  b.engine.quotes.min_requote_interval = milliseconds(50);
  return b;
}

}  // namespace fastmm::bt
