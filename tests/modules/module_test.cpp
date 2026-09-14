// A strategy library in its own STATIC archive, reached only through its registration functions:
// the linker must pull the registration and every factory instantiation out of the archive, with
// no static initialisers and no whole-archive linking (ADR-0012, section 5).
#include "fastmm/strategies/module.hpp"

#include "module/strategies.hpp"
#include "test_support.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/live/live_backend.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/builtin.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace fastmm;
using namespace fastmm::literals;

TEST_CASE("modules: a strategy library in a static archive registers all three transports") {
  StrategyRegistry reg;
  test_mm::register_strategies(reg);
  const StrategyEntry* e = reg.find("test_module_mm");
  REQUIRE(e != nullptr);
  CHECK(e->supports(TransportKind::Sim));
  CHECK(e->supports(TransportKind::Replay));
  CHECK(e->supports(TransportKind::Live));
  CHECK(e->schema->find("edge_ticks") != nullptr);
  CHECK_NOTHROW(test_mm::register_strategies(reg));  // already present
  CHECK(reg.entries().size() == 1);
  // Built-ins and a module side by side.
  register_builtin_strategies(reg);
  CHECK(reg.entries().size() == 4);
}

TEST_CASE("modules: a name taken by different code is a conflict and changes nothing") {
  StrategyRegistry reg;
  register_builtin_strategies(reg);
  const StrategyEntry::Factory before =
      reg.find("basic_mm")->factories[static_cast<std::size_t>(TransportKind::Sim)];
  CHECK(before == &sim_factory<BasicMM>);
  CHECK_THROWS_AS(test_mm::register_shadow_basic_mm(reg), StrategyConflict);
  CHECK(reg.find("basic_mm")->factories[static_cast<std::size_t>(TransportKind::Sim)] == before);
  // Alone, the same registration is fine: the explicit instantiation in another file links.
  StrategyRegistry alone;
  test_mm::register_shadow_basic_mm(alone);
  CHECK(alone.find("basic_mm")->supports(TransportKind::Sim));
  CHECK_FALSE(alone.find("basic_mm")->supports(TransportKind::Live));
}

TEST_CASE("modules: the archive's factories build and run engines for sim, replay and live") {
  bt::register_builtin_strategies();
  test_mm::register_strategies(StrategyRegistry::instance());

  auto cfg = bt::BacktestConfig::single_instrument("BTCUSDT", 0.01_px, 0.00001_qty);
  cfg.duration = seconds(20);
  cfg.set_seed(7);
  cfg.generator.limit_rate_per_s = 400;
  cfg.generator.market_rate_per_s = 30;
  cfg.generator.market_qty_median_lots = 1500;
  cfg.generator.mid_step_rate_per_s = 20;
  cfg.measure_wall_clock = false;
  const std::filesystem::path journal = fastmm::test::tmp_dir() / "module_test_session.fmj";
  std::filesystem::remove(journal);
  cfg.journal_out = journal.string();

  const bt::BacktestResult r = bt::run_backtest(cfg, "test_module_mm");
  CHECK(r.strategy == "test_module_mm");
  CHECK(r.outbound_messages > 0);
  CHECK(r.metrics.fills > 0);

  bt::ReplayOptions opt;
  opt.strategy = "test_module_mm";
  const bt::ReplayResult rep = bt::replay_journal(cfg.journal_out, cfg, opt);
  CHECK(rep.strategy == "test_module_mm");
  CHECK(rep.ok());
  CHECK(rep.outbound_sha256 == r.outbound_sha256);

  // Live: the factory builds the engine on a live backend (not run: no venue here).
  TscClock clock;
  LiveTransport transport;
  RingFeed feed;
  live::LiveBackend backend{&clock, &transport, &feed};
  RunnerDeps deps;
  deps.instruments = &cfg.instruments;
  deps.backend = &backend;
  const std::unique_ptr<IEngineRunner> runner =
      StrategyRegistry::instance().make("test_module_mm", TransportKind::Live, deps);
  REQUIRE(runner != nullptr);
  CHECK(runner->strategy_name() == "test_module_mm");
  deps.params = {{"no_such_param", "1"}};
  CHECK_THROWS_AS(static_cast<void>(StrategyRegistry::instance().make(
                      "test_module_mm", TransportKind::Live, deps)),
                  std::invalid_argument);
}
