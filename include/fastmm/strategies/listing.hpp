#pragma once
// --list-strategies output shared by the command-line entry points (fastmm::strategies library).
//
// Text (one strategy per line, then one line per parameter):
//   basic_mm
//     half_spread_bps            bps     default=5          [0, 1000]  half spread around mid ...
// JSON (--format json), transports in the order sim, replay, live:
//   {"strategies": [{"name": "basic_mm", "transports": ["sim", "replay", "live"],
//     "params": [{"name": "half_spread_bps", "type": "bps", "default": 5, "min": 0, "max": 1000,
//                 "doc": "..."}]}]}
#include "fastmm/strategies/registry.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace fastmm {

enum class ListFormat : std::uint8_t { Text, Json };

// "text" or "json".
[[nodiscard]] std::optional<ListFormat> parse_list_format(std::string_view s) noexcept;

// The strategies of `r` that support `kind`, in registration order.
[[nodiscard]] std::string format_strategies(const StrategyRegistry& r,
                                            TransportKind kind,
                                            ListFormat format);

}  // namespace fastmm
