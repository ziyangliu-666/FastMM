// A pool treasury (core/treasury.hpp) in the simulator: SimTreasuryPort moves the asset between the
// simulated accounts of a pool (SimAccounts::transfer), the engine learns the new balances from
// each account's link, and the treasury, stepped from a strategy hook, stops once they show the
// pool balanced.
#include "fastmm/core/treasury.hpp"

#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/sim/sim_treasury.hpp"

#include <memory>
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
Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

constexpr InstrumentId kBtc{0};
constexpr VenueId kPrimary{0};
constexpr VenueId kMember{1};

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = kPrimary;
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.00001");
  i.min_qty = qt("0.00001");
  i.base = "BTC";
  i.quote = "USDT";
  REQUIRE(t.add(i));
  return t;
}

// Steps the treasury from every book update once the balances are known.
struct TreasuryProbe {
  Treasury* treasury = nullptr;
  TreasuryPort* port = nullptr;
  int steps = 0;

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId, const Book&) noexcept {
    if (!ctx.balances_live()) return;
    std::vector<TreasuryBalance> rows;
    for (const VenueId v : {kPrimary, kMember}) {
      const Balance b = ctx.balance(v, "USDT");
      TreasuryBalance r;
      r.venue = v;
      r.asset.assign("USDT");
      r.free = b.free;
      r.known = b.known;
      r.as_of_ns = b.as_of.ns;
      rows.push_back(r);
    }
    treasury->step(ctx.now().ns, rows, *port);
    ++steps;
  }
};

using TreasuryEngine = Engine<TreasuryProbe, SimClock, SimTransport, InlineFeed>;

}  // namespace

TEST_CASE("sim.treasury: the pool's quote asset is spread over its simulated accounts") {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1'700'000'000).ns}};
  SimTransportConfig tc;
  tc.seed = 7;
  tc.fees = FeeModel::from_bps(0.0, 0.0);
  REQUIRE(tc.pools.add(kMember, kPrimary));
  tc.accounts = {SimAccountConfig{kPrimary, {SimBalance{FixedString<8>("USDT"), nt("1000")}}},
                 SimAccountConfig{kMember, {SimBalance{FixedString<8>("USDT"), nt("0")}}}};
  SimTransport transport(clock, table, tc);
  // The accounts refuse what the sender does not have.
  CHECK_FALSE(transport.accounts() == nullptr);

  TreasuryConfig c;
  c.enabled = true;
  c.asset = "USDT";
  c.members = tc.pools.members(kPrimary);
  c.names[0] = "main";
  c.names[1] = "second";
  c.interval_ns = seconds(1).ns;
  c.min_interval_ns = 0;
  Treasury treasury(c);
  REQUIRE(treasury.open().has_value());
  SimTreasuryPort port(transport, [&clock] { return clock.now(); });

  InlineFeed feed{1 << 22};
  TreasuryProbe probe;
  probe.treasury = &treasury;
  probe.port = &port;
  EngineConfig cfg;
  cfg.pools = tc.pools;
  TreasuryEngine engine(cfg, table, clock, transport, feed, probe);

  MarketGeneratorParams p;
  p.start_mid = px("60000");
  p.tick = px("0.01");
  p.lot = qt("0.00001");
  p.limit_rate_per_s = 300;
  p.market_rate_per_s = 40;
  p.mid_step_rate_per_s = 5;
  p.market_qty_median_lots = 300;
  MarketGenerator gen(p, 7, kBtc, clock.now(), clock.now() + seconds(5));
  SimDriver driver(clock, transport, feed, EngineHooks::for_engine(engine));
  driver.set_generator(&gen, 20);
  driver.run_all();
  driver.finish();

  REQUIRE(probe.steps > 0);
  CHECK(port.transfers() == 1);
  CHECK(treasury.stats().done == 1);
  CHECK(treasury.stats().in_flight == 0);
  const SimAccounts& acct = *transport.accounts();
  CHECK(acct.total(kPrimary, "USDT") == nt("500"));
  CHECK(acct.total(kMember, "USDT") == nt("500"));
  // The engine heard it on each account's link.
  CHECK(engine.balance(kMember, "USDT").known);
  CHECK(engine.balance(kMember, "USDT").total == nt("500"));
  CHECK(engine.balance(kPrimary, "USDT").total == nt("500"));
}

TEST_CASE("sim.treasury: an account transfers what it has free, and only within a pool") {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1'700'000'000).ns}};
  SimTransportConfig tc;
  REQUIRE(tc.pools.add(kMember, kPrimary));
  tc.accounts = {SimAccountConfig{kPrimary, {SimBalance{FixedString<8>("USDT"), nt("100")}}},
                 SimAccountConfig{kMember, {SimBalance{FixedString<8>("USDT"), nt("0")}}}};
  SimTransport transport(clock, table, tc);
  const Timestamp t = clock.now();
  CHECK(transport.transfer(kPrimary, kMember, "USDT", nt("100.5"), t) == TransferState::Failed);
  CHECK(transport.transfer(kPrimary, kMember, "USDT", nt("0"), t) == TransferState::Failed);
  CHECK(transport.transfer(kPrimary, kPrimary, "USDT", nt("1"), t) == TransferState::Failed);
  CHECK(transport.transfer(kPrimary, kMember, "ETH", nt("1"), t) == TransferState::Failed);
  CHECK(transport.transfer(kPrimary, VenueId{5}, "USDT", nt("1"), t) == TransferState::Failed);
  CHECK(transport.transfer(kPrimary, kMember, "USDT", nt("40"), t) == TransferState::Done);
  CHECK(transport.accounts()->total(kPrimary, "USDT") == nt("60"));
  CHECK(transport.accounts()->total(kMember, "USDT") == nt("40"));
}
