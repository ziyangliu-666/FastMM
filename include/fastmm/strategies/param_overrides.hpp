#pragma once
// Per-instrument parameter values (docs/how-to/strategies/live-ab-by-instrument.md).
//
// A strategy that keeps a parameter set per instrument (ParamScope::PerInstrument, every Python
// strategy) holds, for each instrument, which parameters it has a value of its own for: one set
// by an update naming the instrument (fastmm-ctl param --instrument, [strategy.overrides], an [ab]
// arm). An update for every instrument then leaves those values alone; a reset (ParamUpdateMsg::
// kReset) gives the instrument the shared value back.
//
// apply_param_pairs() is the rule, shared by StrategyBase, HotStrategy, ParamPublisher and the
// engine's own count: engine thread, no allocation.
//
// resolve_overrides() turns [strategy.groups] and [strategy.overrides] into one list of values per
// instrument, at start.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/strategies/params.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fastmm {

// The parameters, by schema index, an instrument holds a value of its own for.
using ParamMask = std::bitset<kMaxParams>;

// Applies `m`'s (field, raw value) pairs to per-instrument sets: write(k, i) assigns pair i of `m`
// to instrument k's set. `masks` has one entry per instrument; fields at or past `fields` are
// skipped. An update naming an instrument writes there and marks the fields as the instrument's
// own (a reset clears them); one for every instrument writes to each instrument that does not hold
// the field as its own, or with a reset to every instrument, clearing the marks.
template <class Write>
void apply_param_pairs(const ParamUpdateMsg& m,
                       std::span<ParamMask> masks,
                       std::size_t fields,
                       Write&& write) noexcept {
  const std::size_t n = m.count < ParamUpdateMsg::kMaxFields ? m.count : ParamUpdateMsg::kMaxFields;
  const bool reset = m.resets();
  if (!m.all_instruments()) {
    const std::size_t k = m.hdr.instrument.value;
    if (k >= masks.size()) return;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint16_t f = m.field[i];
      if (f >= fields || f >= kMaxParams) continue;
      write(k, i);
      masks[k].set(f, !reset);
    }
    return;
  }
  for (std::size_t k = 0; k < masks.size(); ++k) {
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint16_t f = m.field[i];
      if (f >= fields || f >= kMaxParams) continue;
      if (reset) {
        masks[k].reset(f);
      } else if (masks[k].test(f)) {
        continue;
      }
      write(k, i);
    }
  }
}

// [strategy.overrides] for one instrument: name = value pairs, in name order.
struct InstrumentParams {
  InstrumentId instrument{};
  std::vector<std::pair<std::string, std::string>> values;
};

// The overrides of [strategy.overrides] per instrument, in instrument order. A key names a group
// of [strategy.groups], a symbol or "venue:symbol"; an instrument's own key wins over its groups.
// `venue_names` name the venue ids (Config::venues order). Throws ConfigError for a key or a
// group member that names no instrument, a symbol on several venues, a group named like a symbol,
// and two groups giving one instrument different values of a parameter.
[[nodiscard]] std::vector<InstrumentParams> resolve_overrides(
    const std::map<std::string, std::vector<std::string>>& groups,
    const std::map<std::string, ParamMap>& overrides,
    const InstrumentTable& instruments,
    std::span<const std::string> venue_names);

// The instruments a group of [strategy.groups], a symbol or "venue:symbol" names; empty when it
// names none. Throws ConfigError for a symbol on several venues.
[[nodiscard]] std::vector<InstrumentId> resolve_scope(
    std::string_view key,
    const std::map<std::string, std::vector<std::string>>& groups,
    const InstrumentTable& instruments,
    std::span<const std::string> venue_names);

}  // namespace fastmm
