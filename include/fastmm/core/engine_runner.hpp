#pragma once
// IEngineRunner: the only virtual seam in the system. The registry hands the app a runner
// for a (strategy, transport kind) pair; run()/stop() are called once each, never on the
// hot path. EngineRunner<E> adapts a concrete Engine instantiation.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/reject_counters.hpp"

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
};

// What a running engine publishes for other threads (monitors, fastmm-live's control loop): the
// runner stats, kill-switch state and the latency snapshot, refreshed with the latency publication
// (every second) and immediately whenever a kill switch trips or is reset.
struct EngineLiveStats {
  RunnerStats stats;
  std::uint64_t kills = 0;        // global trips (EngineStats::kills)
  std::uint64_t venue_kills = 0;  // venue trips (EngineStats::venue_kills)
  std::uint32_t kill_flags = 0;   // RiskEngine::kill_flags(): bit 0 global, bit 1 + venue per venue
  KillReason kill_reason = KillReason::None;  // why the global flag was first set
  std::uint8_t pad_[3] = {};
  // Why each venue's flag was first set, by venue id (RiskEngine::venue_slot).
  std::array<KillReason, kKillVenueSlots> venue_kill_reasons{};
  LatencySnapshot latency;
};

// Human-readable one-line summary (src/core/engine_runner.cpp).
std::string format_runner_stats(const RunnerStats& s);

class IEngineRunner {
 public:
  virtual ~IEngineRunner() = default;
  virtual void run() = 0;          // blocks until stop() or the feed ends
  virtual void stop() = 0;         // thread-safe request to leave run()
  virtual std::size_t step() = 0;  // process a bounded batch (sim/backtest driver)
  [[nodiscard]] virtual RunnerStats stats() const = 0;
  // Safe to call from any thread while run() is active (a seqlocked copy, up to a second old).
  [[nodiscard]] virtual EngineLiveStats live_stats() const { return {}; }
  [[nodiscard]] virtual std::string_view strategy_name() const = 0;
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
  [[nodiscard]] RunnerStats stats() const override { return engine_->runner_stats(); }
  [[nodiscard]] EngineLiveStats live_stats() const override { return engine_->live_stats(); }
  [[nodiscard]] std::string_view strategy_name() const override { return Strategy::name(); }
  [[nodiscard]] Engine& engine() noexcept { return *engine_; }
  [[nodiscard]] Strategy& strategy() noexcept { return *strategy_; }

 private:
  std::unique_ptr<Strategy> strategy_;
  std::unique_ptr<Engine> engine_;
};

}  // namespace fastmm
