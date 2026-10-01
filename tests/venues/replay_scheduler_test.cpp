// ReplayScheduler against a fake venue: paging with rows an earlier session booked, window
// narrowing, the watermark after an empty window and near "now", retry timing, the generation
// after a disconnect, id cursors, windows and the history floor, the due trigger, order lookups.
#include "fastmm/venues/replay_scheduler.hpp"

#include "test_support.hpp"

#include "fastmm/net/reactor.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;

namespace {

constexpr std::int64_t kMin = 60'000;
constexpr std::int64_t kDay = 24LL * 3600 * 1000;
constexpr std::int64_t kNow = 1'789'000'000'000;  // venue ms

struct Rig {
  ReplayScheduler<std::string> sched;
  std::vector<ReplayQuery> queries;
  std::vector<std::string> emitted;
  std::vector<bool> finished;
  std::int64_t now_ms = kNow;
  bool can_send = true;

  explicit Rig(ReplayLimits limits = {}, std::size_t streams = 1) {
    ReplaySchedulerBase::Hooks h;
    h.ready = [] { return true; };
    h.now_ms = [this] { return now_ms; };
    h.query = [this](const ReplayQuery& q) {
      queries.push_back(q);
      return can_send;
    };
    h.finished = [this](bool complete) { finished.push_back(complete); };
    sched.setup("fake", "row(s)", limits, h, [this](std::size_t, const std::string& r) {
      if (r.rfind("other:", 0) == 0) return false;  // not traded here
      emitted.push_back(r);
      return true;
    });
    sched.set_streams(streams);
    sched.start_at(kNow - 10 * kMin);
    sched.open(true);
  }
  const ReplayQuery& last() const { return queries.back(); }
  void answer(std::vector<std::pair<std::int64_t, std::string>> rows,
              bool more = false,
              std::string next = {},
              const ReplayQuery* q = nullptr) {
    ReplayPage<std::string> p;
    for (auto& [t, id] : rows) p.rows.push_back(ReplayRow<std::string>{t, 0, id, id});
    p.more = more;
    p.next = std::move(next);
    sched.answer(q != nullptr ? *q : last(), std::move(p));
  }
};

}  // namespace

TEST_CASE("replay_scheduler: a full page with rows already booked still asks for the next") {
  // Every row the venue returned counts towards a full page, also the ones not forwarded (Spot
  // used to count the forwarded ones: a page with a known id passed for the last one).
  ReplayLimits l;
  l.page_rows = 3;
  Rig r(l);
  r.sched.resume(kNow - 10 * kMin, {"b"});
  REQUIRE(r.sched.run());
  REQUIRE(r.queries.size() == 1);
  CHECK(r.last().start_ms == kNow - 10 * kMin);
  CHECK(r.last().end_ms == 0);
  r.answer({{kNow - 9 * kMin, "a"}, {kNow - 8 * kMin, "b"}, {kNow - 7 * kMin, "other:c"}});
  REQUIRE(r.queries.size() == 2);  // full: asked again from the newest row
  CHECK(r.last().start_ms == kNow - 7 * kMin);
  CHECK(r.finished.empty());
  // The newest row's millisecond comes back: known by key, not forwarded again.
  r.answer({{kNow - 7 * kMin, "other:c"}, {kNow - 6 * kMin, "d"}});
  CHECK(r.emitted == std::vector<std::string>{"a", "d"});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);
}

TEST_CASE("replay_scheduler: a full page that cannot move past its start is incomplete") {
  ReplayLimits l;
  l.page_rows = 2;
  Rig r(l);
  REQUIRE(r.sched.run());
  const std::int64_t start = r.last().start_ms;
  r.answer({{start, "a"}, {start, "b"}});
  CHECK(r.queries.size() == 1);
  REQUIRE(r.finished.size() == 1);
  CHECK_FALSE(r.finished[0]);
  CHECK(r.emitted == std::vector<std::string>{"a", "b"});
}

TEST_CASE("replay_scheduler: newest-first pages, a window narrowed to its oldest row") {
  ReplayLimits l;
  l.newest_first = true;
  l.window_pages = 2;
  Rig r(l);
  REQUIRE(r.sched.run());
  const std::int64_t s = r.last().start_ms;
  r.answer({{s + 50, "e"}, {s + 40, "d"}}, true, "p2");
  REQUIRE(r.queries.size() == 2);
  CHECK(r.last().page == "p2");
  CHECK(r.last().start_ms == s);
  r.answer({{s + 30, "c"}, {s + 20, "b2"}}, true, "p3");
  // Two pages and still more: read again up to the oldest row seen, which ends the window.
  REQUIRE(r.queries.size() == 3);
  CHECK(r.last().page.empty());
  CHECK(r.last().start_ms == s);
  CHECK(r.last().end_ms == s + 20);
  CHECK(r.emitted.empty());
  r.answer({{s + 20, "b2"}, {s + 20, "b1"}, {s + 10, "a"}});
  CHECK(r.emitted == std::vector<std::string>{"a", "b1", "b2"});  // oldest first, venue order kept
  // Then from after that end: the newest part again, open-ended.
  REQUIRE(r.queries.size() == 4);
  CHECK(r.last().start_ms == s + 21);
  CHECK(r.last().end_ms == 0);
  r.answer({{s + 50, "e"}, {s + 40, "d"}, {s + 30, "c"}});
  CHECK(r.emitted == std::vector<std::string>{"a", "b1", "b2", "c", "d", "e"});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);

  // Every row of a window in one millisecond: it cannot be narrowed.
  Rig m(l);
  REQUIRE(m.sched.run());
  const std::int64_t s2 = m.last().start_ms;
  m.answer({{s2, "x"}}, true, "p2");
  m.answer({{s2, "y"}}, true, "p3");
  REQUIRE(m.finished.size() == 1);
  CHECK_FALSE(m.finished[0]);
  CHECK(m.emitted.empty());
}

TEST_CASE("replay_scheduler: an empty final window moves the watermark to the settle margin") {
  // Both the account that traded nothing and the one that did: the watermark goes to the
  // replay's start less settle_ms, never further, so a row indexed late is still asked for.
  ReplayLimits l;
  l.settle_ms = kMin;
  Rig r(l);
  r.sched.restart_from(kNow - 3 * kDay);
  REQUIRE(r.sched.run());
  r.answer({});
  CHECK(r.sched.since_ms(0) == kNow - kMin);
  r.now_ms = kNow + 10 * kMin;
  REQUIRE(r.sched.run());
  CHECK(r.last().start_ms == kNow - kMin);
}

TEST_CASE("replay_scheduler: rows near now are read again and a late one is still found") {
  ReplayLimits l;
  l.settle_ms = kMin;
  Rig r(l);
  REQUIRE(r.sched.run());
  // Two executions 10 s and 5 s before the query; one at 20 s is indexed only later.
  r.answer({{kNow - 10'000, "b"}, {kNow - 5'000, "c"}});
  CHECK(r.sched.since_ms(0) == kNow - kMin);  // not past the settle margin
  r.now_ms = kNow + kMin;
  REQUIRE(r.sched.run());
  CHECK(r.last().start_ms == kNow - kMin);
  r.answer({{kNow - 20'000, "a"}, {kNow - 10'000, "b"}, {kNow - 5'000, "c"}, {kNow + 30'000, "d"}});
  CHECK(r.emitted == std::vector<std::string>{"b", "c", "a", "d"});
  CHECK(r.sched.since_ms(0) == kNow);
  // The next one starts where the settle margin left it; rows before it are not asked for.
  r.now_ms = kNow + 2 * kMin;
  REQUIRE(r.sched.run());
  CHECK(r.last().start_ms == kNow);
  r.answer({{kNow + 30'000, "d"}});
  CHECK(r.emitted.size() == 4);
}

TEST_CASE("replay_scheduler: an incomplete replay is retried kRetryNs after it ended") {
  Rig r;
  REQUIRE(r.sched.run());
  r.sched.failed(r.last());
  REQUIRE(r.finished.size() == 1);
  CHECK_FALSE(r.finished[0]);
  CHECK(r.sched.retry_pending());
  const std::int64_t now = net::Reactor::now_ns();
  r.sched.on_timer(now + 1'000'000'000);
  CHECK(r.queries.size() == 1);  // not on the next tick
  r.sched.on_timer(now + ReplaySchedulerBase::kRetryNs + 1'000'000);
  REQUIRE(r.queries.size() == 2);
  CHECK(r.queries[1].start_ms == r.queries[0].start_ms);  // from the same watermark
  r.answer({{kNow - kMin, "a"}});
  CHECK(r.finished.back());
  CHECK_FALSE(r.sched.retry_pending());
  // A query that cannot even be sent is a failure the same way.
  r.can_send = false;
  r.sched.on_timer(net::Reactor::now_ns() + 2 * ReplaySchedulerBase::kRetryNs +
                   r.sched.limits().sweep_ns);
  CHECK_FALSE(r.finished.back());
  CHECK(r.sched.retry_pending());
  // While all is well: one a sweep interval.
  Rig w;
  w.sched.on_timer(net::Reactor::now_ns());  // the first tick sweeps
  REQUIRE(w.queries.size() == 1);
  w.answer({});
  w.sched.on_timer(net::Reactor::now_ns() + 1'000'000'000);
  CHECK(w.queries.size() == 1);
  w.sched.on_timer(net::Reactor::now_ns() + w.sched.limits().sweep_ns + 1);
  CHECK(w.queries.size() == 2);
}

TEST_CASE("replay_scheduler: a query never answered fails after kQueryTimeoutNs") {
  // Deribit asks on its WebSocket, which may stay up with the query lost: the replay, and the
  // reconciliation waiting for it, used to wait for the connection to drop.
  Rig r({}, 2);
  REQUIRE(r.sched.run());
  REQUIRE(r.queries.size() == 2);
  const ReplayQuery lost = r.queries[1];
  r.answer({{kNow - kMin, "a"}}, false, {}, r.queries.data());
  CHECK(r.finished.empty());
  const std::int64_t now = net::Reactor::now_ns();
  r.sched.on_timer(now + ReplaySchedulerBase::kQueryTimeoutNs - 1'000'000'000);
  CHECK(r.finished.empty());
  CHECK(r.sched.active());
  r.sched.on_timer(now + ReplaySchedulerBase::kQueryTimeoutNs + 1'000'000);
  REQUIRE(r.finished.size() == 1);
  CHECK_FALSE(r.finished[0]);
  CHECK_FALSE(r.sched.active());
  CHECK(r.sched.timeouts() == 1);
  CHECK(r.sched.retry_pending());
  CHECK(r.queries.size() == 2);  // the retry waits kRetryNs, not the same tick
  CHECK_FALSE(r.sched.expects(lost));
  // Its answer, late: nobody's.
  r.answer({{kNow - kMin, "late"}}, false, {}, &lost);
  CHECK(r.emitted == std::vector<std::string>{"a"});
  CHECK(r.finished.size() == 1);

  // The retry asks the lost stream again from its watermark; the late answer is still ignored.
  r.sched.on_timer(net::Reactor::now_ns() + ReplaySchedulerBase::kRetryNs + 1'000'000);
  REQUIRE(r.queries.size() == 4);
  CHECK(r.queries[3].start_ms == lost.start_ms);
  r.answer({{kNow - kMin, "late"}}, false, {}, &lost);
  CHECK(r.emitted == std::vector<std::string>{"a"});
  r.answer({}, false, {}, &r.queries[2]);
  r.answer({{kNow - kMin, "b"}}, false, {}, &r.queries[3]);
  CHECK(r.emitted == std::vector<std::string>{"a", "b"});
  REQUIRE(r.finished.size() == 2);
  CHECK(r.finished[1]);
  CHECK_FALSE(r.sched.retry_pending());
}

TEST_CASE("replay_scheduler: after close() a reply is ignored and nothing runs until open()") {
  Rig r;
  REQUIRE(r.sched.run());
  const ReplayQuery before = r.last();
  r.sched.close();
  CHECK_FALSE(r.sched.active());
  r.answer({{kNow - kMin, "a"}}, false, {}, &before);
  r.sched.failed(before);
  CHECK(r.emitted.empty());
  CHECK(r.finished.empty());  // nobody waits for an aborted replay
  CHECK_FALSE(r.sched.run());
  r.sched.due_in(0);
  r.sched.on_timer(net::Reactor::now_ns() + 10 * ReplaySchedulerBase::kRetryNs);
  CHECK(r.queries.size() == 1);

  r.sched.open(true);
  REQUIRE(r.sched.run());
  REQUIRE(r.queries.size() == 2);
  CHECK(r.last().generation > before.generation);
  r.answer({{kNow - kMin, "a"}}, false, {}, &before);  // still not this replay's
  CHECK(r.emitted.empty());
  r.answer({{kNow - kMin, "a"}});
  CHECK(r.emitted == std::vector<std::string>{"a"});

  // abort(): the transport is gone; the replay is retried, its replies ignored.
  REQUIRE(r.sched.run());
  const ReplayQuery cut = r.last();
  r.sched.abort();
  CHECK(r.sched.retry_pending());
  r.answer({{kNow, "b"}}, false, {}, &cut);
  CHECK(r.emitted.size() == 1);
  r.sched.on_timer(net::Reactor::now_ns() + ReplaySchedulerBase::kRetryNs + 1);
  CHECK(r.queries.size() == 4);
}

TEST_CASE("replay_scheduler: id cursors per stream") {
  ReplayLimits l;
  l.page_rows = 2;
  Rig r(l, 2);
  r.sched.set_cursor(1, 500);
  REQUIRE(r.sched.run());
  REQUIRE(r.queries.size() == 2);
  CHECK(r.queries[0].from_id == 0);
  CHECK(r.queries[1].from_id == 500);
  auto page = [&](const ReplayQuery& q, std::vector<std::pair<std::int64_t, std::string>> rows) {
    ReplayPage<std::string> p;
    for (auto& [seq, id] : rows) p.rows.push_back(ReplayRow<std::string>{kNow - kMin, seq, id, id});
    r.sched.answer(q, std::move(p));
  };
  page(r.queries[0], {{41, "t41"}});
  CHECK(r.sched.from_id(0) == 42);  // the time watermark gave way to the id after the highest
  page(r.queries[1], {{499, "t499"}, {500, "t500"}});  // full; one below what was asked for
  REQUIRE(r.queries.size() == 3);
  CHECK(r.queries[2].from_id == 501);
  CHECK(r.finished.empty());
  page(r.queries[2], {});
  CHECK(r.emitted == std::vector<std::string>{"t41", "t500"});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);
  REQUIRE(r.sched.run());
  CHECK(r.queries[3].from_id == 42);
  CHECK(r.queries[4].from_id == 501);
}

// A session that died inside its replay stored ids above the cursor's start; the replay reads from
// the start and skips them, on their own stream only.
TEST_CASE("replay_scheduler: known ids at or after a cursor are skipped on their stream") {
  ReplayLimits l;
  l.page_rows = 4;
  Rig r(l, 2);
  r.sched.set_cursor(0, 101);
  r.sched.set_cursor(1, 101);
  r.sched.set_known_ids(0, {105, 101});  // any order
  REQUIRE(r.sched.run());
  REQUIRE(r.queries.size() == 2);
  auto page = [&](const ReplayQuery& q, std::vector<std::pair<std::int64_t, std::string>> rows) {
    ReplayPage<std::string> p;
    for (auto& [seq, id] : rows) p.rows.push_back(ReplayRow<std::string>{kNow - kMin, seq, id, id});
    r.sched.answer(q, std::move(p));
  };
  page(r.queries[0], {{101, "a101"}, {102, "a102"}, {105, "a105"}});
  page(r.queries[1], {{101, "b101"}, {105, "b105"}});  // another symbol's ids: its own
  CHECK(r.emitted == std::vector<std::string>{"a102", "b101", "b105"});
  CHECK(r.sched.from_id(0) == 106);
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);
}

TEST_CASE("replay_scheduler: windows, the history split and the history floor") {
  ReplayLimits l;
  l.window_ms = 7 * kDay;
  l.history_ms = 90 * kDay;
  l.settle_ms = kMin;
  Rig r(l);
  r.sched.restart_from(kNow - 10 * kDay);
  REQUIRE(r.sched.run());
  CHECK(r.last().start_ms == kNow - 10 * kDay);
  CHECK(r.last().end_ms == kNow - 3 * kDay - 1);
  r.answer({});
  CHECK(r.last().start_ms == kNow - 3 * kDay);
  CHECK(r.last().end_ms == 0);
  r.answer({});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);

  // Older than the venue keeps: from the floor, and not exact.
  r.sched.restart_from(kNow - 100 * kDay);
  REQUIRE(r.sched.run());
  CHECK(r.last().start_ms == kNow - 90 * kDay);
  while (r.finished.size() < 2) r.answer({});
  CHECK_FALSE(r.finished[1]);
  CHECK(r.queries.size() == 2 + 13);

  // A history endpoint that does not cover the recent one: the history window ends where the
  // recent one starts, and the recent one reads the rest.
  ReplayLimits d;
  d.recent_ms = 23 * 3600 * 1000;
  d.history_covers_recent = false;
  Rig h(d);
  h.sched.restart_from(kNow - 30 * 3600 * 1000);
  REQUIRE(h.sched.run());
  CHECK(h.last().history);
  CHECK(h.last().end_ms == kNow - d.recent_ms);
  h.answer({{kNow - 29 * 3600 * 1000, "old"}});
  CHECK_FALSE(h.last().history);
  CHECK(h.last().start_ms == kNow - d.recent_ms + 1);
  CHECK(h.last().end_ms == 0);
  h.answer({{kNow - 3'600'000, "new"}});
  CHECK(h.emitted == std::vector<std::string>{"old", "new"});
  // One endpoint for both: one open window.
  ReplayLimits o;
  o.recent_ms = 3 * kDay;
  Rig one(o);
  one.sched.restart_from(kNow - 13 * kDay);
  REQUIRE(one.sched.run());
  CHECK(one.last().history);
  CHECK(one.last().end_ms == 0);
}

TEST_CASE("replay_scheduler: due_in runs one replay a moment later, once") {
  ReplayLimits l;
  l.sweep_ns = 0;
  Rig r(l);
  const std::int64_t now = net::Reactor::now_ns();
  r.sched.due_in(1'000'000'000);
  r.sched.on_timer(now);
  CHECK(r.queries.empty());
  r.sched.on_timer(now + 2'000'000'000);
  REQUIRE(r.queries.size() == 1);
  r.sched.due_in(0);  // while one runs: after it
  r.sched.on_timer(now + 3'000'000'000);
  CHECK(r.queries.size() == 1);
  r.answer({});
  r.sched.on_timer(now + 3'000'000'000);
  CHECK(r.queries.size() == 2);
  r.answer({});
  r.sched.on_timer(now + 60'000'000'000);
  CHECK(r.queries.size() == 2);
}

TEST_CASE("replay_scheduler: a restart while a replay runs goes on from its start, as one replay") {
  // fastmm-gateway: strategy a attaches (its store ends at -5 min) and its replay runs; b attaches
  // (its store ends at -8 min) before that replay has ended. b's gap was read only if the replay
  // running had started early enough, and after a failed one the retry started at b's start, so a
  // gap before it was never read.
  Rig r;
  r.sched.resume(kNow - 5 * kMin, {"a1"});
  REQUIRE(r.sched.run());
  REQUIRE(r.queries.size() == 1);
  CHECK(r.last().start_ms == kNow - 5 * kMin);
  r.sched.resume(kNow - 8 * kMin, {"b1"});
  CHECK(r.sched.run());  // joined
  CHECK(r.queries.size() == 1);
  r.answer({{kNow - 4 * kMin, "a1"}, {kNow - 3 * kMin, "x"}});
  CHECK(r.finished.empty());  // not before b's start has been read
  CHECK(r.queries.size() == 1);
  r.sched.on_timer(net::Reactor::now_ns());  // the next tick
  REQUIRE(r.queries.size() == 2);
  CHECK(r.last().start_ms == kNow - 8 * kMin);
  r.answer({{kNow - 7 * kMin, "b1"}, {kNow - 6 * kMin, "y"}, {kNow - 3 * kMin, "x"}});
  // Both stores' rows skipped; x again, as a restart reads again what it covers.
  CHECK(r.emitted == std::vector<std::string>{"x", "y", "x"});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);

  // A restart later than where a failed replay started: it goes on from the earlier.
  Rig f;
  f.sched.resume(kNow - 8 * kMin, {});
  REQUIRE(f.sched.run());
  f.sched.resume(kNow - 5 * kMin, {});
  f.sched.failed(f.last());
  CHECK(f.finished.empty());
  CHECK_FALSE(f.sched.retry_pending());
  f.sched.on_timer(net::Reactor::now_ns());
  REQUIRE(f.queries.size() == 2);
  CHECK(f.last().start_ms == kNow - 8 * kMin);
  f.answer({});
  REQUIRE(f.finished.size() == 1);
  CHECK(f.finished[0]);  // the one that went on read it all
}

// ---- order lookups (Hooks::lookup) -----------------------------------------------------------
namespace {

// Rows "<key>@<order>" name an order by the venue's id; the connector can name it once `named`
// holds it. Emitted as "<key>+" (naming its order) or "<key>-" (naming none), "?" appended when
// marked to be sent again (emitting_unresolved()).
struct LookupRig {
  ReplayScheduler<std::string> sched;
  std::vector<ReplayQuery> queries;
  std::vector<ReplayLookup> lookups;
  std::vector<std::string> emitted;
  std::vector<bool> finished;
  std::vector<std::string> named;
  bool can_look_up = true;

  static std::string order_of(const std::string& r) {
    const std::size_t at = r.find('@');
    return at == std::string::npos ? std::string{} : r.substr(at + 1);
  }
  [[nodiscard]] bool is_named(const std::string& order) const {
    return order.empty() || std::find(named.begin(), named.end(), order) != named.end();
  }

  explicit LookupRig(ReplayLimits limits = {}) {
    ReplaySchedulerBase::Hooks h;
    h.ready = [] { return true; };
    h.now_ms = [] { return kNow; };
    h.query = [this](const ReplayQuery& q) {
      queries.push_back(q);
      return true;
    };
    h.finished = [this](bool complete) { finished.push_back(complete); };
    h.lookup = [this](const ReplayLookup& l) {
      lookups.push_back(l);
      return can_look_up;
    };
    sched.setup(
        "fake",
        "row(s)",
        limits,
        h,
        [this](std::size_t, const std::string& r) {
          emitted.push_back(r.substr(0, r.find('@')) + (is_named(order_of(r)) ? "+" : "-") +
                            (sched.emitting_unresolved() ? "?" : ""));
          return true;
        },
        [this](std::size_t, const std::string& r) {
          const std::string o = order_of(r);
          return is_named(o) ? std::string{} : o;
        });
    sched.set_streams(1);
    sched.start_at(kNow - 10 * kMin);
    sched.open(true);
  }
  void answer(const std::vector<std::string>& rows) {
    ReplayPage<std::string> p;
    std::int64_t t = queries.back().start_ms;
    for (const std::string& r : rows) p.rows.push_back({t++, 0, r.substr(0, r.find('@')), r});
    sched.answer(queries.back(), std::move(p));
  }
  // The venue's answer to the last lookup of `order`.
  void look_up(const std::string& order, LookupResult res) {
    for (auto it = lookups.rbegin(); it != lookups.rend(); ++it) {
      if (it->order_id != order) continue;
      if (res == LookupResult::Named) named.push_back(order);
      sched.looked_up(*it, res);
      return;
    }
    FAIL("no lookup of " << order);
  }
  void next_replay() {
    sched.on_timer(net::Reactor::now_ns() + ReplaySchedulerBase::kRetryNs + 1'000'000);
  }
};

}  // namespace

TEST_CASE("replay_scheduler: rows naming an order by the venue's id wait for its lookup") {
  ReplayLimits l;
  l.max_lookups = 2;
  LookupRig r(l);
  REQUIRE(r.sched.run());
  r.answer({"a@1", "b@1", "c@2", "d@3", "e"});
  // One lookup per order, two in this replay: 3 is over the budget.
  REQUIRE(r.lookups.size() == 2);
  CHECK(r.lookups[0].order_id == "1");
  CHECK(r.lookups[1].order_id == "2");
  CHECK(r.emitted.empty());  // the window waits
  r.look_up("1", LookupResult::Named);
  CHECK(r.emitted.empty());
  r.look_up("2", LookupResult::NotOurs);
  // d is sent again later: marked. c's order is not ours: it stays naming none, unmarked.
  CHECK(r.emitted == std::vector<std::string>{"a+", "b+", "c-", "d-?", "e+"});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);        // lookups do not make a replay incomplete
  CHECK(r.sched.held() == 1);  // d: over the budget; c's order is not ours
  CHECK(r.sched.retry_pending());

  // The next replay asks for d's order and sends d again, naming it.
  r.next_replay();
  REQUIRE(r.lookups.size() == 3);
  CHECK(r.lookups[2].order_id == "3");
  r.look_up("3", LookupResult::Named);
  r.answer({"f@2"});  // not ours: not asked again
  CHECK(r.lookups.size() == 3);
  CHECK(r.emitted == std::vector<std::string>{"a+", "b+", "c-", "d-?", "e+", "d+", "f-"});
  REQUIRE(r.finished.size() == 2);
  CHECK(r.finished[1]);
  CHECK(r.sched.held() == 0);
  CHECK_FALSE(r.sched.retry_pending());
  CHECK(r.sched.lookups() == 3);
}

TEST_CASE("replay_scheduler: a failed lookup is asked again at the next replay, a few times") {
  LookupRig r;
  REQUIRE(r.sched.run());
  r.answer({"a@1"});
  REQUIRE(r.lookups.size() == 1);
  r.look_up("1", LookupResult::Failed);
  // Not held back: the row goes out naming no order, marked, and its order is asked for again.
  CHECK(r.emitted == std::vector<std::string>{"a-?"});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.finished[0]);
  CHECK(r.sched.held() == 1);
  CHECK(r.sched.retry_pending());
  r.next_replay();
  REQUIRE(r.lookups.size() == 2);
  r.answer({});
  CHECK(r.finished.size() == 1);  // the replay waits for the lookup too
  r.look_up("1", LookupResult::Named);
  CHECK(r.emitted == std::vector<std::string>{"a-?", "a+"});
  CHECK(r.finished.size() == 2);
  CHECK(r.sched.held() == 0);

  // One that keeps failing (or cannot be sent: no rate-limit headroom) is given up after
  // kMaxLookupAttempts replays: a last copy, unmarked, still naming no order.
  LookupRig f;
  f.can_look_up = false;
  REQUIRE(f.sched.run());
  f.answer({"a@7"});
  CHECK(f.emitted == std::vector<std::string>{"a-?"});
  for (std::uint32_t i = 0; i < ReplaySchedulerBase::kMaxLookupAttempts; ++i) {
    INFO("replay " << i);
    CHECK(f.sched.held() == 1);
    REQUIRE(f.sched.retry_pending());
    f.next_replay();
    f.answer({});
  }
  CHECK(f.sched.held() == 0);
  CHECK_FALSE(f.sched.retry_pending());
  CHECK(f.lookups.size() == 1 + ReplaySchedulerBase::kMaxLookupAttempts);
  CHECK(f.emitted == std::vector<std::string>{"a-?", "a-"});

  // A retry the venue answers "not ours": the last copy, unmarked.
  LookupRig n;
  REQUIRE(n.sched.run());
  n.answer({"a@9"});
  n.look_up("9", LookupResult::Failed);
  n.next_replay();
  n.answer({});
  n.look_up("9", LookupResult::NotOurs);
  CHECK(n.emitted == std::vector<std::string>{"a-?", "a-"});
  CHECK(n.sched.held() == 0);
}

TEST_CASE("replay_scheduler: a lookup never answered releases its window after kQueryTimeoutNs") {
  LookupRig r;
  REQUIRE(r.sched.run());
  r.answer({"a@1", "b"});
  REQUIRE(r.lookups.size() == 1);
  const std::int64_t now = net::Reactor::now_ns();
  r.sched.on_timer(now + ReplaySchedulerBase::kQueryTimeoutNs - 1'000'000'000);
  CHECK(r.emitted.empty());
  r.sched.on_timer(now + ReplaySchedulerBase::kQueryTimeoutNs + 1'000'000);
  CHECK(r.emitted == std::vector<std::string>{"a-?", "b+"});
  REQUIRE(r.finished.size() == 1);
  CHECK(r.sched.held() == 1);
  // Its late answer is an earlier replay's: nothing moves.
  const ReplayLookup late = r.lookups[0];
  r.named.emplace_back("1");
  r.sched.looked_up(late, LookupResult::Named);
  CHECK(r.emitted.size() == 2);

  // A disconnect while a window waits: nothing goes out, and the rows are read again after it.
  LookupRig c;
  REQUIRE(c.sched.run());
  c.answer({"a@1"});
  REQUIRE(c.lookups.size() == 1);
  c.sched.close();
  c.look_up("1", LookupResult::Named);
  CHECK(c.emitted.empty());
  c.sched.open(true);
  REQUIRE(c.sched.run());
  CHECK(c.queries.back().start_ms == c.queries.front().start_ms);
  c.answer({"a@1"});  // named now: no lookup
  CHECK(c.lookups.size() == 1);
  CHECK(c.emitted == std::vector<std::string>{"a+"});
}
