#pragma once
// IEngineRunner: the only virtual seam in the system. The registry hands the app a runner
// for a (strategy, transport kind) pair; run()/stop() are called once each, never on the
// hot path. EngineRunner<E> adapts a concrete Engine instantiation.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/perp_book.hpp"
#include "fastmm/core/reject_counters.hpp"
#include "fastmm/core/underlying.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fastmm {

struct RunnerStats {
  std::uint64_t events = 0;
  std::uint64_t book_updates = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t fills = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t journal_overflows = 0;
  std::uint64_t records_dropped = 0;  // store records the ring could not take
  std::uint64_t transport_full = 0;
  std::uint64_t timers_fired = 0;
  std::int64_t realized_pnl_raw = 0;  // Notional raw (1e-8)
  std::int64_t unrealized_pnl_raw = 0;
  std::int64_t fees_raw = 0;
  std::uint64_t tick_to_trade_p50_ns = 0;
  std::uint64_t tick_to_trade_p99_ns = 0;
  std::uint64_t venue_rejects = 0;       // order rejects reported by the venues
  RejectCounts risk_rejects_by_reason;   // sums to risk_rejects
  RejectCounts venue_rejects_by_reason;  // sums to venue_rejects
  std::int64_t funding_raw = 0;          // part of realized_pnl_raw
  // Quote News held back after their old order ended: the balance no longer covered them
  // (QuoteStats::kept_balance). A side that stays unquoted for this shows here, not in the rejects.
  std::uint64_t balance_withheld = 0;
  // The first reason the global kill switch was set for, and the engine time it was set (0 while
  // it is not set).
  KillReason kill_reason = KillReason::None;
  std::int64_t kill_ts_ns = 0;
};

// One row of the engine's balance table (core/balance_book.hpp) as the monitors see it: the
// estimate of free, locked and total, the venue's equity and maintenance margin, raw amounts in the
// asset. `account`: the venue's account-wide margin row.
struct LiveBalance {
  std::int64_t free_raw = 0;
  std::int64_t locked_raw = 0;
  std::int64_t total_raw = 0;
  std::int64_t equity_raw = 0;
  std::int64_t maintenance_raw = 0;
  std::int64_t as_of_ns = 0;  // venue time of the last report
  char asset[9] = {};
  std::uint8_t venue = 0;
  std::uint8_t account = 0;
  std::uint8_t known = 0;  // the venue has reported it
  std::uint8_t pad_[4] = {};
  // What an internal transfer can take (BalanceBook::Row::transferable): free, capped by what the
  // venue lets leave the account where it says.
  std::int64_t transferable_raw = 0;
};
static_assert(sizeof(LiveBalance) == 72);
inline constexpr std::size_t kMaxLiveBalances = 32;

// One instrument of the perp table (core/perp_book.hpp) as the monitors see it: the venue's last
// mark, index and funding, and their ages at the time of publication (-1: never reported).
struct LivePerp {
  std::int64_t mark_raw = 0;
  std::int64_t index_raw = 0;
  double funding_rate = 0.0;  // per funding_interval
  std::int64_t funding_interval_ns = 0;
  std::int64_t next_funding_ns = 0;  // venue time; 0: continuous or not reported
  std::int64_t open_interest_raw = 0;
  std::int64_t mark_age_ns = -1;
  std::int64_t index_age_ns = -1;
  std::int64_t funding_age_ns = -1;
  std::uint64_t reports = 0;
  std::uint32_t instrument = 0;
  std::uint8_t venue = 0;
  std::uint8_t valued_at_mark = 0;  // the position is valued at `mark` now
  std::uint8_t mark_stale = 0;
  std::uint8_t funding_stale = 0;
};
static_assert(sizeof(LivePerp) == 88);
inline constexpr std::size_t kMaxLivePerps = 32;

// Fills `out` from one row of a PerpBook at `now`.
inline void fill_live_perp(
    LivePerp& out, const PerpBook& book, InstrumentId id, VenueId venue, Timestamp now) noexcept {
  const PerpRow& r = book.row(id);
  const auto age = [&](Timestamp at) { return at.valid() ? (now - at).ns : std::int64_t{-1}; };
  out.mark_raw = r.mark.raw;
  out.index_raw = r.index.raw;
  out.funding_rate = r.funding_rate;
  out.funding_interval_ns = r.funding_interval.ns;
  out.next_funding_ns = r.next_funding.ns;
  out.open_interest_raw = r.open_interest.raw;
  out.mark_age_ns = age(r.mark_at);
  out.index_age_ns = age(r.index_at);
  out.funding_age_ns = age(r.funding_at);
  out.reports = r.reports;
  out.instrument = id.value;
  out.venue = venue.value;
  out.valued_at_mark = r.marking ? 1 : 0;
  out.mark_stale = book.mark(id, now).stale ? 1 : 0;
  out.funding_stale = book.funding(id, now).stale ? 1 : 0;
}

// What a running engine publishes for other threads (monitors, fastmm-live's control loop): the
// runner stats, kill-switch state and the latency snapshot, refreshed with the latency publication
// (every second) and immediately whenever a kill switch trips or is reset.
struct EngineLiveStats {
  RunnerStats stats;
  std::uint64_t kills = 0;        // global trips (EngineStats::kills)
  std::uint64_t venue_kills = 0;  // venue trips (EngineStats::venue_kills)
  std::uint32_t kill_flags = 0;   // RiskEngine::kill_flags(): bit 0 global, bit 1 + venue per venue
  KillReason kill_reason = KillReason::None;  // why the global flag was first set
  // Operator flatten (ControlCommand::Flatten): how far it got, how many instruments in its scope
  // still hold a position and how many reduce-only orders it has sent.
  FlattenState flatten_state = FlattenState::Off;
  std::uint8_t pad_[2] = {};
  std::uint32_t flatten_instruments_left = 0;
  std::uint64_t flatten_orders = 0;
  // Why each venue's flag was first set, by venue id (RiskEngine::venue_slot).
  std::array<KillReason, kKillVenueSlots> venue_kill_reasons{};
  // Time-weighted quoting presence for the session: what a market-maker programme measures.
  std::int64_t quoting_elapsed_ns = 0;
  std::int64_t quoting_two_sided_ns = 0;
  // [risk] max_loss as the engine applies it now (fastmm-ctl limits changes it); zero when off.
  std::int64_t max_loss_raw = 0;
  // Parameter updates (ParamUpdateMsg) the engine has applied; the last one (its publisher's
  // sequence number, origin and source, and the engine time it was applied at); and the
  // publish_seq of the last one from the control socket, which fastmm-ctl param --wait waits on.
  // The [risk] bucket: whole tokens left (-1: no limit), the wait for the next, rate and burst.
  std::int64_t risk_tokens = -1;
  std::int64_t risk_token_wait_ns = 0;
  std::uint32_t risk_orders_per_sec = 0;
  std::uint32_t risk_burst = 0;
  std::uint64_t param_updates = 0;
  std::uint64_t param_control_seq = 0;
  std::uint64_t param_last_seq = 0;
  std::int64_t param_last_ns = 0;
  ParamUpdateMsg::Origin param_last_origin = ParamUpdateMsg::Origin::Strategy;
  char param_last_source[ParamUpdateMsg::kSourceLen] = {};
  // [risk.underlying], by the index of the session's UnderlyingPlan: the net position in base units
  // (raw Qty; not known while an inverse contract with a position has no current mark) and the
  // limit the engine applies now (0: none).
  struct Underlying {
    std::int64_t net_raw = 0;
    std::int64_t max_net_raw = 0;
    bool known = true;
  };
  std::array<Underlying, kMaxUnderlyings> underlyings{};
  // The balance table, in its order (the first kMaxLiveBalances rows).
  std::uint32_t balance_count = 0;
  std::array<LiveBalance, kMaxLiveBalances> balances{};
  // The perp table: the instruments that have reported, in id order (the first kMaxLivePerps).
  std::uint32_t perp_count = 0;
  std::array<LivePerp, kMaxLivePerps> perps{};
  LatencySnapshot latency;
};

// Human-readable one-line summary (src/core/engine_runner.cpp).
std::string format_runner_stats(const RunnerStats& s);

// Run-to-completion (fastmm-live [engine] threading = "single"): one iteration of the venue's
// network loop on the engine thread; returns the number of things it handled (0: idle).
using InlinePollFn = std::size_t (*)(void* ctx) noexcept;

class IEngineRunner {
 public:
  virtual ~IEngineRunner() = default;
  virtual void run() = 0;          // blocks until stop() or the feed ends
  virtual void stop() = 0;         // thread-safe request to leave run()
  virtual std::size_t step() = 0;  // process a bounded batch (sim/backtest driver)
  // run() with `poll(ctx)` before every step, on the calling thread. False: not supported.
  virtual bool run_inline(InlinePollFn /*poll*/, void* /*ctx*/) { return false; }
  // Processes the events the feed holds now (no timers); 0 when called from inside the engine.
  virtual std::size_t drain() { return 0; }
  [[nodiscard]] virtual RunnerStats stats() const = 0;
  // Safe to call from any thread while run() is active (a seqlocked copy, up to a second old).
  [[nodiscard]] virtual EngineLiveStats live_stats() const { return {}; }
  [[nodiscard]] virtual std::string_view strategy_name() const = 0;
  // The strategy's state taken since the last call (every EngineConfig::state_interval and at the
  // end), to be written to EngineConfig::state_file; false: none, or the strategy keeps no state.
  // Any thread while run() is active.
  virtual bool take_strategy_state(std::string& /*out*/) { return false; }
  // Bytes the strategy restores when ControlCommand::TakeOver reaches the engine (a warm standby's
  // state file, read once it holds the instance lock). Any thread, before the message is pushed.
  // NOLINTNEXTLINE(performance-unnecessary-value-param): by value, an override keeps it
  virtual void stage_strategy_state(std::string /*bytes*/) {}
};

// Owns a strategy + engine pair and exposes them through IEngineRunner.
template <class Engine, class Strategy>
class EngineRunner final : public IEngineRunner {
 public:
  EngineRunner(std::unique_ptr<Strategy> strategy, std::unique_ptr<Engine> engine)
      : strategy_(std::move(strategy)), engine_(std::move(engine)) {}
  void run() override { engine_->run(); }
  void stop() override { engine_->stop(); }
  std::size_t step() override { return engine_->step(); }
  bool run_inline(InlinePollFn poll, void* ctx) override {
    engine_->run_inline(poll, ctx);
    return true;
  }
  std::size_t drain() override { return engine_->drain(); }
  [[nodiscard]] RunnerStats stats() const override { return engine_->runner_stats(); }
  [[nodiscard]] EngineLiveStats live_stats() const override { return engine_->live_stats(); }
  [[nodiscard]] std::string_view strategy_name() const override { return Strategy::name(); }
  bool take_strategy_state(std::string& out) override { return engine_->take_strategy_state(out); }
  void stage_strategy_state(std::string bytes) override {
    engine_->stage_strategy_state(std::move(bytes));
  }
  [[nodiscard]] Engine& engine() noexcept { return *engine_; }
  [[nodiscard]] Strategy& strategy() noexcept { return *strategy_; }

 private:
  std::unique_ptr<Strategy> strategy_;
  std::unique_ptr<Engine> engine_;
};

}  // namespace fastmm
