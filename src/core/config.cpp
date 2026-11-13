// TOML loading / validation / redaction. toml++ is confined to this translation unit.
#include "fastmm/config/config.hpp"

#include "fastmm/config/env_subst.hpp"
#include "fastmm/config/schema.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/strategies/ab_test.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <toml++/toml.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace fastmm {

namespace {

// An IPv4 or IPv6 address as written (inet_pton): no name, no port.
bool valid_ip_literal(const std::string& ip) {
  unsigned char buf[sizeof(in6_addr)];
  return ::inet_pton(AF_INET, ip.c_str(), buf) == 1 || ::inet_pton(AF_INET6, ip.c_str(), buf) == 1;
}

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

// Validates one table's keys against the schema section name. An unknown key is an error: a
// renamed or misspelled key would otherwise leave the real one at its default. With
// `check_unknown` false a key the schema does not know is left alone, because something else owns
// it: the venue a [venues.<name>] section names validates its own keys (venues/registry.hpp).
void validate_table(const toml::table& t, std::string_view section, bool check_unknown = true) {
  for (const auto& [k, v] : t) {
    const KeySpec* spec = find_spec(section, k.str());
    if (spec == nullptr) {
      if (check_unknown) {
        fail_at(v,
                fmt::format(
                    "unknown key '{}.{}' (see docs/reference/configuration.md)", section, k.str()));
      }
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
// Letters, digits and `extra`, 1 to `max` characters.
bool plain_name(std::string_view s, std::size_t max, std::string_view extra) noexcept {
  if (s.empty() || s.size() > max) return false;
  for (const char c : s) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum && extra.find(c) == std::string_view::npos) return false;
  }
  return true;
}

// [ab] and its [[ab.arms]]. The parameters are checked against the strategy's schema when a
// session or backtest starts (strategies/ab_test.hpp); this checks the shape.
void parse_ab(const toml::table& t, AbSection& ab) {
  validate_table(t, "ab");
  get(t, "name", ab.name);
  if (!plain_name(ab.name, 32, "_-."))
    fail_at(*t.get("name"), "ab.name must be 1 to 32 letters, digits, '_', '-' or '.'");
  get(t, "assignment", ab.assignment);
  const bool by_time = ab.assignment == "time" || ab.assignment == "crossover";
  if (!by_time && ab.assignment != "instrument" && ab.assignment != "branch")
    fail_at(*t.get("assignment"), "ab.assignment must be instrument, time, crossover or branch");
  get(t, "seed", ab.seed);
  get(t, "block_s", ab.block_s);
  if (by_time && ab.block_s <= 0) {
    if (const auto* n = t.get("block_s")) fail_at(*n, "ab.block_s must be > 0");
    fail_at(t, fmt::format("ab.assignment = \"{}\" needs block_s", ab.assignment));
  }
  get(t, "block_offset_s", ab.block_offset_s);
  if (t.contains("block_offset_s") &&
      (!by_time || ab.block_offset_s < 0 || ab.block_offset_s >= ab.block_s)) {
    fail_at(*t.get("block_offset_s"),
            by_time ? "ab.block_offset_s must be >= 0 and below block_s"
                    : fmt::format("ab.block_offset_s: assignment = \"{}\" has no blocks",
                                  ab.assignment));
  }
  if (!by_time && t.contains("block_s")) {
    fail_at(*t.get("block_s"),
            fmt::format("ab.block_s: assignment = \"{}\" has no blocks", ab.assignment));
  }
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
  for (const std::string_view key : {std::string_view("start"), std::string_view("end")}) {
    const auto* n = t.get(key);
    if (n == nullptr) continue;
    std::string& text = key == "start" ? ab.start : ab.end;
    text = n->value_or(std::string{});
    const std::optional<std::int64_t> ns = parse_utc_time(text);
    if (!ns)
      fail_at(*n, fmt::format("ab.{}: '{}' is not a UTC time YYYY-MM-DD[THH:MM[:SS]]", key, text));
    (key == "start" ? start_ns : end_ns) = *ns;
  }
  if (!ab.start.empty() && !ab.end.empty() && end_ns <= start_ns)
    fail_at(*t.get("end"), "ab.end must be after ab.start");
  if (const auto* w = t.get_as<toml::table>("weights")) {
    if (ab.assignment == "time" || ab.assignment == "branch") {
      fail_at(*w,
              fmt::format("ab.weights: assignment = \"{}\" puts every instrument in the same arm",
                          ab.assignment));
    }
    validate_table(*w, "ab.weights");
    for (const auto& [k, v] : *w) {
      const double x = v.value_or(-1.0);
      if (!(x >= 0.0) || !std::isfinite(x))
        fail_at(v, fmt::format("ab.weights.\"{}\" must be a number >= 0", k.str()));
      ab.weights[std::string(k.str())] = x;
    }
  }
  if (const auto* a = t.get_as<toml::table>("assign")) {
    if (ab.assignment == "time" || ab.assignment == "branch") {
      fail_at(*a,
              fmt::format("ab.assign: assignment = \"{}\" puts every instrument in the same arm",
                          ab.assignment));
    }
    if (t.contains("weights")) fail_at(*a, "ab.assign and ab.weights: give one of them");
    for (const auto& [k, v] : *a) {
      const std::optional<std::string> arm = v.value<std::string>();
      if (!arm) fail_at(v, fmt::format("ab.assign.\"{}\" must be an arm's name", k.str()));
      ab.assign[std::string(k.str())] = *arm;
    }
  }
  const toml::node* arms = t.get("arms");
  const toml::array* list = arms != nullptr ? arms->as_array() : nullptr;
  if (list == nullptr || !list->is_array_of_tables())
    fail_at(arms != nullptr ? *arms : static_cast<const toml::node&>(t),
            "ab.arms must be [[ab.arms]] tables");
  if (list->size() < 2 || list->size() > kMaxAbArms)
    fail_at(*arms, fmt::format("[ab] needs 2 to {} [[ab.arms]], got {}", kMaxAbArms, list->size()));
  std::set<std::string> owned;
  for (const toml::node& node : *list) {
    const toml::table& a = *node.as_table();
    validate_table(a, "ab.arms[]");
    AbArmSection arm;
    get(a, "name", arm.name);
    if (!plain_name(arm.name, 16, "_-"))
      fail_at(*a.get("name"), "ab.arms.name must be 1 to 16 letters, digits, '_' or '-'");
    for (const AbArmSection& other : ab.arms) {
      if (other.name == arm.name)
        fail_at(*a.get("name"), fmt::format("ab.arms: two arms are named '{}'", arm.name));
    }
    if (const auto* p = a.get_as<toml::table>("params")) {
      validate_table(*p, "ab.arms[].params");
      for (const auto& [k, v] : *p) {
        if (v.is_table() || v.is_array())
          fail_at(v, fmt::format("ab.arms.params.{} must be a scalar", k.str()));
        arm.params[std::string(k.str())] = stringify(v);
        owned.insert(std::string(k.str()));
      }
    }
    ab.arms.push_back(std::move(arm));
  }
  if (owned.size() > ParamUpdateMsg::kMaxFields)
    fail_at(*arms,
            fmt::format("[ab]: the arms name {} parameters; at most {}",
                        owned.size(),
                        ParamUpdateMsg::kMaxFields));
}

// [risk.underlying] / [gateway.underlying]: one table per base asset, each with max_net (base
// units, a non-negative decimal).
void get_underlying(const toml::table& parent, std::string_view section, UnderlyingSpec& out) {
  const toml::node* n = parent.get("underlying");
  if (n == nullptr) return;
  const auto* t = n->as_table();
  if (t == nullptr)
    fail_at(*n,
            fmt::format("{}.underlying must be a table of base assets: [{}.underlying.BTC] "
                        "max_net = 0.5",
                        section,
                        section));
  const std::string sub = fmt::format("{}.underlying.*", section);
  for (const auto& [k, v] : *t) {
    const std::string name(k.str());
    const auto* u = v.as_table();
    if (u == nullptr)
      fail_at(v,
              fmt::format("{}.underlying.{} must be a table: [{}.underlying.{}] max_net = ...",
                          section,
                          name,
                          section,
                          name));
    bool valid = !name.empty() && name.size() <= UnderlyingName::kCapacity;
    for (const char ch : name) valid = valid && ch != ':' && ch != ' ' && ch != '"';
    if (!valid)
      fail_at(v,
              fmt::format("[{}.underlying.{}]: the base asset must be 1 to {} characters",
                          section,
                          name,
                          UnderlyingName::kCapacity));
    const toml::node* m = u->get("max_net");
    if (m == nullptr)
      fail_at(v, fmt::format("[{}.underlying.{}] needs max_net (base units)", section, name));
    validate_table(*u, sub);
    const std::string text = stringify(*m);
    const auto q = Qty::from_decimal(text);
    if (!q || q->raw < 0)
      fail_at(
          *m,
          fmt::format(
              "{}.underlying.{}.max_net: '{}' is not a non-negative decimal", section, name, text));
    for (const auto& [other, unused] : out.max_net) {
      if (same_currency(other, name))
        fail_at(v,
                fmt::format("[{}.underlying.{}]: {} is named already (base assets ignore case)",
                            section,
                            name,
                            other));
    }
    out.max_net[name] = text;
  }
  if (out.max_net.size() > kMaxUnderlyings)
    fail_at(*t, fmt::format("[{}.underlying]: at most {} base assets", section, kMaxUnderlyings));
}

// [venues.<primary>.treasury]: the keys and their ranges. Whether the venue is a pool's primary and
// the accounts named are its pool's is checked once every venue is read (check_treasuries).
void parse_treasury(const toml::table& t, VenueSection& v) {
  validate_table(t, "venues.*.treasury");
  TreasurySection& s = v.treasury;
  s.configured = true;
  const std::string at = fmt::format("venues.{}.treasury", v.name);
  get(t, "enabled", s.enabled);
  get(t, "dry_run", s.dry_run);
  get(t, "asset", s.asset);
  get(t, "threshold", s.threshold);
  get(t, "state_file", s.state_file);
  get(t, "targets", s.targets);
  get(t, "headroom", s.headroom);
  get(t, "reserve", s.reserve);
  if (s.threshold < 0.0 || s.threshold > 1.0)
    fail_at(*t.get("threshold"), at + ".threshold must be 0 to 1");
  if (s.targets != "weights" && s.targets != "demand")
    fail_at(*t.get("targets"), at + ".targets must be \"weights\" or \"demand\"");
  if (s.headroom < 0.0) fail_at(*t.get("headroom"), at + ".headroom must be at least 0");
  const auto decimal = [&](std::string_view key, std::string& out) {
    const toml::node* n = t.get(key);
    if (n == nullptr) return;
    out = stringify(*n);
    const auto d = Notional::parse(out);
    if (!d || d->raw < 0)
      fail_at(*n, fmt::format("{}.{}: '{}' is not a non-negative decimal", at, key, out));
  };
  decimal("min_amount", s.min_amount);
  decimal("max_amount", s.max_amount);
  decimal("step", s.step);
  const auto seconds = [&](std::string_view key, std::int64_t& out, std::int64_t least) {
    const toml::node* n = t.get(key);
    if (n == nullptr) return;
    get(t, key, out);
    if (out < least) fail_at(*n, fmt::format("{}.{} must be at least {}", at, key, least));
  };
  seconds("interval_s", s.interval_s, 1);
  seconds("min_interval_s", s.min_interval_s, 0);
  seconds("max_per_hour", s.max_per_hour, 0);
  seconds("cooldown_s", s.cooldown_s, 0);
  seconds("timeout_s", s.timeout_s, 1);
  seconds("settle_s", s.settle_s, 0);
  if (const auto* w = t.get_as<toml::table>("weights")) {
    for (const auto& [k, val] : *w) {
      const auto x = val.value<double>();
      if (!x || *x < 0.0)
        fail_at(val, fmt::format("{}.weights.{} must be a non-negative number", at, k.str()));
      s.weights[std::string(k.str())] = *x;
    }
  }
  if (const auto* m = t.get_as<toml::table>("min_free")) {
    for (const auto& [k, val] : *m) {
      const std::string text = stringify(val);
      const auto d = Notional::parse(text);
      if (!d || d->raw < 0)
        fail_at(
            val,
            fmt::format("{}.min_free.{}: '{}' is not a non-negative decimal", at, k.str(), text));
      s.min_free[std::string(k.str())] = text;
    }
  }
  if (s.enabled && s.asset.empty()) fail_at(t, at + ": enabled needs asset");
  if (s.asset.size() > 8) fail_at(*t.get("asset"), at + ".asset: at most 8 characters");
}

// [venues.<primary>.pool]: the keys and their ranges; the venue being a pool's primary and the
// accounts and symbols named are checked once everything is read (check_pools).
void parse_pool(const toml::table& t, VenueSection& v) {
  validate_table(t, "venues.*.pool");
  PoolSection& s = v.pool;
  s.configured = true;
  const std::string at = fmt::format("venues.{}.pool", v.name);
  get(t, "placement", s.placement);
  if (s.placement != "balance" && s.placement != "home")
    fail_at(*t.get("placement"), at + ".placement must be \"balance\" or \"home\"");
  get(t, "spill", s.spill);
  if (s.spill != "never" && s.spill != "flat" && s.spill != "busy")
    fail_at(*t.get("spill"), at + ".spill must be \"never\", \"flat\" or \"busy\"");
  if (const toml::node* n = t.get("spill_at")) {
    const auto x = n->value<double>();
    if (!x || !(*x > 0.0) || *x > 1.0) fail_at(*n, at + ".spill_at must be above 0 and at most 1");
    s.spill_at = *x;
  }
  const auto ranged =
      [&](std::string_view key, std::int64_t& out, std::int64_t lo, std::int64_t hi) {
        const toml::node* n = t.get(key);
        if (n == nullptr) return;
        get(t, key, out);
        if (out < lo || out > hi)
          fail_at(*n, fmt::format("{}.{} must be {} to {}", at, key, lo, hi));
      };
  ranged("reduce_accounts", s.reduce_accounts, 1, static_cast<std::int64_t>(PoolMembers::kMax));
  ranged("plan_interval_s", s.plan_interval_s, 0, 604800);  // a week
  ranged("plan_max_moves", s.plan_max_moves, 0, 256);
  if (const toml::node* n = t.get("plan_lookback_h")) {
    const auto x = n->value<double>();
    if (!x || !(*x >= 1.0 / 360.0) || *x > 168.0)
      fail_at(*n, at + ".plan_lookback_h must be a number of hours from 10 s to 168");
    s.plan_lookback_h = *x;
  }
  if (const auto* h = t.get_as<toml::table>("homes")) {
    for (const auto& [k, val] : *h) {
      const auto name = val.value<std::string>();
      if (!name) fail_at(val, fmt::format("{}.homes.{} must be an account name", at, k.str()));
      s.homes[std::string(k.str())] = *name;
    }
  }
  if (const toml::node* n = t.get("sweep")) {
    if (const auto b = n->value<bool>()) {
      s.sweep = *b ? "true" : "false";
    } else if (n->value<std::string>().value_or("") == "auto") {
      s.sweep = "auto";
    } else {
      fail_at(*n, at + ".sweep must be true, false or \"auto\"");
    }
  }
  ranged("sweep_delay_ms", s.sweep_delay_ms, 0, 3'600'000);
  ranged("sweep_dust_s", s.sweep_dust_s, 0, 604800);
  ranged("sweep_retry_s", s.sweep_retry_s, 0, 86400);
}

// [venues.<primary>.transfers]: the keys and their ranges; the venue having a pool table is checked
// with the pools (check_pools).
void parse_transfers(const toml::table& t, VenueSection& v) {
  validate_table(t, "venues.*.transfers");
  TransfersSection& s = v.transfers;
  s.configured = true;
  const std::string at = fmt::format("venues.{}.transfers", v.name);
  const auto ranged =
      [&](std::string_view key, std::int64_t& out, std::int64_t lo, std::int64_t hi) {
        const toml::node* n = t.get(key);
        if (n == nullptr) return;
        get(t, key, out);
        if (out < lo || out > hi)
          fail_at(*n, fmt::format("{}.{} must be {} to {}", at, key, lo, hi));
      };
  ranged("max_in_flight", s.max_in_flight, 1, static_cast<std::int64_t>(TransferLane::kSlots));
  ranged("per_minute", s.per_minute, 0, 100000);
  ranged("per_hour", s.per_hour, 0, 1000000);
  ranged("per_day", s.per_day, 0, 1000000);
  ranged("breaker_s", s.breaker_s, 0, 86400);
  ranged("timeout_s", s.timeout_s, 1, 86400);
  ranged("status_interval_s", s.status_interval_s, 1, 3600);
  get(t, "dry_run", s.dry_run);
  get(t, "state_file", s.state_file);
}

// A pool table belongs to a pool's primary; its pins name the pool's accounts and the primary's
// instruments.
void check_pools(const toml::table& doc, const Config& cfg) {
  const PoolPlan plan = cfg.pool_plan();
  for (const VenueSection& v : cfg.venues) {
    if (v.transfers.configured && !v.pool.configured) {
      const std::string msg = fmt::format(
          "venues.{}.transfers: the transfer lane belongs to a pool with a [venues.{}.pool] table",
          v.name,
          v.name);
      if (const toml::node* n = doc["venues"][v.name]["transfers"].node()) fail_at(*n, msg);
      throw ConfigError(msg);
    }
    if (!v.pool.configured) continue;
    const toml::node* node = doc["venues"][v.name]["pool"].node();
    const auto fail_here = [&](const std::string& msg) {
      if (node != nullptr) fail_at(*node, msg);
      throw ConfigError(msg);
    };
    if (!v.pool_of.empty())
      fail_here(
          fmt::format("venues.{}.pool: {} is a member of pool '{}'; the table goes on the primary",
                      v.name,
                      v.name,
                      v.pool_of));
    const VenueId id = cfg.venue_id(v.name);
    if (!plan.pooled(id))
      fail_here(fmt::format("venues.{}.pool: {} has no pool (no [venues.<name>] pool_of = \"{}\")",
                            v.name,
                            v.name,
                            v.name));
    const PoolMembers members = plan.members(id);
    for (const auto& [symbol, account] : v.pool.homes) {
      const VenueId a = cfg.venue_id(account);
      if (!a.valid() || !members.contains(a))
        fail_here(fmt::format("venues.{}.pool.homes.{}: '{}' is not an account of the pool",
                              v.name,
                              symbol,
                              account));
      const bool listed = std::any_of(
          cfg.instruments.begin(), cfg.instruments.end(), [&](const InstrumentSection& i) {
            return i.venue == v.name && i.symbol == symbol;
          });
      if (!listed)
        fail_here(fmt::format(
            "venues.{}.pool.homes: {} is not an instrument of {}", v.name, symbol, v.name));
    }
  }
}

// A treasury belongs to a pool's primary, and names only the pool's accounts.
void check_treasuries(const toml::table& doc, const Config& cfg) {
  const PoolPlan plan = cfg.pool_plan();
  for (const VenueSection& v : cfg.venues) {
    if (!v.treasury.configured) continue;
    const toml::node* node = doc["venues"][v.name]["treasury"].node();
    const auto fail_here = [&](const std::string& msg) {
      if (node != nullptr) fail_at(*node, msg);
      throw ConfigError(msg);
    };
    const VenueId id = cfg.venue_id(v.name);
    if (!v.pool_of.empty()) {
      fail_here(
          fmt::format("venues.{}.treasury: {} is a member of pool '{}'; the treasury goes on "
                      "the primary",
                      v.name,
                      v.name,
                      v.pool_of));
    }
    if (!plan.pooled(id)) {
      fail_here(
          fmt::format("venues.{}.treasury: {} has no pool (no [venues.<name>] pool_of = \"{}\")",
                      v.name,
                      v.name,
                      v.name));
    }
    const PoolMembers members = plan.members(id);
    const auto in_pool = [&](const std::string& name) {
      const VenueId a = cfg.venue_id(name);
      return a.valid() && members.contains(a);
    };
    for (const auto& [name, w] : v.treasury.weights) {
      if (!in_pool(name))
        fail_here(fmt::format(
            "venues.{}.treasury.weights: '{}' is not an account of the pool", v.name, name));
    }
    for (const auto& [name, m] : v.treasury.min_free) {
      if (!in_pool(name))
        fail_here(fmt::format(
            "venues.{}.treasury.min_free: '{}' is not an account of the pool", v.name, name));
    }
    if (v.treasury.targets == "demand" && !v.pool.configured) {
      fail_here(fmt::format(
          "venues.{}.treasury: targets = \"demand\" funds each account for its home instruments; "
          "it needs a [venues.{}.pool] table",
          v.name,
          v.name));
    }
    if (!v.treasury.weights.empty()) {
      double sum = 0;
      for (const VenueId a : members) {
        const auto it = v.treasury.weights.find(cfg.venues[a.value].name);
        sum += it == v.treasury.weights.end() ? 1.0 : it->second;
      }
      if (sum <= 0) fail_here(fmt::format("venues.{}.treasury.weights: all zero", v.name));
    }
  }
}

// [gateway.shared."<venue>:<symbol>"] primary = "<engine name>": instruments of [[instruments]]
// that several strategies may trade at once.
void get_shared(const toml::table& gateway, Config& cfg) {
  const toml::node* n = gateway.get("shared");
  if (n == nullptr) return;
  const auto* t = n->as_table();
  if (t == nullptr)
    fail_at(*n, "gateway.shared must be a table: [gateway.shared.\"venue:symbol\"]");
  for (const auto& [k, v] : *t) {
    const std::string where(k.str());
    const auto* e = v.as_table();
    if (e == nullptr)
      fail_at(v,
              fmt::format("gateway.shared.\"{}\" must be a table: [gateway.shared.\"{}\"] "
                          "primary = \"<engine name>\"",
                          where,
                          where));
    validate_table(*e, "gateway.shared.*");
    const std::size_t colon = where.find(':');
    const std::string venue = colon == std::string::npos ? std::string{} : where.substr(0, colon);
    const std::string symbol = colon == std::string::npos ? std::string{} : where.substr(colon + 1);
    if (venue.empty() || symbol.empty())
      fail_at(v, fmt::format("[gateway.shared.\"{}\"]: expected \"venue:symbol\"", where));
    if (cfg.venue(venue) == nullptr)
      fail_at(v, fmt::format("[gateway.shared.\"{}\"]: unknown venue '{}'", where, venue));
    bool listed = false;
    for (const InstrumentSection& i : cfg.instruments)
      listed = listed || (i.venue == venue && i.symbol == symbol);
    if (!listed)
      fail_at(v,
              fmt::format("[gateway.shared.\"{}\"]: {} is not in [[instruments]]", where, symbol));
    std::string primary;
    get(*e, "primary", primary);
    if (primary.size() > 63)
      fail_at(v,
              fmt::format("gateway.shared.\"{}\".primary: an [engine] name is at most 63 "
                          "characters",
                          where));
    cfg.gateway.shared[where] = primary;
  }
}

// An optional number: absent leaves `out` empty, so "not set" and "set to 0" stay distinct.
void get_optional(const toml::table& t, std::string_view key, std::optional<double>& out) {
  if (const auto* n = t.get(key)) {
    if (const auto v = n->value<double>()) out = v;
  }
}

bool looks_like_inline_secret(std::string_view key, std::string_view value) noexcept {
  const bool secret_key = key.find("secret") != std::string_view::npos ||
                          key.find("key") != std::string_view::npos ||
                          key.find("token") != std::string_view::npos ||
                          key.find("password") != std::string_view::npos ||
                          key.find("passphrase") != std::string_view::npos;
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
                                                        "ab",
                                                        "risk",
                                                        "capacity",
                                                        "gateway",
                                                        "accounting",
                                                        "logging",
                                                        "shadow",
                                                        "sim",
                                                        "backtest",
                                                        "storage"};
  for (const auto& [k, v] : doc) {
    bool known = false;
    for (auto s : kKnownSections) known = known || s == k.str();
    if (!known) fail_at(v, fmt::format("unknown section [{}]", k.str()));
  }

  // [engine]
  if (const auto* t = doc["engine"].as_table()) {
    validate_table(*t, "engine");
    auto& e = cfg.engine;
    get(*t, "name", e.name);
    get(*t, "cpu", e.cpu);
    if (const auto* arr = t->get_as<toml::array>("net_cpus")) {
      for (const auto& n : *arr)
        e.net_cpus.push_back(static_cast<int>(n.value_or<std::int64_t>(-1)));
    }
    if (const auto* arr = t->get_as<toml::array>("aux_cpus")) {
      for (const auto& n : *arr) {
        const std::int64_t c = n.value_or<std::int64_t>(-1);
        if (c < 0 || c >= 1024) fail_at(n, "aux_cpus entries must be CPU numbers, 0 to 1023");
        e.aux_cpus.push_back(static_cast<int>(c));
      }
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
    get(*t, "checkpoint_s", e.checkpoint_s);
    if (e.checkpoint_s < 0)
      fail_at(*t->get("checkpoint_s"), "checkpoint_s must be >= 0 (0 disables)");
    get(*t, "epoch_file", e.epoch_file);
    get(*t, "kill_file", e.kill_file);
    get(*t, "instance_lock", e.instance_lock);
    get(*t, "lock_file", e.lock_file);
    get(*t, "handoff_timeout_ms", e.handoff_timeout_ms);
    if (e.handoff_timeout_ms <= 0)
      fail_at(*t->get("handoff_timeout_ms"), "handoff_timeout_ms must be > 0");
    if (const auto* arr = t->get_as<toml::array>("markout_ms")) {
      for (const toml::node& n : *arr) {
        const std::int64_t ms = n.value_or<std::int64_t>(0);
        if (ms <= 0 || ms > 86'400'000) fail_at(n, "markout_ms: each horizon is 1 to 86400000 ms");
        e.markout_ms.push_back(ms);
      }
      if (e.markout_ms.size() > 4) fail_at(*arr, "markout_ms: at most 4 horizons");
    }
    get(*t, "stall_ms", e.stall_ms);
    if (e.stall_ms < 0) fail_at(*t->get("stall_ms"), "stall_ms must be >= 0 (0 disables)");
    get(*t, "rng_seed", e.rng_seed);
    get(*t, "md_ring_bytes", e.md_ring_bytes);
    get(*t, "order_ring_bytes", e.order_ring_bytes);
    get(*t, "journal_ring_bytes", e.journal_ring_bytes);
    get(*t, "max_events_per_step", e.max_events_per_step);
    get(*t, "feed_budget_per_ring", e.feed_budget_per_ring);
    if (e.feed_budget_per_ring == 0)
      fail_at(*t->get("feed_budget_per_ring"), "feed_budget_per_ring must be >= 1");
    get(*t, "net_spin_dedicated", e.net_spin_dedicated);
    get(*t, "crossed_grace_ms", e.crossed_grace_ms);
    get(*t, "ack_timeout_ms", e.ack_timeout_ms);
    if (e.ack_timeout_ms < 0)
      fail_at(*t->get("ack_timeout_ms"), "ack_timeout_ms must be >= 0 (0 disables)");
    get(*t, "flatten_interval_ms", e.flatten_interval_ms);
    if (e.flatten_interval_ms <= 0)
      fail_at(*t->get("flatten_interval_ms"), "flatten_interval_ms must be > 0");
    get(*t, "flatten_timeout_ms", e.flatten_timeout_ms);
    if (e.flatten_timeout_ms < 0)
      fail_at(*t->get("flatten_timeout_ms"), "flatten_timeout_ms must be >= 0 (0 never gives up)");
    get(*t, "flatten_slippage_bps", e.flatten_slippage_bps);
    if (e.flatten_slippage_bps < 0)
      fail_at(*t->get("flatten_slippage_bps"), "flatten_slippage_bps must be >= 0");
    get(*t, "queue_conservatism", e.queue_conservatism);
    if (!(e.queue_conservatism >= 0.0 && e.queue_conservatism <= 1.0))
      fail_at(*t->get("queue_conservatism"), "queue_conservatism must be in [0, 1]");
    get(*t, "latency_publish_ms", e.latency_publish_ms);
    get(*t, "tsc_recalibrate_s", e.tsc_recalibrate_s);
    if (e.tsc_recalibrate_s < 0)
      fail_at(*t->get("tsc_recalibrate_s"), "tsc_recalibrate_s must be >= 0 (0 disables)");
    get(*t, "timer_slack_ns", e.timer_slack_ns);
    if (e.timer_slack_ns < 0)
      fail_at(*t->get("timer_slack_ns"), "timer_slack_ns must be >= 0 (0 keeps the kernel's)");
    get(*t, "lock_memory", e.lock_memory);
    if (const auto* n = t->get("cpu_dma_latency_us")) {
      const auto v = n->value<std::int64_t>();
      if (v && (*v < -1 || *v > std::numeric_limits<std::int32_t>::max()))
        fail_at(*n, "cpu_dma_latency_us must be -1 (no request) or 0..2147483647");
      get(*t, "cpu_dma_latency_us", e.cpu_dma_latency_us);
    }
    get(*t, "rt_priority", e.rt_priority);
    if (e.rt_priority < 0 || e.rt_priority > 99)
      fail_at(*t->get("rt_priority"), "rt_priority must be 0 (off) or 1..99");
    get(*t, "net_rt_priority", e.net_rt_priority);
    if (e.net_rt_priority < 0 || e.net_rt_priority > 99)
      fail_at(*t->get("net_rt_priority"), "net_rt_priority must be 0 (off) or 1..99");
    get(*t, "log_irq_affinity", e.log_irq_affinity);
    get(*t, "restore_position", e.restore_position);
    get(*t, "min_requote_ticks", e.min_requote_ticks);
    get(*t, "min_requote_interval_ms", e.min_requote_interval_ms);
    get(*t, "min_qty_bps", e.min_qty_bps);
    get(*t, "reject_backoff_ms", e.reject_backoff_ms);
    get(*t, "reject_backoff_max_ms", e.reject_backoff_max_ms);
    get(*t, "quote_token_reserve", e.quote_token_reserve);
    if (e.quote_token_reserve < 0)
      fail_at(*t->get("quote_token_reserve"), "quote_token_reserve must be >= 0");
    get(*t, "send_token_reserve", e.send_token_reserve);
    if (e.send_token_reserve < 0)
      fail_at(*t->get("send_token_reserve"), "send_token_reserve must be >= 0");
    get(*t, "quote_room_reserve", e.quote_room_reserve);
    if (e.quote_room_reserve < -1)
      fail_at(*t->get("quote_room_reserve"), "quote_room_reserve must be >= -1");
    get(*t, "post_only", e.post_only);
    get(*t, "supports_replace", e.supports_replace);
    get(*t, "on_kill", e.on_kill);
    if (e.on_kill != "exit" && e.on_kill != "stay")
      fail_at(*t->get("on_kill"), "on_kill must be exit|stay");
    get(*t, "cancel_unknown", e.cancel_unknown);
    if (e.cancel_unknown != "all" && e.cancel_unknown != "own")
      fail_at(*t->get("cancel_unknown"), "cancel_unknown must be all|own");
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
      validate_table(*t, "venues.*", /*check_unknown=*/false);
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
      str("api_passphrase", v.api_passphrase);
      str("ca_file", v.ca_file);
      get(*t, "testnet", v.testnet);
      get(*t, "supports_replace", v.supports_replace);
      get(*t, "public_only", v.public_only);
      get(*t, "pool_of", v.pool_of);
      get(*t, "reduce_only", v.reduce_only);
      get(*t, "insecure_tls", v.insecure_tls);
      get(*t, "recv_window_ms", v.recv_window_ms);
      get(*t, "source_ip", v.source_ip);
      get(*t, "source_interface", v.source_interface);
      if (!v.source_ip.empty() && !v.source_interface.empty())
        fail_at(*t->get("source_interface"),
                fmt::format("venues.{}: source_ip and source_interface are exclusive: name one",
                            name.str()));
      if (!v.source_ip.empty() && !valid_ip_literal(v.source_ip))
        fail_at(*t->get("source_ip"),
                fmt::format("venues.{}.source_ip '{}' is not an IPv4 or IPv6 address",
                            name.str(),
                            v.source_ip));
      if (v.source_interface.size() >= 16)
        fail_at(*t->get("source_interface"),
                fmt::format("venues.{}.source_interface '{}' is longer than an interface name",
                            name.str(),
                            v.source_interface));
      get(*t, "md_copies", v.md_copies);
      if (v.md_copies < 0 || v.md_copies > kMaxMdCopies)
        fail_at(*t->get("md_copies"),
                fmt::format("venues.{}.md_copies must be 0 to {}", name.str(), kMaxMdCopies));
      if (const auto* arr = t->get_as<toml::array>("md_copy_cpus")) {
        for (const auto& n : *arr)
          v.md_copy_cpus.push_back(static_cast<int>(n.value_or<std::int64_t>(-1)));
        if (std::cmp_greater(v.md_copy_cpus.size(), v.md_copies))
          fail_at(*t->get("md_copy_cpus"),
                  fmt::format("venues.{}.md_copy_cpus names {} cores for {} md_copies",
                              name.str(),
                              v.md_copy_cpus.size(),
                              v.md_copies));
      }
      get(*t, "fill_audit_interval_s", v.fill_audit_interval_s);
      get(*t, "fill_audit_lag_s", v.fill_audit_lag_s);
      get(*t, "fill_audit_mode", v.fill_audit_mode);
      if (v.fill_audit_interval_s != 0 && v.fill_audit_interval_s < kMinFillAuditIntervalS)
        fail_at(*t->get("fill_audit_interval_s"),
                fmt::format("venues.{}.fill_audit_interval_s must be 0 (off) or at least {}",
                            name.str(),
                            kMinFillAuditIntervalS));
      if (v.fill_audit_lag_s < 0)
        fail_at(*t->get("fill_audit_lag_s"),
                fmt::format("venues.{}.fill_audit_lag_s must be >= 0", name.str()));
      if (v.fill_audit_mode != "report" && v.fill_audit_mode != "book")
        fail_at(*t->get("fill_audit_mode"),
                fmt::format(R"(venues.{}.fill_audit_mode must be "report" or "book")", name.str()));
      if (const auto* fees = t->get_as<toml::table>("fees")) {
        validate_table(*fees, "venues.*.fees");
        get(*fees, "maker_bps", v.fees.maker_bps);
        get(*fees, "taker_bps", v.fees.taker_bps);
      }
      if (const auto* tr = t->get_as<toml::table>("treasury")) parse_treasury(*tr, v);
      if (const auto* pl = t->get_as<toml::table>("pool")) parse_pool(*pl, v);
      if (const auto* tr = t->get_as<toml::table>("transfers")) parse_transfers(*tr, v);
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
      validate_table(*t, "instruments[]");
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
      get_decimal(*t, "initial_margin", i.initial_margin);
      if (cfg.venue(i.venue) == nullptr)
        fail_at(*t->get("venue"),
                fmt::format("instrument '{}' references unknown venue '{}'", i.symbol, i.venue));
      cfg.instruments.push_back(std::move(i));
    }
  }

  // Account pools ([venues.<x>] pool_of): the member and its primary are the same kind of venue,
  // a member is nobody's primary, trades no instrument of its own and is not market data only.
  {
    PoolPlan plan;
    for (const VenueSection& v : cfg.venues) {
      if (v.pool_of.empty()) continue;
      const toml::node* at = doc["venues"][v.name]["pool_of"].node();
      const auto fail_pool = [&](const std::string& msg) {
        if (at != nullptr) fail_at(*at, msg);
        throw ConfigError(msg);
      };
      const VenueSection* primary = cfg.venue(v.pool_of);
      if (primary == nullptr) {
        fail_pool(fmt::format("venues.{}.pool_of: unknown venue '{}'", v.name, v.pool_of));
        continue;
      }
      if (primary == &v) {
        fail_pool(fmt::format("venues.{}.pool_of: a venue cannot be its own primary", v.name));
      }
      if (!primary->pool_of.empty()) {
        fail_pool(
            fmt::format("venues.{}.pool_of: '{}' is itself a member of pool '{}'; name the primary",
                        v.name,
                        v.pool_of,
                        primary->pool_of));
      }
      if (primary->kind != v.kind) {
        fail_pool(fmt::format("venues.{}.pool_of: kind '{}' differs from the primary's '{}'",
                              v.name,
                              v.kind,
                              primary->kind));
      }
      if (v.public_only) {
        fail_pool(fmt::format("venues.{}: a pool member takes orders; public_only cannot be set",
                              v.name));
      }
      for (const InstrumentSection& i : cfg.instruments) {
        if (i.venue != v.name) continue;
        fail_pool(
            fmt::format("venues.{}: a pool member has no instruments of its own; put {} on "
                        "the primary '{}'",
                        v.name,
                        i.symbol,
                        v.pool_of));
      }
      if (!plan.add(cfg.venue_id(v.name), cfg.venue_id(v.pool_of))) {
        fail_pool(fmt::format("venues.{}.pool_of: pool '{}' has more than {} accounts",
                              v.name,
                              v.pool_of,
                              PoolPlan::kMaxMembers));
      }
    }
  }
  check_treasuries(doc, cfg);
  check_pools(doc, cfg);

  // [strategy]
  if (const auto* t = doc["strategy"].as_table()) {
    validate_table(*t, "strategy");
    get(*t, "name", cfg.strategy.name);
    get(*t, "max_param_age_ms", cfg.strategy.max_param_age_ms);
    if (cfg.strategy.max_param_age_ms < 0)
      fail_at(*t->get("max_param_age_ms"), "max_param_age_ms must be >= 0 (0 disables)");
    get(*t, "state_file", cfg.strategy.state_file);
    get(*t, "state_interval_s", cfg.strategy.state_interval_s);
    if (cfg.strategy.state_interval_s <= 0)
      fail_at(*t->get("state_interval_s"), "state_interval_s must be > 0");
    if (const auto* p = t->get_as<toml::table>("params")) {
      for (const auto& [k, v] : *p) {
        if (v.is_table() || v.is_array())
          fail_at(v, fmt::format("strategy.params.{} must be a scalar", k.str()));
        cfg.strategy.params[std::string(k.str())] = stringify(v);
      }
    }
    if (const auto* g = t->get_as<toml::table>("groups")) {
      for (const auto& [k, v] : *g) {
        const std::string name(k.str());
        if (!plain_name(name, 32, "_-."))
          fail_at(v, "strategy.groups: a group name is 1 to 32 letters, digits, '_', '-' or '.'");
        const toml::array* list = v.as_array();
        if (list == nullptr || list->empty())
          fail_at(v, fmt::format("strategy.groups.{} must be a list of instruments", name));
        std::vector<std::string>& members = cfg.strategy.groups[name];
        for (const toml::node& m : *list) {
          const std::optional<std::string> sym = m.value<std::string>();
          if (!sym || sym->empty())
            fail_at(m, fmt::format("strategy.groups.{}: \"symbol\" or \"venue:symbol\"", name));
          members.push_back(*sym);
        }
      }
    }
    if (const auto* o = t->get_as<toml::table>("overrides")) {
      for (const auto& [k, v] : *o) {
        const toml::table* table = v.as_table();
        if (table == nullptr)
          fail_at(v, fmt::format("strategy.overrides.\"{}\" must be a table", k.str()));
        ParamMap& values = cfg.strategy.overrides[std::string(k.str())];
        for (const auto& [name, value] : *table) {
          if (value.is_table() || value.is_array())
            fail_at(
                value,
                fmt::format("strategy.overrides.\"{}\".{} must be a scalar", k.str(), name.str()));
          values[std::string(name.str())] = stringify(value);
        }
      }
    }
  } else if (doc.contains("strategy")) {
    fail_at(*doc.get("strategy"), "[strategy] must be a table");
  }

  // [ab]
  if (const auto* t = doc["ab"].as_table()) {
    parse_ab(*t, cfg.ab);
  } else if (doc.contains("ab")) {
    fail_at(*doc.get("ab"), "[ab] must be a table");
  }

  // [risk]
  if (const auto* t = doc["risk"].as_table()) {
    validate_table(*t, "risk");
    auto& r = cfg.risk;
    get_decimal(*t, "max_order_qty", r.max_order_qty);
    get_decimal(*t, "max_order_notional", r.max_order_notional);
    get_decimal(*t, "max_position", r.max_position);
    get(*t, "max_open_orders", r.max_open_orders);
    get(*t, "price_collar_bps", r.price_collar_bps);
    get(*t, "fat_finger_bps", r.fat_finger_bps);
    get(*t, "stale_md_ms", r.stale_md_ms);
    get_decimal(*t, "max_loss", r.max_loss);
    get(*t, "max_loss_period", r.max_loss_period);
    if (r.max_loss_period != "total" && r.max_loss_period != "utc_day")
      throw ConfigError("risk.max_loss_period must be \"total\" or \"utc_day\"");
    get_decimal(*t, "max_gross_notional", r.max_gross_notional);
    get_decimal(*t, "max_net_notional", r.max_net_notional);
    get(*t, "orders_per_sec", r.orders_per_sec);
    get(*t, "burst", r.burst);
    get(*t, "stp", r.stp);
    get(*t, "max_feed_lag_ms", r.max_feed_lag_ms);
    if (r.max_feed_lag_ms < 0) throw ConfigError("risk.max_feed_lag_ms must be >= 0");
    get(*t, "fill_cluster_names", r.fill_cluster_names);
    get(*t, "fill_cluster_window_ms", r.fill_cluster_window_ms);
    get(*t, "fill_cluster_hold_ms", r.fill_cluster_hold_ms);
    if (r.fill_cluster_names < 0 ||
        r.fill_cluster_names > static_cast<int>(FillClusterConfig::kMaxNames))
      throw ConfigError(fmt::format("risk.fill_cluster_names must be 0 (off) to {}",
                                    FillClusterConfig::kMaxNames));
    if (r.fill_cluster_window_ms < 0 || r.fill_cluster_hold_ms < 0)
      throw ConfigError("risk.fill_cluster_window_ms and fill_cluster_hold_ms must be >= 0");
    get(*t, "check_balance", r.check_balance);
    get(*t, "allow_unlimited", r.allow_unlimited);
    get(*t, "reduce_only", r.reduce_only);
    get_underlying(*t, "risk", r.underlying);
  }

  // [capacity]
  if (const auto* t = doc["capacity"].as_table()) {
    validate_table(*t, "capacity");
    CapacitySection& c = cfg.capacity;
    c.configured = true;
    get(*t, "mode", c.mode);
    if (c.mode != "off" && c.mode != "observe" && c.mode != "new" && c.mode != "enforce")
      fail_at(*t->get("mode"),
              "capacity.mode must be \"off\", \"observe\", \"new\" or \"enforce\"");
    get(*t, "value", c.value);
    if (c.value != "off" && c.value != "learned")
      fail_at(*t->get("value"), "capacity.value must be \"off\" or \"learned\"");
    const auto ranged =
        [&](std::string_view key, std::int64_t& out, std::int64_t lo, std::int64_t hi) {
          const toml::node* n = t->get(key);
          if (n == nullptr) return;
          get(*t, key, out);
          if (out < lo || out > hi)
            fail_at(*n, fmt::format("capacity.{} must be {} to {}", key, lo, hi));
        };
    ranged("orders_10s", c.orders_10s, 0, 1'000'000);
    ranged("window_ms", c.window_ms, 100, 3'600'000);
    ranged("orders_1d", c.orders_1d, 0, 1'000'000'000);
    ranged("orders_per_day", c.orders_per_day, 0, 1'000'000'000);
    ranged("weight_1m", c.weight_1m, 0, 100'000'000);
    get(*t, "fill_credit", c.fill_credit);
    const auto positive = [&](std::string_view key, double& out) {
      const toml::node* n = t->get(key);
      if (n == nullptr) return;
      const auto x = n->value<double>();
      if (!x || !(*x > 0.0) || *x > 1e6)
        fail_at(*n, fmt::format("capacity.{} must be a number above 0", key));
      out = *x;
    };
    positive("margin_bps", c.margin_bps);
    positive("full_move_bps", c.full_move_bps);
    if (const auto* arr = t->get_as<toml::array>("venues")) {
      for (const auto& n : *arr) {
        const auto name = n.value<std::string>();
        if (!name || cfg.venue(*name) == nullptr)
          fail_at(n, "capacity.venues: each entry must name a [venues.<name>]");
        c.venues.push_back(*name);
      }
    }
  } else if (doc.contains("capacity")) {
    fail_at(*doc.get("capacity"), "[capacity] must be a table");
  }

  // [gateway]
  if (const auto* t = doc["gateway"].as_table()) {
    validate_table(*t, "gateway");
    get(*t, "orders_per_sec", cfg.gateway.orders_per_sec);
    get(*t, "burst", cfg.gateway.burst);
    get_decimal(*t, "max_open_notional", cfg.gateway.max_open_notional);
    get_decimal(*t, "max_loss", cfg.gateway.max_loss);
    get_decimal(*t, "max_gross_notional", cfg.gateway.max_gross_notional);
    get_decimal(*t, "max_net_notional", cfg.gateway.max_net_notional);
    get_underlying(*t, "gateway", cfg.gateway.underlying);
    get_shared(*t, cfg);
    get(*t, "check_balance", cfg.gateway.check_balance);
  }

  // [accounting]: after [[instruments]], whose entries the FX sources must name.
  if (const auto* t = doc["accounting"].as_table()) {
    validate_table(*t, "accounting");
    AccountingSpec& a = cfg.accounting;
    get(*t, "reporting_currency", a.reporting_currency);
    get(*t, "mark", a.mark);
    if (a.mark != "venue" && a.mark != "mid")
      fail_at(*t->get("mark"), "accounting.mark must be \"venue\" or \"mid\"");
    get(*t, "stale_mark_ms", a.stale_mark_ms);
    if (a.stale_mark_ms <= 0) fail_at(*t->get("stale_mark_ms"), "stale_mark_ms must be > 0");
    get(*t, "stale_funding_ms", a.stale_funding_ms);
    if (a.stale_funding_ms <= 0)
      fail_at(*t->get("stale_funding_ms"), "stale_funding_ms must be > 0");
    get(*t, "stale_fx_ms", a.stale_fx_ms);
    if (a.stale_fx_ms < 0) fail_at(*t->get("stale_fx_ms"), "stale_fx_ms must be >= 0");
    const auto valid_ccy = [](std::string_view c) {
      if (c.empty() || c.size() > Currency::kCapacity) return false;
      for (const char ch : c) {
        if (ch == ':' || ch == ' ' || ch == '"') return false;
      }
      return true;
    };
    if (const auto* n = t->get("reporting_currency");
        n != nullptr && !valid_ccy(a.reporting_currency))
      fail_at(*n,
              fmt::format("accounting.reporting_currency must be 1 to {} characters",
                          Currency::kCapacity));
    if (const auto* fx = t->get_as<toml::table>("fx")) {
      if (a.reporting_currency.empty())
        fail_at(*fx, "[accounting.fx] needs [accounting] reporting_currency");
      for (const auto& [k, v] : *fx) {
        const std::string ccy(k.str());
        if (!v.is_string())
          fail_at(v, fmt::format("accounting.fx.{} must be a string, \"venue:symbol\"", ccy));
        if (!valid_ccy(ccy))
          fail_at(v,
                  fmt::format("accounting.fx: currency '{}' must be 1 to {} characters",
                              ccy,
                              Currency::kCapacity));
        if (ccy == a.reporting_currency)
          fail_at(v, fmt::format("accounting.fx.{}: the reporting currency needs no source", ccy));
        const std::string where = v.value_or(std::string{});
        const std::size_t colon = where.find(':');
        const std::string venue =
            colon == std::string::npos ? std::string{} : where.substr(0, colon);
        const std::string symbol =
            colon == std::string::npos ? std::string{} : where.substr(colon + 1);
        if (venue.empty() || symbol.empty())
          fail_at(v,
                  fmt::format("accounting.fx.{} = \"{}\": expected \"venue:symbol\"", ccy, where));
        if (cfg.venue(venue) == nullptr)
          fail_at(v,
                  fmt::format("accounting.fx.{} = \"{}\": unknown venue '{}'", ccy, where, venue));
        bool listed = false;
        for (const InstrumentSection& i : cfg.instruments)
          listed = listed || (i.venue == venue && i.symbol == symbol);
        if (!listed)
          fail_at(v,
                  fmt::format("accounting.fx.{} = \"{}\": {} is not in [[instruments]]; the "
                              "source must be an instrument the session subscribes to (enabled "
                              "= false if it is not traded)",
                              ccy,
                              where,
                              symbol));
        a.fx[ccy] = where;
      }
    }
    if (a.fx.size() >= kMaxCurrencies)
      fail_at(*t, fmt::format("[accounting.fx]: at most {} sources", kMaxCurrencies - 1));
  } else if (doc.contains("accounting")) {
    fail_at(*doc.get("accounting"), "[accounting] must be a table");
  }

  // [logging]
  if (const auto* t = doc["logging"].as_table()) {
    validate_table(*t, "logging");
    get(*t, "level", cfg.logging.level);
    get(*t, "file", cfg.logging.file);
    get(*t, "mirror_level", cfg.logging.mirror_level);
    get(*t, "status_interval_s", cfg.logging.status_interval_s);
    if (t->contains("status_interval_s") && cfg.logging.status_interval_s < 0)
      fail_at(*t->get("status_interval_s"), "status_interval_s must be >= 0 (0 = none)");
    parse_level(cfg.logging.level);
    parse_level(cfg.logging.mirror_level);
  }

  if (const auto* t = doc["shadow"].as_table()) {
    validate_table(*t, "shadow");
    ShadowSection& s = cfg.shadow;
    s.enabled = t->at_path("enabled").value_or(s.enabled);
    s.threads = static_cast<int>(t->at_path("threads").value_or(std::int64_t{s.threads}));
    s.window_s = static_cast<int>(t->at_path("window_s").value_or(std::int64_t{s.window_s}));
    s.settle_s = static_cast<int>(t->at_path("settle_s").value_or(std::int64_t{s.settle_s}));
    s.max_lag_s = static_cast<int>(t->at_path("max_lag_s").value_or(std::int64_t{s.max_lag_s}));
    s.nice = static_cast<int>(t->at_path("nice").value_or(std::int64_t{s.nice}));
    s.idle_exit_s =
        static_cast<int>(t->at_path("idle_exit_s").value_or(std::int64_t{s.idle_exit_s}));
    s.program = t->at_path("program").value_or(s.program);
    s.report = t->at_path("report").value_or(s.report);
    s.status = t->at_path("status").value_or(s.status);
    if (s.threads < 1) fail_at(*t->get("threads"), "shadow.threads must be >= 1");
    if (s.window_s < 1) fail_at(*t->get("window_s"), "shadow.window_s must be >= 1");
    if (s.settle_s < 0) fail_at(*t->get("settle_s"), "shadow.settle_s must be >= 0");
    if (s.max_lag_s < 0) fail_at(*t->get("max_lag_s"), "shadow.max_lag_s must be >= 0");
    if (const auto* a = t->get_as<toml::array>("cpus")) {
      for (const toml::node& n : *a)
        s.cpus.push_back(static_cast<int>(n.value_or(std::int64_t{-1})));
    }
    if (const auto* grid = t->get_as<toml::table>("grid")) {
      for (const auto& [k, v] : *grid) {
        const auto* values = v.as_array();
        if (values == nullptr || values->empty())
          fail_at(v, fmt::format("shadow.grid.{} must be a non-empty array", k.str()));
        std::vector<std::string> out;
        for (const toml::node& n : *values) out.push_back(stringify(n));
        s.grid.emplace_back(std::string(k.str()), std::move(out));
      }
    }
    if (const auto* points = t->get("points")) {
      const auto* list = points->as_array();
      if (list == nullptr) fail_at(*points, "shadow.points must be [[shadow.points]] tables");
      for (const toml::node& n : *list) {
        if (!n.is_table()) fail_at(n, "shadow.points must be [[shadow.points]] tables");
        std::map<std::string, std::string> p;
        for (const auto& [k, v] : *n.as_table()) p[std::string(k.str())] = stringify(v);
        s.points.push_back(std::move(p));
      }
    }
  } else if (doc.contains("shadow")) {
    fail_at(*doc.get("shadow"), "[shadow] must be a table");
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

PoolPlan Config::pool_plan() const noexcept {
  PoolPlan plan;
  for (const VenueSection& v : venues) {
    if (!v.pool_of.empty()) static_cast<void>(plan.add(venue_id(v.name), venue_id(v.pool_of)));
  }
  return plan;
}

std::uint32_t Config::reduce_only_venues() const noexcept {
  std::uint32_t mask = 0;
  for (const VenueSection& v : venues) {
    const VenueId id = venue_id(v.name);
    if (v.reduce_only && id.valid() && id.value < 32U) mask |= 1U << id.value;
  }
  return mask;
}

TreasuryConfig Config::treasury_config(std::string_view primary) const {
  TreasuryConfig c;
  const VenueSection* v = venue(primary);
  if (v == nullptr || !v->treasury.configured) return c;
  const TreasurySection& s = v->treasury;
  c.enabled = s.enabled;
  c.dry_run = s.dry_run;
  c.asset = s.asset;
  c.targets = s.targets == "demand" ? TreasuryTargets::Demand : TreasuryTargets::Weights;
  c.headroom = s.headroom;
  c.reserve = s.reserve;
  c.members = pool_plan().members(venue_id(primary));
  for (std::size_t i = 0; i < c.members.size(); ++i) {
    const std::string& name = venues[c.members[i].value].name;
    c.names[i] = name;
    if (const auto w = s.weights.find(name); w != s.weights.end()) c.weight[i] = w->second;
    if (const auto m = s.min_free.find(name); m != s.min_free.end())
      c.min_free[i] = Notional::parse(m->second).value_or(Notional{});
  }
  c.threshold = s.threshold;
  c.min_amount = Notional::parse(s.min_amount).value_or(Notional{});
  c.max_amount = Notional::parse(s.max_amount).value_or(Notional{});
  c.step = Notional::parse(s.step).value_or(Notional{});
  constexpr std::int64_t kS = 1'000'000'000;
  c.interval_ns = s.interval_s * kS;
  c.min_interval_ns = s.min_interval_s * kS;
  c.max_per_hour = static_cast<std::uint32_t>(s.max_per_hour);
  c.cooldown_ns = s.cooldown_s * kS;
  c.timeout_ns = s.timeout_s * kS;
  c.settle_ns = s.settle_s * kS;
  c.state_file = !s.state_file.empty()
                     ? s.state_file
                     : engine.journal_dir + "/" + engine.name + "." + v->name + ".treasury";
  return c;
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
  l.max_gross_notional = parse_fixed<Notional>(risk.max_gross_notional, "max_gross_notional");
  l.max_net_notional = parse_fixed<Notional>(risk.max_net_notional, "max_net_notional");
  l.orders_per_sec = static_cast<std::uint32_t>(risk.orders_per_sec);
  l.burst = static_cast<std::uint32_t>(risk.burst);
  l.stp = risk.stp;
  l.max_feed_lag_ms = static_cast<std::uint32_t>(risk.max_feed_lag_ms);
  return l;
}

FillClusterConfig Config::fill_cluster_config() const {
  FillClusterConfig c;
  c.names = static_cast<std::uint32_t>(risk.fill_cluster_names);
  c.window = milliseconds(risk.fill_cluster_window_ms);
  c.hold = milliseconds(risk.fill_cluster_hold_ms);
  return c;
}

std::string Config::unlimited_risk() const {
  if (risk.allow_unlimited) return {};
  const RiskLimits l = risk_limits();
  std::string missing;
  if (!l.max_position.is_positive() && !l.max_order_notional.is_positive())
    missing = "max_position (or max_order_notional)";
  if (!l.max_loss.is_positive()) missing += missing.empty() ? "max_loss" : " and max_loss";
  if (missing.empty()) return {};
  return "[risk] sets no " + missing +
         ", and this session trades with real keys. Set them, or allow_unlimited = true in [risk] "
         "to trade without";
}

CapacityConfig Config::capacity_config() const {
  CapacityConfig c;
  const CapacitySection& s = capacity;
  c.mode = s.mode == "observe"   ? CapacityMode::Observe
           : s.mode == "new"     ? CapacityMode::New
           : s.mode == "enforce" ? CapacityMode::Enforce
                                 : CapacityMode::Off;
  c.value = s.value == "learned" ? CapacityValue::Learned : CapacityValue::Off;
  c.orders_10s = s.orders_10s;
  c.window_ms = s.window_ms;
  c.fill_credit = s.fill_credit;
  c.orders_1d = s.orders_1d;
  c.orders_per_day = s.orders_per_day;
  c.weight_1m = s.weight_1m;
  c.margin_bps = s.margin_bps;
  c.full_move_bps = s.full_move_bps;
  if (!s.venues.empty()) {
    const PoolPlan plan = pool_plan();
    c.accounts = 0;
    for (const std::string& name : s.venues) {
      const VenueId v = venue_id(name);
      if (!v.valid()) continue;
      for (const VenueId m : plan.members(v))
        if (m.value < 32) c.accounts |= 1U << m.value;
      if (v.value < 32) c.accounts |= 1U << v.value;
    }
  }
  return c;
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
  q.token_reserve = engine.quote_token_reserve;
  q.send_reserve = engine.send_token_reserve;
  q.room_reserve = engine.quote_room_reserve;
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
  if (engine.checkpoint_s != 0) kv("checkpoint_s", engine.checkpoint_s);
  kq("epoch_file", engine.epoch_file);
  kq("kill_file", engine.kill_file);
  kv("instance_lock", engine.instance_lock);
  kq("lock_file", engine.lock_file);
  kv("handoff_timeout_ms", engine.handoff_timeout_ms);
  if (!engine.markout_ms.empty()) {
    std::string list;
    for (const std::int64_t ms : engine.markout_ms)
      list += (list.empty() ? "" : ", ") + std::to_string(ms);
    out += "markout_ms = [" + list + "]\n";
  }
  kv("stall_ms", engine.stall_ms);
  kv("rng_seed", engine.rng_seed);
  kv("max_events_per_step", engine.max_events_per_step);
  kv("feed_budget_per_ring", engine.feed_budget_per_ring);
  kv("net_spin_dedicated", engine.net_spin_dedicated);
  kv("crossed_grace_ms", engine.crossed_grace_ms);
  kv("ack_timeout_ms", engine.ack_timeout_ms);
  kv("flatten_interval_ms", engine.flatten_interval_ms);
  kv("flatten_timeout_ms", engine.flatten_timeout_ms);
  kv("flatten_slippage_bps", engine.flatten_slippage_bps);
  kv("queue_conservatism", engine.queue_conservatism);
  kv("min_requote_ticks", engine.min_requote_ticks);
  kv("min_requote_interval_ms", engine.min_requote_interval_ms);
  kv("min_qty_bps", engine.min_qty_bps);
  kv("reject_backoff_ms", engine.reject_backoff_ms);
  kv("reject_backoff_max_ms", engine.reject_backoff_max_ms);
  kv("quote_token_reserve", engine.quote_token_reserve);
  if (engine.send_token_reserve != 0) kv("send_token_reserve", engine.send_token_reserve);
  if (engine.quote_room_reserve != 0) kv("quote_room_reserve", engine.quote_room_reserve);
  kv("post_only", engine.post_only);
  kv("supports_replace", engine.supports_replace);
  kq("on_kill", engine.on_kill);
  if (engine.cancel_unknown != "all") kq("cancel_unknown", engine.cancel_unknown);
  for (const auto& v : venues) {
    fmt::format_to(std::back_inserter(out), "\n[venues.{}]\n", v.name);
    kq("kind", v.kind);
    if (!v.ws_url.empty()) kq("ws_url", v.ws_url);
    if (!v.ws_api_url.empty()) kq("ws_api_url", v.ws_api_url);
    if (!v.rest_url.empty()) kq("rest_url", v.rest_url);
    if (!v.api_key.empty()) kq("api_key", "***");
    if (!v.api_secret.empty()) kq("api_secret", "***");
    if (!v.api_passphrase.empty()) kq("api_passphrase", "***");
    kv("testnet", v.testnet);
    kv("supports_replace", v.supports_replace);
    kv("public_only", v.public_only);
    if (!v.pool_of.empty()) kq("pool_of", v.pool_of);
    if (v.reduce_only) kv("reduce_only", v.reduce_only);
    kv("insecure_tls", v.insecure_tls);
    if (!v.ca_file.empty()) kq("ca_file", v.ca_file);
    kv("recv_window_ms", v.recv_window_ms);
    if (!v.source_ip.empty()) kq("source_ip", v.source_ip);
    if (!v.source_interface.empty()) kq("source_interface", v.source_interface);
    if (v.md_copies > 0) kv("md_copies", v.md_copies);
    if (!v.md_copy_cpus.empty())
      fmt::format_to(
          std::back_inserter(out), "md_copy_cpus = [{}]\n", fmt::join(v.md_copy_cpus, ", "));
    if (v.fill_audit_interval_s > 0) {
      kv("fill_audit_interval_s", v.fill_audit_interval_s);
      kv("fill_audit_lag_s", v.fill_audit_lag_s);
      kq("fill_audit_mode", v.fill_audit_mode);
    }
    fmt::format_to(std::back_inserter(out),
                   "fees = {{ maker_bps = {}, taker_bps = {} }}\n",
                   v.fees.maker_bps,
                   v.fees.taker_bps);
    if (v.treasury.configured) {
      fmt::format_to(std::back_inserter(out),
                     "treasury = {{ enabled = {}, dry_run = {}, asset = \"{}\", targets = \"{}\", "
                     "reserve = {} }}\n",
                     v.treasury.enabled,
                     v.treasury.dry_run,
                     v.treasury.asset,
                     v.treasury.targets,
                     v.treasury.reserve);
    }
    if (v.pool.configured) {
      fmt::format_to(
          std::back_inserter(out),
          "pool = {{ placement = \"{}\", reduce_accounts = {}, spill = \"{}\", spill_at = {}, "
          "sweep = {}, plan_interval_s = {}, plan_lookback_h = {}, plan_max_moves = {} }} ({} "
          "pinned)\n",
          v.pool.placement,
          v.pool.reduce_accounts,
          v.pool.spill,
          v.pool.spill_at,
          v.pool.sweep,
          v.pool.plan_interval_s,
          v.pool.plan_lookback_h,
          v.pool.plan_max_moves,
          v.pool.homes.size());
    }
    if (v.transfers.configured) {
      fmt::format_to(std::back_inserter(out),
                     "transfers = {{ max_in_flight = {}, per_minute = {}, per_hour = {}, per_day = "
                     "{}, breaker_s = {}, dry_run = {} }}\n",
                     v.transfers.max_in_flight,
                     v.transfers.per_minute,
                     v.transfers.per_hour,
                     v.transfers.per_day,
                     v.transfers.breaker_s,
                     v.transfers.dry_run);
    }
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
    if (!i.initial_margin.empty()) kq("initial_margin", i.initial_margin);
  }
  out += "\n[strategy]\n";
  kq("name", strategy.name);
  if (strategy.max_param_age_ms != 0) kv("max_param_age_ms", strategy.max_param_age_ms);
  if (!strategy.state_file.empty()) kq("state_file", strategy.state_file);
  if (strategy.state_interval_s != 300) kv("state_interval_s", strategy.state_interval_s);
  if (!strategy.params.empty()) {
    out += "\n[strategy.params]\n";
    for (const auto& [k, v] : strategy.params) kq(k, v);
  }
  if (!strategy.groups.empty()) {
    out += "\n[strategy.groups]\n";
    for (const auto& [name, members] : strategy.groups) {
      std::string list;
      for (const std::string& m : members) {
        if (!list.empty()) list += ", ";
        list += fmt::format("\"{}\"", m);
      }
      fmt::format_to(std::back_inserter(out), "{} = [{}]\n", name, list);
    }
  }
  for (const auto& [key, values] : strategy.overrides) {
    fmt::format_to(std::back_inserter(out), "\n[strategy.overrides.\"{}\"]\n", key);
    for (const auto& [k, v] : values) kq(k, v);
  }
  if (ab.enabled()) {
    out += "\n[ab]\n";
    kq("name", ab.name);
    kq("assignment", ab.assignment);
    kv("seed", ab.seed);
    if (ab.block_s != 0) kv("block_s", ab.block_s);
    if (ab.block_offset_s != 0) kv("block_offset_s", ab.block_offset_s);
    if (!ab.start.empty()) kq("start", ab.start);
    if (!ab.end.empty()) kq("end", ab.end);
    if (!ab.weights.empty()) {
      out += "\n[ab.weights]\n";
      for (const auto& [k, v] : ab.weights) kv(fmt::format("\"{}\"", k), v);
    }
    if (!ab.assign.empty()) {
      out += "\n[ab.assign]\n";
      for (const auto& [k, v] : ab.assign) kq(fmt::format("\"{}\"", k), v);
    }
    for (const AbArmSection& a : ab.arms) {
      out += "\n[[ab.arms]]\n";
      kq("name", a.name);
      if (a.params.empty()) continue;
      out += "\n[ab.arms.params]\n";
      for (const auto& [k, v] : a.params) kq(k, v);
    }
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
  if (risk.max_loss_period != "total") kq("max_loss_period", risk.max_loss_period);
  if (!risk.max_gross_notional.empty()) kq("max_gross_notional", risk.max_gross_notional);
  if (!risk.max_net_notional.empty()) kq("max_net_notional", risk.max_net_notional);
  kv("orders_per_sec", risk.orders_per_sec);
  kv("burst", risk.burst);
  kv("stp", risk.stp);
  if (risk.max_feed_lag_ms != 0) kv("max_feed_lag_ms", risk.max_feed_lag_ms);
  if (risk.fill_cluster_names != 0) {
    kv("fill_cluster_names", risk.fill_cluster_names);
    kv("fill_cluster_window_ms", risk.fill_cluster_window_ms);
    kv("fill_cluster_hold_ms", risk.fill_cluster_hold_ms);
  }
  if (!risk.check_balance) kv("check_balance", risk.check_balance);
  if (risk.allow_unlimited) kv("allow_unlimited", risk.allow_unlimited);
  if (risk.reduce_only) kv("reduce_only", risk.reduce_only);
  for (const auto& [k, v] : risk.underlying.max_net) {
    fmt::format_to(std::back_inserter(out), "\n[risk.underlying.{}]\n", k);
    kq("max_net", v);
  }
  if (capacity.configured) {
    out += "\n[capacity]\n";
    kq("mode", capacity.mode);
    kq("value", capacity.value);
    kv("orders_10s", capacity.orders_10s);
    kv("window_ms", capacity.window_ms);
    kv("fill_credit", capacity.fill_credit);
    kv("orders_1d", capacity.orders_1d);
    kv("orders_per_day", capacity.orders_per_day);
    kv("weight_1m", capacity.weight_1m);
    kv("margin_bps", capacity.margin_bps);
    kv("full_move_bps", capacity.full_move_bps);
    if (!capacity.venues.empty()) {
      out += "venues = [";
      for (std::size_t k = 0; k < capacity.venues.size(); ++k)
        out += (k == 0 ? "\"" : ", \"") + capacity.venues[k] + "\"";
      out += "]\n";
    }
  }
  if (gateway.any()) {
    out += "\n[gateway]\n";
    kv("orders_per_sec", gateway.orders_per_sec);
    kv("burst", gateway.burst);
    if (!gateway.max_open_notional.empty()) kq("max_open_notional", gateway.max_open_notional);
    if (!gateway.max_loss.empty()) kq("max_loss", gateway.max_loss);
    if (!gateway.max_gross_notional.empty()) kq("max_gross_notional", gateway.max_gross_notional);
    if (!gateway.max_net_notional.empty()) kq("max_net_notional", gateway.max_net_notional);
    if (!gateway.check_balance) kv("check_balance", gateway.check_balance);
    for (const auto& [k, v] : gateway.underlying.max_net) {
      fmt::format_to(std::back_inserter(out), "\n[gateway.underlying.{}]\n", k);
      kq("max_net", v);
    }
    for (const auto& [k, v] : gateway.shared) {
      fmt::format_to(std::back_inserter(out), "\n[gateway.shared.\"{}\"]\n", k);
      if (!v.empty()) kq("primary", v);
    }
  }
  const AccountingSpec kAccountingDefaults;
  const bool perp_keys = accounting.mark != kAccountingDefaults.mark ||
                         accounting.stale_mark_ms != kAccountingDefaults.stale_mark_ms ||
                         accounting.stale_funding_ms != kAccountingDefaults.stale_funding_ms ||
                         accounting.stale_fx_ms != kAccountingDefaults.stale_fx_ms;
  if (accounting.configured() || perp_keys) {
    out += "\n[accounting]\n";
    if (accounting.configured()) kq("reporting_currency", accounting.reporting_currency);
    if (perp_keys) {
      kq("mark", accounting.mark);
      kv("stale_mark_ms", accounting.stale_mark_ms);
      kv("stale_funding_ms", accounting.stale_funding_ms);
      kv("stale_fx_ms", accounting.stale_fx_ms);
    }
    if (!accounting.fx.empty()) {
      out += "\n[accounting.fx]\n";
      for (const auto& [k, v] : accounting.fx) kq(k, v);
    }
  }
  out += "\n[logging]\n";
  kq("level", logging.level);
  if (!logging.file.empty()) kq("file", logging.file);
  kq("mirror_level", logging.mirror_level);
  if (logging.status_interval_s >= 0) kv("status_interval_s", logging.status_interval_s);
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
    t.insert_or_assign(key, text);
    return;
  }
  t.insert_or_assign(key, text);
}

// [risk.underlying] / [gateway.underlying]: max_net as the text it was given, as the other limits.
toml::table underlying_table(const UnderlyingSpec& spec) {
  toml::table t;
  for (const auto& [k, v] : spec.max_net) {
    toml::table u;
    u.insert("max_net", v);
    t.insert(k, std::move(u));
  }
  return t;
}

// [venues.<primary>.treasury], every key with its value (decimals as their text).
toml::table treasury_table(const TreasurySection& s) {
  toml::table t;
  t.insert("enabled", s.enabled);
  t.insert("dry_run", s.dry_run);
  t.insert("asset", s.asset);
  t.insert("targets", s.targets);
  t.insert("headroom", s.headroom);
  t.insert("reserve", s.reserve);
  toml::table w;
  for (const auto& [k, v] : s.weights) w.insert(k, v);
  t.insert("weights", std::move(w));
  toml::table m;
  for (const auto& [k, v] : s.min_free) m.insert(k, v);
  t.insert("min_free", std::move(m));
  t.insert("threshold", s.threshold);
  t.insert("min_amount", s.min_amount);
  t.insert("max_amount", s.max_amount);
  t.insert("step", s.step);
  t.insert("interval_s", s.interval_s);
  t.insert("min_interval_s", s.min_interval_s);
  t.insert("max_per_hour", s.max_per_hour);
  t.insert("cooldown_s", s.cooldown_s);
  t.insert("timeout_s", s.timeout_s);
  t.insert("settle_s", s.settle_s);
  if (!s.state_file.empty()) t.insert("state_file", s.state_file);
  return t;
}

// [venues.<primary>.pool], every key with its value.
toml::table pool_table(const PoolSection& s) {
  toml::table t;
  t.insert("placement", s.placement);
  t.insert("reduce_accounts", s.reduce_accounts);
  t.insert("spill", s.spill);
  t.insert("spill_at", s.spill_at);
  t.insert("plan_interval_s", s.plan_interval_s);
  t.insert("plan_lookback_h", s.plan_lookback_h);
  t.insert("plan_max_moves", s.plan_max_moves);
  toml::table h;
  for (const auto& [k, v] : s.homes) h.insert(k, v);
  t.insert("homes", std::move(h));
  if (s.sweep == "auto") {
    t.insert("sweep", s.sweep);
  } else {
    t.insert("sweep", s.sweep == "true");
  }
  t.insert("sweep_delay_ms", s.sweep_delay_ms);
  t.insert("sweep_dust_s", s.sweep_dust_s);
  t.insert("sweep_retry_s", s.sweep_retry_s);
  return t;
}

// [venues.<primary>.transfers], every key with its value.
toml::table transfers_table(const TransfersSection& s) {
  toml::table t;
  t.insert("max_in_flight", s.max_in_flight);
  t.insert("per_minute", s.per_minute);
  t.insert("per_hour", s.per_hour);
  t.insert("per_day", s.per_day);
  t.insert("breaker_s", s.breaker_s);
  t.insert("timeout_s", s.timeout_s);
  t.insert("status_interval_s", s.status_interval_s);
  t.insert("dry_run", s.dry_run);
  if (!s.state_file.empty()) t.insert("state_file", s.state_file);
  return t;
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
  if (!engine.aux_cpus.empty()) {
    toml::array aux;
    for (int c : engine.aux_cpus) aux.push_back(static_cast<std::int64_t>(c));
    e.insert("aux_cpus", std::move(aux));
  }
  e.insert("spin_mode", engine.spin_mode);
  e.insert("net_backend", engine.net_backend);
  e.insert("threading", engine.threading);
  e.insert("journal", engine.journal);
  e.insert("journal_dir", engine.journal_dir);
  e.insert("journal_sync", engine.journal_sync);
  e.insert("journal_max_bytes", static_cast<std::int64_t>(engine.journal_max_bytes));
  e.insert("journal_retention_days", static_cast<std::int64_t>(engine.journal_retention_days));
  if (engine.checkpoint_s != 0) e.insert("checkpoint_s", engine.checkpoint_s);
  e.insert("epoch_file", engine.epoch_file);
  e.insert("kill_file", engine.kill_file);
  e.insert("instance_lock", engine.instance_lock);
  e.insert("lock_file", engine.lock_file);
  e.insert("handoff_timeout_ms", static_cast<std::int64_t>(engine.handoff_timeout_ms));
  // Only when set, so a configuration without it keeps its effective text and hash.
  if (!engine.markout_ms.empty()) {
    toml::array horizons;
    for (const std::int64_t ms : engine.markout_ms) horizons.push_back(ms);
    e.insert("markout_ms", std::move(horizons));
  }
  e.insert("stall_ms", static_cast<std::int64_t>(engine.stall_ms));
  e.insert("rng_seed", static_cast<std::int64_t>(engine.rng_seed));
  e.insert("md_ring_bytes", static_cast<std::int64_t>(engine.md_ring_bytes));
  e.insert("order_ring_bytes", static_cast<std::int64_t>(engine.order_ring_bytes));
  e.insert("journal_ring_bytes", static_cast<std::int64_t>(engine.journal_ring_bytes));
  e.insert("max_events_per_step", static_cast<std::int64_t>(engine.max_events_per_step));
  e.insert("feed_budget_per_ring", static_cast<std::int64_t>(engine.feed_budget_per_ring));
  e.insert("net_spin_dedicated", engine.net_spin_dedicated);
  e.insert("crossed_grace_ms", static_cast<std::int64_t>(engine.crossed_grace_ms));
  e.insert("ack_timeout_ms", static_cast<std::int64_t>(engine.ack_timeout_ms));
  e.insert("flatten_interval_ms", static_cast<std::int64_t>(engine.flatten_interval_ms));
  e.insert("flatten_timeout_ms", static_cast<std::int64_t>(engine.flatten_timeout_ms));
  e.insert("flatten_slippage_bps", static_cast<std::int64_t>(engine.flatten_slippage_bps));
  e.insert("queue_conservatism", engine.queue_conservatism);
  e.insert("latency_publish_ms", static_cast<std::int64_t>(engine.latency_publish_ms));
  e.insert("tsc_recalibrate_s", static_cast<std::int64_t>(engine.tsc_recalibrate_s));
  e.insert("timer_slack_ns", engine.timer_slack_ns);
  e.insert("lock_memory", engine.lock_memory);
  e.insert("cpu_dma_latency_us", static_cast<std::int64_t>(engine.cpu_dma_latency_us));
  e.insert("rt_priority", static_cast<std::int64_t>(engine.rt_priority));
  e.insert("net_rt_priority", static_cast<std::int64_t>(engine.net_rt_priority));
  e.insert("log_irq_affinity", engine.log_irq_affinity);
  e.insert("restore_position", engine.restore_position);
  e.insert("min_requote_ticks", static_cast<std::int64_t>(engine.min_requote_ticks));
  e.insert("min_requote_interval_ms", static_cast<std::int64_t>(engine.min_requote_interval_ms));
  e.insert("min_qty_bps", static_cast<std::int64_t>(engine.min_qty_bps));
  e.insert("post_only", engine.post_only);
  e.insert("supports_replace", engine.supports_replace);
  e.insert("reject_backoff_ms", static_cast<std::int64_t>(engine.reject_backoff_ms));
  e.insert("reject_backoff_max_ms", static_cast<std::int64_t>(engine.reject_backoff_max_ms));
  e.insert("quote_token_reserve", static_cast<std::int64_t>(engine.quote_token_reserve));
  if (engine.send_token_reserve != 0)
    e.insert("send_token_reserve", static_cast<std::int64_t>(engine.send_token_reserve));
  if (engine.quote_room_reserve != 0)
    e.insert("quote_room_reserve", static_cast<std::int64_t>(engine.quote_room_reserve));
  e.insert("on_kill", engine.on_kill);
  // Only when set, so a configuration without it keeps its effective text and hash.
  if (engine.cancel_unknown != "all") e.insert("cancel_unknown", engine.cancel_unknown);
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
      t.insert("public_only", v.public_only);
      if (!v.pool_of.empty()) t.insert("pool_of", v.pool_of);
      if (v.reduce_only) t.insert("reduce_only", true);
      t.insert("insecure_tls", v.insecure_tls);
      t.insert("ca_file", v.ca_file);
      t.insert("recv_window_ms", static_cast<std::int64_t>(v.recv_window_ms));
      if (!v.source_ip.empty()) t.insert("source_ip", v.source_ip);
      if (!v.source_interface.empty()) t.insert("source_interface", v.source_interface);
      if (v.md_copies > 0) t.insert("md_copies", static_cast<std::int64_t>(v.md_copies));
      if (!v.md_copy_cpus.empty()) {
        toml::array copy_cpus;
        for (int c : v.md_copy_cpus) copy_cpus.push_back(static_cast<std::int64_t>(c));
        t.insert("md_copy_cpus", std::move(copy_cpus));
      }
      if (v.fill_audit_interval_s > 0) {
        t.insert("fill_audit_interval_s", static_cast<std::int64_t>(v.fill_audit_interval_s));
        t.insert("fill_audit_lag_s", static_cast<std::int64_t>(v.fill_audit_lag_s));
        t.insert("fill_audit_mode", v.fill_audit_mode);
      }
      toml::table fees;
      fees.insert("maker_bps", v.fees.maker_bps);
      fees.insert("taker_bps", v.fees.taker_bps);
      t.insert("fees", std::move(fees));
      if (v.treasury.configured) t.insert("treasury", treasury_table(v.treasury));
      if (v.pool.configured) t.insert("pool", pool_table(v.pool));
      if (v.transfers.configured) t.insert("transfers", transfers_table(v.transfers));
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
      // Only when set, so a configuration without it keeps its effective text and hash.
      if (!i.initial_margin.empty()) t.insert("initial_margin", i.initial_margin);
      arr.push_back(std::move(t));
    }
    root.insert("instruments", std::move(arr));
  }

  toml::table st;
  st.insert("name", strategy.name);
  if (strategy.max_param_age_ms != 0) st.insert("max_param_age_ms", strategy.max_param_age_ms);
  if (!strategy.state_file.empty()) st.insert("state_file", strategy.state_file);
  if (strategy.state_interval_s != 300) st.insert("state_interval_s", strategy.state_interval_s);
  toml::table params;
  for (const auto& [k, v] : strategy.params) params.insert_or_assign(k, v);
  st.insert("params", std::move(params));
  // Only when set, so a configuration without them keeps its effective text and hash.
  if (!strategy.groups.empty()) {
    toml::table groups;
    for (const auto& [name, members] : strategy.groups) {
      toml::array list;
      for (const std::string& m : members) list.push_back(m);
      groups.insert(name, std::move(list));
    }
    st.insert("groups", std::move(groups));
  }
  if (!strategy.overrides.empty()) {
    toml::table overrides;
    for (const auto& [key, values] : strategy.overrides) {
      toml::table x;
      for (const auto& [k, v] : values) x.insert_or_assign(k, v);
      overrides.insert(key, std::move(x));
    }
    st.insert("overrides", std::move(overrides));
  }
  root.insert("strategy", std::move(st));

  // Only when set, so a configuration without it keeps its effective text and hash.
  if (ab.enabled()) {
    toml::table t;
    t.insert("name", ab.name);
    t.insert("assignment", ab.assignment);
    t.insert("seed", static_cast<std::int64_t>(ab.seed));
    if (ab.block_s != 0) t.insert("block_s", ab.block_s);
    if (ab.block_offset_s != 0) t.insert("block_offset_s", ab.block_offset_s);
    if (!ab.start.empty()) t.insert("start", ab.start);
    if (!ab.end.empty()) t.insert("end", ab.end);
    if (!ab.weights.empty()) {
      toml::table w;
      for (const auto& [k, v] : ab.weights) w.insert_or_assign(k, v);
      t.insert("weights", std::move(w));
    }
    if (!ab.assign.empty()) {
      toml::table a;
      for (const auto& [k, v] : ab.assign) a.insert_or_assign(k, v);
      t.insert("assign", std::move(a));
    }
    toml::array arms;
    for (const AbArmSection& a : ab.arms) {
      toml::table x;
      x.insert("name", a.name);
      if (!a.params.empty()) {
        toml::table p;
        for (const auto& [k, v] : a.params) p.insert_or_assign(k, v);
        x.insert("params", std::move(p));
      }
      arms.push_back(std::move(x));
    }
    t.insert("arms", std::move(arms));
    root.insert("ab", std::move(t));
  }

  toml::table r;
  r.insert("max_order_qty", risk.max_order_qty);
  r.insert("max_order_notional", risk.max_order_notional);
  r.insert("max_position", risk.max_position);
  r.insert("max_open_orders", static_cast<std::int64_t>(risk.max_open_orders));
  r.insert("price_collar_bps", static_cast<std::int64_t>(risk.price_collar_bps));
  r.insert("fat_finger_bps", static_cast<std::int64_t>(risk.fat_finger_bps));
  r.insert("stale_md_ms", static_cast<std::int64_t>(risk.stale_md_ms));
  r.insert("max_loss", risk.max_loss);
  if (risk.max_loss_period != "total") r.insert("max_loss_period", risk.max_loss_period);
  // Only when set, so a configuration without them keeps its effective text and hash; a replay
  // needs them to refuse what the recorded session refused.
  if (!risk.max_gross_notional.empty()) r.insert("max_gross_notional", risk.max_gross_notional);
  if (!risk.max_net_notional.empty()) r.insert("max_net_notional", risk.max_net_notional);
  r.insert("orders_per_sec", static_cast<std::int64_t>(risk.orders_per_sec));
  r.insert("burst", static_cast<std::int64_t>(risk.burst));
  r.insert("stp", risk.stp);
  // Only when set, so a configuration without it keeps its effective text and hash.
  if (risk.max_feed_lag_ms != 0)
    r.insert("max_feed_lag_ms", static_cast<std::int64_t>(risk.max_feed_lag_ms));
  if (risk.fill_cluster_names != 0) {
    r.insert("fill_cluster_names", static_cast<std::int64_t>(risk.fill_cluster_names));
    r.insert("fill_cluster_window_ms", static_cast<std::int64_t>(risk.fill_cluster_window_ms));
    r.insert("fill_cluster_hold_ms", static_cast<std::int64_t>(risk.fill_cluster_hold_ms));
  }
  if (!risk.check_balance) r.insert("check_balance", false);
  if (risk.allow_unlimited) r.insert("allow_unlimited", true);
  if (risk.reduce_only) r.insert("reduce_only", true);
  if (risk.underlying.configured()) r.insert("underlying", underlying_table(risk.underlying));
  root.insert("risk", std::move(r));

  // Only when present, so a configuration without it keeps its effective text and hash.
  if (capacity.configured) {
    toml::table c;
    c.insert("mode", capacity.mode);
    c.insert("value", capacity.value);
    c.insert("orders_10s", capacity.orders_10s);
    c.insert("window_ms", capacity.window_ms);
    c.insert("fill_credit", capacity.fill_credit);
    c.insert("orders_1d", capacity.orders_1d);
    c.insert("orders_per_day", capacity.orders_per_day);
    c.insert("weight_1m", capacity.weight_1m);
    c.insert("margin_bps", capacity.margin_bps);
    c.insert("full_move_bps", capacity.full_move_bps);
    if (!capacity.venues.empty()) {
      toml::array v;
      for (const std::string& n : capacity.venues) v.push_back(n);
      c.insert("venues", std::move(v));
    }
    root.insert("capacity", std::move(c));
  }

  // Only when set, so a configuration without it keeps its effective text and hash.
  if (gateway.any()) {
    toml::table g;
    g.insert("orders_per_sec", static_cast<std::int64_t>(gateway.orders_per_sec));
    g.insert("burst", static_cast<std::int64_t>(gateway.burst));
    g.insert("max_open_notional", gateway.max_open_notional);
    g.insert("max_loss", gateway.max_loss);
    g.insert("max_gross_notional", gateway.max_gross_notional);
    g.insert("max_net_notional", gateway.max_net_notional);
    if (!gateway.check_balance) g.insert("check_balance", false);
    if (gateway.underlying.configured())
      g.insert("underlying", underlying_table(gateway.underlying));
    if (!gateway.shared.empty()) {
      toml::table sh;
      for (const auto& [k, v] : gateway.shared) {
        toml::table entry;
        if (!v.empty()) entry.insert("primary", v);
        sh.insert(k, std::move(entry));
      }
      g.insert("shared", std::move(sh));
    }
    root.insert("gateway", std::move(g));
  }

  // Only when set, like [gateway]: a replay converts and values as the recorded session did.
  const AccountingSpec kAccountingDefaults;
  const bool perp_keys = accounting.mark != kAccountingDefaults.mark ||
                         accounting.stale_mark_ms != kAccountingDefaults.stale_mark_ms ||
                         accounting.stale_funding_ms != kAccountingDefaults.stale_funding_ms ||
                         accounting.stale_fx_ms != kAccountingDefaults.stale_fx_ms;
  if (accounting.configured() || perp_keys) {
    toml::table a;
    if (accounting.configured()) {
      a.insert("reporting_currency", accounting.reporting_currency);
      toml::table fx;
      for (const auto& [k, v] : accounting.fx) fx.insert(k, v);
      a.insert("fx", std::move(fx));
    }
    if (perp_keys) {
      a.insert("mark", accounting.mark);
      a.insert("stale_mark_ms", static_cast<std::int64_t>(accounting.stale_mark_ms));
      a.insert("stale_funding_ms", static_cast<std::int64_t>(accounting.stale_funding_ms));
      a.insert("stale_fx_ms", static_cast<std::int64_t>(accounting.stale_fx_ms));
    }
    root.insert("accounting", std::move(a));
  }

  toml::table lg;
  lg.insert("level", logging.level);
  lg.insert("file", logging.file);
  lg.insert("mirror_level", logging.mirror_level);
  if (logging.status_interval_s >= 0) lg.insert("status_interval_s", logging.status_interval_s);
  root.insert("logging", std::move(lg));

  if (shadow.configured()) {
    toml::table t;
    t.insert("enabled", shadow.enabled);
    t.insert("threads", shadow.threads);
    t.insert("window_s", shadow.window_s);
    t.insert("settle_s", shadow.settle_s);
    t.insert("max_lag_s", shadow.max_lag_s);
    t.insert("nice", shadow.nice);
    t.insert("idle_exit_s", shadow.idle_exit_s);
    if (!shadow.cpus.empty()) {
      toml::array list;
      for (const int c : shadow.cpus) list.push_back(c);
      t.insert("cpus", std::move(list));
    }
    if (!shadow.program.empty()) t.insert("program", shadow.program);
    if (!shadow.report.empty()) t.insert("report", shadow.report);
    if (!shadow.status.empty()) t.insert("status", shadow.status);
    if (!shadow.grid.empty()) {
      toml::table grid;
      for (const auto& [k, values] : shadow.grid) {
        toml::array a;
        for (const std::string& v : values) a.push_back(v);
        grid.insert(k, std::move(a));
      }
      t.insert("grid", std::move(grid));
    }
    if (!shadow.points.empty()) {
      toml::array points;
      for (const auto& p : shadow.points) {
        toml::table row;
        for (const auto& [k, v] : p) row.insert(k, v);
        points.push_back(std::move(row));
      }
      t.insert("points", std::move(points));
    }
    root.insert("shadow", std::move(t));
  }
  if (!sim.values.empty()) root.insert("sim", generic_table(sim));
  if (!backtest.values.empty()) root.insert("backtest", generic_table(backtest));
  if (!storage.values.empty()) root.insert("storage", generic_table(storage));

  std::ostringstream ss;
  ss << root << "\n";
  return ss.str();
}

PoolHomesPlan pool_homes(const Config& cfg, const InstrumentTable& table) {
  PoolHomesPlan plan;
  for (const VenueSection& v : cfg.venues) {
    if (!v.pool.configured) continue;
    const VenueId primary = cfg.venue_id(v.name);
    if (!primary.valid() || primary.value >= plan.pool.size()) continue;
    PoolPlacement& p = plan.pool[primary.value];
    p.configured = true;
    p.placement = v.pool.placement == "home" ? Placement::Home : Placement::Balance;
    p.reduce_accounts = static_cast<std::uint8_t>(v.pool.reduce_accounts);
    p.spill = v.pool.spill == "flat"   ? HomeSpill::Flat
              : v.pool.spill == "busy" ? HomeSpill::Busy
                                       : HomeSpill::Never;
    p.spill_at = v.pool.spill_at;
    p.plan_interval = seconds(v.pool.plan_interval_s);
    p.plan_lookback = Duration{static_cast<std::int64_t>(v.pool.plan_lookback_h * 3600e9)};
    p.plan_max_moves = static_cast<std::uint32_t>(v.pool.plan_max_moves);
    LaneConfig& l = plan.lane[primary.value];
    l.sweep = v.pool.sweep == "true"   ? SweepMode::On
              : v.pool.sweep == "auto" ? SweepMode::Auto
                                       : SweepMode::Off;
    l.sweep_delay = milliseconds(v.pool.sweep_delay_ms);
    l.sweep_dust = seconds(v.pool.sweep_dust_s);
    l.sweep_retry = seconds(v.pool.sweep_retry_s);
    const TransfersSection& tr = v.transfers;
    l.max_in_flight = static_cast<std::uint32_t>(tr.max_in_flight);
    l.per_minute = static_cast<std::uint32_t>(tr.per_minute);
    l.per_hour = static_cast<std::uint32_t>(tr.per_hour);
    l.per_day = static_cast<std::uint32_t>(tr.per_day);
    l.breaker = seconds(tr.breaker_s);
    l.timeout = seconds(tr.timeout_s);
    l.status_interval = seconds(tr.status_interval_s);
    l.dry_run = tr.dry_run;
    l.state_file = !tr.state_file.empty() ? tr.state_file
                                          : cfg.engine.journal_dir + "/" + cfg.engine.name + "." +
                                                v.name + ".transfers";
    for (const auto& [symbol, account] : v.pool.homes) {
      for (const Instrument& inst : table) {
        if (inst.venue == primary && inst.symbol.view() == symbol &&
            inst.id.value < plan.pin.size())
          plan.pin[inst.id.value] = cfg.venue_id(account);
      }
    }
  }
  return plan;
}

}  // namespace fastmm
