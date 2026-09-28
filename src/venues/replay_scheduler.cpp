#include "fastmm/venues/replay_scheduler.hpp"

#include "fastmm/core/log.hpp"
#include "fastmm/net/reactor.hpp"

#include <algorithm>

namespace fastmm::venues {

void ReplaySchedulerBase::setup(std::string name,
                                std::string what,
                                ReplayLimits limits,
                                Hooks hooks) {
  name_ = std::move(name);
  what_ = std::move(what);
  limits_ = limits;
  hooks_ = std::move(hooks);
}

void ReplaySchedulerBase::reset_streams() noexcept {
  for (std::size_t i = 0; i < streams_.size(); ++i) {
    Stream& s = streams_[i];
    s.running = false;
    s.awaiting = false;
    s.rows.clear();
    drop_rows(i);
  }
}

void ReplaySchedulerBase::close() noexcept {
  take_again();
  open_ = false;
  ++generation_;  // a reply the shutdown aborts is not this replay's any more
  active_ = false;
  pending_ = 0;
  retry_at_ns_ = 0;
  due_at_ns_ = 0;
  reset_streams();
}

void ReplaySchedulerBase::abort() noexcept {
  if (!active_) return;
  ++generation_;
  active_ = false;
  pending_ = 0;
  reset_streams();
  take_again();
  retry_at_ns_ = net::Reactor::now_ns() + kRetryNs;
}

void ReplaySchedulerBase::set_streams(std::size_t n) {
  const std::size_t old = streams_.size();
  if (n < old) {
    for (std::size_t i = n; i < old; ++i) drop_rows(i);
  }
  streams_.resize(n);
  for (std::size_t i = old; i < n; ++i) streams_[i].since_ms = default_since_ms_;
}

void ReplaySchedulerBase::start_at(std::int64_t since_ms) {
  if (default_since_ms_ <= 0) default_since_ms_ = since_ms;
  for (Stream& s : streams_) {
    if (s.since_ms <= 0) s.since_ms = default_since_ms_;
  }
}

void ReplaySchedulerBase::restart_from(std::int64_t since_ms) {
  // The replay running started before the caller knew: it goes on from here once it has ended.
  if (active_) {
    again_from_ms_ = again_from_ms_ > 0 ? std::min(again_from_ms_, since_ms) : since_ms;
    return;
  }
  default_since_ms_ = since_ms;
  for (Stream& s : streams_) {
    s.since_ms = since_ms;
    s.read.clear();
    s.from_id = 0;
  }
}

void ReplaySchedulerBase::resume(std::int64_t since_ms, const std::vector<std::string>& known) {
  if (active_) {
    known_.insert(known.begin(), known.end());
  } else {
    known_ = {known.begin(), known.end()};
  }
  restart_from(since_ms);
}

// A restart asked for while a replay ran: every stream starts from there, or from where that replay
// left it if that is earlier (a window it did not read in full).
void ReplaySchedulerBase::take_again() noexcept {
  if (again_from_ms_ <= 0) return;
  const std::int64_t from = again_from_ms_;
  again_from_ms_ = 0;
  default_since_ms_ = std::min(default_since_ms_ > 0 ? default_since_ms_ : from, from);
  for (Stream& s : streams_) {
    s.since_ms = s.since_ms > 0 ? std::min(s.since_ms, from) : from;
    s.read.clear();
    s.from_id = 0;
  }
}

void ReplaySchedulerBase::set_cursor(std::size_t stream, std::int64_t from_id) {
  if (stream < streams_.size() && from_id > 0) streams_[stream].from_id = from_id;
}

std::int64_t ReplaySchedulerBase::since_ms(std::size_t stream) const noexcept {
  return stream < streams_.size() ? streams_[stream].since_ms : 0;
}

std::int64_t ReplaySchedulerBase::from_id(std::size_t stream) const noexcept {
  return stream < streams_.size() ? streams_[stream].from_id : 0;
}

bool ReplaySchedulerBase::run() {
  if (!open_) return false;
  if (active_) return true;
  if (!hooks_.ready || !hooks_.ready()) return false;
  start();
  return true;
}

void ReplaySchedulerBase::due_in(std::int64_t delay_ns) {
  const std::int64_t at = net::Reactor::now_ns() + delay_ns;
  if (due_at_ns_ == 0 || at < due_at_ns_) due_at_ns_ = at;
}

void ReplaySchedulerBase::on_timer(std::int64_t now_ns) {
  if (!open_) return;
  if (active_) {
    // A retry it schedules waits for a later tick: kRetryNs after the replay ended.
    expire(now_ns);
    return;
  }
  const bool retry = retry_at_ns_ != 0 && now_ns >= retry_at_ns_;
  const bool due = due_at_ns_ != 0 && now_ns >= due_at_ns_;
  const bool sweep =
      limits_.sweep_ns > 0 && retry_at_ns_ == 0 && now_ns - last_start_ns_ >= limits_.sweep_ns;
  if (!retry && !due && !sweep) return;
  if (!hooks_.ready || !hooks_.ready()) return;
  if (due) due_at_ns_ = 0;
  start();
}

void ReplaySchedulerBase::start() {
  ++generation_;
  ++replays_;
  active_ = true;
  ok_ = true;
  emitted_ = 0;
  retry_at_ns_ = 0;  // this attempt replaces the retry; its own failure schedules the next
  last_start_ns_ = net::Reactor::now_ns();
  now_ms_ = hooks_.now_ms();
  // Held by the loop itself, so a stream that ends inside it cannot finish the replay early.
  pending_ = 1;
  for (std::size_t i = 0; i < streams_.size(); ++i) {
    Stream& s = streams_[i];
    if (s.since_ms <= 0) s.since_ms = default_since_ms_ > 0 ? default_since_ms_ : now_ms_;
    s.running = true;
    s.awaiting = false;
    s.cursor_ms = s.since_ms;
    s.pages = 0;
    ++pending_;
    begin_window(i);
  }
  stream_done(streams_.size(), true);
}

void ReplaySchedulerBase::begin_window(std::size_t i) {
  Stream& s = streams_[i];
  if (s.pages >= limits_.max_pages) {
    FASTMM_LOG_WARN(
        "{}: {} replay stopped after {} page(s); it continues later", name_, what_, s.pages);
    stream_done(i, false);
    return;
  }
  ReplayQuery& q = s.q;
  q = ReplayQuery{};
  q.generation = generation_;
  q.stream = i;
  q.now_ms = now_ms_;
  if (s.from_id > 0) {
    q.from_id = s.from_id;
  } else {
    std::int64_t start = s.cursor_ms;
    if (limits_.history_ms > 0 && start < now_ms_ - limits_.history_ms) {
      start = now_ms_ - limits_.history_ms;
      FASTMM_LOG_ERROR("{}: {} before {} are beyond the venue's history; replaying from there",
                       name_,
                       what_,
                       start);
      s.since_ms = start;
      s.read.clear();
      ok_ = false;
    }
    std::int64_t end = 0;
    q.history = limits_.recent_ms > 0 && start < now_ms_ - limits_.recent_ms;
    if (q.history && !limits_.history_covers_recent) end = now_ms_ - limits_.recent_ms;
    if (limits_.window_ms > 0 && now_ms_ - start > limits_.window_ms - kClockSlackMs) {
      // Never past the settle margin: the window after it reads the rest, and settles.
      const std::int64_t e = std::min(start + limits_.window_ms - 1, now_ms_ - limits_.settle_ms);
      end = end == 0 ? e : std::min(end, e);
    }
    if (end != 0 && end < start) end = start;
    q.start_ms = start;
    q.end_ms = end;
  }
  s.window_pages = 0;
  s.low_ms = 0;
  s.rows.clear();
  drop_rows(i);
  send(i);
}

void ReplaySchedulerBase::send(std::size_t i) {
  Stream& s = streams_[i];
  ++s.pages;
  s.awaiting = true;
  s.sent_ns = net::Reactor::now_ns();
  if (hooks_.query(s.q)) return;
  if (!expects(s.q)) return;  // the hook moved the generation on
  s.awaiting = false;
  stream_done(i, false);
}

bool ReplaySchedulerBase::expects(const ReplayQuery& q) const noexcept {
  return active_ && q.generation == generation_ && q.stream < streams_.size() &&
         streams_[q.stream].awaiting && streams_[q.stream].q.page == q.page &&
         streams_[q.stream].q.start_ms == q.start_ms && streams_[q.stream].q.end_ms == q.end_ms &&
         streams_[q.stream].q.from_id == q.from_id;
}

void ReplaySchedulerBase::failed(const ReplayQuery& q) {
  if (!expects(q)) return;
  streams_[q.stream].awaiting = false;
  stream_done(q.stream, false);
}

void ReplaySchedulerBase::page(const ReplayQuery& q,
                               std::vector<Entry> rows,
                               bool more,
                               std::string next) {
  if (!expects(q)) return;
  const std::size_t i = q.stream;
  Stream& s = streams_[i];
  s.awaiting = false;
  if (limits_.page_rows > 0 && rows.size() >= limits_.page_rows) more = true;
  for (Entry& e : rows) {
    if (s.low_ms == 0 || e.time_ms < s.low_ms) s.low_ms = e.time_ms;
    s.rows.push_back(std::move(e));
  }
  if (limits_.newest_first && more && !next.empty()) {
    if (++s.window_pages < limits_.window_pages) {
      s.q.page = std::move(next);
      send(i);
      return;
    }
    // Too many rows for one window: what was read is its newest part. Read the window again up
    // to its oldest row seen, so that it can still be emitted oldest first.
    if (s.low_ms <= s.q.start_ms) {
      FASTMM_LOG_WARN("{}: more than {} pages of {} in one millisecond; the replay is incomplete",
                      name_,
                      limits_.window_pages,
                      what_);
      stream_done(i, false);
      return;
    }
    s.q.end_ms = s.low_ms;
    s.q.page.clear();
    s.window_pages = 0;
    s.low_ms = 0;
    s.rows.clear();
    drop_rows(i);
    send(i);
    return;
  }
  close_window(i, more && next.empty());
}

// The window (or the ascending page) is read: its rows go out oldest first, skipping those read
// before and those an earlier session booked, and the watermark moves.
void ReplaySchedulerBase::close_window(std::size_t i, bool more) {
  Stream& s = streams_[i];
  const ReplayQuery& q = s.q;
  if (limits_.newest_first) std::reverse(s.rows.begin(), s.rows.end());
  std::stable_sort(s.rows.begin(), s.rows.end(), [](const Entry& a, const Entry& b) {
    return a.time_ms < b.time_ms;
  });
  std::int64_t newest = 0;
  std::int64_t high_seq = 0;
  for (const Entry& e : s.rows) {
    newest = std::max(newest, e.time_ms);
    high_seq = std::max(high_seq, e.seq);
    // Before what was asked for: an earlier query read it (a venue bound may be inclusive).
    if (q.from_id > 0 ? (e.seq > 0 && e.seq < q.from_id) : e.time_ms < q.start_ms) continue;
    if (!s.read.emplace(e.key, e.time_ms).second) continue;  // forwarded before
    if (known_.contains(e.key)) continue;                    // the earlier session booked it
    if (emit_row(i, e.ref)) ++emitted_;
  }
  const bool had_rows = !s.rows.empty();
  s.rows.clear();
  drop_rows(i);
  if (high_seq > 0) s.from_id = std::max(s.from_id, high_seq + 1);
  if (more) {
    // Full: ask again from the newest row (its millisecond again: the rows there are known).
    const bool advanced = q.from_id > 0 ? s.from_id > q.from_id : had_rows && newest > q.start_ms;
    if (!advanced) {
      FASTMM_LOG_WARN(
          "{}: a full page of {} that cannot move past its start; the replay is "
          "incomplete",
          name_,
          what_);
      stream_done(i, false);
      return;
    }
    commit(s, newest);
    s.cursor_ms = newest;
    begin_window(i);
    return;
  }
  if (q.from_id <= 0 && q.end_ms != 0) {
    // A bounded window read in full: the next one starts after it.
    commit(s, q.end_ms + 1);
    s.cursor_ms = q.end_ms + 1;
    begin_window(i);
    return;
  }
  // Everything up to the replay's start is in.
  commit(s, now_ms_);
  stream_done(i, true);
}

// The watermark moves up to `read_to`, but not past the replay's start less the settle margin; the
// rows read at or after it stay known by key.
void ReplaySchedulerBase::commit(Stream& s, std::int64_t read_to) const {
  const std::int64_t since = std::max(s.since_ms, std::min(read_to, now_ms_ - limits_.settle_ms));
  s.since_ms = since;
  std::erase_if(s.read, [since](const auto& kv) { return kv.second < since; });
}

void ReplaySchedulerBase::stream_done(std::size_t i, bool ok) {
  if (i < streams_.size()) {
    Stream& s = streams_[i];
    s.running = false;
    s.awaiting = false;
    s.rows.clear();
    drop_rows(i);
  }
  if (!ok) ok_ = false;
  if (pending_ > 0) --pending_;
  if (pending_ > 0 || !active_) return;
  finish();
}

// A query the venue never answers (a WebSocket that stays up, a REST connect that hangs) would
// keep the replay, and a reconciliation waiting for it, running until the connection drops.
void ReplaySchedulerBase::expire(std::int64_t now_ns) {
  const std::uint64_t gen = generation_;
  for (std::size_t i = 0; i < streams_.size(); ++i) {
    Stream& s = streams_[i];
    if (!s.awaiting || now_ns - s.sent_ns < kQueryTimeoutNs) continue;
    ++timeouts_;
    FASTMM_LOG_WARN("{}: {} query got no answer in {} s; the replay is incomplete",
                    name_,
                    what_,
                    kQueryTimeoutNs / 1'000'000'000);
    s.awaiting = false;
    stream_done(i, false);
    // The last stream finished the replay: its hook may have started another one.
    if (!active_ || generation_ != gen) return;
  }
}

void ReplaySchedulerBase::finish() {
  active_ = false;
  if (emitted_ > 0) FASTMM_LOG_INFO("{}: replayed {} {}", name_, emitted_, what_);
  // A restart asked for meanwhile: the replay goes on from there at the next timer tick, and
  // whoever waits for it (a reconciliation) waits for that too, so it learns about the rows the
  // restart was for. That one alone says complete or not: it starts no later than where this one
  // left any stream.
  if (again_from_ms_ > 0) {
    take_again();
    if (open_) {
      due_at_ns_ = net::Reactor::now_ns();
      return;
    }
    ok_ = false;
  }
  if (!ok_) retry_at_ns_ = net::Reactor::now_ns() + kRetryNs;
  if (hooks_.finished) hooks_.finished(ok_);
}

}  // namespace fastmm::venues
