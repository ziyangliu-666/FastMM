#pragma once
// ParamSchedule: parameter updates of a backtest, delivered to the engine at simulated times
// (ADR-0013). SimDriver hands the earliest update to the engine when its time comes, like any other
// input, so the journal records it at that time and a replay applies it at the same point.
//
//   sim::ParamSchedule schedule;
//   schedule.at(start + seconds(1), update);                     // an update built elsewhere
//   schedule.set_delay(milliseconds(5));                         // sink(): 5 ms after the publish
//   ParamPublisher pub(schedule.sink(backend.clock), strategy.params());
//   session.set_param_schedule(&schedule);                       // bt::BacktestSession
//
// Pending updates do not keep a run alive: SimDriver ends when its data does.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <cstddef>
#include <cstdint>
#include <map>

namespace fastmm::sim {

class ParamSchedule {
 public:
  // Delivers `m` at `ts`; updates with the same time keep the order they were added in.
  void at(Timestamp ts, const ParamUpdateMsg& m) { queue_.emplace(ts.ns, m); }

  // Added to the time of updates that arrive through sink() (the slow tier's delay).
  void set_delay(Duration d) noexcept { delay_ = d; }
  [[nodiscard]] Duration delay() const noexcept { return delay_; }
  // A ParamPublisher sink: each update is delivered delay() after `clock`'s time at the publish.
  [[nodiscard]] ParamSink sink(const SimClock& clock) noexcept {
    clock_ = &clock;
    return {this, [](void* c, const ParamUpdateMsg& m) noexcept {
              auto* self = static_cast<ParamSchedule*>(c);
              try {
                self->at(self->clock_->now() + self->delay_, m);
                return true;
              } catch (...) {
                return false;
              }
            }};
  }

  [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }
  // The earliest delivery time (Timestamp::max() when empty).
  [[nodiscard]] Timestamp next_ts() const noexcept {
    return queue_.empty() ? Timestamp::max() : Timestamp{queue_.begin()->first};
  }
  // Removes the earliest update; its recv_ts is the delivery time.
  [[nodiscard]] ParamUpdateMsg pop() noexcept {
    ParamUpdateMsg m = queue_.begin()->second;
    m.hdr.recv_ts = Timestamp{queue_.begin()->first};
    queue_.erase(queue_.begin());
    return m;
  }

 private:
  std::multimap<std::int64_t, ParamUpdateMsg> queue_;
  Duration delay_{};
  const SimClock* clock_ = nullptr;
};

}  // namespace fastmm::sim
