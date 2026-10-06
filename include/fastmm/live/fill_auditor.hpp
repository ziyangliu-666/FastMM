#pragma once
// FillAuditor: fastmm-live's periodic fill audit ([venues.<name>] fill_audit_interval_s). Every
// interval it asks the venue for the account's executions in a window ending fill_audit_lag_s ago
// (Venue::audit_executions, on the venue's network thread: its REST channel and rate limiter),
// reads what the store holds for the venue over the same window (store::Reader::booked_fills, every
// session of the engine) and compares them execution by execution (core/fill_audit.hpp). What
// differs is logged (WARN per execution, at most kMaxLogged per kind and audit, then an ERROR
// summary) and counted (counters(): the status file and the metrics export). With
// fill_audit_mode = "book" an execution the engine never booked is booked: the venue's execution
// replay runs from the first such one (Venue::request_executions), and the OMS keeps only what it
// has not seen.
//
// Windows follow each other from the session's start: one audit covers what the last one did not,
// and one that could not be read (the venue's or the store's side) is read again with the next. The
// thread is the auditor's own (fm-audit); it waits for the venue's answer and reads the store
// there, so neither the engine nor a network thread waits for it.
#include "fastmm/core/fill_audit.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/venues/venue.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fastmm::live {

struct FillAuditTarget {
  std::string venue;                   // [venues.<name>]
  venues::Venue* connector = nullptr;  // outlives the auditor
  // Runs a task on the venue's network thread (its reactor's post()).
  std::function<void(std::function<void()>)> post;
  std::int64_t interval_ms = 0;
  std::int64_t lag_ms = 0;
  bool book = false;  // fill_audit_mode = "book"
};

// Per venue, cumulative for the session. Read from any thread.
struct FillAuditCounters {
  std::uint64_t audits = 0;      // audits that compared both sides
  std::uint64_t failures = 0;    // audits that could not read one side (asked again)
  std::uint64_t missing = 0;     // executions the venue has and the engine never booked
  std::uint64_t phantom = 0;     // executions the engine booked and the venue does not have
  std::uint64_t mismatched = 0;  // executions both have that differ in a field
  std::uint64_t duplicates = 0;  // executions the engine stored more than once
  std::uint64_t booked = 0;      // "book": replays started for missing executions
};

class FillAuditor {
 public:
  // The store's executions for `venue` with the venue's time in [from_ms, to_ms].
  using ReadBooked = std::function<Result<std::vector<AuditFill>, std::string>(
      const std::string& venue, std::int64_t from_ms, std::int64_t to_ms)>;
  // An audit's rows reach this far past its window on both sides: a fill the two sides stamp a
  // little differently is compared rather than reported missing on one and phantom on the other.
  static constexpr std::int64_t kMarginMs = 60'000;
  // The venue's answer is waited for this long; then the audit has failed.
  static constexpr std::int64_t kFetchTimeoutMs = 600'000;
  static constexpr std::size_t kMaxLogged = 20;

  // `start_ms`: where the first window starts (Unix ms, the session's start).
  FillAuditor(std::vector<FillAuditTarget> targets, ReadBooked read, std::int64_t start_ms);
  ~FillAuditor();
  FillAuditor(const FillAuditor&) = delete;
  FillAuditor& operator=(const FillAuditor&) = delete;

  // The thread (fm-audit): each venue's first audit one interval after start().
  void start();
  void stop();

  // One audit of target `i` with the clock at `now_ms` (Unix ms): the window from where the last
  // one ended to now_ms - lag. Blocks until the venue answers (kFetchTimeoutMs at most). Returns
  // the report; an error when a side could not be read or the window is empty. The thread calls it;
  // so can a test.
  Result<FillAuditReport, std::string> audit_once(std::size_t i, std::int64_t now_ms);

  [[nodiscard]] std::size_t size() const noexcept { return targets_.size(); }
  [[nodiscard]] const FillAuditTarget& target(std::size_t i) const { return targets_[i]; }
  [[nodiscard]] FillAuditCounters counters(std::size_t i) const noexcept;

 private:
  struct Counters {
    std::atomic<std::uint64_t> audits{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> missing{0};
    std::atomic<std::uint64_t> phantom{0};
    std::atomic<std::uint64_t> mismatched{0};
    std::atomic<std::uint64_t> duplicates{0};
    std::atomic<std::uint64_t> booked{0};
  };
  void run();
  Result<FillAuditReport, std::string> audit(std::size_t i, std::int64_t now_ms);
  void report(std::size_t i, const FillAuditReport& r);

  std::vector<FillAuditTarget> targets_;
  ReadBooked read_;
  std::vector<std::int64_t> next_from_ms_;  // per target: where its next window starts
  // Per target: what was reported (kind, symbol, id, side) -> the execution's time, so that a
  // window read again reports nothing twice. Pruned behind the window.
  std::vector<std::map<std::string, std::int64_t>> reported_;
  std::unique_ptr<Counters[]> counters_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<bool> stopping_{false};  // stop_, for the waits outside mu_
  std::thread thread_;
};

}  // namespace fastmm::live
