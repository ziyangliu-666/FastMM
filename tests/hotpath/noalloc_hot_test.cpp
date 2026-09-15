// The hot-hook adapter allocates nothing per event (ADR-0013, section 1): Engine<HotStrategy>
// driven by SimDriver with book, fill and timer hooks written in C against hot_abi.h.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/hot_abi.h"
#include "fastmm/strategies/hot_strategy.hpp"

#include <cstdint>
#include <cstring>
#include <memory>

using namespace fastmm;
using namespace fastmm::sim;
using fastmm::test::NoAllocScope;

namespace {

// Float quotes at mid (converted and rounded by the adapter), with keep_passive; counts calls in
// the State field after the 8-byte parameter block.
std::int32_t quote_mid(std::uint8_t* self, fastmm_hot_ctx* c, fastmm_hot_book* b) {
  std::int64_t n = 0;
  std::memcpy(&n, self + 8, sizeof n);
  ++n;
  std::memcpy(self + 8, &n, sizeof n);
  if (b->valid == 0) {
    c->action = FASTMM_HOT_ACTION_PULL;
    return FASTMM_HOT_OK;
  }
  double qty = 0.0;
  std::memcpy(&qty, self, sizeof qty);
  c->bid_px[0] = b->mid;
  c->bid_qty[0] = qty;
  c->ask_px[0] = b->mid;
  c->ask_qty[0] = qty;
  c->n_bids = c->n_asks = 1;
  c->flags = FASTMM_HOT_FLAG_UNCROSS | FASTMM_HOT_FLAG_KEEP_PASSIVE;
  c->action = FASTMM_HOT_ACTION_QUOTE;
  return FASTMM_HOT_OK;
}

// Stale check: pull when the book is older than 50 ms.
std::int32_t pull_stale(std::uint8_t*, fastmm_hot_ctx* c, fastmm_hot_book* b) {
  if (b->ts_ns != 0 && c->now_ns - b->ts_ns > 50'000'000) c->action = FASTMM_HOT_ACTION_PULL;
  return FASTMM_HOT_OK;
}

}  // namespace

TEST_CASE("hotpath.noalloc: hot hooks through HotStrategy in a coupled sim run") {
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

  HotProgram program;
  program.hooks[static_cast<std::size_t>(HotHook::Book)] = &quote_mid;
  program.hooks[static_cast<std::size_t>(HotHook::Fill)] = &quote_mid;
  program.hooks[static_cast<std::size_t>(HotHook::Quoting)] = &quote_mid;
  program.timers[0] = {&pull_stale, milliseconds(100)};
  program.n_timers = 1;
  program.record.assign(16, 0);
  const double qty = 0.002;
  std::memcpy(program.record.data(), &qty, sizeof qty);
  program.param_bytes = 8;
  HotStrategy strategy;
  REQUIRE(strategy.attach(program, table));
  strategy.warm_up(table);

  EngineConfig ec;
  ec.risk.max_position = Qty::from_decimal("0.05").value();
  using E = Engine<HotStrategy, SimClock, SimTransport, InlineFeed>;
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
  const std::uint64_t calls_before = strategy.calls();
  {
    NoAllocScope guard;
    REQUIRE(driver.run_until(start + seconds(10)));
    CHECK(guard.allocations_so_far() == 0);
  }
  CHECK_FALSE(strategy.failed());
  CHECK(strategy.calls() > calls_before + 100);
  CHECK(engine->stats().orders_sent > orders_before);
  CHECK(engine->stats().fills > fills_before);
  CHECK(engine->stats().timers_fired > 0);
}
