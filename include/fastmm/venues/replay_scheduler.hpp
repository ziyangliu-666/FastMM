#pragma once
// ReplayScheduler: reading an account history the private stream may have missed (executions,
// funding payments), written once. It owns which time range or id is asked for next and when; the
// connector owns the endpoint, the row parsing and what a row becomes.
//
// A replay reads every stream (a symbol, a currency, or the whole account) from its watermark to
// now, in parallel, and ends with Hooks::finished(complete). Per stream:
//   * windows: a start (inclusive) and an end (inclusive, 0 = open-ended). A window is at most
//     window_ms long, never starts before history_ms ago (a clamp makes the replay incomplete),
//     and a start older than recent_ms is a history query; when the history endpoint does not
//     cover the recent one (history_covers_recent = false) that window ends where it begins.
//   * pages: rows oldest first are emitted page by page, the next page asked from the newest row
//     (or from the trade id after the highest, when the venue gives ids to page by); rows newest
//     first (newest_first) are collected over the window's pages, by the venue's page token, and
//     emitted oldest first when it is read. A window needing more than window_pages pages is read
//     again up to its oldest row seen. More than max_pages pages in one replay: incomplete.
//   * a page is full, so possibly followed by more, when the venue says so or when it returned
//     page_rows rows (every row counts, also those the connector does not forward).
//   * the watermark: rows before it are never asked for again. It moves to the end of a window
//     read in full, but never past the replay's start less settle_ms: an execution the venue
//     indexes a little late (it shows in the history only after the query ran, with a time before
//     it) is then still inside the next query. The rows read at or after the watermark are
//     remembered by key, so they are not forwarded twice.
//   * rows an earlier session booked (resume()) are skipped; they count as read.
// When:
//   * run(): now, or joined when one is running (a reconciliation, a reconnect).
//   * after an incomplete replay, again kRetryNs after it ended; while all is well, every
//     sweep_ns; due_in(): once, a moment after a stream event (funding). A replay that starts
//     serves them all.
//   * a sweep (the sweep_ns replay alone: not run(), a retry, due_in() or a restart's) skips the
//     streams Hooks::active says had no order activity since their watermark less settle_ms, once
//     they have been read in full since open() or the last restart. A skipped stream's watermark
//     moves as if it had been answered empty; one active later is read from there, which covers
//     every row since the activity. A fill the private stream dropped on an order with no other
//     activity is not looked for then: an id cursor (from_id) still reaches it at the stream's
//     next read, a time watermark does not.
//   * a restart (resume(), restart_from()) while one runs: that replay goes on from the restart's
//     start at the first timer tick after it has ended, and ends (Hooks::finished) after that.
//   * a query not answered within kQueryTimeoutNs has failed (the housekeeping timer checks):
//     the replay ends incomplete, so a reconciliation waiting for it goes ahead without
//     kExecutionsExact, and the retry follows. A late answer is not this replay's any more.
//   * a query the connector cannot send yet (Hooks::can_query: the weight budget is spent, the
//     venue asked for a pause, its channel holds enough requests already) waits, and goes out at
//     a housekeeping tick, or at send_waiting() (the connector's reply handler), once it can: a
//     start with many streams sends what fits and the rest as room comes, and a retry during a
//     pause waits for its end instead of earning the ban. One that waited kDeferTimeoutNs fails.
//   * close() (disconnect) and abort() (the transport the queries went out on is gone) move the
//     generation on: a reply to an earlier query is ignored. After close() nothing runs until
//     open(); after abort() the retry follows.
// Orders by the venue's id (Hooks::lookup, for a history that names an order by its venue id only,
// as Binance's does): before a window's rows go out, those naming an order the connector cannot
// name (the typed layer's `unnamed`) have it asked for, at most ReplayLimits::max_lookups per
// replay, one per order, and the window waits for the answers (kQueryTimeoutNs at most). A row
// still unnamed then (the lookup failed, timed out, or was over the budget) goes out naming no
// order, marked as one to be sent again (emitting_unresolved()), and a copy is kept (kMaxHeld):
// the next replays ask again and send the row once more (the same key: an engine keeps one),
// naming its order, or, after kMaxLookupAttempts or when the venue says the order is not ours
// (LookupResult::NotOurs, not asked for again), unmarked and naming none. Rows kept past answer()
// must own their data.
//
// Reactor thread only; control path (std::function, strings), nothing per order or per market-data
// message but OrderActivity::note, one store.
#include "fastmm/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fastmm::venues {

struct ReplayLimits {
  std::int64_t window_ms = 0;         // longest range one query may span; 0: any
  std::int64_t history_ms = 0;        // how far back the venue answers; 0: forever
  std::int64_t recent_ms = 0;         // older starts are history queries; 0: one endpoint
  bool history_covers_recent = true;  // false: a history window ends at now - recent_ms
  std::size_t page_rows = 0;          // a page with this many rows may have more; 0: the venue says
  bool newest_first = false;          // rows newest first, paged by token within a window
  std::size_t window_pages = 20;      // pages of one window before it is narrowed
  std::size_t max_pages = 100;        // pages per stream and replay
  std::int64_t settle_ms = 60'000;    // how late the venue may index a row
  std::int64_t sweep_ns = 60'000'000'000;  // a replay while all is well; 0: none
  std::size_t max_lookups = 16;            // order lookups per replay (Hooks::lookup)
};

// An order a replayed row names by the venue's id only. The connector asks the venue for it and
// answers ReplaySchedulerBase::looked_up(), passing it back.
struct ReplayLookup {
  std::uint64_t generation = 0;
  std::size_t stream = 0;
  std::string order_id;
};

enum class LookupResult : std::uint8_t {
  Named,    // the connector can name the order now
  NotOurs,  // the venue's answer names no order of FastMM's: not asked for again
  Failed,   // transport, status, unreadable: asked again at the next replay
};

// One page to ask for. The connector sends it and answers with ReplayScheduler::answer() or
// failed(), passing the query back.
struct ReplayQuery {
  std::uint64_t generation = 0;
  std::size_t stream = 0;
  std::int64_t now_ms = 0;    // venue time the replay started at
  std::int64_t start_ms = 0;  // inclusive; 0 with from_id
  std::int64_t end_ms = 0;    // inclusive; 0: open-ended
  std::int64_t from_id = 0;   // > 0: the rows from this id on, no time range
  std::string page;           // the venue's token for the next page of the window; empty: first
  bool history = false;       // start_ms is older than ReplayLimits::recent_ms
};

// A row as the venue returned it: `key` is the id the engine books it under (the dedupe key),
// `seq` a numeric id to page by (from_id), 0 when the venue has none.
template <class Row>
struct ReplayRow {
  std::int64_t time_ms = 0;
  std::int64_t seq = 0;
  std::string key;
  Row row;
};

template <class Row>
struct ReplayPage {
  std::vector<ReplayRow<Row>> rows;  // every row returned, in the venue's order
  bool more = false;                 // the venue says more follow
  std::string next;                  // newest_first: the token for the next page
};

class ReplaySchedulerBase {
 public:
  static constexpr std::int64_t kRetryNs = 5'000'000'000;
  // A query the venue has not answered in this long has failed. Above the REST channel's own
  // timeout (http_timeout_ms, 5 s by default), which fails a REST query first; what it bounds is
  // a query on a WebSocket that stays up (Deribit), where nothing else does.
  static constexpr std::int64_t kQueryTimeoutNs = 30'000'000'000;
  // A query that could not be sent for this long (Hooks::can_query) fails its stream.
  static constexpr std::int64_t kDeferTimeoutNs = 180'000'000'000;
  // An open-ended window this close to window_ms gets an end: the venue's clock is not ours.
  static constexpr std::int64_t kClockSlackMs = 60'000;
  // Rows sent naming no order that the next replays try to name, and how many times.
  static constexpr std::size_t kMaxHeld = 256;
  static constexpr std::uint32_t kMaxLookupAttempts = 5;

  struct Hooks {
    std::function<bool()> ready;                    // the venue can be asked now
    std::function<std::int64_t()> now_ms;           // venue time
    std::function<bool(const ReplayQuery&)> query;  // sends it; false: could not
    std::function<void(bool complete)> finished;    // the replay is over
    // Optional: asks the venue for an order (ReplayLookup); false: could not (rate limit, no
    // channel), which counts as a failed lookup.
    std::function<bool(const ReplayLookup&)> lookup;
    // Optional: a query may go out now. False defers it to a later housekeeping tick (the
    // connector's rate limiter: its bulk share of the weight is spent, or the venue asked for a
    // pause) instead of failing its stream.
    std::function<bool()> can_query;
    // Optional: a sweep reads `stream` only when this says the account may have executions on
    // it at or after `since_ms` (venue time): an order sent, an execution report received. A
    // connector that cannot tell (since_ms before it started counting) says true. Absent: every
    // stream, every sweep.
    std::function<bool(std::size_t stream, std::int64_t since_ms)> active = {};
  };

  ReplaySchedulerBase(const ReplaySchedulerBase&) = delete;
  ReplaySchedulerBase& operator=(const ReplaySchedulerBase&) = delete;

  // connect(): replays run from now on when `enabled`. disconnect(): nothing in progress counts
  // and nothing runs until open().
  void open(bool enabled) noexcept { open_ = enabled; }
  void close() noexcept;
  // The transport the queries went out on is gone: the replay is dropped and retried.
  void abort() noexcept;

  // Starts a replay, or joins the one running. False: none can run now.
  bool run();
  // A replay delay_ns from now (unless one is due sooner).
  void due_in(std::int64_t delay_ns);
  // The housekeeping timer: queries past kQueryTimeoutNs, retry, due, sweep.
  void on_timer(std::int64_t now_ns);
  // Room for the queries Hooks::can_query held back (a reply came in): they go now, as far as it
  // lets them, rather than at the next housekeeping tick.
  void send_waiting();
  // The query failed (transport, status, unreadable reply).
  void failed(const ReplayQuery& q);
  // The venue's answer to a lookup. One of an earlier replay only counts for NotOurs.
  void looked_up(const ReplayLookup& l, LookupResult r);
  // `q` is the query this replay waits for: false for a reply to one a close(), an abort() or a
  // new replay moved on from, which the connector drops without counting it.
  [[nodiscard]] bool expects(const ReplayQuery& q) const noexcept;

  // Streams: indices 0..n-1. A new stream starts at the default watermark.
  void set_streams(std::size_t n);
  [[nodiscard]] std::size_t streams() const noexcept { return streams_.size(); }
  // Where a stream nobody placed starts: connect() passes the venue's time, unless resume() or
  // restart_from() said otherwise before.
  void start_at(std::int64_t since_ms);
  // A restart: every stream from `since_ms`, skipping the rows `known` names (their keys). While a
  // replay runs, the ids join those it skips and the start is taken as restart_from() takes it.
  void resume(std::int64_t since_ms, const std::vector<std::string>& known);
  // Every stream from `since_ms` again, cursors dropped (the caller knows of rows this connector
  // never heard about). While a replay runs, it goes on from there at the first timer tick after
  // it has ended (from the earliest such start, or earlier where it did not read a window in
  // full), as part of the same replay: Hooks::finished comes after that. fastmm-gateway attaches
  // strategies whose stores end at different times, one while another's replay runs.
  void restart_from(std::int64_t since_ms);
  // A stream's next id (Binance fromId), from a restart's store.
  void set_cursor(std::size_t stream, std::int64_t from_id);
  // Ids of `stream` (Entry::seq) that are booked already, ascending: rows carrying one are not
  // emitted. For ids at or after a cursor that an earlier session stored out of order.
  void set_known_ids(std::size_t stream, std::vector<std::int64_t> ids);

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] bool retry_pending() const noexcept { return retry_at_ns_ != 0; }
  [[nodiscard]] std::uint64_t replays() const noexcept { return replays_; }
  // Queries that got no answer within kQueryTimeoutNs, or waited kDeferTimeoutNs to be sent.
  [[nodiscard]] std::uint64_t timeouts() const noexcept { return timeouts_; }
  // Streams a sweep did not read (Hooks::active).
  [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }
  // Queries that waited for Hooks::can_query at least once.
  [[nodiscard]] std::uint64_t deferrals() const noexcept { return deferrals_; }
  // Streams whose next query waits for Hooks::can_query now.
  [[nodiscard]] std::size_t deferred() const noexcept;
  // True while the emit callback runs for a row that goes out naming no order and that a later
  // replay will send again, naming it or, having given up, not (OrderFillMsg::kUnresolved).
  [[nodiscard]] bool emitting_unresolved() const noexcept { return unresolved_; }
  // Order lookups sent, and rows kept for the next replay to name.
  [[nodiscard]] std::uint64_t lookups() const noexcept { return lookups_total_; }
  [[nodiscard]] std::size_t held() const noexcept { return held_.size(); }
  [[nodiscard]] std::int64_t since_ms(std::size_t stream) const noexcept;
  [[nodiscard]] std::int64_t from_id(std::size_t stream) const noexcept;
  [[nodiscard]] const ReplayLimits& limits() const noexcept { return limits_; }

 protected:
  struct Entry {
    std::int64_t time_ms = 0;
    std::int64_t seq = 0;
    std::string key;
    std::size_t ref = 0;  // the derived class's row
  };

  ReplaySchedulerBase() = default;
  ~ReplaySchedulerBase() = default;
  void setup(std::string name, std::string what, ReplayLimits limits, Hooks hooks);
  void page(const ReplayQuery& q, std::vector<Entry> rows, bool more, std::string next);
  virtual bool emit_row(std::size_t stream, std::size_t ref) = 0;
  virtual void drop_rows(std::size_t stream) = 0;
  // The venue's id of the order a row names when the connector cannot name it; empty otherwise.
  virtual std::string unnamed_row(std::size_t stream, std::size_t ref) = 0;
  // Rows kept for the next replay (by `id`): kept, still unnamed?, sent again, dropped.
  virtual void hold_row(std::size_t stream, std::size_t ref, std::uint64_t id) = 0;
  virtual std::string unnamed_held(std::uint64_t id) = 0;
  virtual bool emit_held(std::uint64_t id) = 0;
  virtual void drop_held(std::uint64_t id) = 0;

 private:
  struct Stream {
    std::int64_t since_ms = 0;                           // the watermark
    std::unordered_map<std::string, std::int64_t> read;  // keys read at or after it -> time
    std::int64_t from_id = 0;                            // > 0: the next id to ask from
    std::vector<std::int64_t> known_ids;                 // set_known_ids(), ascending
    bool read_once = false;  // read in full since open() or the last restart: a sweep may skip it
    // The replay in progress.
    bool running = false;
    bool awaiting = false;         // a query is out
    bool deferred = false;         // the next query waits for Hooks::can_query
    std::int64_t deferred_ns = 0;  // since when
    std::int64_t cursor_ms = 0;    // the next window's start
    ReplayQuery q;                 // the query out (or last sent)
    std::int64_t sent_ns = 0;      // when it went out
    std::size_t pages = 0;
    std::size_t window_pages = 0;
    std::int64_t low_ms = 0;  // oldest row of the window so far (newest_first)
    std::vector<Entry> rows;  // the window so far (newest_first)
    // The window is read and waits for this many lookups before it goes out.
    std::size_t resolving = 0;
    bool resolve_more = false;  // close_window's `more`, for then
  };
  // A lookup out: the streams (or kHeldWaiter) whose rows wait for it.
  struct Lookup {
    std::size_t stream = 0;
    std::string order_id;
    std::int64_t sent_ns = 0;
    std::vector<std::size_t> waiters;
  };
  // A row sent naming no order, kept to be named by a later replay.
  struct Held {
    std::uint64_t id = 0;
    std::size_t stream = 0;
    std::string key;
    std::string order_id;
    std::uint32_t attempts = 0;
  };
  static constexpr std::size_t kHeldWaiter = static_cast<std::size_t>(-1);
  static constexpr std::size_t kMaxNotOurs = 4096;

  // `sweep`: the periodic replay alone, which skips the quiet streams (Hooks::active).
  void start(bool sweep = false);
  void begin_window(std::size_t i);
  void send(std::size_t i);
  void send_deferred(std::int64_t now_ns);
  void close_window(std::size_t i, bool more);
  void emit_window(std::size_t i, bool more);
  bool ask_lookups(std::size_t i);
  void ask_held();
  void held_done();
  // Sends the lookup of `order_id` for `waiter` unless one is out; true: the waiter waits for it.
  bool want_lookup(std::size_t stream, const std::string& order_id, std::size_t waiter);
  void lookup_done(const std::string& key);
  void waiter_done(std::size_t waiter);
  [[nodiscard]] bool will_retry(std::size_t i, const Entry& e, const std::string& order) const;
  void hold(std::size_t i, const Entry& e, std::string order);
  void drop_lookups() noexcept;
  [[nodiscard]] static std::string lookup_key(std::size_t stream, const std::string& order_id);
  void commit(Stream& s, std::int64_t read_to) const;
  void stream_done(std::size_t i, bool ok);
  void expire(std::int64_t now_ns);
  void finish();
  void reset_streams() noexcept;
  void take_again() noexcept;

  std::string name_;
  std::string what_;
  ReplayLimits limits_;
  Hooks hooks_;
  std::vector<Stream> streams_;
  std::unordered_set<std::string> known_;
  std::int64_t default_since_ms_ = 0;
  std::int64_t again_from_ms_ = 0;  // a restart_from() while a replay ran; 0: none
  bool open_ = false;
  bool active_ = false;
  bool ok_ = true;
  std::size_t pending_ = 0;
  std::uint64_t generation_ = 0;
  std::int64_t now_ms_ = 0;         // venue time the replay started at
  std::int64_t last_start_ns_ = 0;  // the sweep
  std::int64_t retry_at_ns_ = 0;    // 0: none
  std::int64_t due_at_ns_ = 0;      // 0: none
  std::uint64_t replays_ = 0;
  std::uint64_t timeouts_ = 0;
  std::uint64_t deferrals_ = 0;
  std::uint64_t skipped_ = 0;
  std::size_t emitted_ = 0;
  // Order lookups (Hooks::lookup).
  std::unordered_map<std::string, Lookup> lookups_;  // lookup_key -> out
  std::unordered_set<std::string> not_ours_;         // lookup_key
  std::vector<Held> held_;
  std::uint64_t next_held_ = 0;
  std::size_t held_waiting_ = 0;  // lookups the held rows wait for in this replay
  std::size_t lookups_sent_ = 0;  // in this replay
  bool unresolved_ = false;       // emit_window: the row being emitted will be sent again
  std::size_t not_held_ = 0;      // rows sent naming no order past kMaxHeld, in this replay
  std::uint64_t lookups_total_ = 0;
};

// Order activity per instrument, for Hooks::active: when the connector last sent an order on it
// or heard of one (an ack, an execution report). note() is one store, on the order path; the
// times are Reactor::now_ns(), compared in venue time at the sweep.
class OrderActivity {
 public:
  // Instruments 0..n-1 are counted, from the first call's `now_ns` on.
  void track(std::size_t n, std::int64_t now_ns) {
    if (start_ns_ == 0) start_ns_ = now_ns;
    if (n > last_ns_.size()) last_ns_.resize(n, 0);
  }
  void note(InstrumentId id, std::int64_t now_ns) noexcept {
    if (id.value < last_ns_.size()) last_ns_[id.value] = now_ns;
  }
  // Activity on `id` at or after venue time `since_ms`, the venue's time being `venue_now_ms` at
  // `now_ns`. True when the counting began after since_ms: nothing says it was quiet.
  [[nodiscard]] bool since(InstrumentId id,
                           std::int64_t since_ms,
                           std::int64_t venue_now_ms,
                           std::int64_t now_ns) const noexcept {
    const auto venue_ms = [&](std::int64_t t_ns) {
      return venue_now_ms - (now_ns - t_ns) / 1'000'000;
    };
    if (start_ns_ == 0 || venue_ms(start_ns_) > since_ms) return true;
    if (id.value >= last_ns_.size()) return true;
    const std::int64_t t = last_ns_[id.value];
    return t != 0 && venue_ms(t) >= since_ms;
  }

 private:
  std::int64_t start_ns_ = 0;
  std::vector<std::int64_t> last_ns_;
};

template <class Row>
class ReplayScheduler final : public ReplaySchedulerBase {
 public:
  using Emit = std::function<bool(std::size_t stream, const Row& row)>;
  // The venue's id of the order `row` names when the connector cannot name it, else empty.
  using Unnamed = std::function<std::string(std::size_t stream, const Row& row)>;

  ReplayScheduler() = default;
  ~ReplayScheduler() = default;
  ReplayScheduler(const ReplayScheduler&) = delete;
  ReplayScheduler& operator=(const ReplayScheduler&) = delete;

  // `name` and `what` for the log ("fake-okx: replayed 3 fill(s)"); `emit` forwards a row and
  // says whether it did (a row of an instrument not traded here is read, not forwarded).
  // `unnamed` goes with Hooks::lookup.
  void setup(std::string name,
             std::string what,
             ReplayLimits limits,
             Hooks hooks,
             Emit emit,
             Unnamed unnamed = {}) {
    ReplaySchedulerBase::setup(std::move(name), std::move(what), limits, std::move(hooks));
    emit_ = std::move(emit);
    unnamed_ = std::move(unnamed);
  }

  // The venue's answer to `q`. Rows of an ascending page (no token) are emitted before this
  // returns, so they may view the reply's buffer, unless they wait for lookups (Hooks::lookup);
  // rows of a newest-first window must own theirs.
  void answer(const ReplayQuery& q, ReplayPage<Row> page) {
    if (!expects(q)) return;
    if (rows_.size() < streams()) rows_.resize(streams());
    std::vector<Row>& buf = rows_[q.stream];
    std::vector<Entry> keys;
    keys.reserve(page.rows.size());
    for (ReplayRow<Row>& r : page.rows) {
      keys.push_back(Entry{r.time_ms, r.seq, std::move(r.key), buf.size()});
      buf.push_back(std::move(r.row));
    }
    this->page(q, std::move(keys), page.more, std::move(page.next));
  }

 private:
  bool emit_row(std::size_t stream, std::size_t ref) override {
    return stream < rows_.size() && ref < rows_[stream].size() && emit_ &&
           emit_(stream, rows_[stream][ref]);
  }
  void drop_rows(std::size_t stream) override {
    if (stream < rows_.size()) rows_[stream].clear();
  }
  std::string unnamed_row(std::size_t stream, std::size_t ref) override {
    if (!unnamed_ || stream >= rows_.size() || ref >= rows_[stream].size()) return {};
    return unnamed_(stream, rows_[stream][ref]);
  }
  void hold_row(std::size_t stream, std::size_t ref, std::uint64_t id) override {
    if (stream < rows_.size() && ref < rows_[stream].size())
      held_rows_.emplace(id, std::make_pair(stream, rows_[stream][ref]));
  }
  std::string unnamed_held(std::uint64_t id) override {
    const auto it = held_rows_.find(id);
    if (it == held_rows_.end() || !unnamed_) return {};
    return unnamed_(it->second.first, it->second.second);
  }
  bool emit_held(std::uint64_t id) override {
    const auto it = held_rows_.find(id);
    return it != held_rows_.end() && emit_ && emit_(it->second.first, it->second.second);
  }
  void drop_held(std::uint64_t id) override { held_rows_.erase(id); }

  Emit emit_;
  Unnamed unnamed_;
  std::vector<std::vector<Row>> rows_;
  std::unordered_map<std::uint64_t, std::pair<std::size_t, Row>> held_rows_;
};

}  // namespace fastmm::venues
