// The state a recorded session started from (backtest/journal_state.hpp): the restored bytes, a
// snapshot split over records, the positions reconciled before the first order, and a backtest
// that starts from them ([backtest] initial_state = "journal").
#include "fastmm/backtest/journal_state.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

std::string positions_bytes(std::vector<StatePosition> ps) {
  std::string s(ps.size() * sizeof(StatePosition), '\0');
  std::memcpy(s.data(), ps.data(), s.size());
  return s;
}

sim::SessionStart start_of(const std::string& path) {
  JournalReader r;
  REQUIRE(r.open(path));
  return journal_start_state(std::span<JournalReader>{&r, 1});
}

// A session: a book, positions restored, the restored state, a first order, later positions and a
// snapshot.
void write_session(const std::string& path, bool restored, bool snapshot) {
  JournalBuilder j(path, true);
  j.book(true, {{"100.00", "5"}}, {{"100.02", "5"}});
  j.position("0.5", "99.00");
  j.position("-2", "50.00", 5, 1);  // another account's instrument
  j.position("0.7", "99.50");       // the latest for the account wins
  if (restored)
    j.strategy_state(StrategyStateMsg::Kind::Restored,
                     1,
                     StrategyStateMsg::kStatePart,
                     "abc",
                     StrategyStateMsg::kAccepted);
  j.now += 1000;
  j.out_new(1, Side::Buy, "99.99", "0.1");
  j.position("9", "1.00");  // after the first order: not the start
  if (snapshot) {
    const std::string big(20'000, 'x');
    j.strategy_state(StrategyStateMsg::Kind::Snapshot, 2, StrategyStateMsg::kStatePart, big);
    j.strategy_state(StrategyStateMsg::Kind::Snapshot,
                     2,
                     StrategyStateMsg::kPositionsPart,
                     positions_bytes({StatePosition{0, 0, {}, qt("1.5").raw, px("98.00").raw}}));
  }
  j.now += 1000;
  j.book(false, {{"100.00", "4"}}, {});
}

}  // namespace

TEST_CASE("backtest.journal_state: the restored state with the positions before the first order") {
  const std::string path = tmp_path("journal_state_restored.fmj");
  write_session(path, true, true);
  const sim::SessionStart s = start_of(path);
  CHECK(s.has_state);
  CHECK(s.strategy == "abc");
  REQUIRE(s.positions.size() == 2);
  CHECK(s.positions[0].position_qty == qt("0.7"));
  CHECK(s.positions[0].avg_px == px("99.50"));
  CHECK(s.positions[0].hdr.venue == VenueId{0});
  CHECK(s.positions[1].position_qty == qt("-2"));
  CHECK(s.positions[1].hdr.venue == VenueId{5});
  CHECK(s.note.find("restored state (3 bytes, taken live)") != std::string::npos);
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(restored_state(r) == std::optional<std::string>("abc"));
}

TEST_CASE(
    "backtest.journal_state: without a restored state, the first snapshot and its positions") {
  const std::string path = tmp_path("journal_state_snapshot.fmj");
  write_session(path, false, true);
  const sim::SessionStart s = start_of(path);
  CHECK(s.has_state);
  CHECK(s.strategy == std::string(20'000, 'x'));  // two records put back together
  REQUIRE(s.positions.size() == 1);
  CHECK(s.positions[0].position_qty == qt("1.5"));
  CHECK(s.positions[0].kind == ReconcileMsg::Kind::Position);
}

TEST_CASE("backtest.journal_state: an older journal gives its reconciled positions alone") {
  const std::string path = tmp_path("journal_state_old.fmj");
  write_session(path, false, false);
  const sim::SessionStart s = start_of(path);
  CHECK_FALSE(s.has_state);
  CHECK(s.strategy.empty());
  CHECK(s.positions.size() == 2);
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK_FALSE(restored_state(r).has_value());
}

namespace {

// Keeps what it was restored with and the position it saw at its first book.
class Remembers : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "remembers"; }
  static inline std::string restored;
  static inline Qty first_position;
  static inline bool seen = false;
  static inline Qty last_quote_qty;
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book&) noexcept {
    last_quote_qty = params().quote_qty;
    if (seen) return;
    seen = true;
    first_position = ctx.position(id).qty;
  }
  std::string_view state() noexcept { return restored; }
  bool restore(std::string_view b) {
    restored.assign(b);
    return true;
  }
};

BacktestConfig journal_config(InitialState s) {
  BacktestConfig c = BacktestConfig::single_instrument("BTCUSDT", px("0.01"), qt("0.001"));
  c.transport.fill_model = sim::FillModel::L2Queue;
  c.markout_horizons.clear();
  c.measure_wall_clock = false;
  c.initial_state = s;
  return c;
}

}  // namespace

TEST_CASE("backtest.journal_state: a backtest starts from the journal's state and positions") {
  const std::string path = tmp_path("journal_state_run.fmj");
  write_session(path, true, false);
  for (const InitialState s : {InitialState::Journal, InitialState::StateFile}) {
    CAPTURE(static_cast<int>(s));
    Remembers::restored.clear();
    Remembers::first_position = Qty{};
    Remembers::seen = false;
    JournalSource src(path, true);
    static_cast<void>(run_backtest<Remembers>(journal_config(s), &src));
    REQUIRE(Remembers::seen);
    if (s == InitialState::Journal) {
      CHECK(Remembers::restored == "abc");
      CHECK(Remembers::first_position == qt("0.7"));  // instrument 1 is not configured
    } else {
      CHECK(Remembers::restored.empty());
      CHECK(Remembers::first_position.is_zero());
    }
  }
  CHECK_THROWS_WITH_AS(
      static_cast<void>(run_backtest<Remembers>(journal_config(InitialState::Journal), nullptr)),
      doctest::Contains("needs a journal as the data source"),
      std::invalid_argument);
}

TEST_CASE("backtest.journal_state: [backtest] initial_state") {
  const auto parse = [](const char* v) {
    std::string text =
        "[[instruments]]\nvenue = \"sim\"\nsymbol = \"BTCUSDT\"\ntick = \"0.01\"\nlot = "
        "\"0.001\"\n\n[venues.sim]\nkind = \"sim\"\n\n[strategy]\nname = \"basic_mm\"\n\n"
        "[backtest]\n";
    if (v != nullptr) text += std::string("initial_state = \"") + v + "\"\n";
    return BacktestConfig::from_config(Config::parse(text));
  };
  CHECK(parse(nullptr).initial_state == InitialState::StateFile);
  CHECK(parse("journal").initial_state == InitialState::Journal);
  CHECK(parse("none").initial_state == InitialState::None);
  CHECK_THROWS_WITH_AS(static_cast<void>(parse("later")),
                       doctest::Contains("backtest.initial_state: 'later'"),
                       ConfigError);
}

TEST_CASE("backtest.journal_state: params_from_journal replays the session's parameter updates") {
  const std::string path = tmp_path("journal_state_params.fmj");
  {
    JournalBuilder j(path, true);
    j.book(true, {{"100.00", "5"}}, {{"100.02", "5"}});
    j.now += 1000;
    j.param_update(2, qt("0.5").raw);  // quote_qty, index 2 of the schema
    j.now += 1000;
    j.book(false, {{"100.00", "4"}}, {});
  }
  for (const bool replay : {false, true}) {
    CAPTURE(replay);
    JournalSource src(path, true);
    BacktestConfig cfg = journal_config(InitialState::StateFile);
    cfg.params_from_journal = replay;
    static_cast<void>(run_backtest<Remembers>(cfg, &src));
    CHECK(Remembers::last_quote_qty == (replay ? qt("0.5") : qt("0.01")));
  }
  BacktestConfig cfg = journal_config(InitialState::StateFile);
  cfg.params_from_journal = true;
  CHECK_THROWS_WITH_AS(static_cast<void>(run_backtest<Remembers>(cfg, nullptr)),
                       doctest::Contains("params_from_journal needs a journal"),
                       std::invalid_argument);
}
