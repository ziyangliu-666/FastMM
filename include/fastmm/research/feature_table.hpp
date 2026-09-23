#pragma once
// FeatureTable: book features and forward mid moves, one row per qualifying market-data event,
// so a predictor can be judged before anyone writes a strategy around it.
//
// Columns are structure-of-arrays, the convention of include/fastmm/backtest/result.hpp: each
// column is one contiguous vector, so Python borrows it as a numpy array without a copy. Prices,
// quantities and spreads are raw 1e-8 fixed point; imbalance is a Ratio in the same scale
// (1e8 == 1.0).
//
// A forward mid is the venue mid at `ts + horizon`, read AT that time and never interpolated -
// the same rule the backtest applies to its fill markouts. A row whose horizon falls after the
// last event of the data, or lands where the book has no two sides, has no forward mid: the
// column holds 0 and HorizonCoverage counts it. It is never substituted with the last known mid.
// This is the discipline of include/fastmm/backtest/markout.hpp (`excluded_past_end`), applied
// per row instead of per fill.
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::research {

// Horizons measured by default, in nanoseconds of event time: 100 ms, 1 s, 10 s, 1 minute.
inline constexpr std::int64_t kDefaultHorizonsNs[] = {
    100'000'000LL, 1'000'000'000LL, 10'000'000'000LL, 60'000'000'000LL};

// "100ms", "1s", "1m": the shortest exact unit, the spelling MarkoutHorizon::label() uses.
[[nodiscard]] std::string horizon_label(std::int64_t ns);

struct FeatureRows {
  std::vector<std::int64_t> ts;  // venue time of the event that produced the row
  std::vector<std::uint32_t> instrument;
  std::vector<std::int64_t> mid;         // (best bid + best ask) / 2, raw 1e-8
  std::vector<std::int64_t> microprice;  // fastmm::microprice(book), raw 1e-8
  std::vector<std::int64_t> best_bid;
  std::vector<std::int64_t> best_ask;
  std::vector<std::int64_t> bid_qty;  // displayed quantity at the best bid, raw 1e-8
  std::vector<std::int64_t> ask_qty;
  std::vector<std::int64_t> imbalance;  // fastmm::imbalance(book, levels), Ratio raw 1e-8
  std::vector<std::int64_t> spread;     // best ask - best bid, raw 1e-8
  // Horizons in nanoseconds, and the venue mid at ts + horizon. forward_mid[h][i] is 0 when that
  // row has no value at horizon h; it is excluded there, never marked at a substitute price.
  std::vector<std::int64_t> horizon_ns;
  std::vector<std::vector<std::int64_t>> forward_mid;

  [[nodiscard]] std::size_t size() const noexcept { return ts.size(); }
  void reserve(std::size_t n);
  void clear() noexcept;
};

// Per horizon, how many rows got a forward mid and why the rest did not. resolved +
// excluded_past_end + excluded_no_mid == FeatureRows::size().
struct HorizonCoverage {
  std::int64_t horizon_ns = 0;
  std::uint64_t resolved = 0;
  std::uint64_t excluded_past_end = 0;  // ts + horizon is after the last event of the data
  std::uint64_t excluded_no_mid = 0;    // the book had no two sides at ts + horizon

  [[nodiscard]] std::string label() const { return horizon_label(horizon_ns); }
};

struct FeatureTable {
  FeatureRows rows;
  std::vector<HorizonCoverage> coverage;  // one entry per horizon, in horizon_ns order
  std::uint64_t events = 0;               // market-data events read from the source
  std::uint64_t book_updates = 0;         // BookDelta / BookSnapshot applied to a book
  std::uint64_t skipped_one_sided = 0;    // sampled events whose book had no two sides
  std::uint64_t skipped_subsample = 0;    // sampled events dropped by FeatureConfig::subsample
  std::int64_t start_ts = 0;              // first event time of the data (0 when empty)
  std::int64_t end_ts = 0;                // last event time; the horizon cut-off

  [[nodiscard]] std::size_t size() const noexcept { return rows.size(); }
  // One line per horizon plus the event counts.
  [[nodiscard]] std::string summary_table() const;
  // Every column, forward mids last; an empty field where a forward mid is unset.
  [[nodiscard]] std::string csv() const;
};

}  // namespace fastmm::research
