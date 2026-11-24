#pragma once
// The zone table of src/core/tzdata_table.cpp (tools/tzdata_gen.py): zones by name in byte order,
// the transitions they index into, and the tzdata release they come from.
#include "fastmm/core/time_zone.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm::tzdata {

using Transition = TzTransition;
inline constexpr std::int64_t kNoRule = TimeZone::kNoRule;

struct Zone {
  const char* name;
  const char* rule;        // the POSIX rule of its TZif file; "" for none
  std::uint32_t first;     // its transitions in kTransitions
  std::uint32_t count;     // how many
  std::int32_t offset0;    // the offset before them (on 2000-01-01)
  std::int64_t rule_from;  // from here on the rule decides; kNoRule: never
};

extern const char kVersion[];
extern const Transition kTransitions[];
extern const Zone kZones[];
extern const std::size_t kZoneCount;

}  // namespace fastmm::tzdata
