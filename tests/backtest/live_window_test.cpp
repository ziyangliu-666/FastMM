// A backtest that starts inside a live session: the engine's positions as of then
// ([backtest.positions]).
#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

BacktestConfig parse(const std::string& backtest) {
  const std::string text =
      "[[instruments]]\nvenue = \"sim\"\nsymbol = \"BTCUSDT\"\ntick = \"0.01\"\nlot = "
      "\"0.001\"\n\n[venues.sim]\nkind = \"sim\"\n\n[venues.sim2]\nkind = \"sim\"\npool_of = "
      "\"sim\"\n\n[strategy]\nname = \"basic_mm\"\n\n[backtest]\n" +
      backtest;
  return BacktestConfig::from_config(Config::parse(text));
}

// The position at the first book.
class FirstPosition : public StrategyBase<BasicMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "first_position"; }
  static inline Qty seen;
  static inline bool done = false;
  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book&) noexcept {
    if (done) return;
    done = true;
    seen = ctx.position(id).qty;
  }
};

}  // namespace

TEST_CASE("backtest.live_window: [backtest.positions]") {
  const BacktestConfig c = parse("[backtest.positions]\nBTCUSDT = \"-0.5@101.25\"\n");
  REQUIRE(c.positions.size() == 1);
  CHECK(c.positions[0].instrument == InstrumentId{0});
  CHECK(c.positions[0].qty == qt("-0.5"));
  CHECK(c.positions[0].avg_px == px("101.25"));
  CHECK(parse("[backtest.positions]\nBTCUSDT = \"2\"\n").positions[0].avg_px.is_zero());
  CHECK(parse("").positions.empty());
  CHECK_THROWS_WITH_AS(static_cast<void>(parse("[backtest.positions]\nETHUSDT = \"1\"\n")),
                       doctest::Contains("no instrument ETHUSDT"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(static_cast<void>(parse("[backtest.positions]\nBTCUSDT = \"one\"\n")),
                       doctest::Contains("is not \"<qty>\""),
                       ConfigError);
}

TEST_CASE("backtest.live_window: the engine starts at the positions given, over the journal's") {
  // A part from the middle of a session: no positions in its records.
  const std::string path = tmp_path("live_window_positions.fmj");
  {
    JournalBuilder j(path, true);
    j.book(true, {{"100.00", "5"}}, {{"100.02", "5"}});
    j.now += 1000;
    j.book(false, {{"100.00", "4"}}, {});
  }
  for (const bool given : {false, true}) {
    CAPTURE(given);
    BacktestConfig cfg = BacktestConfig::single_instrument("BTCUSDT", px("0.01"), qt("0.001"));
    cfg.transport.fill_model = sim::FillModel::L2Queue;
    cfg.markout_horizons.clear();
    cfg.measure_wall_clock = false;
    cfg.initial_state = InitialState::Journal;
    if (given) cfg.positions = {StartPosition{InstrumentId{0}, qt("0.7"), px("99.5")}};
    FirstPosition::done = false;
    JournalSource src(path, true);
    static_cast<void>(run_backtest<FirstPosition>(cfg, &src));
    REQUIRE(FirstPosition::done);
    CHECK(FirstPosition::seen == (given ? qt("0.7") : Qty{}));
  }
}
