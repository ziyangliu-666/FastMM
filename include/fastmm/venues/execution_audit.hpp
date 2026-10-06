#pragma once
// ExecutionAudit: a connector's read of its trade history for a fill audit
// (Venue::audit_executions, core/fill_audit.hpp). It is a ReplayScheduler of its own, beside the
// execution replay: its own streams, cursors and generations, so an audit neither moves the
// replay's watermark nor joins a reconciliation, and the replay never waits for an audit. The
// connector sends its queries through the same REST channel and rate limiter (Hooks::can_query: the
// bulk share), so an audit takes the weight a replay would, and none an order needs.
//
// start() reads every stream from `start_ms` to now, once; the rows go through `convert` into
// AuditFill and those inside [start_ms, end_ms] reach `done` when every stream has answered, with
// complete = false when one could not (a failed or unanswered query, close()). Nothing is retried:
// the caller asks again. Reactor thread only; control path.
#include "fastmm/core/fill_audit.hpp"
#include "fastmm/venues/replay_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::venues {

// Venue::audit_executions' answer: the executions read, and whether every query answered in full.
using ExecutionAuditDone = std::function<void(bool complete, std::vector<AuditFill> rows)>;

template <class Row>
class ExecutionAudit {
 public:
  // The row as an execution; false: not one to audit (an instrument not traded here).
  using Convert = std::function<bool(std::size_t stream, const Row& row, AuditFill& out)>;

  // `limits` as the connector's execution replay has them; the audit drops the sweep and the
  // order lookups. `hooks.finished` and `hooks.lookup` are the audit's own.
  void setup(std::string name, ReplayLimits limits, ReplaySchedulerBase::Hooks hooks, Convert c) {
    limits.sweep_ns = 0;
    limits.max_lookups = 0;
    hooks.lookup = {};
    hooks.active = {};
    hooks.finished = [this](bool complete) { finished(complete); };
    convert_ = std::move(c);
    sched_.setup(std::move(name),
                 "audited execution(s)",
                 limits,
                 std::move(hooks),
                 [this](std::size_t stream, const Row& row) {
                   AuditFill f;
                   if (convert_ && convert_(stream, row, f)) rows_.push_back(std::move(f));
                   return false;  // nothing is forwarded: the audit only reads
                 });
  }

  void set_streams(std::size_t n) { sched_.set_streams(n); }

  // Reads [start_ms, end_ms] (venue time). False, and `done` is not called: one is running, or
  // the connector cannot ask now (Hooks::ready).
  bool start(std::int64_t start_ms, std::int64_t end_ms, ExecutionAuditDone done) {
    if (sched_.active() || done_) return false;
    sched_.open(true);
    sched_.restart_from(start_ms);
    start_ms_ = start_ms;
    end_ms_ = end_ms;
    rows_.clear();
    done_ = std::move(done);
    if (!sched_.run()) {
      done_ = {};
      sched_.close();
      return false;
    }
    return true;
  }

  // The housekeeping timer: queries the rate limiter held back, and those past their timeout.
  void on_timer(std::int64_t now_ns) {
    if (sched_.active()) sched_.on_timer(now_ns);
  }
  // The connection went away: an audit in progress ends incomplete.
  void close() {
    const bool running = sched_.active();
    sched_.close();
    if (running) finished(false);
  }
  [[nodiscard]] bool active() const noexcept { return sched_.active(); }
  [[nodiscard]] std::uint64_t audits() const noexcept { return sched_.replays(); }
  // The connector's reply handler: answer(), failed(), expects(), send_waiting().
  [[nodiscard]] ReplayScheduler<Row>& scheduler() noexcept { return sched_; }

 private:
  void finished(bool complete) {
    // No retry, no next replay: the caller decides when to ask again.
    sched_.close();
    ExecutionAuditDone done = std::move(done_);
    done_ = {};
    std::vector<AuditFill> rows;
    rows.reserve(rows_.size());
    for (AuditFill& f : rows_) {
      if (f.time_ms >= start_ms_ && (end_ms_ == 0 || f.time_ms <= end_ms_))
        rows.push_back(std::move(f));
    }
    rows_.clear();
    if (done) done(complete, std::move(rows));
  }

  ReplayScheduler<Row> sched_;
  Convert convert_;
  ExecutionAuditDone done_;
  std::vector<AuditFill> rows_;
  std::int64_t start_ms_ = 0;
  std::int64_t end_ms_ = 0;
};

}  // namespace fastmm::venues
