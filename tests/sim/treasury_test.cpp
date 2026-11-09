// A pool treasury (core/treasury.hpp) in the simulator: SimTreasuryPort moves the asset between the
// simulated accounts of a pool (SimAccounts::transfer), the engine learns the new balances from
// each account's link, and the treasury, stepped from a strategy hook, stops once they show the
// pool balanced.
#include "fastmm/core/treasury.hpp"

#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/recorded_session.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/sim/sim_treasury.hpp"

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

TEST_CASE("sim.treasury: an account receives an asset it has no row of") {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1'700'000'000).ns}};
  SimTransportConfig tc;
  REQUIRE(tc.pools.add(kMember, kPrimary));
  tc.accounts = {SimAccountConfig{kPrimary, {SimBalance{FixedString<8>("BNB"), nt("5")}}},
                 SimAccountConfig{kMember, {}}};
  SimTransport transport(clock, table, tc);
  CHECK(transport.transfer(kPrimary, kMember, "BNB", nt("2"), clock.now()) == TransferState::Done);
  CHECK(transport.accounts()->total(kPrimary, "BNB") == nt("3"));
  CHECK(transport.accounts()->total(kMember, "BNB") == nt("2"));
  CHECK(transport.accounts()->free(kMember, "BNB") == nt("2"));
  // ... and sends it on: the row and the next transfer are like any other.
  CHECK(transport.transfer(kMember, kPrimary, "BNB", nt("2"), clock.now()) == TransferState::Done);
  CHECK(transport.accounts()->total(kMember, "BNB") == nt("0"));
}

TEST_CASE("sim.treasury: the simulated venue refuses transfers and loses answers as told") {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1'700'000'000).ns}};
  SimTransportConfig tc;
  REQUIRE(tc.pools.add(kMember, kPrimary));
  tc.accounts = {SimAccountConfig{kPrimary, {SimBalance{FixedString<8>("USDT"), nt("1000")}}},
                 SimAccountConfig{kMember, {SimBalance{FixedString<8>("USDT"), nt("0")}}}};
  SimTransport transport(clock, table, tc);
  SimTransferFaults faults;
  faults.p_fail = 0.4;
  faults.p_unknown = 0.4;
  faults.hang = seconds(90);
  faults.seed = 3;
  SimTreasuryPort port(transport, [&clock] { return clock.now(); }, milliseconds(200), faults);
  int failed = 0;
  int lost = 0;
  int pending = 0;
  std::vector<TransferRequest> sent;
  for (int k = 0; k < 40; ++k) {
    TransferRequest r;
    r.client_id = "t" + std::to_string(k);
    r.from = kPrimary;
    r.to = kMember;
    r.asset = "USDT";
    r.amount = nt("1");
    const TransferResult a = port.submit(r);
    failed += a.state == TransferState::Failed ? 1 : 0;
    lost += a.state == TransferState::Unknown ? 1 : 0;
    pending += a.state == TransferState::Pending ? 1 : 0;
    if (a.state != TransferState::Failed) sent.push_back(r);
  }
  CHECK(failed > 0);
  CHECK(lost > 0);
  CHECK(pending > 0);
  CHECK(port.refused() == static_cast<std::uint64_t>(failed));
  CHECK(port.lost() == static_cast<std::uint64_t>(lost));
  // A refusal moves nothing; one whose answer was lost is still Pending at the venue, and is
  // carried out only after the hang.
  clock.advance(seconds(1));
  port.settle(clock.now());
  CHECK(transport.accounts()->total(kMember, "USDT") == Notional::from_raw(nt("1").raw * pending));
  for (const TransferRequest& r : sent) {
    const TransferState s = port.status(r).state;
    CHECK((s == TransferState::Done || s == TransferState::Pending));
  }
  clock.advance(seconds(90));
  for (const TransferRequest& r : sent) CHECK(port.status(r).state == TransferState::Done);
  CHECK(transport.accounts()->total(kMember, "USDT") ==
        Notional::from_raw(nt("1").raw * (pending + lost)));
  CHECK(transport.accounts()->total(kPrimary, "USDT") ==
        Notional::from_raw(nt("1000").raw - nt("1").raw * (pending + lost)));
}

TEST_CASE(
    "sim.treasury: a recorded transfer within a simulated treasury's pool is a pair of flows") {
  TreasuryConfig t;
  t.asset = "USDT";
  PoolPlan plan;
  REQUIRE(plan.add(kMember, kPrimary));
  t.members = plan.members(kPrimary);
  const std::vector<TreasuryConfig> treasuries{t};
  const auto flow = [](VenueId v, const char* amount, std::int64_t at_s, const char* asset) {
    AccountFlowMsg f{};
    init_header(f, EventType::AccountFlow, InstrumentId{}, v);
    f.asset.assign(asset);
    f.amount = nt(amount);
    f.hdr.exch_ts = Timestamp{seconds(at_s).ns};
    return f;
  };
  const AccountFlowMsg out = flow(kPrimary, "-250", 100, "USDT");
  CHECK(pool_transfer(out, flow(kMember, "250", 101, "USDT"), treasuries));
  CHECK(pool_transfer(flow(kMember, "250", 99, "USDT"), out, treasuries));
  // Not a pair: another amount, too far apart, another asset, the same account, an account
  // outside the pool (a deposit, a move to the futures wallet).
  CHECK_FALSE(pool_transfer(out, flow(kMember, "251", 101, "USDT"), treasuries));
  CHECK_FALSE(pool_transfer(out, flow(kMember, "250", 200, "USDT"), treasuries));
  CHECK_FALSE(pool_transfer(out, flow(kMember, "250", 101, "BTC"), treasuries));
  CHECK_FALSE(pool_transfer(out, flow(kPrimary, "250", 101, "USDT"), treasuries));
  CHECK_FALSE(pool_transfer(out, flow(VenueId{4}, "250", 101, "USDT"), treasuries));
  CHECK_FALSE(pool_transfer(out, flow(kMember, "250", 101, "USDT"), {}));
}
