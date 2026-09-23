// End-to-end: Engine<BasicMM, SimClock, SimTransport, InlineFeed> driven by SimDriver on a
// coupled MarketGenerator (real matches) and on recorded data (L2 queue / mirror models).
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/sim/md_aggregator.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.00001");
  i.min_qty = qt("0.00001");
  i.min_notional = Notional::from_decimal("5").value();
  REQUIRE(t.add(i));
  return t;
}

MarketGeneratorParams gen_params() {
  MarketGeneratorParams p;
  p.start_mid = px("60000");
  p.tick = px("0.01");
  p.lot = qt("0.00001");
  p.limit_rate_per_s = 300;
  p.market_rate_per_s = 40;
  p.mid_step_rate_per_s = 5;
  p.market_qty_median_lots = 300;
  return p;
}

EngineConfig engine_config() {
  EngineConfig cfg;
  cfg.rng_seed = 1;
  cfg.max_events_per_step = 64;
  cfg.risk.max_order_qty = qt("0.01");
  cfg.risk.max_position = qt("0.05");
  cfg.risk.max_open_orders = 8;
  cfg.quotes.min_requote_interval = milliseconds(50);
  cfg.quotes.post_only = true;
  return cfg;
}

struct FillCollector final : SimObserver {
  Qty bought{}, sold{};
  Notional fees{};
  std::uint64_t fills = 0;
  std::uint64_t makers = 0;
  void on_fill(const OrderFillMsg& f, Timestamp, const FillContext&) override {
    ++fills;
    if (f.liquidity == Liquidity::Maker) ++makers;
    fees += f.fee;
    (f.side == Side::Buy ? bought : sold) += f.qty;
  }
};

struct RunResult {
  std::string hash;
  std::uint64_t orders = 0;
  std::uint64_t fills = 0;
  std::int64_t realized = 0;
  std::int64_t position = 0;
};

// One coupled-generator run; returns the observable outcome.
RunResult run_coupled(std::uint64_t seed, Duration horizon, bool check = true) {
  const InstrumentTable table = make_table();
  const Timestamp start{seconds(1'700'000'000).ns};
  SimClock clock(start);
  SimTransportConfig tc;
  tc.seed = seed;
  tc.fees = FeeModel::from_bps(-0.5, 3.0);
  tc.stp = StpMode::CancelTaker;
  SimTransport transport(clock, table, tc);
  FillCollector fills;
  transport.set_observer(&fills);
  InlineFeed feed(1 << 22);
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "0.003"},
                                    {"quote_qty", "0.002"},
                                    {"max_inventory", "0.02"},
                                    {"requote_threshold_ticks", "1"},
                                    {"pull_on_stale_ms", "0"},
                                    {"levels", "1"}}));
  using E = Engine<BasicMM, SimClock, SimTransport, InlineFeed>;
  auto engine = std::make_unique<E>(engine_config(), table, clock, transport, feed, strategy);
  MarketGenerator gen(gen_params(), seed, InstrumentId{0}, start, start + horizon);
  SimDriver driver(clock, transport, feed, EngineHooks::for_engine(*engine));
  driver.set_generator(&gen, 20);
  driver.run_all();
  driver.finish();

  RunResult r;
  r.hash = transport.outbound_hash().hex();
  r.orders = engine->stats().orders_sent;
  r.fills = engine->stats().fills;
  r.realized = engine->position(InstrumentId{0}).realized.raw;
  r.position = engine->position(InstrumentId{0}).qty.raw;
  if (check) {
    const MatchingEngine& me = transport.matching_engine();
    const AccountLedger& led = me.ledger(kStrategyAccount);
    CAPTURE(seed);
    CHECK(engine->stats().book_updates > 50);
    CHECK(engine->stats().orders_sent > 10);
    CHECK(engine->stats().fills > 0);
    CHECK(transport.stats().wire_full == 0);
    CHECK(transport.stats().scheduler_full == 0);
    CHECK(engine->stats().transport_full == 0);
    CHECK(engine->stats().journal_overflows == 0);
    CHECK(engine->stats().kills == 0);
    // engine position == venue ledger for account 1 == observer view
    CHECK(engine->position(InstrumentId{0}).qty == led.net());
    CHECK(engine->position(InstrumentId{0}).gross_traded == led.bought + led.sold);
    CHECK(engine->position(InstrumentId{0}).fills == led.fills);
    CHECK(fills.bought == led.bought);
    CHECK(fills.sold == led.sold);
    CHECK(fills.fills == led.fills);
    CHECK(fills.makers == fills.fills);  // post-only quotes only ever make
    CHECK(engine->position(InstrumentId{0}).fees == fills.fees);
    CHECK(engine->position(InstrumentId{0}).qty.abs() <= qt("0.02"));
    // the generator's own ledger mirrors ours
    CHECK(me.ledger(kGeneratorAccount).bought >= led.sold);
    CHECK(gen.stats().markets > 0);
    CHECK(driver.stats().md_delivered > 0);
    CHECK(driver.stats().order_events_delivered >= engine->stats().orders_sent);
    CHECK(engine->oms().open_count() <= 4);
  }
  return r;
}
}  // namespace

TEST_CASE("sim.run: BasicMM on a coupled generator - fills reconcile with the venue ledger") {
  const RunResult r = run_coupled(42, seconds(20));
  CHECK(r.fills > 0);
}

TEST_CASE("sim.run: two runs with the same seed are identical, a different seed is not") {
  const RunResult a = run_coupled(7, seconds(10), false);
  const RunResult b = run_coupled(7, seconds(10), false);
  const RunResult c = run_coupled(8, seconds(10), false);
  CHECK(a.hash == b.hash);
  CHECK(a.orders == b.orders);
  CHECK(a.fills == b.fills);
  CHECK(a.realized == b.realized);
  CHECK(a.position == b.position);
  CHECK(a.hash != c.hash);
}

namespace {
// Records the market-data stream of a generator-only run (no strategy) as raw messages.
struct Recording final : MatchingSink {
  std::vector<std::vector<std::byte>> msgs;
  MdAggregator* agg = nullptr;
  void on_book_change(InstrumentId id, Side s, Price p, Qty q, std::uint64_t u) override {
    agg->on_book_change(id, s, p, q, u);
  }
  void on_trade(
      InstrumentId id, Price p, Qty q, Side aggr, std::uint64_t tid, Timestamp ts) override {
    TradeMsg t{};
    init_header(t, EventType::Trade, id, VenueId{0});
    t.price = p;
    t.qty = q;
    t.trade_id = tid;
    t.aggressor = aggr;
    t.hdr.exch_ts = t.hdr.recv_ts = ts;
    push(t.hdr);
  }
  static void emit(void* ctx, EventHeader& h, Timestamp ts) noexcept {
    h.exch_ts = h.recv_ts = ts;
    static_cast<Recording*>(ctx)->push(h);
  }
  void push(const EventHeader& h) {
    const auto* b = reinterpret_cast<const std::byte*>(&h);
    msgs.emplace_back(b, b + h.len);
  }
};

std::vector<std::vector<std::byte>> record_market(std::uint64_t seed,
                                                  Timestamp start,
                                                  Duration horizon) {
  Recording rec;
  MatchingEngine me(1, &rec);
  MdAggregatorConfig ac;
  MdAggregator agg(1, me, ac, start);
  rec.agg = &agg;
  MarketGenerator gen(gen_params(), seed, InstrumentId{0}, start, start + horizon);
  gen.seed_book(me, 20, start);
  agg.flush(start, &Recording::emit, &rec);
  for (;;) {
    const Timestamp tg = gen.next_ts();
    const Timestamp tf = agg.next_flush_ts();
    if (tg == Timestamp::max() && tf > start + horizon) break;
    if (tg <= tf) {
      gen.step(me);
    } else {
      agg.flush(tf, &Recording::emit, &rec);
    }
  }
  return rec.msgs;
}

struct VectorSource final : MdSource {
  const std::vector<std::vector<std::byte>>* msgs;
  std::size_t i = 0;
  explicit VectorSource(const std::vector<std::vector<std::byte>>& m) : msgs(&m) {}
  const EventHeader* next() override {
    if (i >= msgs->size()) return nullptr;
    return reinterpret_cast<const EventHeader*>((*msgs)[i++].data());
  }
  void reset() override { i = 0; }
};

RunResult run_on_source(const std::vector<std::vector<std::byte>>& msgs, FillModel model) {
  const InstrumentTable table = make_table();
  const Timestamp start{seconds(1'700'000'000).ns};
  SimClock clock(start);
  SimTransportConfig tc;
  tc.seed = 3;
  tc.fill_model = model;
  tc.fees = FeeModel::from_bps(1.0, 4.0);
  tc.queue_conservatism_bps = 5000;
  SimTransport transport(clock, table, tc);
  FillCollector fills;
  transport.set_observer(&fills);
  InlineFeed feed(1 << 22);
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure({{"half_spread_bps", "0.003"},
                                    {"quote_qty", "0.002"},
                                    {"max_inventory", "0.02"},
                                    {"pull_on_stale_ms", "0"}}));
  using E = Engine<BasicMM, SimClock, SimTransport, InlineFeed>;
  auto engine = std::make_unique<E>(engine_config(), table, clock, transport, feed, strategy);
  VectorSource src(msgs);
  SimDriver driver(clock, transport, feed, EngineHooks::for_engine(*engine));
  driver.set_source(&src);
  driver.run_all();
  driver.finish();
  CHECK(driver.stats().source_events == msgs.size());
  CHECK(transport.stats().md_forwarded == msgs.size());
  CHECK(engine->stats().book_updates > 10);
  CHECK(engine->stats().orders_sent > 0);
  CHECK(transport.stats().wire_full == 0);
  CHECK(engine->stats().kills == 0);
  CHECK(engine->position(InstrumentId{0}).qty == fills.bought - fills.sold);
  CHECK(engine->position(InstrumentId{0}).fills == fills.fills);
  CHECK(engine->position(InstrumentId{0}).qty.abs() <= qt("0.02"));
  if (model == FillModel::L2Queue) CHECK(transport.queue().size() <= 2);
  RunResult r;
  r.hash = transport.outbound_hash().hex();
  r.orders = engine->stats().orders_sent;
  r.fills = engine->stats().fills;
  r.position = engine->position(InstrumentId{0}).qty.raw;
  return r;
}
}  // namespace

TEST_CASE("sim.run: recorded market data through the L2 queue model and the mirror model") {
  const Timestamp start{seconds(1'700'000'000).ns};
  const auto msgs = record_market(11, start, seconds(15));
  REQUIRE(msgs.size() > 100);
  const RunResult q1 = run_on_source(msgs, FillModel::L2Queue);
  const RunResult q2 = run_on_source(msgs, FillModel::L2Queue);
  CHECK(q1.hash == q2.hash);
  CHECK(q1.fills > 0);
  const RunResult m1 = run_on_source(msgs, FillModel::Matching);
  const RunResult m2 = run_on_source(msgs, FillModel::Matching);
  CHECK(m1.hash == m2.hash);
  CHECK(m1.orders > 0);
}
