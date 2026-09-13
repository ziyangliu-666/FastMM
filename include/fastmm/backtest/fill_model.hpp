#pragma once
// FillModel selection (8.4): MatchingFillModel == sim::FillModel::Matching (orders rest in
// the simulated book; synthetic flow or mirrored historical levels trade against them),
// L2QueueFillModel == sim::FillModel::L2Queue (queue-position model on historical L2).
#include "fastmm/sim/sim_transport.hpp"

#include <optional>
#include <string_view>

namespace fastmm::bt {

using sim::FillModel;
inline constexpr FillModel kMatchingFillModel = FillModel::Matching;
inline constexpr FillModel kL2QueueFillModel = FillModel::L2Queue;

[[nodiscard]] inline std::optional<FillModel> parse_fill_model(std::string_view s) noexcept {
  if (s == "matching") return FillModel::Matching;
  if (s == "l2_queue" || s == "queue") return FillModel::L2Queue;
  return std::nullopt;
}

}  // namespace fastmm::bt
