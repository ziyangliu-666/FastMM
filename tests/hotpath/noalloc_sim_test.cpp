// The simulator's steady state allocates nothing (5.2 / 8.2): coupled MarketGenerator +
// MatchingEngine + MdAggregator + SimTransport + Engine<BasicMM> driven by SimDriver, and two
// venues with their own latency fed by merged recorded feeds.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/backtest/array_source.hpp"
#include "fastmm/backtest/data_source.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cstdint>
#include <memory>
#include <vector>

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

namespace {
// One venue's recorded feed as columns: a two-sided snapshot every 10 ms around a walk, a trade
// through one side every fourth snapshot.
struct Columns {
  std::vector<std::int64_t> ts;
  std::vector<std::uint8_t> type;
  std::vector<std::uint32_t> inst;
  std::vector<std::int8_t> side;
  std::vector<std::int64_t> price;
  std::vector<std::int64_t> qty;
  std::vector<std::uint64_t> seq;

  Columns(std::uint32_t id, Timestamp start, std::int64_t offset_ns, int steps) {
    Xoshiro256ss rng(id + 1);
    std::int64_t mid = 10'000;  // ticks of 0.01
    for (int k = 0; k < steps; ++k) {
      const std::int64_t t = start.ns + k * 10'000'000LL + offset_ns;
      mid += static_cast<std::int64_t>(rng.next() % 3U) - 1;
      const auto s = static_cast<std::uint64_t>(k) + 1;
      row(t, bt::RowType::Snapshot, id, Side::Buy, (mid - 1) * 1'000'000, s);
      row(t, bt::RowType::Snapshot, id, Side::Sell, (mid + 1) * 1'000'000, s);
      if (k % 4 == 3) {
        const bool sell = (rng.next() & 1U) != 0;
        row(t + 1'000'000,
            bt::RowType::Trade,
            id,
            sell ? Side::Sell : Side::Buy,
            (sell ? mid - 1 : mid + 1) * 1'000'000,
            s);
      }
    }
  }
  void row(
      std::int64_t t, bt::RowType ty, std::uint32_t id, Side sd, std::int64_t px, std::uint64_t s) {
    ts.push_back(t);
    type.push_back(static_cast<std::uint8_t>(ty));
    inst.push_back(id);
    side.push_back(static_cast<std::int8_t>(sd));
    price.push_back(px);
    qty.push_back(Qty::from_decimal("1").value().raw);
    seq.push_back(s);
  }
  [[nodiscard]] bt::ArrayColumns view() const {
    bt::ArrayColumns c;
    c.ts = ts;
    c.type = type;
    c.inst = inst;
    c.side = side;
    c.price_i64 = price;
    c.qty_i64 = qty;
    c.seq = seq;
    return c;
  }
};
}  // namespace

TEST_CASE("hotpath.noalloc: two-venue sim run on merged recorded feeds (L2 queue)") {
  InstrumentTable table;
  for (std::uint8_t v = 0; v < 2; ++v) {
    Instrument inst{};
    inst.symbol = v == 0 ? "AAA" : "BBB";
    inst.venue = VenueId{v};
    inst.flags = Instrument::kEnabled;
    inst.tick = Price::from_decimal("0.01").value();
    inst.lot = Qty::from_decimal("0.001").value();
    inst.min_qty = inst.lot;
    REQUIRE(table.add(inst));
  }
  const Timestamp start{seconds(1'700'000'000).ns};
  SimClock clock(start);
  SimTransportConfig tc;
  tc.seed = 9;
  tc.fill_model = FillModel::L2Queue;
  tc.stp = StpMode::CancelTaker;
  SimVenueConfig slow = tc.venue_config(VenueId{1});
  slow.order_out = slow.ack_in = LatencyParams{microseconds(2500), microseconds(300)};
  slow.md_in = LatencyParams{microseconds(1000), microseconds(100)};
  tc.venues.push_back(slow);
  auto transport = std::make_unique<SimTransport>(clock, table, tc);
  REQUIRE(transport->venue_count() == 2);
  auto feed = std::make_unique<InlineFeed>(1U << 22);
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "0"},
                                    {"skew_bps_per_unit", "0"},
                                    {"quote_qty", "0.01"},
                                    {"max_inventory", "0.5"},
                                    {"pull_on_stale_ms", "0"}}));
  EngineConfig ec;
  ec.risk.max_position = Qty::from_decimal("1").value();
  ec.risk.max_order_qty = Qty::from_decimal("1").value();
  using E = Engine<BasicMM, SimClock, SimTransport, InlineFeed>;
  auto engine = std::make_unique<E>(ec, table, clock, *transport, *feed, strategy);
  const Columns a(0, start, 0, 1200);
  const Columns b(1, start, 3'000'000, 1200);
  auto src_a = std::make_unique<bt::ArraySource>(a.view(), VenueId{0});
  auto src_b = std::make_unique<bt::ArraySource>(b.view(), VenueId{1});
  auto merged =
      std::make_unique<bt::MergedSource>(std::vector<MdSource*>{src_a.get(), src_b.get()});
  SimDriver driver(clock, *transport, *feed, EngineHooks::for_engine(*engine));
  driver.set_source(merged.get());

  // Warm: pools, rings, logger, pages, and our quantity in the feed (OwnQuantity keeps 5 s).
  REQUIRE(driver.run_until(start + seconds(6)));
  const std::uint64_t orders_before = engine->stats().orders_sent;
  const std::uint64_t fills_before = engine->stats().fills;
  {
    NoAllocScope guard;
    REQUIRE(driver.run_until(start + seconds(11)));
    CHECK(guard.allocations_so_far() == 0);
  }
  CHECK(engine->stats().orders_sent > orders_before);
  CHECK(engine->stats().fills > fills_before);
  CHECK(driver.stats().md_delivered > 1000);
}
