#include "fastmm/strategies/param_overrides.hpp"

#include "fastmm/config/config.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <set>

namespace fastmm {

namespace {

// The instruments "symbol" or "venue:symbol" names.
std::vector<InstrumentId> match(std::string_view key,
                                const InstrumentTable& instruments,
                                std::span<const std::string> venue_names) {
  const std::size_t colon = key.find(':');
  std::vector<InstrumentId> out;
  for (const Instrument& inst : instruments) {
    if (colon == std::string_view::npos) {
      if (inst.symbol.view() == key) out.push_back(inst.id);
      continue;
    }
    if (inst.venue.value < venue_names.size() &&
        venue_names[inst.venue.value] == key.substr(0, colon) &&
        inst.symbol.view() == key.substr(colon + 1))
      out.push_back(inst.id);
  }
  if (out.size() > 1)
    throw ConfigError(fmt::format("'{}' is on more than one venue; write it \"venue:symbol\"", key));
  return out;
}

}  // namespace

std::vector<InstrumentId> resolve_scope(std::string_view key,
                                        const std::map<std::string, std::vector<std::string>>& groups,
                                        const InstrumentTable& instruments,
                                        std::span<const std::string> venue_names) {
  const auto g = groups.find(std::string(key));
  if (g == groups.end()) return match(key, instruments, venue_names);
  std::vector<InstrumentId> out;
  for (const std::string& member : g->second) {
    const std::vector<InstrumentId> ids = match(member, instruments, venue_names);
    if (ids.empty())
      throw ConfigError(
          fmt::format("strategy.groups.{}: no instrument '{}'", g->first, member));
    if (std::find(out.begin(), out.end(), ids.front()) == out.end()) out.push_back(ids.front());
  }
  return out;
}

std::vector<InstrumentParams> resolve_overrides(
    const std::map<std::string, std::vector<std::string>>& groups,
    const std::map<std::string, ParamMap>& overrides,
    const InstrumentTable& instruments,
    std::span<const std::string> venue_names) {
  for (const auto& [name, members] : groups) {
    if (!match(name, instruments, venue_names).empty())
      throw ConfigError(fmt::format(
          "strategy.groups.{}: an instrument has that symbol; name the group otherwise", name));
    // Checks its members.
    static_cast<void>(resolve_scope(name, groups, instruments, venue_names));
  }
  struct Value {
    std::string text;
    std::string from;  // the group it came from; empty: the instrument's own key
  };
  std::vector<std::map<std::string, Value>> per(instruments.size());
  // Groups first, then the instruments' own keys over them.
  for (const bool own : {false, true}) {
    for (const auto& [key, values] : overrides) {
      const bool group = groups.contains(key);
      if (group == own) continue;
      const std::vector<InstrumentId> ids = resolve_scope(key, groups, instruments, venue_names);
      if (ids.empty())
        throw ConfigError(fmt::format("strategy.overrides.\"{}\": no instrument or group of that name", key));
      for (const InstrumentId id : ids) {
        for (const auto& [name, text] : values) {
          auto [it, fresh] = per[id.value].try_emplace(name, Value{text, own ? "" : key});
          if (fresh || own) {
            it->second = Value{text, own ? "" : key};
            continue;
          }
          if (it->second.text != text && !it->second.from.empty()) {
            throw ConfigError(fmt::format(
                "strategy.overrides: groups {} and {} give {} different values of {}; set it in "
                "[strategy.overrides.\"{}\"]",
                it->second.from,
                key,
                instruments.get(id).symbol.view(),
                name,
                instruments.get(id).symbol.view()));
          }
        }
      }
    }
  }
  std::vector<InstrumentParams> out;
  for (std::size_t i = 0; i < per.size(); ++i) {
    if (per[i].empty()) continue;
    InstrumentParams p;
    p.instrument = InstrumentId{static_cast<std::uint32_t>(i)};
    for (const auto& [name, v] : per[i]) p.values.emplace_back(name, v.text);
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace fastmm
