// UnderlyingPlan: the configured underlyings of an instrument table and the instruments in each.
#include "fastmm/core/underlying.hpp"

#include "fastmm/core/fx.hpp"

#include <fmt/format.h>

namespace fastmm {

int UnderlyingPlan::find(std::string_view name) const noexcept {
  for (std::uint8_t u = 0; u < count; ++u) {
    if (same_currency(names[u].view(), name)) return u;
  }
  return -1;
}

Result<UnderlyingPlan, std::string> build_underlying_plan(const InstrumentTable& table,
                                                          const UnderlyingSpec& spec,
                                                          std::string_view section) {
  UnderlyingPlan p;
  if (!spec.configured()) return p;
  if (spec.max_net.size() > kMaxUnderlyings)
    return fail(fmt::format("[{}.underlying]: at most {} underlyings", section, kMaxUnderlyings));
  std::uint16_t n = 0;
  for (const auto& [name, text] : spec.max_net) {
    if (name.empty() || name.size() > UnderlyingName::kCapacity)
      return fail(fmt::format("[{}.underlying.{}]: the name must be 1 to {} characters",
                              section,
                              name,
                              UnderlyingName::kCapacity));
    if (p.find(name) >= 0)
      return fail(
          fmt::format("[{}.underlying.{}]: named twice (names ignore case)", section, name));
    Qty max{};
    if (!text.empty()) {
      const auto q = Qty::from_decimal(text);
      if (!q || q->raw < 0)
        return fail(fmt::format(
            "[{}.underlying.{}] max_net: '{}' is not a non-negative decimal", section, name, text));
      max = *q;
    }
    const std::uint8_t u = p.count++;
    p.names[u] = UnderlyingName(name);
    p.max_net[u] = max;
    p.begin[u] = n;
    bool options = false;
    for (const Instrument& inst : table) {
      if (!same_currency(inst.base.view(), name)) continue;
      if (!counts_toward_underlying(inst)) {
        options = true;
        continue;
      }
      p.of[inst.id.value] = static_cast<std::uint8_t>(u + 1);
      p.members[n++] = inst.id;
    }
    p.begin[u + 1] = n;
    if (p.begin[u] == n)
      return fail(fmt::format("[{}.underlying.{}]: no instrument of this session has base {}{}",
                              section,
                              name,
                              name,
                              options ? " other than options, which do not count" : ""));
  }
  return p;
}

}  // namespace fastmm
