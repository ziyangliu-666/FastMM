// A backtest with a pool treasury ([venues.<primary>.treasury]): the session runs it on simulated
// time against the pool's simulated accounts, the transfers land after [backtest]
// transfer_latency_ms, the run stays deterministic and the result reports what moved. Without a
// treasury the result is what it was.
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

// basic_mm on a two-account pool: the primary holds all the quote, the member the base.
BacktestConfig pool_config(bool treasury) {
  BacktestConfig cfg = synthetic_config(11, seconds(30));
  Instrument i = cfg.instruments.get(InstrumentId{0});
  i.base = "BTC";
  i.quote = "USDT";
  InstrumentTable t;
  REQUIRE(t.add(i));
  cfg.instruments = t;
  cfg.strategy = "basic_mm";
  REQUIRE(cfg.transport.pools.add(VenueId{1}, VenueId{0}));
  cfg.engine.pools = cfg.transport.pools;
  cfg.transport.accounts = {account(VenueId{0}, "0", "100000"), account(VenueId{1}, "1", "0")};
  cfg.engine.risk.max_position = qt("0.5");
  if (treasury) {
    TreasuryConfig c;
    c.enabled = true;
    c.asset = "USDT";
    c.members = cfg.transport.pools.members(VenueId{0});
    c.names[0] = "main";
    c.names[1] = "second";
    c.interval_ns = seconds(1).ns;
    c.min_interval_ns = seconds(5).ns;
    cfg.treasuries.push_back(c);
    cfg.transfer_latency = milliseconds(500);
  }
  return cfg;
}

// `member_usdt`: the member's USDT at the end.
BacktestResult run(const BacktestConfig& cfg, Notional* member_usdt = nullptr) {
  BacktestSession session(cfg, nullptr, &BasicMM::schema());
  std::unique_ptr<IEngineRunner> runner = session.backend().make_runner<BasicMM>(session.deps());
  BacktestResult r = session.run(session.backend().hooks, runner.get(), "basic_mm");
  if (member_usdt != nullptr)
    *member_usdt = session.backend().transport.accounts()->total(VenueId{1}, "USDT");
  return r;
}

}  // namespace

TEST_CASE("backtest.treasury: the pool's quote asset moves to the member on simulated time") {
  const BacktestConfig cfg = pool_config(true);
  Notional member_usdt;
  const BacktestResult r = run(cfg, &member_usdt);
  CHECK(r.treasury.pools == 1);
  REQUIRE(r.treasury.done >= 1);
  CHECK(r.treasury.failed == 0);
  REQUIRE_FALSE(r.treasury.transfers.empty());
  const sim::SimTransferRecord& first = r.treasury.transfers.front();
  CHECK(first.from == VenueId{0});
  CHECK(first.to == VenueId{1});
  CHECK(first.asset == "USDT");
  // Sent at the first look (the start) and carried out transfer_latency_ms later.
  CHECK(first.ts.ns == r.start_ts + milliseconds(500).ns);
  CHECK(r.treasury.moved("USDT") >= nt("40000").raw);
  CHECK(member_usdt > nt("0"));
  // In the report.
  CHECK(r.summary_json().find("\"treasury\": {\"pools\": 1, ") != std::string::npos);
  CHECK(r.summary_table().find("treasury sent / done / failed") != std::string::npos);
  CHECK(r.transfers_csv().find("ts_ns,from,to,asset,amount,state,client_id\n") == 0);
  CHECK(r.transfers_csv().find(",0,1,USDT,") != std::string::npos);

  // Deterministic: the same run sends the same orders and the same transfers.
  const BacktestResult again = run(cfg);
  CHECK(again.outbound_sha256 == r.outbound_sha256);
  REQUIRE(again.treasury.transfers.size() == r.treasury.transfers.size());
  for (std::size_t k = 0; k < r.treasury.transfers.size(); ++k) {
    CHECK(again.treasury.transfers[k].ts == r.treasury.transfers[k].ts);
    CHECK(again.treasury.transfers[k].amount == r.treasury.transfers[k].amount);
  }
}

TEST_CASE("backtest.treasury: without a treasury the run and its report are unchanged") {
  const BacktestResult r = run(pool_config(false));
  CHECK(r.treasury.pools == 0);
  CHECK(r.treasury.transfers.empty());
  CHECK(r.summary_json().find("treasury") == std::string::npos);
  CHECK(r.summary_table().find("treasury") == std::string::npos);
}

TEST_CASE("backtest.treasury: from_config takes the enabled treasuries and the transfer latency") {
  const std::string base = R"([venues.sim]
kind = "sim"
[venues.sim_b]
kind = "sim"
pool_of = "sim"
[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
tick = "0.01"
lot = "0.00001"
[strategy]
name = "basic_mm"
[venues.sim.treasury]
enabled = true
asset = "USDT"
state_file = "never-written.treasury"
)";
  const BacktestConfig c =
      BacktestConfig::from_config(Config::parse(base + "[backtest]\ntransfer_latency_ms = 250\n"));
  REQUIRE(c.treasuries.size() == 1);
  CHECK(c.treasuries[0].asset == "USDT");
  CHECK(c.treasuries[0].members.size() == 2);
  CHECK(c.treasuries[0].state_file.empty());  // nothing is written in a backtest
  CHECK(c.transfer_latency == milliseconds(250));
  CHECK(BacktestConfig::from_config(Config::parse(base)).transfer_latency == Duration{});
  CHECK_THROWS_AS(static_cast<void>(BacktestConfig::from_config(
                      Config::parse(base + "[backtest]\ntransfer_latency_ms = -1\n"))),
                  ConfigError);
}
