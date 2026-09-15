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
//
// threaded_sink() also accepts updates from other threads (a model thread of a Python strategy):
// they wait in an inbox until SimDriver next looks at the schedule, and are delivered delay() after
// the simulated time of that moment, which depends on thread timing.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

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

  // Like sink(), for a schedule that other threads publish to as well: an update from the thread
  // that called threaded_sink() (the thread that runs the SimDriver) is delivered delay() after
  // `clock`'s time; one from another thread goes to the inbox (post()).
  [[nodiscard]] ParamSink threaded_sink(const SimClock& clock) noexcept {
    clock_ = &clock;
    driver_thread_ = std::this_thread::get_id();
    return {this, [](void* c, const ParamUpdateMsg& m) noexcept {
              auto* self = static_cast<ParamSchedule*>(c);
              try {
                if (std::this_thread::get_id() == self->driver_thread_) {
                  self->at(self->clock_->now() + self->delay_, m);
                } else {
                  self->post(m);
                }
                return true;
              } catch (...) {
                return false;
              }
            }};
  }

  // Any thread: holds `m` until the next collect().
  void post(const ParamUpdateMsg& m) {
    const std::lock_guard<std::mutex> lock(inbox_mutex_);
    inbox_.push_back(m);
    inbox_pending_.store(true, std::memory_order_release);
  }
  // Driver thread: schedules the posted updates delay() after the clock's time (the clock passed to
  // sink() or threaded_sink()). Cheap when nothing was posted.
  void collect() {
    if (FASTMM_LIKELY(!inbox_pending_.load(std::memory_order_acquire))) return;
    const std::lock_guard<std::mutex> lock(inbox_mutex_);
    const Timestamp ts = clock_ != nullptr ? clock_->now() + delay_ : Timestamp{};
    for (const ParamUpdateMsg& m : inbox_) at(ts, m);
    inbox_.clear();
    inbox_pending_.store(false, std::memory_order_release);
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
  std::thread::id driver_thread_{};
  std::mutex inbox_mutex_;
  std::vector<ParamUpdateMsg> inbox_;
  std::atomic<bool> inbox_pending_{false};
};

}  // namespace fastmm::sim
