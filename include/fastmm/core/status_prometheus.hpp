#pragma once
// Prometheus text exposition (format 0.0.4) of a live status snapshot, for `fastmm-top --metrics`.
//
// The exporter reads the status file the engine already publishes, in fastmm-top's own process:
// scraping adds no work to the engine at all, let alone to the trading thread.
//
// Counters end in _total, durations are seconds, money is the quote currency. Names are stable:
// a dashboard built on them keeps working across releases (new metrics may appear).
#include "fastmm/core/status_segment.hpp"

#include <cstdint>
#include <string>

namespace fastmm {

// `now_ns` is the reader's wall clock, for fastmm_status_age_seconds.
[[nodiscard]] std::string format_status_prometheus(const StatusSnapshot& s, std::int64_t now_ns);

}  // namespace fastmm
