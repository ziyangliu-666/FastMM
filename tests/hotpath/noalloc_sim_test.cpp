// The simulator's steady state allocates nothing (5.2 / 8.2): coupled MarketGenerator +
// MatchingEngine + MdAggregator + SimTransport + Engine<BasicMM> driven by SimDriver.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <memory>

using namespace fastmm;
using namespace fastmm::sim;
using fastmm::test::NoAllocScope;

TEST_CASE("hotpath.noalloc: coupled sim run (generator, matching, transport, engine)") {
  InstrumentTable table;
  Instrument inst{};
  inst.symbol = "BTCUSDT";
  inst.flags = Instrument::kEnabled;
  inst.tick = Price::from_decimal("0.01").value();
  inst.lot = Qty::from_decimal("0.00001").value();
  inst.min_qty = inst.lot;
  REQUIRE(table.add(inst));

  const Timestamp start{seconds(1'700'000'000).ns};
  SimClock clock(start);
  SimTransportConfig tc;
  tc.seed = 9;
  tc.stp = StpMode::CancelTaker;
  tc.fees = FeeModel::from_bps(-0.5, 3.0);
  auto transport = std::make_unique<SimTransport>(clock, table, tc);
  auto feed = std::make_unique<InlineFeed>(1U << 22);
  BasicMM strategy;
  // Quote at the rounded mid so fills keep the fill path inside the measured window. (This was
  // "0.003", which the centi-bps BasicMM rounded to 0; Ratio parameters keep four decimals.)
  REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "0"},
                                    {"skew_bps_per_unit", "0"},
                                    {"quote_qty", "0.002"},
                                    {"max_inventory", "0.02"},
                                    {"pull_on_stale_ms", "50"}}));
  EngineConfig ec;
  ec.risk.max_position = Qty::from_decimal("0.05").value();
  using E = Engine<BasicMM, SimClock, SimTransport, InlineFeed>;
  auto engine = std::make_unique<E>(ec, table, clock, *transport, *feed, strategy);
  MarketGeneratorParams gp;
  gp.limit_rate_per_s = 300;
  gp.market_rate_per_s = 40;
  gp.mid_step_rate_per_s = 5;
  auto gen = std::make_unique<MarketGenerator>(gp, 9, InstrumentId{0}, start, start + seconds(12));
  SimDriver driver(clock, *transport, *feed, EngineHooks::for_engine(*engine));
  driver.set_generator(gen.get(), 20);

  REQUIRE(driver.run_until(start + seconds(2)));  // warm: pools, rings, logger, pages
  const std::uint64_t orders_before = engine->stats().orders_sent;
  const std::uint64_t fills_before = engine->stats().fills;
  {
    NoAllocScope guard;
    REQUIRE(driver.run_until(start + seconds(10)));
    CHECK(guard.allocations_so_far() == 0);
  }
  CHECK(engine->stats().orders_sent > orders_before);
  CHECK(engine->stats().fills > fills_before);
  CHECK(engine->stats().timers_fired > 0);
  CHECK(driver.stats().md_delivered > 100);
}
