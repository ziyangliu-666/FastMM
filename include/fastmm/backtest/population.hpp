#pragma once
// Population backtest: thousands of variants of a strategy over one pass of order-by-order data.
//
// A backtest answers what one configuration would have done. A desk asks a different question:
// which of all the configurations it could run, and how far are they apart. run_backtest() and the
// multiverse (multiverse.hpp) answer it one engine at a time: every variant keeps a whole engine,
// OMS, quote manager, risk check and simulated venue, and each further variant costs most of what
// the first did. Under fill_model = "l3_queue" none of that state depends on the other variants:
// the recorded orders, the venue's books and the engine's view of the market are the same for all
// of them, and a variant owns only its own orders, their places in the recorded queues, its
// position and the decisions of its strategy.
//
// A population keeps that split. One pass reads the data, keeps the recorded orders, the venue's
// books, the engine's books and the market-data wire once (Shared), and each variant is a lane: a
// few hundred bytes per instrument holding the engine's order per slot, the quote manager's slot,
// the venue's order with its arrival place, its last quoted mid, its position and its cash. An
// event reaches a lane only when it can change something there: an execution reaches the lanes
// resting at or better than the executed order, a book update reaches the lanes the mid moved past
// their requote threshold, and a lane's own messages wake it at their arrival times.
//
// Each lane runs basic_mm's quoting (compute_quotes, keep_passive), the quote manager's slot rules
// (hysteresis, pending targets, pulls, replace), the OMS states, the self-trade check and the
// simulated venue's l3_queue rules (post-only check, taker sweep of a replace, arrival place,
// executions that reach it, crosses), with the same fixed-point code, the same message order and
// the same latency paths as the engine. tests/backtest/population_test.cpp and
// fastmm-bench-population --check run each lane's configuration through run_backtest() and compare
// fills, position and PnL exactly.
//
// Scope: basic_mm with one level per side, fill_model = "l3_queue", own_orders_in_feed = false,
// no balances, fixed latencies (no jitter, no drop, no order service time), one venue,
// [risk] limits off except stp. check_population() names the first setting outside it.
#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/lane_kernel.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/md_source.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::bt {

// One variant: basic_mm's parameters and the lane's latencies.
struct LaneSpec {
  Ratio half_spread;  // half_spread_bps
  Ratio skew;         // skew_bps_per_unit
  Qty quote_qty;      // per level
  Qty max_inventory;  // 0 = no cap
  int requote_threshold_ticks = 1;
  Duration order_latency{};  // engine -> venue, orders, replaces and cancels
  Duration ack_latency{};    // venue -> engine, acks and fills
};

// What a lane did, in the units the engine books.
struct LaneResult {
  std::uint64_t fills = 0;
  std::uint64_t maker_fills = 0;
  std::uint64_t taker_fills = 0;
  std::uint64_t orders = 0;    // News sent
  std::uint64_t replaces = 0;  // Replaces sent
  std::uint64_t cancels = 0;   // Cancels sent
  std::uint64_t venue_rejects = 0;
  std::uint64_t risk_rejects = 0;  // stp refusals
  Qty volume{};                    // base
  Notional cash{};                 // sum of signed notional (sells positive), before fees
  Notional fees{};                 // paid (negative: rebates received)
  Notional net_pnl{};              // cash - fees + positions at the venue's final mid
  std::int64_t position_sum = 0;   // sum of |position| raw over instruments at the end
  std::uint64_t fill_hash = 0;     // order-sensitive hash of (time, instrument, side, px, qty)
  std::vector<Qty> positions;      // per instrument at the end
};

// One fill of a lane, at its venue time (as BacktestResult::fills).
struct LaneFill {
  Timestamp ts;
  std::uint32_t instrument;
  Side side;
  Price price;
  Qty qty;
  Liquidity liquidity;
};

struct PopulationStats {
  std::uint64_t source_events = 0;
  std::uint64_t md_events = 0;
  std::uint64_t lane_wakes = 0;
  std::uint64_t lane_requotes = 0;
  double seconds = 0.0;
};

// Empty when `cfg` is inside the scope above, else the first setting outside it.
[[nodiscard]] std::string check_population(const BacktestConfig& cfg);

// `cfg` carries the shared settings (instruments, fees, quote manager, reorder window); each lane
// its own strategy parameters and latencies. Lanes split over `threads` passes (<= 0: hardware
// concurrency), each reading its own source from `source_factory`. Throws std::invalid_argument
// when check_population() refuses `cfg`.
[[nodiscard]] std::vector<LaneResult> run_population(
    const BacktestConfig& cfg,
    std::span<const LaneSpec> lanes,
    const std::function<std::unique_ptr<sim::MdSource>()>& source_factory,
    int threads = 1,
    PopulationStats* stats = nullptr,
    std::vector<std::vector<LaneFill>>* fills = nullptr);

// The shared market of a population written down for lanes that run on their own (lane_kernel.hpp):
// per instrument its records in the order the population takes them, the venue's best levels after
// each book update, every recorded arrival's venue time, and the instruments' settings.
struct Tape {
  std::vector<std::vector<lk::Rec>> recs;  // per instrument
  std::vector<lk::Levels> levels;
  std::vector<std::int64_t> adds;
  std::vector<lk::Inst> insts;
  lk::Shared shared{};

  // A binary file of the above (little-endian, this build's struct layouts).
  void save(const std::string& path) const;
  [[nodiscard]] static Tape load(const std::string& path);
};

// One pass over the data with no lanes, writing the tape. Throws as run_population().
[[nodiscard]] Tape compile_tape(const BacktestConfig& cfg, sim::MdSource& source);

// lane_kernel.hpp's form of a lane: requote_threshold_ticks must be at least 1 (the tape leaves out
// market-data messages that change nothing a lane can see).
[[nodiscard]] lk::Params kernel_params(const LaneSpec& s);

// Every (lane, instrument) of `lanes` run by lane_kernel.hpp over the tape on `threads` threads;
// out[lane * instruments + instrument].
[[nodiscard]] std::vector<lk::Out> run_tape(const Tape& tape,
                                            std::span<const LaneSpec> lanes,
                                            int threads = 1);

// The lane of `cfg`'s own basic_mm parameters and [backtest] latencies.
[[nodiscard]] LaneSpec lane_from_config(const BacktestConfig& cfg);

}  // namespace fastmm::bt
