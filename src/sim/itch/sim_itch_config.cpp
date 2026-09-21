#include "fastmm/sim/itch/sim_itch_config.hpp"

#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/config/config.hpp"

#include <set>
#include <stdexcept>
#include <string_view>

namespace fastmm::sim::itch {

namespace {

template <class Tag>
Fixed<Tag> parse_fixed(std::string_view text, std::string_view what) {
  const auto v = Fixed<Tag>::from_decimal(text);
  if (!v)
    throw std::invalid_argument("sim-itch config: bad decimal for " + std::string(what) + ": '" +
                                std::string(text) + "'");
  return *v;
}

std::int64_t get_range(const GenericSection& s,
                       std::string_view key,
                       std::int64_t def,
                       std::int64_t lo,
                       std::int64_t hi) {
  const std::int64_t v = s.get_int(key, def);
  if (v < lo || v > hi)
    throw std::invalid_argument("sim-itch config: " + std::string(key) + " out of range");
  return v;
}

// "239.1.1.1:31001" or "" (line off).
SimItchLine parse_line(const GenericSection& s, std::string_view key, const SimItchLine& def) {
  SimItchLine line = def;
  if (s.has(key)) {
    const std::string v = s.get_string(key);
    if (v.empty()) {
      line.group.clear();
      line.port = 0;
    } else {
      const std::size_t colon = v.rfind(':');
      if (colon == std::string::npos)
        throw std::invalid_argument("sim-itch config: " + std::string(key) + " must be group:port");
      line.group = v.substr(0, colon);
      const std::string_view digits = std::string_view(v).substr(colon + 1);
      std::int64_t port = 0;
      for (const char c : digits) {
        if (c < '0' || c > '9') port = 65536;
        if (port > 65535) break;
        port = port * 10 + (c - '0');
      }
      if (digits.empty() || port > 65535)
        throw std::invalid_argument("sim-itch config: bad port in " + std::string(key));
      line.port = static_cast<std::uint16_t>(port);
    }
  }
  const std::string drop_key = std::string(key) + "_drop_rate";
  line.drop_rate = s.get_double(drop_key, line.drop_rate);
  return line;
}

}  // namespace

SimItchConfig SimItchConfig::defaults() {
  SimItchConfig c;
  for (const char* sym : {"FMAA", "FMBB"}) {
    SimItchSymbol s;
    s.symbol = sym;
    c.symbols.push_back(s);
  }
  MarketGeneratorParams& g = c.generator;
  g.limit_rate_per_s = 150.0;
  g.cancel_rate_per_order_s = 0.5;
  g.market_rate_per_s = 10.0;
  g.mid_step_rate_per_s = 2.0;
  g.offset_p = 0.35;
  g.base_spread_ticks = 1;
  g.limit_qty_median_lots = 100.0;
  g.limit_qty_sigma = 0.8;
  g.market_qty_median_lots = 50.0;
  g.market_qty_sigma = 1.0;
  g.regimes = true;
  g.regime_switch_rate_per_s = 0.02;
  g.volatile_mult = 4.0;
  g.max_resting = 2000;
  return c;
}

void SimItchConfig::validate() const {
  auto bad = [](const std::string& what) {
    throw std::invalid_argument("sim-itch config: " + what);
  };
  if (symbols.empty()) bad("no symbols");
  if (symbols.size() > 256) bad("at most 256 symbols");
  std::set<std::uint16_t> locates;
  std::set<std::string> names;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    const SimItchSymbol& s = symbols[i];
    if (s.symbol.empty() || s.symbol.size() > 8)
      bad("symbol '" + s.symbol + "' must be 1..8 chars");
    if (!names.insert(s.symbol).second) bad("duplicate symbol " + s.symbol);
    const std::uint16_t loc = s.locate != 0 ? s.locate : static_cast<std::uint16_t>(i + 1);
    if (!locates.insert(loc).second) bad("duplicate locate for " + s.symbol);
    std::uint32_t p4 = 0;
    std::uint32_t shares = 0;
    if (!s.tick.is_positive() || !codecs::nasdaq::price_to_price4(s.tick, p4))
      bad(s.symbol + ": tick must be a positive multiple of 0.0001");
    if (!s.lot.is_positive() || !codecs::nasdaq::qty_to_shares(s.lot, shares))
      bad(s.symbol + ": lot must be a whole number of shares");
    if (!s.start_mid.is_positive()) bad(s.symbol + ": start_mid must be > 0");
  }
  if (session.empty() || session.size() > 10) bad("itch.session must be 1..10 characters");
  if (line_a.group.empty() && line_b.group.empty()) bad("no multicast line (itch.line_a/line_b)");
  for (const SimItchLine* l : {&line_a, &line_b}) {
    if (!l->group.empty() && l->port == 0) bad("line " + l->group + " needs a port");
    if (l->drop_rate < 0.0 || l->drop_rate >= 1.0) bad("drop rates must be in [0, 1)");
  }
  if (max_datagram < 64 || max_datagram > 65507) bad("itch.max_datagram must be 64..65507");
  if (max_messages > 0xFFFE) bad("itch.max_messages must be <= 65534");
  if (burst == 0 || burst > 1024) bad("itch.burst must be 1..1024");
  if (packet_rate < 0.0) bad("itch.packet_rate must be >= 0");
  if (speed <= 0.0) bad("speed must be > 0");
  if (history_messages == 0 || history_bytes < 65536) bad("itch history too small");
  if (stamp_ring == 0 || (stamp_ring & (stamp_ring - 1)) != 0)
    bad("stamp_ring must be a power of two");
  if (glimpse_username.size() > 6 || ouch_username.size() > 6) bad("usernames are 6 characters");
  if (glimpse_password.size() > 10 || ouch_password.size() > 10) bad("passwords are 10 characters");
  if (glimpse_session.size() > 10 || ouch_session.size() > 10) bad("sessions are 10 characters");
  if (generator.offset_p <= 0.0 || generator.offset_p > 1.0) bad("generator.offset_p in (0, 1]");
}

SimItchConfig SimItchConfig::from_config(const Config& cfg) {
  SimItchConfig c = defaults();
  const GenericSection& s = cfg.sim;
  c.symbols.clear();
  const std::string venue = s.get_string("venue", "");
  const Price default_mid = parse_fixed<PriceTag>(s.get_string("start_mid", "100"), "start_mid");
  for (const InstrumentSection& inst : cfg.instruments) {
    if (!inst.enabled) continue;
    if (!venue.empty() && inst.venue != venue) continue;
    SimItchSymbol sym;
    sym.symbol = inst.symbol;
    sym.tick = parse_fixed<PriceTag>(inst.tick, inst.symbol + ".tick");
    sym.lot = parse_fixed<QtyTag>(inst.lot, inst.symbol + ".lot");
    const std::string prefix = "symbols." + inst.symbol + ".";
    sym.start_mid =
        s.has(prefix + "start_mid")
            ? parse_fixed<PriceTag>(s.get_string(prefix + "start_mid"), prefix + "start_mid")
            : default_mid;
    sym.locate = static_cast<std::uint16_t>(get_range(s, prefix + "locate", 0, 0, 65535));
    c.symbols.push_back(std::move(sym));
  }

  c.seed = static_cast<std::uint64_t>(s.get_int("seed", static_cast<std::int64_t>(c.seed)));
  c.speed = s.get_double("speed", c.speed);
  c.busy_poll = s.get_bool("busy_poll", c.busy_poll);
  c.bind_host = s.get_string("bind", c.bind_host);

  c.session = s.get_string("itch.session", c.session);
  c.line_a = parse_line(s, "itch.line_a", c.line_a);
  c.line_b = parse_line(s, "itch.line_b", c.line_b);
  c.interface = s.get_string("itch.interface", c.interface);
  c.source = s.get_string("itch.source", c.source);
  c.ttl = static_cast<int>(get_range(s, "itch.ttl", c.ttl, 0, 255));
  c.loop = s.get_bool("itch.loop", c.loop);
  c.max_datagram =
      static_cast<std::uint32_t>(get_range(s, "itch.max_datagram", c.max_datagram, 64, 65507));
  c.max_messages =
      static_cast<std::uint32_t>(get_range(s, "itch.max_messages", c.max_messages, 0, 0xFFFE));
  c.burst = static_cast<std::uint32_t>(get_range(s, "itch.burst", c.burst, 1, 1024));
  c.packet_rate = s.get_double("itch.packet_rate", c.packet_rate);
  c.flush_us = static_cast<std::uint32_t>(get_range(s, "itch.flush_us", c.flush_us, 0, 1'000'000));
  c.drop_seed = static_cast<std::uint64_t>(
      s.get_int("itch.drop_seed", static_cast<std::int64_t>(c.drop_seed)));
  c.heartbeat_ms =
      static_cast<std::uint32_t>(get_range(s, "itch.heartbeat_ms", c.heartbeat_ms, 1, 3'600'000));
  c.history_messages = static_cast<std::size_t>(get_range(
      s, "itch.history_messages", static_cast<std::int64_t>(c.history_messages), 1, 1LL << 30));
  c.history_bytes = static_cast<std::size_t>(get_range(
      s, "itch.history_bytes", static_cast<std::int64_t>(c.history_bytes), 65536, 1LL << 36));
  c.sndbuf_bytes =
      static_cast<int>(get_range(s, "itch.sndbuf_bytes", c.sndbuf_bytes, 0, 1LL << 30));
  c.rerequest_port =
      static_cast<int>(get_range(s, "itch.rerequest_port", c.rerequest_port, -1, 65535));

  c.glimpse_port = static_cast<int>(get_range(s, "glimpse.port", c.glimpse_port, -1, 65535));
  c.glimpse_username = s.get_string("glimpse.username", c.glimpse_username);
  c.glimpse_password = s.get_string("glimpse.password", c.glimpse_password);
  c.glimpse_session = s.get_string("glimpse.session", c.glimpse_session);
  c.glimpse_max_messages = static_cast<std::size_t>(get_range(
      s, "glimpse.max_messages", static_cast<std::int64_t>(c.glimpse_max_messages), 16, 1LL << 26));
  c.ouch_port = static_cast<int>(get_range(s, "ouch.port", c.ouch_port, -1, 65535));
  c.ouch_username = s.get_string("ouch.username", c.ouch_username);
  c.ouch_password = s.get_string("ouch.password", c.ouch_password);
  c.ouch_session = s.get_string("ouch.session", c.ouch_session);
  c.ouch_history_messages =
      static_cast<std::size_t>(get_range(s,
                                         "ouch.history_messages",
                                         static_cast<std::int64_t>(c.ouch_history_messages),
                                         16,
                                         1LL << 26));

  MarketGeneratorParams& g = c.generator;
  c.generator_enabled = s.get_bool("generator.enabled", c.generator_enabled);
  c.seed_levels = static_cast<int>(get_range(s, "generator.seed_levels", c.seed_levels, 0, 1000));
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
      get_range(s, "generator.max_resting", static_cast<std::int64_t>(g.max_resting), 1, 60'000));
  c.validate();
  return c;
}

SimItchConfig SimItchConfig::load(const std::string& path) {
  Config::LoadOptions opts;
  opts.substitute_env = false;
  return from_config(Config::load(path, opts));
}

}  // namespace fastmm::sim::itch
