#pragma once
// extract_features(): drives an MdSource into the engine's own L2Book and emits a FeatureTable.
//
// The book is the one the simulated venue keeps under FillModel::L2Queue (sim/sim_transport.cpp,
// `queue_on_delta`): BookDelta and BookSnapshot are applied, trades are not. So the mid of a row
// and its forward mids are the same numbers `SimTransport::venue_mid()` reports at those times,
// and a fill of a backtest over the same data can be joined to this table by timestamp.
//
// One pass, no look-ahead. Forward mids are resolved by a rule that reads the book only at times
// the data has already reached:
//
//   on an event at time t, before applying it, resolve every pending row whose ts + h < t;
//   at the end of the data, resolve every pending row whose ts + h <= the last event time.
//
// So a row's value at horizon h is the book after every event at or before ts + h and no other -
// the same state `run_until(ts + h)` leaves the venue in. A row whose ts + h is past the last
// event stays unset and is counted in HorizonCoverage::excluded_past_end.
#include "fastmm/core/time.hpp"
#include "fastmm/research/feature_table.hpp"
#include "fastmm/sim/md_source.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastmm::research {

using sim::MdSource;

// kDefaultHorizonsNs as Durations.
[[nodiscard]] std::vector<Duration> default_horizons();

struct FeatureConfig {
  // Forward horizons. Empty selects default_horizons(); non-positive entries are dropped and the
  // rest are sorted ascending and de-duplicated.
  std::vector<Duration> horizons;
  // Levels summed by fastmm::imbalance(). 1 is the touch, which is all a top-of-book feed shows.
  std::size_t imbalance_levels = 1;
  // Minimum spacing between rows of one instrument. Zero samples every book update; 10 ms keeps
  // at most one row per instrument per 10 ms of event time.
  Duration subsample{};
  // What emits a row. A book update emits one after it has been applied; a trade emits one with
  // the book as it stood when the trade printed. Trades off by default; with book updates off and
  // trades on, the table has exactly one row per trade, at the timestamp of every fill a backtest
  // over the same data can produce, which is what the cross-check against fills.csv needs.
  // Turning both off leaves an empty table.
  bool sample_book_updates = true;
  bool sample_trades = false;
  // Books kept. Events for a higher instrument id are counted and ignored.
  std::uint32_t instruments = 1;
};

// Reads `source` from its current position to the end. Throws std::invalid_argument when
// `instruments` is 0 or `imbalance_levels` is 0.
[[nodiscard]] FeatureTable extract_features(MdSource& source, const FeatureConfig& cfg = {});

}  // namespace fastmm::research
