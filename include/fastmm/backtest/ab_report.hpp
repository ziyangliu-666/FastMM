#pragma once
// The readout of an [ab] test (fastmm-data ab), from the journals of the live sessions or
// backtests that ran it. A journal carries everything it needs: the configuration with [ab] (the
// arms and the design, rebuilt as an AbPlan), the arm updates (ParamUpdate, Origin::Experiment),
// the arm of each order sent (OutNewOrder, OutReplace) and the BookTicker mids the fills are
// marked against.
//
// Each instrument's time is cut into cells: one arm over one block (time, crossover) or over the
// whole test (instrument). A fill belongs to the cell of its order's arm at its time; a fill that
// came after its instrument changed arm goes to the last block of the arm it was sent under. Per
// cell: the time, the fills, the traded notional and, at each horizon, the markout, the fees the
// configuration charges ([venues.<x>.fees], [[instruments]] maker_bps / taker_bps) less any extra
// maker rebate, and their net, over the fills marked at that horizon.
//
// The comparison pairs the arms where the design makes them comparable: the instruments of one
// stratum (instrument), or one instrument over one run of blocks that gives each arm one block
// (time, crossover). A pair counts when it has time in every arm. Each arm is compared with arm
// 0 on the mean difference per pair of the net and notional per instrument-hour, and on the
// difference of net bps over the pairs; the 95% interval comes from resampling clusters of pairs
// (the strata, or the runs of blocks), which keeps what one market moment does to every
// instrument inside one draw. An A/A test's intervals, run the same way, are its noise floor.
#include "fastmm/backtest/markout.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fastmm::bt {

struct AbReportOptions {
  std::vector<std::int64_t> horizons_ns;  // empty: kDefaultMarkoutHorizonsNs
  double maker_rebate_bps = 0.0;          // paid on maker notional, on top of the configured fees
  std::size_t resamples = 2000;
  std::uint64_t seed = 1;
};

struct AbHorizon {
  MarkoutBucket all;  // the fills marked at this horizon
  MarkoutBucket buy;
  MarkoutBucket sell;
  std::int64_t fees_raw = 0;  // configured fees less the rebate on the same fills, quote currency
  [[nodiscard]] std::int64_t net_raw() const noexcept { return all.markout_raw - fees_raw; }
  void add(const AbHorizon& o) noexcept;
};

// One instrument in one arm over one block (the whole test with assignment = "instrument").
struct AbCell {
  std::size_t instrument = 0;  // AbReport::instruments
  int arm = 0;
  std::int64_t block = 0;
  std::int64_t ns = 0;  // time the instrument was in the arm
  std::uint64_t fills = 0;
  std::uint64_t maker_fills = 0;
  std::int64_t notional_raw = 0;  // every fill, quote currency
  std::vector<AbHorizon> horizons;
};

struct AbArmSummary {
  std::string name;
  std::string params;   // "name=value ..."; empty: the configured parameters
  std::int64_t ns = 0;  // instrument time in the arm
  std::uint64_t fills = 0;
  std::uint64_t maker_fills = 0;
  std::int64_t notional_raw = 0;
  std::vector<AbHorizon> horizons;
};

// One arm against arm 0 on one metric. `arm` and `control` are the two sides' values over the
// pairs, `diff` their difference, [lo, hi] its 95% interval.
struct AbEffect {
  int arm = 0;
  std::string metric;  // "net/hour 60s", "net bps 60s", "notional/hour", "fills/hour"
  double treated = 0.0;
  double control = 0.0;
  double diff = 0.0;
  double lo = 0.0;
  double hi = 0.0;
  bool noise = false;  // the A/A run's interval for this metric is known
  double noise_half_width = 0.0;
};

struct AbReport {
  std::string test;
  std::string assignment;
  std::uint64_t seed = 0;
  std::int64_t block_s = 0;
  bool aa = false;  // every arm runs the same parameters
  std::size_t journals = 0;
  std::int64_t first_ns = 0;  // the first and last event read
  std::int64_t last_ns = 0;
  bool mids = false;  // some journal has a BookTicker stream to mark against
  double maker_rebate_bps = 0.0;
  std::vector<std::string> instruments;  // "venue:symbol"
  std::vector<std::int64_t> horizons_ns;
  std::vector<AbArmSummary> arms;
  std::vector<AbCell> cells;
  std::uint64_t outside_fills = 0;   // of orders sent outside any arm
  std::uint64_t repeated_fills = 0;  // an execution reported again: counted once
  std::size_t pairs = 0;             // with time in every arm
  std::size_t incomplete_pairs = 0;  // missing an arm (the test began, ended or stopped in it)
  std::size_t clusters = 0;
  std::size_t resamples = 0;
  std::vector<AbEffect> effects;
};

// Throws std::runtime_error when a journal cannot be read, has no [ab] in its configuration, or
// the journals ran different tests.
[[nodiscard]] AbReport ab_report(std::span<const std::string> journals, const AbReportOptions& opt);
// Puts `aa`'s interval half-widths next to the matching effects of `r` (by metric).
void add_noise_floor(AbReport& r, const AbReport& aa);
[[nodiscard]] std::string format_ab_report(const AbReport& r);
[[nodiscard]] std::string ab_report_json(const AbReport& r);
// One row per cell.
[[nodiscard]] std::string ab_cells_csv(const AbReport& r);

}  // namespace fastmm::bt
