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
//   * a restart (resume(), restart_from()) while one runs: that replay goes on from the restart's
//     start at the first timer tick after it has ended, and ends (Hooks::finished) after that.
//   * a query not answered within kQueryTimeoutNs has failed (the housekeeping timer checks):
//     the replay ends incomplete, so a reconciliation waiting for it goes ahead without
//     kExecutionsExact, and the retry follows. A late answer is not this replay's any more.
//   * close() (disconnect) and abort() (the transport the queries went out on is gone) move the
//     generation on: a reply to an earlier query is ignored. After close() nothing runs until
//     open(); after abort() the retry follows.
//
// Reactor thread only; control path (std::function, strings), nothing per order or per market-data
// message.
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
  // An open-ended window this close to window_ms gets an end: the venue's clock is not ours.
  static constexpr std::int64_t kClockSlackMs = 60'000;

  struct Hooks {
    std::function<bool()> ready;                    // the venue can be asked now
    std::function<std::int64_t()> now_ms;           // venue time
    std::function<bool(const ReplayQuery&)> query;  // sends it; false: could not
    std::function<void(bool complete)> finished;    // the replay is over
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
  // The query failed (transport, status, unreadable reply).
  void failed(const ReplayQuery& q);
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

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] bool retry_pending() const noexcept { return retry_at_ns_ != 0; }
  [[nodiscard]] std::uint64_t replays() const noexcept { return replays_; }
  // Queries that got no answer within kQueryTimeoutNs.
  [[nodiscard]] std::uint64_t timeouts() const noexcept { return timeouts_; }
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

 private:
  struct Stream {
    std::int64_t since_ms = 0;                           // the watermark
    std::unordered_map<std::string, std::int64_t> read;  // keys read at or after it -> time
    std::int64_t from_id = 0;                            // > 0: the next id to ask from
    // The replay in progress.
    bool running = false;
    bool awaiting = false;       // a query is out
    std::int64_t cursor_ms = 0;  // the next window's start
    ReplayQuery q;               // the query out (or last sent)
    std::int64_t sent_ns = 0;    // when it went out
    std::size_t pages = 0;
    std::size_t window_pages = 0;
    std::int64_t low_ms = 0;  // oldest row of the window so far (newest_first)
    std::vector<Entry> rows;  // the window so far (newest_first)
  };

  void start();
  void begin_window(std::size_t i);
  void send(std::size_t i);
  void close_window(std::size_t i, bool more);
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
  std::size_t emitted_ = 0;
};

template <class Row>
class ReplayScheduler final : public ReplaySchedulerBase {
 public:
  using Emit = std::function<bool(std::size_t stream, const Row& row)>;

  ReplayScheduler() = default;
  ~ReplayScheduler() = default;
  ReplayScheduler(const ReplayScheduler&) = delete;
  ReplayScheduler& operator=(const ReplayScheduler&) = delete;

  // `name` and `what` for the log ("fake-okx: replayed 3 fill(s)"); `emit` forwards a row and
  // says whether it did (a row of an instrument not traded here is read, not forwarded).
  void setup(std::string name, std::string what, ReplayLimits limits, Hooks hooks, Emit emit) {
    ReplaySchedulerBase::setup(std::move(name), std::move(what), limits, std::move(hooks));
    emit_ = std::move(emit);
  }

  // The venue's answer to `q`. Rows of an ascending page (no token) are emitted before this
  // returns, so they may view the reply's buffer; rows of a newest-first window must own theirs.
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

  Emit emit_;
  std::vector<std::vector<Row>> rows_;
};

}  // namespace fastmm::venues
