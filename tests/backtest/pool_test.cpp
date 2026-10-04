// A backtest through an account pool ([venues.<member>] pool_of): the member is a simulated venue
// of its own with its own account, basic_mm's buys land on the account that holds the quote and
// its sells on the one that holds the base, and both accounts show the fills.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

sim::SimAccountConfig account(VenueId v, const char* btc, const char* usdt) {
  return sim::SimAccountConfig{v,
                               {sim::SimBalance{FixedString<8>("BTC"), nt(btc)},
                                sim::SimBalance{FixedString<8>("USDT"), nt(usdt)}}};
}

}  // namespace

TEST_CASE("backtest.pool: a two-member pool fills on both accounts") {
  BacktestConfig cfg = synthetic_config(11, seconds(30));
  Instrument i = cfg.instruments.get(InstrumentId{0});
  i.base = "BTC";
  i.quote = "USDT";
  InstrumentTable t;
  REQUIRE(t.add(i));
  cfg.instruments = t;
  cfg.strategy = "basic_mm";
  // Venue 1 is a member of venue 0's pool: the primary holds the quote, the member the base.
  REQUIRE(cfg.transport.pools.add(VenueId{1}, VenueId{0}));
  cfg.engine.pools = cfg.transport.pools;
  cfg.transport.accounts = {account(VenueId{0}, "0", "1000000"), account(VenueId{1}, "1", "0")};
  cfg.engine.risk.max_position = qt("0.5");

  BacktestSession session(cfg, nullptr, &BasicMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<BasicMM>(session.deps());
  const BacktestResult r = session.run(session.backend().hooks, runner.get(), "basic_mm");
  CHECK(r.metrics.orders > 10);
  REQUIRE(r.metrics.fills > 0);
  std::size_t buys = 0;
  std::size_t sells = 0;
  for (std::size_t k = 0; k < r.fills.size(); ++k) {
    (r.fills.side[k] == static_cast<std::int8_t>(Side::Buy) ? buys : sells) += 1;
  }
  CHECK(buys > 0);
  CHECK(sells > 0);
  // The buys bought on the primary (its BTC grew from nothing), the sells sold on the member (its
  // BTC shrank, its USDT grew from nothing); neither account traded the other's side.
  const sim::SimAccounts& acct = *session.backend().transport.accounts();
  CHECK(acct.total(VenueId{0}, "BTC") > nt("0"));
  CHECK(acct.total(VenueId{0}, "USDT") < nt("1000000"));
  CHECK(acct.total(VenueId{1}, "BTC") < nt("1"));
  CHECK(acct.total(VenueId{1}, "USDT") > nt("0"));
  CHECK(r.transport.rejects_balance == 0);
  // The engine's position is the sum of what the two accounts did.
  const Notional bought = acct.total(VenueId{0}, "BTC");
  const Notional sold = Notional::from_raw(nt("1").raw - acct.total(VenueId{1}, "BTC").raw);
  CHECK(r.metrics.final_position ==
        doctest::Approx(Notional::from_raw(bought.raw - sold.raw).to_double()));
}
