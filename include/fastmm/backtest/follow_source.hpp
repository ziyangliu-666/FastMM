#pragma once
// FollowSource: the market data of a session's journal while the session writes it, for a
// multiverse's runs to follow (backtest/shadow.hpp).
//
// JournalSource reads the whole journal before the first event: who rested where, the parameter
// updates, the account's flows. A journal that is still growing has none of that yet, so this
// source reads it with two cursors. A scout goes ahead and takes the records that say something
// about the session; the market data follows `settle` behind it. Before an event of venue time v
// goes out the scout has taken every record up to the first one received at or after v + settle:
// what the event is stripped with, and what the runs know, is that prefix, whatever has been
// written by then and however the blocks fell. The stream is a function of the journal alone, so
// reading a finished journal and following it as it grows give the same stream
// (tests/backtest/shadow_exact_test.cpp).
//
// It equals JournalSource's stream as long as nothing the journal says about a venue time comes
// more than `settle` after it. late() counts the records that did: 0 proves the two streams equal,
// which tests/backtest/shadow_exact_test.cpp checks, and more than 0 means a run may differ from a
// backtest of the finished journal.
//
// The start of a run is read first (the restored state, the positions reconciled before the first
// order, the first balance snapshots): up to `settle` past the first order or the end of the
// reconciliations the engine waits for, whichever comes first, or `start_wait` past its start when
// neither comes. recorded() holds that. What the runs need later comes in the stream as
// SessionRecordMsg, ahead of the market data: parameter updates, operator commands, flows of money,
// account refreshes, each `settle` ahead of its time.
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/journal_state.hpp"
#include "fastmm/backtest/journal_tail.hpp"
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/sim/recorded_session.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace fastmm::bt {

struct FollowOptions {
  Duration settle = seconds(10);
  Duration start_wait = seconds(300);
  Duration poll = milliseconds(5);          // wall time between looks at the end of the file
  Duration trailer_grace = seconds(30);     // JournalTail
  Duration dead_after = seconds(300);       // no block for this long: the writer is gone
  Duration reorder_window = seconds(1);     // the runs' venue ordering: what strip() looks back for
  const std::atomic<bool>* stop = nullptr;  // set: the stream ends where it is
  // A newer session's journal exists: this one's writer is gone once `dead_after_newer` has passed.
  std::function<bool()> superseded;
  Duration dead_after_newer = seconds(5);
  // Every record the scout takes, in journal order, on the reading thread.
  std::function<void(const EventHeader&)> on_record;
};

class FollowSource final : public MdSource {
 public:
  // Waits for the journal's prologue and reads the start of the session (above). Throws
  // std::runtime_error when the journal cannot be read or `stop` comes before there is a start.
  FollowSource(std::string path, const FollowOptions& options);

  const EventHeader* next() override;
  void reset() override;
  [[nodiscard]] Timestamp start_ts() const override { return first_ts_; }
  [[nodiscard]] const sim::RecordedSession* recorded() const override { return &session_; }
  [[nodiscard]] std::string note() const override;
  [[nodiscard]] bool ready() override;

  [[nodiscard]] const JournalReader& prologue() const noexcept { return scout_.prologue(); }
  // No market data in the whole session.
  [[nodiscard]] bool empty() const noexcept { return !first_ts_.valid(); }
  // Records that came more than `settle` after the venue time they speak of, by the stream's reach.
  [[nodiscard]] std::uint64_t late() const noexcept {
    return late_.load(std::memory_order_relaxed);
  }
  // By how much the latest of them came too late for what had gone out, ns.
  [[nodiscard]] std::int64_t late_max_ns() const noexcept {
    return late_max_ns_.load(std::memory_order_relaxed);
  }
  // What came too late for the start of the run: reconciled positions, balances.
  [[nodiscard]] std::uint64_t late_start() const noexcept {
    return late_start_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t seq_gaps() const noexcept {
    return seq_gaps_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t parts() const noexcept {
    return parts_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool writer_gone() const noexcept { return writer_gone_; }
  [[nodiscard]] std::uint64_t md_events() const noexcept {
    return md_events_.load(std::memory_order_relaxed);
  }
  // These counters and the two below can be read from any thread while the stream goes on.
  // The latest venue time the stream has reached and receive time the scout has, ns.
  [[nodiscard]] std::int64_t reached_ns() const noexcept {
    return reached_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::int64_t scout_ns() const noexcept {
    return scout_ns_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] const OwnOrderStripper* stripper() const noexcept { return stripper_.get(); }

 private:
  [[nodiscard]] bool fetch();
  // One more record for the scout; false when there is none now (`ended_` says if there ever will).
  bool scout_step();
  void admit(const EventHeader& h);
  void note_late(Timestamp t, std::int64_t reach) noexcept;
  void push_record(const EventHeader& h);
  [[nodiscard]] bool should_stop() const noexcept;
  void pause() const;
  void check_writer(JournalTail& tail);

  FollowOptions opt_;
  std::string path_;
  JournalTail scout_;
  JournalTail emit_;
  sim::RecordedSession session_;
  SessionRecorder recorder_;
  std::unique_ptr<OwnOrderStripper> stripper_;
  sim::EventBuf buf_;
  bool started_ = false;  // the start of the run has been read
  bool ended_ = false;    // the scout has read the whole session
  bool writer_gone_ = false;
  bool done_ = false;  // the emit cursor has read the whole session
  bool first_order_ = false;
  Timestamp first_ts_{};
  std::int64_t scout_clock_ = 0;
  std::int64_t first_clock_ = 0;
  // When the start is known, ns: the session's first order, or the last of the reconciliations
  // its engine waits for.
  static constexpr std::int64_t kNoMark = INT64_MAX;
  std::int64_t start_mark_ = kNoMark;
  std::uint32_t awaiting_ = 0;
  bool had_awaiting_ = false;
  std::int64_t reach_ = 0;                      // the latest venue time gone out
  std::vector<std::int64_t> instrument_reach_;  // ... per instrument
  std::atomic<std::int64_t> late_max_ns_{0};
  std::unordered_set<std::size_t> fills_seen_;
  std::atomic<std::uint64_t> late_{0};
  std::atomic<std::uint64_t> late_start_{0};
  std::atomic<std::uint64_t> md_events_{0};
  std::atomic<std::uint64_t> parts_{1};
  std::atomic<std::uint64_t> seq_gaps_{0};
  const EventHeader* held_ = nullptr;  // the next market data, in emit_'s block
  std::vector<VenueId> venues_snapshotted_;
  std::deque<std::vector<std::uint64_t>> pending_;  // SessionRecordMsgs not gone out yet
  std::vector<std::uint64_t> record_out_;           // the one gone out last
  std::atomic<std::int64_t> reached_{0};
  std::atomic<std::int64_t> scout_ns_{0};
};

}  // namespace fastmm::bt
