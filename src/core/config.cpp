// TOML loading / validation / redaction. toml++ is confined to this translation unit.
#include "fastmm/config/config.hpp"

#include "fastmm/config/env_subst.hpp"
#include "fastmm/config/schema.hpp"
#include "fastmm/core/journal.hpp"

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <cstring>
#include <fstream>
#include <sstream>

namespace fastmm {

namespace {

int line_of(const toml::node& n) noexcept {
  return static_cast<int>(n.source().begin.line);
}
int col_of(const toml::node& n) noexcept {
  return static_cast<int>(n.source().begin.column);
}

[[noreturn]] void fail_at(const toml::node& n, const std::string& msg) {
  throw ConfigError(msg, line_of(n), col_of(n));
}

// Shortest round-trip text for any scalar (so `tick = 0.01` becomes "0.01", not
// 0.0100000000000000002).
std::string stringify(const toml::node& n) {
  if (const auto* s = n.as_string()) return s->get();
  if (const auto* i = n.as_integer()) return std::to_string(i->get());
  if (const auto* f = n.as_floating_point()) return fmt::format("{}", f->get());
  if (const auto* b = n.as_boolean()) return b->get() ? "true" : "false";
  if (const auto* a = n.as_array()) {
    std::string out = "[";
    bool first = true;
    for (const auto& e : *a) {
      if (!first) out += ", ";
      first = false;
      out += stringify(e);
    }
    return out + "]";
  }
  if (const auto* t = n.as_table()) {
    std::ostringstream ss;
    ss << *t;
    return ss.str();
  }
  std::ostringstream ss;
  n.visit([&](const auto& v) { ss << v; });
  return ss.str();
}

bool type_matches(KeyType t, const toml::node& n) noexcept {
  switch (t) {
    case KeyType::String:
      return n.is_string();
    case KeyType::Int:
      return n.is_integer();
    case KeyType::Float:
      return n.is_number();
    case KeyType::Bool:
      return n.is_boolean();
    case KeyType::IntArray:
      return n.is_array() &&
             (n.as_array()->empty() || n.as_array()->is_homogeneous(toml::node_type::integer));
    case KeyType::StringArray:
      return n.is_array() &&
             (n.as_array()->empty() || n.as_array()->is_homogeneous(toml::node_type::string));
    case KeyType::Table:
      return n.is_table();
    case KeyType::Any:
      return true;
  }
  return false;
}

const KeySpec* find_spec(std::string_view section, std::string_view key) noexcept {
  const KeySpec* wildcard = nullptr;
  for (const KeySpec& s : config_schema()) {
    if (s.section != section) continue;
    if (s.key == key) return &s;
    if (s.key == "*") wildcard = &s;
  }
  return wildcard;
}

// Validates one table's keys against the schema section name; warns on unknown keys. With
// `warn_unknown` false a key the schema does not know is left alone, because something else owns
// it: the venue a [venues.<name>] section names validates its own keys (venues/registry.hpp).
void validate_table(const toml::table& t,
                    std::string_view section,
                    std::vector<std::string>& warnings,
                    bool warn_unknown = true) {
  for (const auto& [k, v] : t) {
    const KeySpec* spec = find_spec(section, k.str());
    if (spec == nullptr) {
      if (warn_unknown)
        warnings.push_back(
            fmt::format("unknown key '{}.{}' ignored (line {})", section, k.str(), line_of(v)));
      continue;
    }
    if (!type_matches(spec->type, v)) {
      fail_at(v,
              fmt::format("'{}.{}' has the wrong type (expected {})",
                          section,
                          k.str(),
                          spec->type == KeyType::String     ? "string"
                          : spec->type == KeyType::Int      ? "integer"
                          : spec->type == KeyType::Float    ? "number"
                          : spec->type == KeyType::Bool     ? "boolean"
                          : spec->type == KeyType::Table    ? "table"
                          : spec->type == KeyType::IntArray ? "integer array"
                                                            : "string array"));
    }
  }
  for (const KeySpec& s : config_schema()) {
    if (s.section == section && s.required && !t.contains(s.key)) {
      throw ConfigError(
          fmt::format("missing required key '{}.{}'", section, s.key), line_of(t), col_of(t));
    }
  }
}

template <class T>
void get(const toml::table& t, std::string_view key, T& out) {
  if (const auto* n = t.get(key)) {
    if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, bool>) {
      out = n->value_or(out);
    } else if constexpr (std::is_integral_v<T>) {
      const auto v = n->value<std::int64_t>();
      if (v) {
        if (*v < 0 && std::is_unsigned_v<T>)
          fail_at(*n, fmt::format("'{}' must be non-negative", key));
        out = static_cast<T>(*v);
      }
    } else if constexpr (std::is_floating_point_v<T>) {
      const auto v = n->value<double>();
      if (v) out = *v;
    }
  }
}
// Any-typed decimal keys (prices/quantities): keep the exact text.
void get_decimal(const toml::table& t, std::string_view key, std::string& out) {
  if (const auto* n = t.get(key)) out = stringify(*n);
}
// An optional number: absent leaves `out` empty, so "not set" and "set to 0" stay distinct.
void get_optional(const toml::table& t, std::string_view key, std::optional<double>& out) {
  if (const auto* n = t.get(key)) {
    const auto v = n->value<double>();
    if (v) out = *v;
  }
}

bool looks_like_inline_secret(std::string_view key, std::string_view value) noexcept {
  const bool secret_key =
      key.find("secret") != std::string_view::npos || key.find("key") != std::string_view::npos ||
      key.find("token") != std::string_view::npos || key.find("password") != std::string_view::npos;
  return secret_key && value.size() > 32 && !has_env_reference(value);
}

void flatten(const toml::table& t, const std::string& prefix, GenericSection& out) {
  for (const auto& [k, v] : t) {
    const std::string name =
        prefix.empty() ? std::string(k.str()) : prefix + "." + std::string(k.str());
    if (const auto* sub = v.as_table()) {
      flatten(*sub, name, out);
    } else {
      out.values[name] = stringify(v);
      out.lines[name] = line_of(v);
    }
  }
}

std::uint64_t fnv1a(std::string_view s) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (char ch : s) {
    h ^= static_cast<unsigned char>(ch);
    h *= 1099511628211ULL;
  }
  return h;
}

LogLevel parse_level(std::string_view s) {
  if (s == "trace") return LogLevel::Trace;
  if (s == "debug") return LogLevel::Debug;
  if (s == "info") return LogLevel::Info;
  if (s == "warn" || s == "warning") return LogLevel::Warn;
  if (s == "error") return LogLevel::Error;
  if (s == "off") return LogLevel::Off;
  throw ConfigError(fmt::format("unknown log level '{}'", s));
}

}  // namespace

// ---- GenericSection --------------------------------------------------------------------

std::string GenericSection::get_string(std::string_view key, std::string_view def) const {
  const auto it = values.find(std::string(key));
  return it == values.end() ? std::string(def) : it->second;
}
std::int64_t GenericSection::get_int(std::string_view key, std::int64_t def) const {
  const auto it = values.find(std::string(key));
  if (it == values.end()) return def;
  return std::strtoll(it->second.c_str(), nullptr, 10);
}
double GenericSection::get_double(std::string_view key, double def) const {
  const auto it = values.find(std::string(key));
  if (it == values.end()) return def;
  return std::strtod(it->second.c_str(), nullptr);
}
bool GenericSection::get_bool(std::string_view key, bool def) const {
  const auto it = values.find(std::string(key));
  if (it == values.end()) return def;
  return it->second == "true" || it->second == "1";
}

// ---- Config --------------------------------------------------------------------------------

std::uint64_t Config::text_hash(std::string_view text) noexcept {
  return fnv1a(text);
}

Config Config::load(const std::string& path, const LoadOptions& opts) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ConfigError("cannot open config file '" + path + "'");
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse(ss.str(), opts, path);
}

Config Config::parse(std::string_view text, const LoadOptions& opts, std::string_view source_name) {
  Config cfg;
  cfg.source = std::string(source_name);
  cfg.hash = fnv1a(text);

  toml::table doc;
  try {
    doc = toml::parse(text, source_name);
  } catch (const toml::parse_error& e) {
    throw ConfigError(std::string("TOML syntax error: ") + std::string(e.description()),
                      static_cast<int>(e.source().begin.line),
                      static_cast<int>(e.source().begin.column));
  }

  static constexpr std::string_view kKnownSections[] = {"engine",
                                                        "venues",
                                                        "instruments",
                                                        "strategy",
                                                        "risk",
                                                        "logging",
                                                        "sim",
                                                        "backtest",
                                                        "storage"};
  for (const auto& [k, v] : doc) {
    bool known = false;
    for (auto s : kKnownSections) known = known || s == k.str();
    if (!known)
      cfg.warnings.push_back(
          fmt::format("unknown section [{}] ignored (line {})", k.str(), line_of(v)));
  }

  // [engine]
  if (const auto* t = doc["engine"].as_table()) {
    validate_table(*t, "engine", cfg.warnings);
    auto& e = cfg.engine;
    get(*t, "name", e.name);
    get(*t, "cpu", e.cpu);
    if (const auto* arr = t->get_as<toml::array>("net_cpus")) {
      for (const auto& n : *arr)
        e.net_cpus.push_back(static_cast<int>(n.value_or<std::int64_t>(-1)));
    }
    get(*t, "spin_mode", e.spin_mode);
    if (e.spin_mode != "busy" && e.spin_mode != "adaptive")
      fail_at(*t->get("spin_mode"), "spin_mode must be busy|adaptive");
    get(*t, "net_backend", e.net_backend);
    if (e.net_backend != "epoll" && e.net_backend != "io_uring")
      fail_at(*t->get("net_backend"), "net_backend must be epoll|io_uring");
    get(*t, "threading", e.threading);
    if (e.threading != "split" && e.threading != "single")
      fail_at(*t->get("threading"), "threading must be split|single");
    get(*t, "journal", e.journal);
    get(*t, "journal_dir", e.journal_dir);
    get(*t, "journal_sync", e.journal_sync);
    if (JournalSync mode{}; !parse_journal_sync(e.journal_sync, mode))
      fail_at(*t->get("journal_sync"), "journal_sync must be async|fdatasync");
    get(*t, "journal_max_bytes", e.journal_max_bytes);
    if (e.journal_max_bytes != 0 && e.journal_max_bytes < kJournalBlockBytes) {
      fail_at(*t->get("journal_max_bytes"),
              "journal_max_bytes must be 0 or at least one block (1048576)");
    }
    get(*t, "journal_retention_days", e.journal_retention_days);
    if (e.journal_retention_days < 0)
      fail_at(*t->get("journal_retention_days"), "journal_retention_days must be >= 0 (0 keeps)");
    get(*t, "epoch_file", e.epoch_file);
    get(*t, "kill_file", e.kill_file);
    get(*t, "rng_seed", e.rng_seed);
    get(*t, "md_ring_bytes", e.md_ring_bytes);
    get(*t, "order_ring_bytes", e.order_ring_bytes);
    get(*t, "journal_ring_bytes", e.journal_ring_bytes);
    get(*t, "max_events_per_step", e.max_events_per_step);
    get(*t, "crossed_grace_ms", e.crossed_grace_ms);
    get(*t, "ack_timeout_ms", e.ack_timeout_ms);
    if (e.ack_timeout_ms < 0)
      fail_at(*t->get("ack_timeout_ms"), "ack_timeout_ms must be >= 0 (0 disables)");
    get(*t, "latency_publish_ms", e.latency_publish_ms);
    get(*t, "tsc_recalibrate_s", e.tsc_recalibrate_s);
    if (e.tsc_recalibrate_s < 0)
      fail_at(*t->get("tsc_recalibrate_s"), "tsc_recalibrate_s must be >= 0 (0 disables)");
    get(*t, "timer_slack_ns", e.timer_slack_ns);
    if (e.timer_slack_ns < 0)
      fail_at(*t->get("timer_slack_ns"), "timer_slack_ns must be >= 0 (0 keeps the kernel's)");
    get(*t, "lock_memory", e.lock_memory);
    get(*t, "min_requote_ticks", e.min_requote_ticks);
    get(*t, "min_requote_interval_ms", e.min_requote_interval_ms);
    get(*t, "min_qty_bps", e.min_qty_bps);
    get(*t, "reject_backoff_ms", e.reject_backoff_ms);
    get(*t, "reject_backoff_max_ms", e.reject_backoff_max_ms);
    get(*t, "post_only", e.post_only);
    get(*t, "supports_replace", e.supports_replace);
    get(*t, "on_kill", e.on_kill);
    if (e.on_kill != "exit" && e.on_kill != "stay")
      fail_at(*t->get("on_kill"), "on_kill must be exit|stay");
    for (std::size_t b : {e.md_ring_bytes, e.order_ring_bytes, e.journal_ring_bytes}) {
      if ((b & (b - 1)) != 0 || b < 64)
        throw ConfigError("engine ring sizes must be powers of two >= 64", line_of(*t), col_of(*t));
    }
  }

  // [venues.<name>]
  if (const auto* venues = doc["venues"].as_table()) {
    for (const auto& [name, node] : *venues) {
      const auto* t = node.as_table();
      if (t == nullptr) fail_at(node, fmt::format("[venues.{}] must be a table", name.str()));
      validate_table(*t, "venues.*", cfg.warnings, /*warn_unknown=*/false);
      VenueSection v;
      v.name = std::string(name.str());
      auto str = [&](std::string_view key, std::string& out) {
        const auto* n = t->get(key);
        if (n == nullptr) return;
        std::string raw = n->value_or(std::string{});
        if (opts.substitute_env) {
          auto r = substitute_env(raw);
          if (!r)
            fail_at(*n,
                    fmt::format("venues.{}.{}: environment variable '{}' is not set",
                                name.str(),
                                key,
                                r.error()));
          raw = *r;
        } else if (!opts.allow_inline_secrets && looks_like_inline_secret(key, raw)) {
          fail_at(*n,
                  fmt::format("venues.{}.{} looks like an inline secret; use ${{ENV_VAR}} or "
                              "--allow-inline-secrets",
                              name.str(),
                              key));
        }
        // even with env substitution, refuse literal secrets in the file
        const std::string literal = n->value_or(std::string{});
        if (!opts.allow_inline_secrets && looks_like_inline_secret(key, literal)) {
          fail_at(*n,
                  fmt::format("venues.{}.{} looks like an inline secret; use ${{ENV_VAR}} or "
                              "--allow-inline-secrets",
                              name.str(),
                              key));
        }
        out = raw;
      };
      str("kind", v.kind);
      str("ws_url", v.ws_url);
      str("ws_api_url", v.ws_api_url);
      str("rest_url", v.rest_url);
      str("api_key", v.api_key);
      str("api_secret", v.api_secret);
      str("ca_file", v.ca_file);
      get(*t, "testnet", v.testnet);
      get(*t, "supports_replace", v.supports_replace);
      get(*t, "insecure_tls", v.insecure_tls);
      get(*t, "recv_window_ms", v.recv_window_ms);
      if (const auto* fees = t->get_as<toml::table>("fees")) {
        validate_table(*fees, "venues.*.fees", cfg.warnings);
        get(*fees, "maker_bps", v.fees.maker_bps);
        get(*fees, "taker_bps", v.fees.taker_bps);
      }
      // Everything the generic parser did not read belongs to the connector: keep it verbatim,
      // with its line, for the venue that owns it (venues/registry.hpp).
      for (const auto& [k, val] : *t) {
        if (find_spec("venues.*", k.str()) != nullptr) continue;
        v.extra[std::string(k.str())] = stringify(val);
        v.extra_lines[std::string(k.str())] = line_of(val);
      }
      cfg.venues.push_back(std::move(v));
    }
    if (cfg.venues.size() > kMaxVenuesConfig) throw ConfigError("too many venues");
  }
  if (cfg.single_threaded() && cfg.venues.size() > 1) {
    throw ConfigError(
        fmt::format("[engine] threading = \"single\" runs one venue; this configuration has {}",
                    cfg.venues.size()));
  }

  // [[instruments]]
  if (const auto* arr = doc["instruments"].as_array()) {
    for (const auto& node : *arr) {
      const auto* t = node.as_table();
      if (t == nullptr) fail_at(node, "[[instruments]] entries must be tables");
      validate_table(*t, "instruments[]", cfg.warnings);
      InstrumentSection i;
      get(*t, "venue", i.venue);
      get(*t, "symbol", i.symbol);
      get(*t, "base", i.base);
      get(*t, "quote", i.quote);
      get(*t, "asset_class", i.asset_class);
      get_decimal(*t, "tick", i.tick);
      get_decimal(*t, "lot", i.lot);
      get_decimal(*t, "min_qty", i.min_qty);
      get_decimal(*t, "max_qty", i.max_qty);
      get_decimal(*t, "min_notional", i.min_notional);
      get_decimal(*t, "contract_multiplier", i.contract_multiplier);
      get(*t, "enabled", i.enabled);
      get(*t, "price_decimals", i.price_decimals);
      get(*t, "expiry", i.expiry);
      get_decimal(*t, "strike", i.strike);
      get(*t, "option_type", i.option_type);
      get_optional(*t, "maker_bps", i.maker_bps);
      get_optional(*t, "taker_bps", i.taker_bps);
      if (cfg.venue(i.venue) == nullptr)
        fail_at(*t->get("venue"),
                fmt::format("instrument '{}' references unknown venue '{}'", i.symbol, i.venue));
      cfg.instruments.push_back(std::move(i));
    }
  }

  // [strategy]
  if (const auto* t = doc["strategy"].as_table()) {
    validate_table(*t, "strategy", cfg.warnings);
    get(*t, "name", cfg.strategy.name);
    get(*t, "max_param_age_ms", cfg.strategy.max_param_age_ms);
    if (cfg.strategy.max_param_age_ms < 0)
      fail_at(*t->get("max_param_age_ms"), "max_param_age_ms must be >= 0 (0 disables)");
    if (const auto* p = t->get_as<toml::table>("params")) {
      for (const auto& [k, v] : *p) {
        if (v.is_table() || v.is_array())
          fail_at(v, fmt::format("strategy.params.{} must be a scalar", k.str()));
        cfg.strategy.params[std::string(k.str())] = stringify(v);
      }
    }
  } else if (doc.contains("strategy")) {
    fail_at(*doc.get("strategy"), "[strategy] must be a table");
  }

  // [risk]
  if (const auto* t = doc["risk"].as_table()) {
    validate_table(*t, "risk", cfg.warnings);
    auto& r = cfg.risk;
    get_decimal(*t, "max_order_qty", r.max_order_qty);
    get_decimal(*t, "max_order_notional", r.max_order_notional);
    get_decimal(*t, "max_position", r.max_position);
    get(*t, "max_open_orders", r.max_open_orders);
    get(*t, "price_collar_bps", r.price_collar_bps);
    get(*t, "fat_finger_bps", r.fat_finger_bps);
    get(*t, "stale_md_ms", r.stale_md_ms);
    get_decimal(*t, "max_loss", r.max_loss);
    get(*t, "orders_per_sec", r.orders_per_sec);
    get(*t, "burst", r.burst);
    get(*t, "stp", r.stp);
  }

  // [logging]
  if (const auto* t = doc["logging"].as_table()) {
    validate_table(*t, "logging", cfg.warnings);
    get(*t, "level", cfg.logging.level);
    get(*t, "file", cfg.logging.file);
    get(*t, "mirror_level", cfg.logging.mirror_level);
    parse_level(cfg.logging.level);
    parse_level(cfg.logging.mirror_level);
  }

  if (const auto* t = doc["sim"].as_table()) flatten(*t, "", cfg.sim);
  if (const auto* t = doc["backtest"].as_table()) flatten(*t, "", cfg.backtest);
  if (const auto* t = doc["storage"].as_table()) flatten(*t, "", cfg.storage);

  return cfg;
}

const VenueSection* Config::venue(std::string_view name) const noexcept {
  for (const auto& v : venues) {
    if (v.name == name) return &v;
  }
  return nullptr;
}

VenueId Config::venue_id(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < venues.size(); ++i) {
    if (venues[i].name == name) return VenueId{static_cast<std::uint8_t>(i)};
  }
  return VenueId{};
}

namespace {
template <class F>
F parse_fixed(const std::string& s, const char* what) {
  if (s.empty()) return F{};
  const auto v = F::from_decimal(s);
  if (!v) throw ConfigError(fmt::format("risk.{}: '{}' is not a valid decimal", what, s));
  return *v;
}
}  // namespace

RiskLimits Config::risk_limits() const {
  RiskLimits l;
  l.max_order_qty = parse_fixed<Qty>(risk.max_order_qty, "max_order_qty");
  l.max_order_notional = parse_fixed<Notional>(risk.max_order_notional, "max_order_notional");
  l.max_position = parse_fixed<Qty>(risk.max_position, "max_position");
  l.max_open_orders = static_cast<std::uint32_t>(risk.max_open_orders);
  l.price_collar_bps = risk.price_collar_bps;
  l.fat_finger_bps = risk.fat_finger_bps;
  l.stale_md = milliseconds(risk.stale_md_ms);
  l.max_loss = parse_fixed<Notional>(risk.max_loss, "max_loss");
  l.orders_per_sec = static_cast<std::uint32_t>(risk.orders_per_sec);
  l.burst = static_cast<std::uint32_t>(risk.burst);
  l.stp = risk.stp;
  return l;
}

QuoteParams Config::quote_params() const {
  QuoteParams q;
  q.min_requote_ticks = engine.min_requote_ticks;
  q.min_requote_interval = milliseconds(engine.min_requote_interval_ms);
  q.min_qty_bps = engine.min_qty_bps;
  q.post_only = engine.post_only;
  q.supports_replace = engine.supports_replace;
  q.reject_backoff = milliseconds(engine.reject_backoff_ms);
  q.reject_backoff_max = milliseconds(engine.reject_backoff_max_ms);
  return q;
}

LogLevel Config::log_level() const {
  return parse_level(logging.level);
}
LogLevel Config::mirror_level() const {
  return parse_level(logging.mirror_level);
}
SpinMode Config::spin_mode() const {
  return engine.spin_mode == "busy" ? SpinMode::Busy : SpinMode::Adaptive;
}

std::string Config::redacted() const {
  std::string out;
  auto kv = [&](std::string_view k, const auto& v) {
    fmt::format_to(std::back_inserter(out), "{} = {}\n", k, v);
  };
  auto kq = [&](std::string_view k, const std::string& v) {
    fmt::format_to(std::back_inserter(out), "{} = \"{}\"\n", k, v);
  };
  out += "[engine]\n";
  kq("name", engine.name);
  kv("cpu", engine.cpu);
  kq("spin_mode", engine.spin_mode);
  kq("net_backend", engine.net_backend);
  kq("threading", engine.threading);
  kv("journal", engine.journal);
  kq("journal_dir", engine.journal_dir);
  kq("epoch_file", engine.epoch_file);
  kq("kill_file", engine.kill_file);
  kv("rng_seed", engine.rng_seed);
  kv("max_events_per_step", engine.max_events_per_step);
  kv("crossed_grace_ms", engine.crossed_grace_ms);
  kv("ack_timeout_ms", engine.ack_timeout_ms);
  kv("min_requote_ticks", engine.min_requote_ticks);
  kv("min_requote_interval_ms", engine.min_requote_interval_ms);
  kv("min_qty_bps", engine.min_qty_bps);
  kv("reject_backoff_ms", engine.reject_backoff_ms);
  kv("reject_backoff_max_ms", engine.reject_backoff_max_ms);
  kv("post_only", engine.post_only);
  kv("supports_replace", engine.supports_replace);
  kq("on_kill", engine.on_kill);
  for (const auto& v : venues) {
    fmt::format_to(std::back_inserter(out), "\n[venues.{}]\n", v.name);
    kq("kind", v.kind);
    if (!v.ws_url.empty()) kq("ws_url", v.ws_url);
    if (!v.ws_api_url.empty()) kq("ws_api_url", v.ws_api_url);
    if (!v.rest_url.empty()) kq("rest_url", v.rest_url);
    if (!v.api_key.empty()) kq("api_key", "***");
    if (!v.api_secret.empty()) kq("api_secret", "***");
    kv("testnet", v.testnet);
    kv("supports_replace", v.supports_replace);
    kv("insecure_tls", v.insecure_tls);
    if (!v.ca_file.empty()) kq("ca_file", v.ca_file);
    kv("recv_window_ms", v.recv_window_ms);
    fmt::format_to(std::back_inserter(out),
                   "fees = {{ maker_bps = {}, taker_bps = {} }}\n",
                   v.fees.maker_bps,
                   v.fees.taker_bps);
  }
  for (const auto& i : instruments) {
    out += "\n[[instruments]]\n";
    kq("venue", i.venue);
    kq("symbol", i.symbol);
    kq("asset_class", i.asset_class);
    kq("tick", i.tick);
    kq("lot", i.lot);
    if (!i.min_qty.empty()) kq("min_qty", i.min_qty);
    if (!i.max_qty.empty()) kq("max_qty", i.max_qty);
    if (!i.min_notional.empty()) kq("min_notional", i.min_notional);
    kq("contract_multiplier", i.contract_multiplier);
    kv("enabled", i.enabled);
    if (i.maker_bps) kv("maker_bps", *i.maker_bps);
    if (i.taker_bps) kv("taker_bps", *i.taker_bps);
  }
  out += "\n[strategy]\n";
  kq("name", strategy.name);
  if (strategy.max_param_age_ms != 0) kv("max_param_age_ms", strategy.max_param_age_ms);
  if (!strategy.params.empty()) {
    out += "\n[strategy.params]\n";
    for (const auto& [k, v] : strategy.params) kq(k, v);
  }
  out += "\n[risk]\n";
  if (!risk.max_order_qty.empty()) kq("max_order_qty", risk.max_order_qty);
  if (!risk.max_order_notional.empty()) kq("max_order_notional", risk.max_order_notional);
  if (!risk.max_position.empty()) kq("max_position", risk.max_position);
  kv("max_open_orders", risk.max_open_orders);
  kv("price_collar_bps", risk.price_collar_bps);
  kv("fat_finger_bps", risk.fat_finger_bps);
  kv("stale_md_ms", risk.stale_md_ms);
  if (!risk.max_loss.empty()) kq("max_loss", risk.max_loss);
  kv("orders_per_sec", risk.orders_per_sec);
  kv("burst", risk.burst);
  kv("stp", risk.stp);
  out += "\n[logging]\n";
  kq("level", logging.level);
  if (!logging.file.empty()) kq("file", logging.file);
  kq("mirror_level", logging.mirror_level);
  const std::pair<const GenericSection*, std::string_view> generic[] = {
      {&sim, "sim"}, {&backtest, "backtest"}, {&storage, "storage"}};
  for (const auto& [sec, name] : generic) {
    if (sec->values.empty()) continue;
    fmt::format_to(std::back_inserter(out), "\n[{}]\n", name);
    for (const auto& [k, v] : sec->values) kq(k, v);
  }
  return out;
}

namespace {

// A connector's venue key was stored as text (the connector owns its type, not this file): emit
// the TOML literal that stringifies back to exactly this text, a quoted string when there is
// none, so parsing the result gives the same VenueSection::extra back.
void insert_typed(toml::table& t, const std::string& key, const std::string& text) {
  try {
    toml::table probe = toml::parse("v = " + text);
    if (toml::node* n = probe.get("v"); n != nullptr && !n->is_string() && stringify(*n) == text) {
      t.insert_or_assign(key, std::move(*n));
      return;
    }
  } catch (const toml::parse_error&) {  // not a TOML literal: it can only be a string
  }
  t.insert_or_assign(key, text);
}

toml::table generic_table(const GenericSection& g) {
  toml::table t;
  for (const auto& [k, v] : g.values)
    t.insert_or_assign(k, v);  // flatten() reads dotted names back
  return t;
}

}  // namespace

std::string Config::effective_toml() const {
  toml::table root;

  toml::table e;
  e.insert("name", engine.name);
  e.insert("cpu", static_cast<std::int64_t>(engine.cpu));
  toml::array cpus;
  for (int c : engine.net_cpus) cpus.push_back(static_cast<std::int64_t>(c));
  e.insert("net_cpus", std::move(cpus));
  e.insert("spin_mode", engine.spin_mode);
  e.insert("net_backend", engine.net_backend);
  e.insert("threading", engine.threading);
  e.insert("journal", engine.journal);
  e.insert("journal_dir", engine.journal_dir);
  e.insert("journal_sync", engine.journal_sync);
  e.insert("journal_max_bytes", static_cast<std::int64_t>(engine.journal_max_bytes));
  e.insert("journal_retention_days", static_cast<std::int64_t>(engine.journal_retention_days));
  e.insert("epoch_file", engine.epoch_file);
  e.insert("kill_file", engine.kill_file);
  e.insert("rng_seed", static_cast<std::int64_t>(engine.rng_seed));
  e.insert("md_ring_bytes", static_cast<std::int64_t>(engine.md_ring_bytes));
  e.insert("order_ring_bytes", static_cast<std::int64_t>(engine.order_ring_bytes));
  e.insert("journal_ring_bytes", static_cast<std::int64_t>(engine.journal_ring_bytes));
  e.insert("max_events_per_step", static_cast<std::int64_t>(engine.max_events_per_step));
  e.insert("crossed_grace_ms", static_cast<std::int64_t>(engine.crossed_grace_ms));
  e.insert("ack_timeout_ms", static_cast<std::int64_t>(engine.ack_timeout_ms));
  e.insert("latency_publish_ms", static_cast<std::int64_t>(engine.latency_publish_ms));
  e.insert("tsc_recalibrate_s", static_cast<std::int64_t>(engine.tsc_recalibrate_s));
  e.insert("timer_slack_ns", engine.timer_slack_ns);
  e.insert("lock_memory", engine.lock_memory);
  e.insert("min_requote_ticks", static_cast<std::int64_t>(engine.min_requote_ticks));
  e.insert("min_requote_interval_ms", static_cast<std::int64_t>(engine.min_requote_interval_ms));
  e.insert("min_qty_bps", static_cast<std::int64_t>(engine.min_qty_bps));
  e.insert("post_only", engine.post_only);
  e.insert("supports_replace", engine.supports_replace);
  e.insert("reject_backoff_ms", static_cast<std::int64_t>(engine.reject_backoff_ms));
  e.insert("reject_backoff_max_ms", static_cast<std::int64_t>(engine.reject_backoff_max_ms));
  e.insert("on_kill", engine.on_kill);
  root.insert("engine", std::move(e));

  if (!venues.empty()) {
    toml::table vs;
    for (const VenueSection& v : venues) {
      toml::table t;
      t.insert("kind", v.kind);
      t.insert("ws_url", v.ws_url);
      t.insert("ws_api_url", v.ws_api_url);
      t.insert("rest_url", v.rest_url);
      t.insert("testnet", v.testnet);
      t.insert("supports_replace", v.supports_replace);
      t.insert("insecure_tls", v.insecure_tls);
      t.insert("ca_file", v.ca_file);
      t.insert("recv_window_ms", static_cast<std::int64_t>(v.recv_window_ms));
      toml::table fees;
      fees.insert("maker_bps", v.fees.maker_bps);
      fees.insert("taker_bps", v.fees.taker_bps);
      t.insert("fees", std::move(fees));
      for (const auto& [k, val] : v.extra) insert_typed(t, k, val);
      vs.insert(v.name, std::move(t));
    }
    root.insert("venues", std::move(vs));
  }

  if (!instruments.empty()) {
    toml::array arr;
    for (const InstrumentSection& i : instruments) {
      toml::table t;
      t.insert("venue", i.venue);
      t.insert("symbol", i.symbol);
      t.insert("base", i.base);
      t.insert("quote", i.quote);
      t.insert("asset_class", i.asset_class);
      t.insert("tick", i.tick);
      t.insert("lot", i.lot);
      t.insert("min_qty", i.min_qty);
      t.insert("max_qty", i.max_qty);
      t.insert("min_notional", i.min_notional);
      t.insert("contract_multiplier", i.contract_multiplier);
      t.insert("enabled", i.enabled);
      t.insert("price_decimals", static_cast<std::int64_t>(i.price_decimals));
      t.insert("expiry", i.expiry);
      t.insert("strike", i.strike);
      t.insert("option_type", i.option_type);
      if (i.maker_bps) t.insert("maker_bps", *i.maker_bps);
      if (i.taker_bps) t.insert("taker_bps", *i.taker_bps);
      arr.push_back(std::move(t));
    }
    root.insert("instruments", std::move(arr));
  }

  toml::table st;
  st.insert("name", strategy.name);
  if (strategy.max_param_age_ms != 0) st.insert("max_param_age_ms", strategy.max_param_age_ms);
  toml::table params;
  for (const auto& [k, v] : strategy.params) params.insert_or_assign(k, v);
  st.insert("params", std::move(params));
  root.insert("strategy", std::move(st));

  toml::table r;
  r.insert("max_order_qty", risk.max_order_qty);
  r.insert("max_order_notional", risk.max_order_notional);
  r.insert("max_position", risk.max_position);
  r.insert("max_open_orders", static_cast<std::int64_t>(risk.max_open_orders));
  r.insert("price_collar_bps", static_cast<std::int64_t>(risk.price_collar_bps));
  r.insert("fat_finger_bps", static_cast<std::int64_t>(risk.fat_finger_bps));
  r.insert("stale_md_ms", static_cast<std::int64_t>(risk.stale_md_ms));
  r.insert("max_loss", risk.max_loss);
  r.insert("orders_per_sec", static_cast<std::int64_t>(risk.orders_per_sec));
  r.insert("burst", static_cast<std::int64_t>(risk.burst));
  r.insert("stp", risk.stp);
  root.insert("risk", std::move(r));

  toml::table lg;
  lg.insert("level", logging.level);
  lg.insert("file", logging.file);
  lg.insert("mirror_level", logging.mirror_level);
  root.insert("logging", std::move(lg));

  if (!sim.values.empty()) root.insert("sim", generic_table(sim));
  if (!backtest.values.empty()) root.insert("backtest", generic_table(backtest));
  if (!storage.values.empty()) root.insert("storage", generic_table(storage));

  std::ostringstream ss;
  ss << root << "\n";
  return ss.str();
}

}  // namespace fastmm
