#include "fastmm/backtest/backtest_config.hpp"

#include "fastmm/backtest/fill_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace fastmm::bt {

namespace {
std::int64_t non_negative(const GenericSection& s, std::string_view key, std::int64_t def) {
  const std::int64_t v = s.get_int(key, def);
  if (v < 0) throw ConfigError("backtest." + std::string(key) + " must be >= 0");
  return v;
}
std::int64_t positive(const GenericSection& s, std::string_view key, std::int64_t def) {
  const std::int64_t v = s.get_int(key, def);
  if (v <= 0) throw ConfigError(std::string(key) + " must be > 0");
  return v;
}

// Every [backtest] key from_config reads (docs/reference/configuration.md#backtest).
constexpr std::array<std::string_view, 15> kBacktestKeys = {"source",
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
                                                            "latency_md_us",
                                                            "latency_md_jitter_us"};

void warn_unknown_backtest_keys(const GenericSection& bt, std::vector<std::string>& warnings) {
  for (const auto& [key, value] : bt.values) {
    if (std::find(kBacktestKeys.begin(), kBacktestKeys.end(), key) != kBacktestKeys.end()) continue;
    const auto line = bt.lines.find(key);
    warnings.push_back(
        "unknown key 'backtest." + key + "' ignored" +
        (line == bt.lines.end() ? std::string() : " (line " + std::to_string(line->second) + ")"));
  }
}
}  // namespace

BacktestConfig BacktestConfig::from_config(const Config& cfg) {
  BacktestConfig b;
  b.engine.rng_seed = cfg.engine.rng_seed;
  b.engine.max_events_per_step = cfg.engine.max_events_per_step;
  b.engine.crossed_grace = milliseconds(cfg.engine.crossed_grace_ms);
  b.engine.max_param_age = milliseconds(cfg.strategy.max_param_age_ms);
  b.engine.latency_publish_interval = milliseconds(cfg.engine.latency_publish_ms);
  b.engine.spin_mode = SpinMode::Busy;
  b.engine.risk = cfg.risk_limits();
  b.engine.quotes = cfg.quote_params();
  b.instruments = load_instruments(cfg);
  if (b.instruments.size() == 0) throw ConfigError("backtest: no [[instruments]] configured");
  b.strategy = cfg.strategy.name;
  b.params = cfg.strategy.params;
  b.config_toml = cfg.effective_toml();

  const GenericSection& bt = cfg.backtest;
  const GenericSection& sm = cfg.sim;
  b.warnings = cfg.warnings;
  warn_unknown_backtest_keys(bt, b.warnings);
  b.set_seed(static_cast<std::uint64_t>(bt.get_int("seed", sm.get_int("seed", 1))));
  b.source = bt.get_string("source", "");
  b.path = bt.get_string("path", "");
  b.output_dir = bt.get_string("output_dir", "runs/backtest");
  b.journal_out = bt.get_string("journal_out", "");
  b.equity_bar = seconds(positive(bt, "equity_bar_s", 1));
  b.duration = seconds(positive(bt, "duration_s", sm.get_int("duration_s", 60)));
  b.initial_capital = bt.get_double("initial_capital", 0.0);
  b.generator_seed_levels = static_cast<int>(positive(sm, "seed_levels", 20));

  sim::SimTransportConfig& t = b.transport;
  const auto fm = parse_fill_model(bt.get_string("fill_model", "matching"));
  if (!fm) throw ConfigError("backtest.fill_model must be matching | l2_queue");
  t.fill_model = *fm;
  const double c = bt.get_double("queue_conservatism", 1.0);
  if (!(c >= 0.0 && c <= 1.0)) throw ConfigError("backtest.queue_conservatism must be in [0, 1]");
  t.queue_conservatism_bps = static_cast<std::int64_t>(std::llround(c * 10'000.0));
  const double p_drop = bt.get_double("p_drop", 0.0);
  if (!(p_drop >= 0.0 && p_drop < 1.0)) throw ConfigError("backtest.p_drop must be in [0, 1)");
  const Duration fixed = microseconds(non_negative(bt, "latency_fixed_us", 200));
  const Duration jitter = microseconds(non_negative(bt, "latency_jitter_us", 50));
  t.order_out = sim::LatencyParams{fixed, jitter, p_drop};
  t.ack_in = sim::LatencyParams{fixed, jitter, 0.0};
  t.md_in = sim::LatencyParams{microseconds(non_negative(bt, "latency_md_us", 0)),
                               microseconds(non_negative(bt, "latency_md_jitter_us", 0)),
                               0.0};
  t.supports_replace = cfg.engine.supports_replace;
  t.stp = cfg.risk.stp ? sim::StpMode::CancelTaker : sim::StpMode::None;
  t.md.interval = milliseconds(positive(sm, "depth_update_ms", 100));
  t.md.book_ticker = sm.get_bool("book_ticker", true);
  t.venue = VenueId{0};
  if (!cfg.venues.empty()) {
    t.fees = sim::FeeModel::from_bps(cfg.venues[0].fees.maker_bps, cfg.venues[0].fees.taker_bps);
  }

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
