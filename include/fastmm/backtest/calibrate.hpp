#pragma once
// Calibration of the l2_queue fill model and the simulated latencies against live sessions
// (fastmm-data calibrate).
//
// Per journal: fill_check (fill_check.hpp) over a grid of queue_conservatism, then again without
// the BookTicker, without the trade tape and without both, which says what each input is worth;
// the time to fill and the model's queue ahead at the fills; and each venue's order round trips.
//
// The conservatism is picked by the error rate, (live only + model only) / (orders filled either
// way): lowest wins, ties go to the smaller |model/live qty - 1|, then to the larger value (the
// more conservative). Cross-validation fits it on each session alone and scores it on the others;
// the value printed in the snippet is fitted on all of them. A grid on which every value scores
// the same on every session does not identify it (the queues these sessions had were set by the
// ticker's touch, where the conservatism does not act), and the snippet keeps 1.
//
// Latency, per venue, from the journal's own clocks: send (the OutNewOrder's engine time) to the
// venue's time on the first ack (whole milliseconds on Binance: + 0.5 ms), and send to the first
// ack received (the round trip, one clock). The simulated venue draws each leg as fixed + a
// lognormal excess with mean `jitter` (sigma 0.5, sim/latency_model.hpp), whose 5th and 50th
// percentiles are fixed + 0.388 jitter and fixed + 0.8825 jitter: the fit solves those two for the
// measured ones. With millisecond venue times each one-way latency is only known to a millisecond
// interval; the sends fall anywhere in their millisecond, so fixed and jitter are fitted to the
// intervals by maximum likelihood, and the ack leg gets the rest of the fitted round trip.
//
// The one-way legs are fitted on isolated messages (sent at least 1 ms after the one before on
// their connection); cancels, timed by their cancel acks, get a path of their own from 20 or more.
// The messages sent within 1 ms of the one before fit the venue's per-connection service time
// (order_service_us): each is predicted at the later of its path's median and the previous
// message's prediction plus the service time, and the grid value with the smallest mean error
// wins. replay_cancels() then re-runs the fill check with the session's cancels taken in by the
// simulated venue (FillCheckOptions::cancel_paths), before and after these keys.
//
// compare_backtests() re-runs each session as a strip_own backtest with its embedded configuration
// (or a given one), once as configured and once with the fitted keys, and sets both beside the live
// session: orders, fills, time to fill, the PnL decomposition and markouts, every one of the three
// marked against the same mids (the journal's BookTickers, as recorded).
#include "fastmm/backtest/fill_check.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::bt {

// Orders by whether they filled live and whether the model filled them.
struct FillScore {
  std::uint64_t both = 0;
  std::uint64_t live_only = 0;
  std::uint64_t model_only = 0;
  std::uint64_t neither = 0;
  Qty live_qty;
  Qty model_qty;

  [[nodiscard]] static FillScore of(const FillCheckSummary& s) noexcept;
  FillScore& operator+=(const FillScore& o) noexcept;
  [[nodiscard]] std::uint64_t live() const noexcept { return both + live_only; }
  [[nodiscard]] double hit_rate() const noexcept;    // both / filled live
  [[nodiscard]] double miss_rate() const noexcept;   // live only / filled live
  [[nodiscard]] double false_rate() const noexcept;  // model only / not filled live
  // (live only + model only) / (both + live only + model only); 0 when nothing filled.
  [[nodiscard]] double error() const noexcept;
  [[nodiscard]] double qty_ratio() const noexcept;  // model / live quantity, 0 without live
};

// Percentiles of a sample (p10, p50, p90, p99), in its unit.
struct Quantiles {
  std::uint64_t n = 0;
  double p10 = 0;
  double p50 = 0;
  double p90 = 0;
  double p99 = 0;
  [[nodiscard]] static Quantiles of(std::vector<double> xs);
};

// One venue's order round trips and the latency model fitted to them (microseconds).
struct VenueLatency {
  // One order message on the venue's connection: its send time (engine clock), the venue time it
  // was taken in (its ack's, a cancel's cancel ack's; 0: none), and whether it is a cancel.
  struct Message {
    std::int64_t sent_ns = 0;
    std::int64_t venue_ns = 0;
    bool cancel = false;
  };
  VenueId venue;
  std::string name;
  bool ms_venue_times = false;             // every ack's venue time is a whole millisecond
  std::vector<double> to_venue_us;         // send -> the ack's venue time
  std::vector<double> round_trip_us;       // send -> first ack received
  std::vector<double> cancel_to_venue_us;  // cancel send -> its cancel ack's venue time
  std::vector<Message> messages;           // in send order; sessions one after another
  std::int64_t fixed_us = 0;               // [backtest] latency_fixed_us
  std::int64_t jitter_us = 0;              // latency_jitter_us
  std::int64_t ack_us = 0;                 // latency_ack_us
  std::int64_t ack_jitter_us = 0;          // latency_ack_jitter_us
  bool cancel_fitted = false;              // enough isolated cancels for a path of their own
  std::int64_t cancel_fixed_us = 0;        // latency_cancel_us
  std::int64_t cancel_jitter_us = 0;       // latency_cancel_jitter_us
  std::int64_t service_us = 0;             // order_service_us
  std::uint64_t burst_messages = 0;        // timed messages sent within 1 ms of the one before
  double burst_err_us = 0;      // their mean |venue time - predicted| without a service time
  double burst_err_fit_us = 0;  // ... with service_us
  void fit();
  // Mean |venue time - predicted| over the burst messages, the venue taking one message per
  // `service` us (0: none) after each path's median latency.
  [[nodiscard]] double burst_error(std::int64_t service) const;

 private:
  void fit_service();
};

struct SessionCalibration {
  std::string path;
  std::string name;       // file name without the directory
  FillCheckResult check;  // the l2_queue model, one entry per conservatism value
  std::vector<FillScore> grid;
  // The same grid without the BookTicker, without the trade tape, and without both.
  std::vector<FillScore> no_touch;
  std::vector<FillScore> no_tape;
  std::vector<FillScore> no_both;
  std::vector<VenueLatency> latency;
  // At the fitted conservatism, the session's cancels taken in by the simulated venue at their
  // recorded send times (FillCheckOptions::cancel_paths): through the fitted order path alone
  // (what a backtest did without the cancel path and service time), and through everything
  // fitted.
  bool cancels_replayed = false;
  FillScore cancels_before;
  FillScore cancels_after;
};

// Fit on `fit`, score on `test` (indices into Calibration::sessions).
struct CalibrationFold {
  std::vector<std::size_t> fit;
  std::vector<std::size_t> test;
  std::size_t pick = 0;  // index into Calibration::conservatism
  FillScore fit_score;
  FillScore test_score;
  FillScore test_best;  // the best value on the held-out sessions themselves
  std::size_t test_best_pick = 0;
};

struct Calibration {
  std::vector<double> conservatism;
  std::vector<SessionCalibration> sessions;
  std::vector<CalibrationFold> folds;  // one per session: fitted on it, scored on the others
  std::size_t pick = 0;                // fitted on every session
  bool identified = false;             // some session scores the grid values differently
  std::vector<VenueLatency> latency;   // every session's round trips, per venue

  // The [backtest] keys of the snippet in its order, values as TOML text (fill_model,
  // md_arrival, queue_conservatism, latency_*; [backtest.venues.<name>] keys as
  // "venues.<name>.latency_fixed_us" when there are several venues).
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> backtest_keys() const;
};

// The best index of `grid` by the rule above.
[[nodiscard]] std::size_t pick_conservatism(std::span<const double> conservatism,
                                            std::span<const FillScore> grid);

// Reads every journal (throws std::runtime_error when one cannot be opened).
[[nodiscard]] Calibration calibrate(std::span<const std::string> journals,
                                    std::span<const double> conservatism);
// Each venue's order round trips in one journal.
[[nodiscard]] std::vector<VenueLatency> measure_latency(JournalReader& reader);
// Fills SessionCalibration::cancels_* when a venue has a cancel path or a service time fitted.
void replay_cancels(Calibration& c);

[[nodiscard]] std::string format_calibration(const Calibration& c);
[[nodiscard]] std::string calibration_snippet(const Calibration& c);
[[nodiscard]] std::string calibration_csv(const Calibration& c);

// What a session traded, marked against the journal's mids.
struct TradeSummary {
  std::uint64_t orders = 0;   // new orders sent
  std::uint64_t rejects = 0;  // of them, refused by the venue (post-only, balance, ...)
  std::uint64_t fills = 0;
  std::uint64_t buys = 0;
  double qty = 0;       // base units
  double notional = 0;  // quote currency
  double time_to_fill_p50_ms = 0;
  // net == capture + drift - fees (backtest/metrics.hpp PnlDecomposition), quote currency.
  double capture = 0;
  double drift = 0;
  double fees = 0;
  double net = 0;
  double position = 0;              // base units at the end
  std::vector<double> markout_bps;  // per horizon, notional-weighted
  std::vector<std::uint64_t> markout_fills;
};

struct BacktestGap {
  std::string session;
  std::vector<std::int64_t> horizons_ns;
  TradeSummary live;
  TradeSummary before;  // the configuration as given
  TradeSummary after;   // with the fitted keys
};

// `configs`: TOML files to run instead of the sessions' embedded configurations: none, one for
// every session, or one per session. Throws std::runtime_error / ConfigError.
[[nodiscard]] std::vector<BacktestGap> compare_backtests(const Calibration& c,
                                                         std::span<const std::string> configs);
[[nodiscard]] std::string format_backtest_gaps(std::span<const BacktestGap> gaps);

}  // namespace fastmm::bt
