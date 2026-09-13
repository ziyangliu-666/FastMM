#pragma once
// MarketGenerator (8.3): synthetic order flow that drives a MatchingEngine as account 0.
//
//   * a latent mid random-walks on the tick grid (Poisson steps of +-1 tick);
//   * limit orders arrive as a Poisson process, side uniform, priced at a geometric
//     offset from the latent touch, quantity lognormal in lots;
//   * every resting order is cancelled after an exponential lifetime;
//   * market orders arrive as a Poisson process with lognormal quantity;
//   * optional two-state volatility regime (calm / volatile) scaling the mid step and
//     market-order rates.
//
// The generator is a discrete-event process: next_ts() is the time of its next action and
// step() performs exactly one. Everything random comes from the seeded Xoshiro256ss, so two
// generators with the same seed and parameters produce identical order streams.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastmm::sim {

struct MarketGeneratorParams {
  Price start_mid = Price::from_int(60'000);
  Price tick = Price::from_raw(1'000'000);  // 0.01
  Qty lot = Qty::from_raw(1'000);           // 0.00001
  double limit_rate_per_s = 200.0;          // limit order arrivals
  double cancel_rate_per_order_s = 0.5;     // each resting order's hazard
  double market_rate_per_s = 10.0;          // market order arrivals
  double mid_step_rate_per_s = 2.0;         // latent mid +-1 tick steps
  double offset_p = 0.35;                   // P(offset = k) = p (1-p)^k ticks from touch
  int base_spread_ticks = 1;                // touch = mid -+ base_spread_ticks
  double limit_qty_median_lots = 200.0;
  double limit_qty_sigma = 0.8;
  double market_qty_median_lots = 100.0;
  double market_qty_sigma = 1.0;
  bool regimes = true;
  double regime_switch_rate_per_s = 0.02;
  double volatile_mult = 4.0;  // mid-step and market rates in the volatile regime
  std::size_t max_resting = 4000;
};

struct MarketGeneratorStats {
  std::uint64_t limits = 0;
  std::uint64_t cancels = 0;
  std::uint64_t markets = 0;
  std::uint64_t mid_steps = 0;
  std::uint64_t regime_switches = 0;
  std::uint64_t rejected = 0;
};

class MarketGenerator {
 public:
  MarketGenerator(const MarketGeneratorParams& p,
                  std::uint64_t seed,
                  InstrumentId instrument,
                  Timestamp start,
                  Timestamp end = Timestamp::max());
  MarketGenerator(const MarketGenerator&) = delete;
  MarketGenerator& operator=(const MarketGenerator&) = delete;

  // Seeds the book with `levels` resting orders per side so the first snapshot is usable.
  void seed_book(MatchingEngine& me, int levels, Timestamp now) noexcept;

  [[nodiscard]] Timestamp next_ts() const noexcept;
  // Performs the next action (at next_ts()) against `me`.
  void step(MatchingEngine& me) noexcept;
  void set_end(Timestamp end) noexcept { end_ = end; }

  [[nodiscard]] Price mid() const noexcept { return mid_; }
  [[nodiscard]] bool volatile_regime() const noexcept { return volatile_; }
  [[nodiscard]] const MarketGeneratorStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const MarketGeneratorParams& params() const noexcept { return p_; }
  [[nodiscard]] InstrumentId instrument() const noexcept { return inst_; }
  [[nodiscard]] std::size_t tracked_orders() const noexcept { return resting_.size(); }

 private:
  enum class Kind : std::uint8_t { Limit, Cancel, Market, MidStep, Regime };
  [[nodiscard]] Duration exp_delay(double rate_per_s) noexcept;
  [[nodiscard]] Qty lognormal_qty(double median_lots, double sigma) noexcept;
  [[nodiscard]] double regime_mult() const noexcept { return volatile_ ? p_.volatile_mult : 1.0; }
  void do_limit(MatchingEngine& me, Timestamp now) noexcept;
  void do_cancel(MatchingEngine& me, Timestamp now) noexcept;
  void do_market(MatchingEngine& me, Timestamp now) noexcept;
  void compact(const MatchingEngine& me) noexcept;
  void reschedule_cancel(Timestamp now) noexcept;

  MarketGeneratorParams p_;
  Xoshiro256ss rng_;
  InstrumentId inst_;
  Timestamp end_;
  Price mid_;
  bool volatile_ = false;
  Timestamp next_limit_;
  Timestamp next_cancel_;
  Timestamp next_market_;
  Timestamp next_mid_;
  Timestamp next_regime_;
  std::uint64_t next_id_ = 1;
  std::vector<ClientOrderId> resting_;  // reserved once; may hold ids already gone
  MarketGeneratorStats stats_{};
};

}  // namespace fastmm::sim
