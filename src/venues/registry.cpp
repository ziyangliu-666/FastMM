#include "fastmm/venues/registry.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <cstdlib>
#include <stdexcept>

namespace fastmm::venues {

// Registration functions of the connectors FastMM ships, one translation unit each.
void register_binance_venue(VenueRegistry& r);
void register_binance_usdm_venue(VenueRegistry& r);
void register_bybit_venue(VenueRegistry& r);
void register_deribit_venue(VenueRegistry& r);
void register_nasdaq_itch_venue(VenueRegistry& r);

namespace {

bool is_int(std::string_view s) noexcept {
  if (s.empty()) return false;
  std::size_t i = (s.front() == '-' || s.front() == '+') ? 1 : 0;
  if (i == s.size()) return false;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

bool is_float(std::string_view s) {
  if (s.empty()) return false;
  const std::string text(s);
  char* end = nullptr;
  errno = 0;
  std::strtod(text.c_str(), &end);
  return errno == 0 && end == text.c_str() + text.size();
}

// An array as VenueSection::extra stores it: stringify() writes "[a, b]" (elements unquoted).
bool is_array(std::string_view s) noexcept {
  return s.size() >= 2 && s.front() == '[' && s.back() == ']';
}

// The text a [venues.<name>] key was written with against the type its venue declared. Strings and
// Any accept anything: the connector parses them itself and reports its own error.
bool text_matches(KeyType t, std::string_view text) {
  switch (t) {
    case KeyType::Int:
      return is_int(text);
    case KeyType::Float:
      return is_float(text);
    case KeyType::Bool:
      return text == "true" || text == "false";
    case KeyType::IntArray:
    case KeyType::StringArray:
      return is_array(text);
    case KeyType::String:
    case KeyType::Table:
    case KeyType::Any:
      return true;
  }
  return true;
}

constexpr std::string_view type_name(KeyType t) noexcept {
  switch (t) {
    case KeyType::String:
      return "string";
    case KeyType::Int:
      return "integer";
    case KeyType::Float:
      return "number";
    case KeyType::Bool:
      return "boolean";
    case KeyType::IntArray:
      return "integer array";
    case KeyType::StringArray:
      return "string array";
    case KeyType::Table:
      return "table";
    case KeyType::Any:
      return "any";
  }
  return "?";
}

const VenueKeySpec* find_key(const VenueEntry& e, std::string_view key) noexcept {
  for (const VenueKeySpec& k : e.keys) {
    if (k.key == key) return &k;
  }
  return nullptr;
}

int line_of(const VenueSection& s, const std::string& key) noexcept {
  const auto it = s.extra_lines.find(key);
  return it == s.extra_lines.end() ? 0 : it->second;
}

const VenueEntry& entry_for(const VenueRegistry& r, const VenueSection& s) {
  const VenueEntry* e = r.find(s.kind);
  if (e == nullptr) {
    throw std::invalid_argument(fmt::format(
        "venue '{}': unsupported kind '{}' (registered: {})", s.name, s.kind, r.kinds()));
  }
  return *e;
}

// The strict half of validate_venues(): everything that must stop the session. Unknown keys are
// forward compatibility and only warn, so they are not checked here.
void check_types(const VenueEntry& e, const VenueSection& s) {
  for (const auto& [key, text] : s.extra) {
    const VenueKeySpec* spec = find_key(e, key);
    if (spec == nullptr || text_matches(spec->type, text)) continue;
    throw ConfigError(
        fmt::format(
            "'venues.{}.{}' has the wrong type (expected {})", s.name, key, type_name(spec->type)),
        line_of(s, key),
        1);
  }
  for (const VenueKeySpec& k : e.keys) {
    if (k.required && !s.extra.contains(std::string(k.key)))
      throw ConfigError(fmt::format("missing required key 'venues.{}.{}'", s.name, k.key));
  }
}

}  // namespace

VenueRegistry& VenueRegistry::instance() {
  static VenueRegistry r;
  return r;
}

AddResult VenueRegistry::add(VenueEntry entry) {
  if (entry.name.empty() || entry.make == nullptr) return AddResult::Invalid;
  for (std::string_view a : entry.aliases) {
    if (a.empty() || a == entry.name) return AddResult::Invalid;
  }
  for (const VenueEntry& e : entries_) {
    if (e.name == entry.name)
      return e.make == entry.make ? AddResult::AlreadyPresent : AddResult::Conflict;
  }
  // `entry.name` is free as a name; it (or an alias) may still be another entry's alias.
  if (find(entry.name) != nullptr) return AddResult::Conflict;
  for (std::string_view a : entry.aliases) {
    if (find(a) != nullptr) return AddResult::Conflict;
  }
  entries_.push_back(entry);
  return AddResult::Added;
}

const VenueEntry* VenueRegistry::find(std::string_view kind) const noexcept {
  for (const VenueEntry& e : entries_) {
    if (e.name == kind) return &e;
    for (std::string_view a : e.aliases) {
      if (a == kind) return &e;
    }
  }
  return nullptr;
}

std::string VenueRegistry::kinds() const {
  std::string s;
  for (const VenueEntry& e : entries_) {
    if (!s.empty()) s += ", ";
    s += e.name;
    for (std::string_view a : e.aliases) {
      s += ", ";
      s += a;
    }
  }
  return s;
}

void register_builtin_venues(VenueRegistry& r) {
  register_binance_venue(r);
  register_binance_usdm_venue(r);
  register_bybit_venue(r);
  register_deribit_venue(r);
  register_nasdaq_itch_venue(r);
}

void validate_venues(const Config& cfg,
                     std::vector<std::string>& warnings,
                     const VenueRegistry& r) {
  if (&r == &VenueRegistry::instance()) register_builtin_venues();
  for (const VenueSection& s : cfg.venues) {
    const VenueEntry& e = entry_for(r, s);
    check_types(e, s);
    for (const auto& [key, text] : s.extra) {
      if (find_key(e, key) != nullptr) continue;
      const int line = line_of(s, key);
      warnings.push_back(fmt::format("unknown key 'venues.{}.{}' ignored{}",
                                     s.name,
                                     key,
                                     line > 0 ? fmt::format(" (line {})", line) : std::string()));
    }
  }
}

std::unique_ptr<Venue> make_venue(VenueId id,
                                  const VenueSection& section,
                                  const VenueFactoryOptions& opts,
                                  const VenueRegistry& r) {
  const VenueEntry& e = entry_for(r, section);
  check_types(e, section);
  return e.make(id, section, opts);
}

std::unique_ptr<Venue> make_venue(VenueId id,
                                  const VenueSection& section,
                                  const VenueFactoryOptions& opts) {
  register_builtin_venues();
  return make_venue(id, section, opts, VenueRegistry::instance());
}

}  // namespace fastmm::venues
