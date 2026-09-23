#pragma once
// evaluate_signal(): what a candidate predictor is worth, per horizon, before anyone writes a
// strategy around it.
//
// Three things come out of one pass over a FeatureTable:
//
//   * the information coefficient, Spearman rank correlation between the signal and the forward
//     mid move, and the same coefficient over equal-sized contiguous blocks of the sample, which
//     is what says whether one hour carried it;
//   * a decile table of the forward mid move by signal value, in basis points of the mid;
//   * the conditional touch markout: for each bucket, what a quote resting at the touch would
//     have made or lost by the horizon, one number per side, in basis points of the mid.
//
// The touch markout is the one that decides whether a passive quote is viable. For a row with mid
// m, best bid b, best ask a and forward mid f:
//
//   buy_bps  = (f - b) / m * 1e4      a resting bid that filled at b, marked at f
//   sell_bps = (a - f) / m * 1e4      a resting ask that filled at a, marked at f
//
// which split into (m - b) / m, the half spread the quote earned, plus or minus (f - m) / m, the
// move the mid made afterwards. Both sides of one quote average to the half spread, because the
// move cancels; the point of conditioning on a signal is that in a given bucket only one of them
// is the side that fills. The numbers are gross: subtract the venue's maker fee to compare.
//
// Rows without a forward mid at a horizon are left out of every statistic of that horizon, and
// counted; they are never marked at a substitute price.
#include "fastmm/research/feature_table.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::research {

// A built-in column, so the common signals need no array from the caller.
enum class Feature : std::uint8_t {
  Imbalance = 0,       // (bid qty - ask qty) / (bid qty + ask qty) at the touch, in [-1, 1]
  MicropriceEdge = 1,  // (microprice - mid) / mid, in basis points
  Spread = 2,          // (best ask - best bid) / mid, in basis points
};

[[nodiscard]] std::string_view to_string(Feature f) noexcept;
// The feature of every row, in row order, in the units above.
[[nodiscard]] std::vector<double> feature_values(const FeatureTable& t, Feature f);

struct SignalBucket {
  double lo = 0.0;  // signal range of the bucket, inclusive
  double hi = 0.0;
  std::uint64_t n = 0;
  double mean_signal = 0.0;
  double forward_bps = 0.0;      // mean (forward mid - mid) / mid, basis points
  double buy_bps = 0.0;          // resting bid filled at the touch, marked at the horizon
  double sell_bps = 0.0;         // resting ask filled at the touch, marked at the horizon
  double half_spread_bps = 0.0;  // (mid - best bid) / mid; (buy_bps + sell_bps) / 2
};

struct HorizonEval {
  std::int64_t horizon_ns = 0;
  std::uint64_t n = 0;  // rows that had a forward mid at this horizon
  std::uint64_t excluded_past_end = 0;
  std::uint64_t excluded_no_mid = 0;
  double ic = 0.0;  // Spearman, signal against forward mid move; NaN when n < 2
  // The same coefficient per contiguous equal-sized block of the measured rows.
  std::vector<double> block_ic;
  double block_ic_mean = 0.0;
  double block_ic_stdev = 0.0;  // population standard deviation over the blocks
  double block_ic_min = 0.0;
  double block_ic_max = 0.0;
  double block_sign_agreement = 0.0;  // share of blocks whose IC has the sign of `ic`
  std::vector<SignalBucket> buckets;  // in ascending signal order

  [[nodiscard]] std::string label() const { return horizon_label(horizon_ns); }
};

struct SignalEval {
  std::string signal;
  std::uint64_t rows = 0;    // rows of the table the signal was measured on
  std::uint32_t blocks = 0;  // blocks the sample was split into
  std::vector<HorizonEval> horizons;

  // Per horizon: the coverage and IC line, then the bucket table.
  [[nodiscard]] std::string table() const;
};

struct EvalConfig {
  std::uint32_t buckets = 10;  // equal-count buckets of the signal; 10 is a decile table
  std::uint32_t blocks = 10;   // contiguous equal-sized blocks the IC is recomputed over
};

// `values` must hold one signal value per row of `t`, in row order. Throws
// std::invalid_argument on a length mismatch or a bucket/block count of 0.
[[nodiscard]] SignalEval evaluate_signal(const FeatureTable& t,
                                         std::span<const double> values,
                                         std::string_view name,
                                         const EvalConfig& cfg = {});
// evaluate_signal() over feature_values(t, f).
[[nodiscard]] SignalEval evaluate_feature(const FeatureTable& t,
                                          Feature f,
                                          const EvalConfig& cfg = {});

}  // namespace fastmm::research
