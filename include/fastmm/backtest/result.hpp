#pragma once
// BacktestResult (8.4): structure-of-arrays rows (numpy-friendly: each column is one
// contiguous vector) plus the summary Metrics and the raw counters of every component.
// Prices, quantities and notionals are raw 1e-8 fixed-point int64; the CSV/JSON writers
// format them as exact decimals.
#include "fastmm/backtest/metrics.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/params.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::bt {

struct FillRows {
  std::vector<std::int64_t> ts;  // venue time of the execution
  std::vector<std::uint32_t> instrument;
  std::vector<std::int8_t> side;    // 0 buy / 1 sell
  std::vector<std::int64_t> price;  // raw 1e-8
  std::vector<std::int64_t> qty;
  std::vector<std::int64_t> fee;  // raw notional, negative == rebate
  std::vector<std::uint64_t> cl_ord_id;
  std::vector<std::uint8_t> liquidity;  // Liquidity: 1 maker, 2 taker
  std::vector<std::int64_t> mid;        // venue mid at fill time (raw)
  std::vector<std::int64_t> best_bid;   // venue touch at fill time (raw); 0 when that side is empty
  std::vector<std::int64_t> best_ask;
  // Displayed quantity still ahead of the order when it filled (raw); -1 when the fill model
  // does not track a queue position (FillModel::Matching).
  std::vector<std::int64_t> queue_ahead;
  // Markout horizons, in nanoseconds of simulated time, and the venue mid of the fill's
  // instrument at fill ts + horizon (raw). markout_mid[h][i] is 0 when the run ended before the
  // horizon or the book had no two sides there: that fill is excluded from the horizon, never
  // marked at a substitute price.
  std::vector<std::int64_t> markout_horizon_ns;
  std::vector<std::vector<std::int64_t>> markout_mid;

  [[nodiscard]] std::size_t size() const noexcept { return ts.size(); }
  void reserve(std::size_t n);
};

struct EquityRows {
  std::vector<std::int64_t> ts;        // bar end
  std::vector<std::int64_t> realized;  // raw notional
  std::vector<std::int64_t> unrealized;
  std::vector<std::int64_t> fees;
  std::vector<std::int64_t> position;  // raw qty, net over all instruments
  std::vector<std::int64_t> mid;       // raw price, instrument 0
  std::vector<std::uint8_t> quoted;    // bit 0 bid resting, bit 1 ask resting (instrument 0)
  [[nodiscard]] std::size_t size() const noexcept { return ts.size(); }
  [[nodiscard]] std::int64_t equity(std::size_t i) const noexcept {
    return realized[i] + unrealized[i] - fees[i];
  }
  void reserve(std::size_t n);
};

inline constexpr std::uint8_t kOrderKindNew = 0;
inline constexpr std::uint8_t kOrderKindCancel = 1;
inline constexpr std::uint8_t kOrderKindReplace = 2;

struct OrderRows {
  std::vector<std::int64_t> ts;          // engine send time (virtual)
  std::vector<std::int64_t> venue_ts;    // arrival at the venue; 0 when dropped
  std::vector<std::int64_t> trigger_ts;  // T0 of the event that caused the order (0 unknown)
  std::vector<std::uint64_t> cl_ord_id;
  std::vector<std::uint32_t> instrument;
  std::vector<std::int8_t> side;  // 0 buy / 1 sell / -1 n/a (cancel, replace)
  std::vector<std::int64_t> price;
  std::vector<std::int64_t> qty;
  std::vector<std::uint8_t> kind;  // kOrderKindNew / Cancel / Replace
  std::vector<std::uint8_t> type;  // OrderType (new orders)
  [[nodiscard]] std::size_t size() const noexcept { return ts.size(); }
  void reserve(std::size_t n);
};

// Wall time of one slow method of a Python strategy over a backtest.
struct SlowMethodTiming {
  std::string name;
  std::uint64_t calls = 0;
  std::uint64_t wall_p50_ns = 0;
  std::uint64_t wall_p99_ns = 0;
  std::uint64_t wall_max_ns = 0;
};

struct BacktestResult {
  std::string strategy;
  ParamMap params;
  std::uint64_t seed = 0;
  FillRows fills;
  EquityRows equity;
  OrderRows orders;
  Metrics metrics;
  RunnerStats engine;  // the engine's own counters and positions
  sim::SimTransportStats transport;
  std::string outbound_sha256;
  std::uint64_t outbound_messages = 0;
  std::uint64_t md_events = 0;  // market-data messages delivered to the engine
  std::uint64_t engine_steps = 0;
  std::int64_t start_ts = 0;
  std::int64_t end_ts = 0;
  double wall_seconds = 0.0;
  // Slow methods of a Python strategy (the Python package fills this in; empty otherwise).
  std::vector<SlowMethodTiming> slow_methods;

  [[nodiscard]] const Metrics& summary() const noexcept { return metrics; }
  // Human-readable table, one metric per line.
  [[nodiscard]] std::string summary_table() const;
  [[nodiscard]] std::string summary_json() const;
  [[nodiscard]] std::string equity_csv() const;
  [[nodiscard]] std::string fills_csv() const;
  [[nodiscard]] std::string orders_csv() const;
  // Writes equity.csv, fills.csv, orders.csv and summary.json into `dir` (created if needed).
  // Returns false on I/O failure.
  [[nodiscard]] bool write_all(const std::string& dir) const;
};

}  // namespace fastmm::bt
