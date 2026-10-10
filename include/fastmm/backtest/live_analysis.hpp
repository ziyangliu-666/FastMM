#pragma once
// What a live session's fills were worth, from its journal alone (fastmm-data analyze): per
// instrument and overall, the fills, maker share and traded notional, the spread they captured
// against the venue mid and their markouts and adverse selection at each horizon (markout.hpp),
// next to the order traffic. The mid is the recorded BookTicker stream's (mid_series.hpp); a
// session that recorded none has no markouts, only the counts.
#include "fastmm/backtest/markout.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fastmm::bt {

struct LiveAnalysisInstrument {
  std::string symbol;
  std::uint64_t fills = 0;
  std::uint64_t maker_fills = 0;
  double bought = 0.0;    // base units
  double sold = 0.0;      // base units
  double notional = 0.0;  // quote currency
};

struct LiveAnalysis {
  std::string strategy;
  std::int64_t first_ns = 0;  // venue time of the first and last event
  std::int64_t last_ns = 0;
  std::uint64_t orders = 0;  // new orders sent
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t rejects = 0;  // venue refusals
  std::uint64_t fills = 0;    // executions, each once
  std::uint64_t maker_fills = 0;
  std::uint64_t repeated_fills = 0;  // the same execution reported again (a replay): counted once
  bool mids = false;                 // the journal has a BookTicker stream to mark against
  std::vector<LiveAnalysisInstrument> instruments;  // by instrument id
  std::vector<MarkoutHorizon> horizons;
};

// Throws std::runtime_error when the journal cannot be read.
[[nodiscard]] LiveAnalysis analyze_live(const std::string& path,
                                        std::span<const std::int64_t> horizons_ns);
[[nodiscard]] std::string format_live_analysis(const LiveAnalysis& a);
[[nodiscard]] std::string live_analysis_json(const LiveAnalysis& a);

}  // namespace fastmm::bt
