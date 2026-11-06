#pragma once
// fastmm-pnl ab: an [ab] test's results per arm, from the store (docs/reference/cli.md).
//
// Reader::ab_data() reads the test's fills with their arm, the mids [engine] markout_ms recorded
// after them, and each arm's instrument-hours from the arm assignments in param_updates.
// ab_readout() turns that into per-arm totals and, for every other arm against arm 0, the
// difference of each rate with a 95% interval from resampling whole UTC days.
//
//   pnl at h   side x (mid h after the fill - price) x qty x multiplier - fee, settlement currency
//   rate       an arm's total over its instrument-hours, or over its notional for bps
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::store {

struct AbFill {
  std::uint64_t session_id = 0;
  std::uint64_t seq = 0;  // fills.seq
  std::int64_t ts_ns = 0;
  int arm = -1;  // index into AbData::arms
  std::string symbol;
  int side = 0;  // +1 buy, -1 sell
  bool maker = false;
  double price = 0.0;
  double qty = 0.0;         // the execution, or the part of an estimate no execution named
  double multiplier = 1.0;  // contract size
  bool inverse = false;     // an inverse contract: no markout
  double fee = 0.0;         // booked, settlement currency; negative for a rebate
  // The mid at each AbData::horizons_ms entry; 0 where none was recorded.
  std::vector<double> mid;
};

// Instrument-hours one arm ran on one UTC day.
struct AbExposure {
  std::int64_t day = 0;  // days since 1970-01-01
  int arm = 0;
  double hours = 0.0;
};

struct AbData {
  std::string test;
  std::vector<std::string> arms;      // arm 0 is the control
  std::vector<std::string> params;    // each arm's parameters, "name=value ..."
  std::vector<std::int32_t> horizons_ms;  // recorded horizons, ascending, 0 (the fill) excluded
  std::vector<AbFill> fills;
  std::vector<AbExposure> exposure;
  std::size_t sessions = 0;
  std::size_t inverse_fills = 0;  // fills on inverse contracts, counted but not marked
};

struct AbReadoutOptions {
  std::vector<std::int32_t> horizons_ms;  // empty: every recorded horizon
  std::size_t resamples = 2000;
  std::uint64_t seed = 1;
};

struct AbArmTotals {
  std::string arm;
  std::string params;
  double inst_hours = 0.0;
  std::size_t days = 0;
  std::size_t fills = 0;
  std::size_t maker_fills = 0;
  double notional = 0.0;
  double fees = 0.0;
  std::vector<std::size_t> marked;  // per horizon: fills with a mid at it
  std::vector<double> markout_bps;  // per horizon, over the marked fills' notional
  std::vector<double> pnl;          // per horizon, marked fills only
};

struct AbComparison {
  std::string arm;
  std::string metric;
  double value = 0.0;
  double control = 0.0;
  double diff = 0.0;
  double lo = 0.0;  // 95% interval of diff
  double hi = 0.0;
};

struct AbReadout {
  std::string test;
  std::vector<std::int32_t> horizons_ms;
  std::vector<AbArmTotals> arms;
  std::vector<AbComparison> comparisons;
  std::size_t days = 0;  // UTC days with time in every arm: the clusters
  std::size_t resamples = 0;
};

// Throws std::invalid_argument for a horizon the store has not recorded.
[[nodiscard]] AbReadout ab_readout(const AbData& data, const AbReadoutOptions& opts);
// The aligned tables fastmm-pnl ab prints.
[[nodiscard]] std::string format_ab_readout(const AbReadout& r, const AbData& data);
// One CSV row per arm and UTC day: instrument-hours, fills, notional, fees, pnl per horizon.
[[nodiscard]] std::string ab_daily_csv(const AbReadout& r, const AbData& data);

}  // namespace fastmm::store
