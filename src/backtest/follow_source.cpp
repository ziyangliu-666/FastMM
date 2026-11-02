#include "fastmm/backtest/follow_source.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/core/thread_utils.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace fastmm::bt {

namespace {

bool is_order_event(const EventHeader& h) noexcept {
  switch (h.type) {
    case EventType::OrderAck:
    case EventType::OrderFill:
    case EventType::OrderCancelAck:
    case EventType::OrderExpired:
      return true;
    default:
      return false;
  }
}

// How far back a stripped event's venue time can lie behind the latest order event: the settle, the
// venue ordering's window, and a margin for the feed's own lag behind the order stream.
Duration own_retention(const FollowOptions& o) noexcept {
  return o.settle + o.reorder_window * 2 + seconds(30);
}

}  // namespace

FollowSource::FollowSource(std::string path, const FollowOptions& options)
    : opt_(options),
      path_(std::move(path)),
      scout_(path_, options.trailer_grace),
      emit_(path_, options.trailer_grace),
      recorder_(session_) {
  emit_.set_release_behind(true);
  while (!scout_.open()) {
    if (should_stop()) throw std::runtime_error("follow: stopped before " + path_ + " appeared");
    pause();
  }
  while (!emit_.open()) pause();
  const JournalReader& head = scout_.prologue();
  if (feed_shows_own(head.header()))
    stripper_ = std::make_unique<OwnOrderStripper>(own_retention(opt_));
  for (const JournalParam& p : head.params())
    session_.params.table.emplace_back(std::string(p.name), p.type);
  session_.growing = true;
  awaiting_ = head.has_session() ? head.header().await_reconcile : 0;
  had_awaiting_ = awaiting_ != 0;
  instrument_reach_.assign(head.instruments().size(), 0);
  // The start of the run.
  while (!started_) {
    if (scout_step()) continue;
    if (ended_) break;
    if (should_stop()) throw std::runtime_error("follow: stopped at the start of " + path_);
    pause();
  }
  recorder_.finish();
  started_ = true;
  // The first market data: the run's clock starts there.
  while (held_ == nullptr && !done_) {
    if (should_stop()) throw std::runtime_error("follow: stopped at the start of " + path_);
    if (!fetch() && held_ == nullptr) pause();
  }
  if (held_ != nullptr) first_ts_ = venue_ts(*held_);
}

bool FollowSource::should_stop() const noexcept {
  return opt_.stop != nullptr && opt_.stop->load(std::memory_order_relaxed);
}

void FollowSource::pause() const {
  sleep_for(opt_.poll);
}

void FollowSource::check_writer(JournalTail& tail) {
  const bool gone = tail.idle_for(opt_.dead_after) ||
                    (opt_.superseded && tail.idle_for(opt_.dead_after_newer) && opt_.superseded());
  if (gone) {
    scout_.give_up();
    emit_.give_up();
    writer_gone_ = true;
  }
}

bool FollowSource::scout_step() {
  if (ended_) return false;
  const EventHeader* h = nullptr;
  const JournalTail::Step step = scout_.next(h);
  if (step == JournalTail::Step::End) {
    ended_ = true;
    return false;
  }
  if (step == JournalTail::Step::Wait) {
    check_writer(scout_);
    return false;
  }
  parts_.store(scout_.parts(), std::memory_order_relaxed);
  seq_gaps_.store(scout_.seq_gaps(), std::memory_order_relaxed);
  admit(*h);
  return true;
}

void FollowSource::note_late(Timestamp t, std::int64_t reach) noexcept {
  if (!started_ || !t.valid() || t.ns >= reach) return;
  late_.fetch_add(1, std::memory_order_relaxed);
  if (reach - t.ns > late_max_ns_.load(std::memory_order_relaxed))
    late_max_ns_.store(reach - t.ns, std::memory_order_relaxed);
}

void FollowSource::push_record(const EventHeader& h) {
  std::vector<std::uint64_t> bytes((sizeof(SessionRecordMsg) + h.len + 7) / 8);
  auto* m = reinterpret_cast<SessionRecordMsg*>(bytes.data());
  init_header(*m,
              EventType::Padding,
              InstrumentId{},
              VenueId{},
              static_cast<std::uint32_t>(sizeof(SessionRecordMsg) + h.len));
  std::memcpy(bytes.data() + sizeof(SessionRecordMsg) / 8, &h, h.len);
  pending_.push_back(std::move(bytes));
}

void FollowSource::admit(const EventHeader& h) {
  std::int64_t at = h.recv_ts.ns;
  if (h.type == EventType::EngineTime && h.len >= sizeof(EngineTimeMsg))
    at = msg_cast<EngineTimeMsg>(&h).engine_ts.ns;
  if (at > scout_clock_) {
    scout_clock_ = at;
    scout_ns_.store(at, std::memory_order_relaxed);
  }
  if (first_clock_ == 0) first_clock_ = scout_clock_;
  if (opt_.on_record) opt_.on_record(h);
  if (stripper_) stripper_->take(h);
  const bool outbound = (h.flags & EventHeader::kOutbound) != 0;
  // An order's times bear on its own instrument's market data. An execution the venue reports again
  // later (OrderFillMsg::kReplayed) says nothing new.
  if (stripper_ && !outbound && is_order_event(h)) {
    bool repeated = false;
    if (h.type == EventType::OrderFill) {
      const std::string_view exec = msg_cast<OrderFillMsg>(&h).exec_id.view();
      if (!exec.empty()) {
        repeated = !fills_seen_
                        .insert(std::hash<std::string_view>{}(exec) ^
                                (std::size_t{h.instrument.value} << 40U))
                        .second;
      }
    }
    if (!repeated) {
      note_late(venue_ts(h),
                h.instrument.value < instrument_reach_.size()
                    ? instrument_reach_[h.instrument.value]
                    : reach_);
    }
  }
  if (!started_) {
    recorder_.add(h);
    if (outbound && (h.type == EventType::OutNewOrder || h.type == EventType::OutReplace) &&
        !first_order_) {
      first_order_ = true;
      start_mark_ = std::min(start_mark_, scout_clock_);
    }
    // The venues the engine waits for have all reconciled: it has what it starts from.
    if (!outbound && h.type == EventType::Reconcile &&
        msg_cast<ReconcileMsg>(&h).kind == ReconcileMsg::Kind::End && h.venue.value < 32) {
      awaiting_ &= ~(1U << h.venue.value);
      if (awaiting_ == 0 && had_awaiting_) start_mark_ = std::min(start_mark_, scout_clock_);
    }
    if ((start_mark_ != kNoMark && scout_clock_ >= start_mark_ + opt_.settle.ns) ||
        scout_clock_ >= first_clock_ + opt_.start_wait.ns)
      started_ = true;
    return;
  }
  if (outbound) return;
  // What the runs read after their start.
  switch (h.type) {
    case EventType::ParamUpdate:
      if (h.len >= sizeof(ParamUpdateMsg)) {
        note_late(h.recv_ts, reach_);
        push_record(h);
      }
      return;
    case EventType::Control:
      if (h.len >= sizeof(ControlMsg) &&
          sim::replayable_control(msg_cast<ControlMsg>(&h).command)) {
        note_late(h.recv_ts, reach_);
        push_record(h);
      }
      return;
    case EventType::AccountFlow:
      if (h.len >= sizeof(AccountFlowMsg)) {
        note_late(sim::account_time(h), reach_);
        push_record(h);
      }
      return;
    case EventType::Balance:
      if (h.len >= sizeof(BalanceMsg)) {
        const auto& b = msg_cast<BalanceMsg>(&h);
        if ((b.flags & BalanceMsg::kAccount) != 0) return;
        // A venue whose first snapshot comes after the start of the run starts from what is
        // configured here, and from that snapshot in a backtest of the finished journal.
        const auto had = [&] {
          for (const BalanceMsg& m : session_.balances) {
            if (m.hdr.venue == h.venue && (m.flags & BalanceMsg::kSnapshotEnd) != 0) return true;
          }
          return std::find(venues_snapshotted_.begin(), venues_snapshotted_.end(), h.venue) !=
                 venues_snapshotted_.end();
        };
        if (!had()) {
          late_start_.fetch_add(1, std::memory_order_relaxed);
          if ((b.flags & BalanceMsg::kSnapshotEnd) != 0) venues_snapshotted_.push_back(h.venue);
        }
        if ((b.flags & BalanceMsg::kSnapshotEnd) != 0) {
          note_late(sim::account_time(h), reach_);
          push_record(h);
        }
      }
      return;
    case EventType::Reconcile:
      if (!first_order_ && msg_cast<ReconcileMsg>(&h).kind == ReconcileMsg::Kind::Position)
        late_start_.fetch_add(1, std::memory_order_relaxed);
      return;
    default:
      return;
  }
}

// Brings the stream up to its next market data and the scout past its horizon, without waiting:
// true once that event can go out, or the session has ended. The scout moves only for an event the
// other cursor holds: however the blocks fall, it has read the same records for each event.
bool FollowSource::fetch() {
  for (;;) {
    if (done_) return true;
    if (held_ == nullptr) {
      const EventHeader* h = nullptr;
      const JournalTail::Step step = emit_.next(h);
      if (step == JournalTail::Step::End) {
        // What comes after the last market data still counts: the session's last fills.
        if (writer_gone_) scout_.give_up();
        while (!ended_) {
          if (!scout_step() && !ended_) return false;
        }
        done_ = true;
        return true;
      }
      if (step == JournalTail::Step::Wait) {
        check_writer(emit_);
        if (!writer_gone_) return false;
        continue;
      }
      if ((h->flags & EventHeader::kOutbound) != 0 || !JournalSource::is_market_data(h->type))
        continue;
      held_ = h;
    }
    if (ended_ || scout_clock_ >= venue_ts(*held_).ns + opt_.settle.ns) return true;
    if (!scout_step() && !ended_) return false;
  }
}

bool FollowSource::ready() {
  if (should_stop()) return true;
  return fetch() || !pending_.empty();
}

const EventHeader* FollowSource::next() {
  for (;;) {
    while (!ready()) pause();
    if (!pending_.empty()) {
      record_out_ = std::move(pending_.front());
      pending_.pop_front();
      return reinterpret_cast<const EventHeader*>(record_out_.data());
    }
    if (done_ || should_stop()) return nullptr;
    const EventHeader* h = held_;
    held_ = nullptr;
    const std::int64_t v = venue_ts(*h).ns;
    if (v > reach_) {
      reach_ = v;
      reached_.store(v, std::memory_order_relaxed);
    }
    if (h->instrument.value < instrument_reach_.size() &&
        v > instrument_reach_[h->instrument.value])
      instrument_reach_[h->instrument.value] = v;
    md_events_.fetch_add(1, std::memory_order_relaxed);
    if (stripper_) {
      h = stripper_->strip(*h, buf_);
      if (h == nullptr) continue;
    }
    return h;
  }
}

void FollowSource::reset() {
  throw std::logic_error("follow: a journal being followed is read once");
}

std::string FollowSource::note() const {
  std::string s = fmt::format("journal: followed, {} parts, {} events", parts(), md_events());
  if (seq_gaps() != 0)
    s += fmt::format("; WARNING: the sequence numbers break {} times", seq_gaps());
  if (late() != 0) s += fmt::format("; {} records came later than the settle time", late());
  if (writer_gone_) s += "; the writer stopped without closing it";
  return s;
}

}  // namespace fastmm::bt
