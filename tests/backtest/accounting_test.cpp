// [accounting] in the backtester and replay: a run with a USDT and a BTC-settled instrument reports
// its totals in USDT, at the mid of BTCUSDT, and replays to the same orders.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/pnl.hpp"
#include "fastmm/backtest/replay.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/config/config.hpp"

#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// BTCUSDT (instrument 0, USDT) and ETHBTC (instrument 1, BTC: same tick, lot and prices, so the
// synthetic tape serves both), converted to USDT at BTCUSDT's mid.
BacktestConfig two_currency_config(bool accounting) {
  BacktestConfig c = synthetic_config(31, seconds(8));
  c.transport.fill_model = sim::FillModel::L2Queue;
  c.transport.queue_conservatism_bps = 5000;
  Instrument& btc = c.instruments.get(InstrumentId{0});
  btc.base = "BTC";
  btc.quote = "USDT";
  Instrument eth = btc;
  eth.symbol = "ETHBTC";
  eth.base = "ETH";
  eth.quote = "BTC";
  REQUIRE(c.instruments.add(eth));
  c.venue_names = {"sim"};
  if (accounting) {
    c.accounting.reporting_currency = "USDT";
    c.accounting.fx["BTC"] = "sim:BTCUSDT";
  }
  c.engine.fx = c.fx_plan(c.instruments);
  return c;
}

// The synthetic tape of instrument 0, and the same rows for instrument 1.
std::string tape(const BacktestConfig& cfg) {
  SyntheticSourceConfig sc;
  sc.generator = cfg.generator;
  sc.seed = 31;
  sc.duration = seconds(8);
  SyntheticSource synth(sc);
  return source_to_csv(synth);
}
std::string as_instrument_1(const std::string& text) {
  std::istringstream in(text);
  std::string out;
  for (std::string line; std::getline(in, line);) {
    const std::size_t a = line.find(',');
    const std::size_t b = a == std::string::npos ? a : line.find(',', a + 1);
    const std::size_t c = b == std::string::npos ? b : line.find(',', b + 1);
    if (c != std::string::npos && line.substr(b + 1, c - b - 1) == "0")
      line = line.substr(0, b + 1) + "1" + line.substr(c);
    out += line + "\n";
  }
  return out;
}

BacktestResult run(const BacktestConfig& cfg, const std::string& text) {
  CsvSource a = CsvSource::from_text(text);
  CsvSource b = CsvSource::from_text(as_instrument_1(text));
  MergedSource m({&a, &b});
  return run_backtest(cfg, "basic_mm", &m);
}

}  // namespace

TEST_CASE("backtest.accounting: a run over USDT and BTC instruments reports its totals in USDT") {
  const BacktestConfig cfg = two_currency_config(true);
  REQUIRE(cfg.engine.fx.active());
  const std::string text = tape(cfg);
  const BacktestResult r = run(cfg, text);
  // Fees per currency from the venue's fills.
  Notional usdt{};
  Notional btc{};
  std::size_t eth_fills = 0;
  for (std::size_t i = 0; i < r.fills.size(); ++i) {
    const Notional fee = Notional::from_raw(r.fills.fee[i]);
    if (r.fills.instrument[i] == 1) {
      btc += fee;
      ++eth_fills;
    } else {
      usdt += fee;
    }
  }
  REQUIRE(eth_fills > 0);
  REQUIRE(eth_fills < r.fills.size());
  REQUIRE_FALSE(btc.is_zero());
  // The ledger's last bar: USDT as it is, BTC at that bar's BTCUSDT mid.
  const FxRate rate = FxRate::from_mid(Price::from_raw(r.equity.mid.back()), false);
  CHECK(r.equity.fees.back() == (usdt + convert(btc, rate)).raw);
  // The engine converts at its own books' mid: the same up to the last tick's move.
  const double engine_fees = Notional::from_raw(r.engine.fees_raw).to_double();
  const double ledger_fees = Notional::from_raw(r.equity.fees.back()).to_double();
  CHECK(std::fabs(engine_fees - ledger_fees) <= std::fabs(ledger_fees) * 0.01 + 1e-6);

  // Without [accounting] the same run adds BTC to USDT.
  const BacktestResult raw = run(two_currency_config(false), text);
  CHECK(raw.outbound_sha256 == r.outbound_sha256);  // nothing here limits on the totals
  CHECK(raw.equity.fees.back() == (usdt + btc).raw);
}

TEST_CASE("backtest.accounting: from_config builds the plan and refuses an uncovered one") {
  const std::string base = R"([venues.sim]
kind = "sim"
[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
tick = "0.01"
lot = "0.00001"
[[instruments]]
venue = "sim"
symbol = "ETHBTC"
base = "ETH"
quote = "BTC"
tick = "0.00001"
lot = "0.001"
[strategy]
name = "basic_mm"
[accounting]
reporting_currency = "USDT"
)";
  const BacktestConfig ok =
      BacktestConfig::from_config(Config::parse(base + "[accounting.fx]\nBTC = \"sim:BTCUSDT\"\n"));
  CHECK(ok.engine.fx.active());
  CHECK(ok.engine.fx.ccy[1] == 1);
  // No source: a warning without limits, an error with one.
  const BacktestConfig warned = BacktestConfig::from_config(Config::parse(base));
  CHECK_FALSE(warned.engine.fx.active());
  bool found = false;
  for (const std::string& w : warned.warnings)
    found = found || w.find("no source for BTC") != std::string::npos;
  CHECK(found);
  CHECK_THROWS_WITH_AS(static_cast<void>(BacktestConfig::from_config(
                           Config::parse(base + "[risk]\nmax_loss = \"100\"\n"))),
                       doctest::Contains("ETHBTC settles in BTC"),
                       ConfigError);
}

TEST_CASE("backtest.accounting: a recording with [accounting] replays to the same orders") {
  // The cap decides orders here: an ETHBTC quote is 120 BTC, some 7 million USDT, against 1000;
  // as a BTC number it would pass.
  BacktestConfig cfg = two_currency_config(true);
  cfg.engine.risk.max_gross_notional = Notional::from_int(1000);
  const std::string path = (fastmm::test::tmp_dir() / "accounting_replay.fmj").string();
  std::filesystem::remove(path);
  cfg.journal_out = path;
  cfg.strategy = "basic_mm";
  const BacktestResult rec = run(cfg, tape(cfg));
  CHECK(rec.engine.risk_rejects_by_reason[RejectReason::MaxGrossNotional] > 0);
  BacktestConfig rp = cfg;
  rp.journal_out.clear();
  rp.engine.fx = FxPlan{};  // replay builds it again from the journal's table
  const ReplayResult res = replay_journal(path, rp);
  CHECK(res.first_mismatch == -1);
  CHECK(res.outbound_sha256 == rec.outbound_sha256);
  CHECK(res.ok());
  // Without [accounting] the same journal does not replay: the recording converted.
  BacktestConfig none = rp;
  none.accounting = AccountingSpec{};
  CHECK_FALSE(replay_journal(path, none).ok());
}
