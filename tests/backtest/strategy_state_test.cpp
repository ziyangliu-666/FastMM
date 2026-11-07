// A strategy's learnt state in the journal's checkpoints: StateWriter / StateReader, the
// state_version every state record carries and the refusal of another version, checkpoints the
// strategy asks for (ctx.request_checkpoint) and their replay, the room a checkpoint leaves in the
// journal ring, and a backtest from a mid-session checkpoint against a cold start.
#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/journal_state.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/core/session_state.hpp"
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/state_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// Quotes one lot on each side, `dist` ticks behind the touch. A fill on a side widens that side by
// one tick and every second each side narrows by a tenth towards one tick: what it quotes depends
// on its own fills, which no warm-up on market data can learn.
class Learner : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "learner"; }
  static inline std::uint32_t version = 1;
  static inline bool cold = false;        // restore() refuses: a strategy that keeps no state
  static inline std::size_t padding = 0;  // bytes of state beyond the model
  static inline Duration ask_every{};     // ctx.request_checkpoint() at this period
  static inline std::vector<std::int64_t> asked;

  [[nodiscard]] std::uint32_t state_version() const noexcept { return version; }

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    static_cast<void>(ctx.every(seconds(1), 1));
    if (ask_every.ns > 0) static_cast<void>(ctx.every(ask_every, 2));
    buf_.resize(sizeof(Model) + 16 + padding);
    pad_.resize(padding);
  }
  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t tag) noexcept {
    if (tag == 2) {
      asked.push_back(ctx.now().ns);
      ctx.request_checkpoint();
      return;
    }
    for (double& d : m_.dist) d = 1.0 + (d - 1.0) * 0.9;
  }
  template <class Ctx>
  void on_fill(Ctx&, const Fill& f) noexcept {
    m_.dist[f.side == Side::Buy ? 0 : 1] += 1.0;
    ++m_.fills;
  }
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    if (!b.is_valid()) return;
    const Instrument& inst = ctx.instrument(id);
    DesiredQuotes q;
    q.bid(b.best_bid().price - inst.ticks(std::lround(m_.dist[0])), inst.lot);
    q.ask(b.best_ask().price + inst.ticks(std::lround(m_.dist[1])), inst.lot);
    static_cast<void>(ctx.set_quotes(id, q));
  }

  std::string_view state() noexcept {
    StateWriter w(std::span<std::byte>(buf_.data(), buf_.size()));
    w.put(m_);
    w.put_array(std::span<const std::byte>(pad_));  // stands in for a big model
    return w.view();
  }
  bool restore(std::string_view bytes) noexcept {
    if (cold) return false;
    StateReader r(bytes);
    Model m;
    std::size_t n = 0;
    static std::vector<std::byte> sink(padding);
    if (!r.get(m) || !r.get_array(sink.data(), sink.size(), n) || !r.done()) return false;
    m_ = m;
    return true;
  }

 private:
  struct Model {
    std::array<double, 2> dist{2.0, 2.0};
    std::uint64_t fills = 0;
  };
  Model m_;
  std::vector<std::byte> buf_;
  std::vector<std::byte> pad_;
};

BacktestConfig learner_config() {
  BacktestConfig c = synthetic_config(33, seconds(60));
  c.transport.fill_model = sim::FillModel::L2Queue;
  for (sim::LatencyParams* p : {&c.transport.order_out, &c.transport.ack_in})
    *p = sim::LatencyParams{microseconds(300), Duration{}, 0.0};
  c.markout_horizons.clear();
  return c;
}

void reset_learner() {
  Learner::version = 1;
  Learner::cold = false;
  Learner::padding = 0;
  Learner::ask_every = Duration{};
  Learner::asked.clear();
}

// Orders a journal sent, by (side, price) and the 100 ms they went out in.
using Key = std::tuple<int, std::int64_t, std::int64_t>;
std::map<Key, int> orders_in(const std::string& path, std::int64_t from, std::int64_t to) {
  JournalReader r;
  REQUIRE(r.open(path));
  std::map<Key, int> out;
  std::map<std::uint64_t, Side> sides;  // by client order id, for the replaces
  r.for_each([&](const EventHeader* h) {
    if ((h->flags & EventHeader::kOutbound) == 0) return;
    if (h->type == EventType::OutNewOrder)
      sides[msg_cast<OutNewOrderMsg>(h).cl_ord_id.value] = msg_cast<OutNewOrderMsg>(h).side;
    if (h->type == EventType::OutReplace)
      sides[msg_cast<OutReplaceMsg>(h).cl_ord_id.value] =
          sides[msg_cast<OutReplaceMsg>(h).orig_cl_ord_id.value];
    if (h->recv_ts.ns < from || h->recv_ts.ns >= to) return;
    Price px;
    Side side;
    if (h->type == EventType::OutNewOrder) {
      px = msg_cast<OutNewOrderMsg>(h).price;
      side = msg_cast<OutNewOrderMsg>(h).side;
    } else if (h->type == EventType::OutReplace) {
      px = msg_cast<OutReplaceMsg>(h).price;
      side = Side::Sell;
      if (const auto it = sides.find(msg_cast<OutReplaceMsg>(h).orig_cl_ord_id.value);
          it != sides.end())
        side = it->second;
    } else {
      return;
    }
    ++out[Key{static_cast<int>(side), px.raw, (h->recv_ts.ns - from) / 100'000'000}];
  });
  return out;
}

// The share of `live`'s orders that `run` also sent, at the same price on the same side in the
// same 100 ms.
double matched(const std::map<Key, int>& live, const std::map<Key, int>& run) {
  int total = 0;
  int both = 0;
  for (const auto& [k, n] : live) {
    total += n;
    if (const auto it = run.find(k); it != run.end()) both += std::min(n, it->second);
  }
  return total == 0 ? 0.0 : static_cast<double>(both) / total;
}

ReplayStrategy learner_replay() {
  ReplayStrategy s;
  s.name = "learner";
  s.schema = &Learner::schema();
  s.make = [](RunnerDeps& deps) {
    return static_cast<sim::ReplayBackend*>(deps.backend)->make_runner<Learner>(deps);
  };
  return s;
}

}  // namespace

TEST_CASE("strategy_state: StateWriter and StateReader round trip and refuse what does not fit") {
  std::array<std::byte, 64> buf{};
  StateWriter w(buf);
  const std::array<std::int32_t, 3> xs{7, -1, 42};
  w.put(1.5);
  w.put_array(std::span<const std::int32_t>(xs));
  REQUIRE(w.ok());
  CHECK(w.size() == 8 + 4 + 12);

  StateReader r(w.view());
  double d = 0;
  std::array<std::int32_t, 4> back{};
  std::size_t n = 0;
  CHECK(r.get(d));
  CHECK(r.get_array(back.data(), back.size(), n));
  CHECK(r.done());
  CHECK(d == 1.5);
  CHECK(n == 3);
  CHECK(back[2] == 42);

  // Too few elements to hold the array, and bytes beyond the end.
  StateReader small(w.view());
  CHECK(small.get(d));
  CHECK_FALSE(small.get_array(back.data(), 2, n));
  CHECK_FALSE(small.ok());
  StateReader past(w.view().substr(0, 14));
  CHECK(past.get(d));
  std::uint32_t c = 0;
  CHECK(past.get(c));
  CHECK_FALSE(past.get(c));

  // A write past the buffer fails the writer and leaves no bytes.
  std::array<std::byte, 8> tiny{};
  StateWriter full(tiny);
  full.put(1.0);
  CHECK(full.ok());
  full.put(std::uint8_t{1});
  CHECK_FALSE(full.ok());
  CHECK(full.view().empty());
}

TEST_CASE("strategy_state: a checkpoint carries the state and its version, and restores it") {
  reset_learner();
  Learner::version = 7;
  BacktestConfig a = learner_config();
  a.engine.checkpoint_interval = seconds(10);
  a.journal_out = tmp_path("strategy_state_versioned.fmj");
  const BacktestResult r = run_backtest<Learner>(a);
  CHECK(r.engine.checkpoints == 6);
  CHECK(r.engine.checkpoints_skipped == 0);
  CHECK(r.engine.checkpoint_bytes >= 24);

  sim::RecordedSession s;
  {
    JournalReader jr;
    REQUIRE(jr.open(a.journal_out));
    read_states(std::span<JournalReader>{&jr, 1}, s);
  }
  REQUIRE(s.checkpoints.size() == 6);
  for (const sim::Checkpoint& c : s.checkpoints) {
    CHECK(c.version == 7);
    CHECK(c.strategy.size() == 8 + 8 + 8 + 4);  // dist[2], fills, an empty array
  }
  const sim::SessionStart at = s.start(a.start + seconds(35));
  CHECK(at.version == 7);
  CHECK(at.at == a.start + seconds(30));
}

TEST_CASE("strategy_state: a checkpoint of another state_version is refused") {
  reset_learner();
  BacktestConfig a = learner_config();
  a.duration = seconds(20);
  a.engine.checkpoint_interval = seconds(5);
  a.journal_out = tmp_path("strategy_state_v1.fmj");
  static_cast<void>(run_backtest<Learner>(a));

  Learner::version = 2;
  BacktestConfig b = learner_config();
  b.initial_state = InitialState::Journal;
  b.start_at = a.start + seconds(12);
  JournalSource src(a.journal_out, true);
  CHECK_THROWS_WITH_AS(static_cast<void>(run_backtest<Learner>(b, &src)),
                       doctest::Contains("has state_version 1 of learner; this build restores "
                                         "version 2"),
                       std::invalid_argument);
  CHECK_THROWS_WITH_AS(static_cast<void>(replay_journal(a.journal_out, a, {}, learner_replay())),
                       doctest::Contains("replay: the journal's state at"),
                       std::invalid_argument);

  // Without the strategy's state, the same start passes.
  b.initial_state = InitialState::None;
  b.start_at = Timestamp{};
  JournalSource plain(a.journal_out, true);
  CHECK_NOTHROW(static_cast<void>(run_backtest<Learner>(b, &plain)));
  reset_learner();
}

TEST_CASE("strategy_state: the state file records the state_version, and another is refused") {
  reset_learner();
  Learner::version = 4;
  const std::string path = tmp_path("strategy_state_v4.state");
  BacktestConfig a = learner_config();
  a.duration = seconds(10);
  a.engine.state_file = path;
  static_cast<void>(run_backtest<Learner>(a));
  std::string bytes;
  std::uint32_t version = 0;
  REQUIRE(read_strategy_state(path, bytes, version));
  CHECK(version == 4);
  CHECK(bytes.size() == 28);

  Learner::version = 5;
  CHECK_THROWS_WITH_AS(static_cast<void>(run_backtest<Learner>(a)),
                       doctest::Contains("the state file " + path +
                                         " has state_version 4 of learner; this build restores "
                                         "version 5"),
                       std::invalid_argument);

  // A file from before the head is version 0, and a strategy of version 0 takes it.
  REQUIRE(write_file_atomic(path, bytes));
  Learner::version = 0;
  CHECK_NOTHROW(static_cast<void>(run_backtest<Learner>(a)));
  reset_learner();
}

TEST_CASE("strategy_state: a checkpoint the strategy asks for is written and replays") {
  reset_learner();
  Learner::ask_every = milliseconds(7'300);
  BacktestConfig a = learner_config();
  a.duration = seconds(30);
  a.journal_out = tmp_path("strategy_state_asked.fmj");
  const BacktestResult r = run_backtest<Learner>(a);
  REQUIRE(Learner::asked.size() == 4);
  CHECK(r.engine.checkpoints == 4);

  sim::RecordedSession s;
  {
    JournalReader jr;
    REQUIRE(jr.open(a.journal_out));
    read_states(std::span<JournalReader>{&jr, 1}, s);
  }
  REQUIRE(s.checkpoints.size() == 4);
  for (std::size_t i = 0; i < 4; ++i) CHECK(s.checkpoints[i].at.ns == Learner::asked[i]);

  // fastmm-replay repeats them at the journal's markers, not at the strategy's own asks.
  const ReplayResult rr = replay_journal(a.journal_out, a, ReplayOptions{}, learner_replay());
  INFO("first mismatch ", rr.first_mismatch, ": ", rr.expected_message, " / ", rr.actual_message);
  CHECK(rr.ok());
  CHECK(rr.outbound_messages > 0);
  reset_learner();
}

TEST_CASE("strategy_state: a checkpoint that would crowd the journal ring is skipped") {
  reset_learner();
  Learner::padding = 14U << 20;  // with the model, more than the 16 MiB ring leaves room for
  BacktestConfig a = learner_config();
  a.duration = seconds(10);
  a.engine.checkpoint_interval = seconds(4);
  a.journal_out = tmp_path("strategy_state_big.fmj");
  const BacktestResult r = run_backtest<Learner>(a);
  CHECK(r.engine.checkpoints == 0);
  CHECK(r.engine.checkpoints_skipped == 2);
  CHECK(r.engine.journal_overflows == 0);

  Learner::padding = 4U << 20;  // 4 MiB fits
  a.journal_out = tmp_path("strategy_state_4m.fmj");
  const BacktestResult ok = run_backtest<Learner>(a);
  CHECK(ok.engine.checkpoints == 2);
  CHECK(ok.engine.checkpoint_bytes >= 4U << 20);
  reset_learner();
}

TEST_CASE(
    "strategy_state: from a mid-session checkpoint a run follows the session, cold it does not") {
  reset_learner();
  BacktestConfig a = learner_config();
  a.engine.checkpoint_interval = seconds(5);
  a.journal_out = tmp_path("strategy_state_session.fmj");
  const BacktestResult live = run_backtest<Learner>(a);
  REQUIRE(live.engine.fills > 20);

  const Timestamp t = a.start + seconds(30);
  const std::int64_t to = (a.start + seconds(60)).ns;
  const std::map<Key, int> want = orders_in(a.journal_out, t.ns, to);
  REQUIRE(want.size() > 50);

  const auto fills_after = [&](const BacktestResult& r) {
    return std::count_if(
        r.fills.ts.begin(), r.fills.ts.end(), [&](std::int64_t x) { return x >= t.ns; });
  };
  std::array<double, 2> share{};
  std::array<std::ptrdiff_t, 2> fills{};
  for (const bool cold : {false, true}) {
    Learner::cold = cold;
    BacktestConfig b = learner_config();
    b.initial_state = InitialState::Journal;
    b.start_at = t;
    b.journal_out = tmp_path(cold ? "strategy_state_cold.fmj" : "strategy_state_warm.fmj");
    JournalSource src(a.journal_out, true);
    const BacktestResult r = run_backtest<Learner>(b, &src);
    share[cold ? 1 : 0] = matched(want, orders_in(b.journal_out, t.ns, to));
    fills[cold ? 1 : 0] = fills_after(r);
  }
  MESSAGE("orders of the session's last 30 s matched: warm ",
          share[0],
          ", cold ",
          share[1],
          "; fills then: session ",
          fills_after(live),
          ", warm ",
          fills[0],
          ", cold ",
          fills[1]);
  CHECK(share[0] > share[1] + 0.2);
  reset_learner();
}
