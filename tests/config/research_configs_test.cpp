// configs/research/lead-mm-solfdusd.toml backtests a journal that holds all three instruments:
// the disabled leader and fx books are kept and read, orders go to the target only, and the
// quotes are pulled once the leader goes quiet.
#include "test_support.hpp"

#include "fastmm/backtest/backtest_config.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/config/config.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

using namespace fastmm;

namespace {

constexpr std::int64_t kT0 = 1'000'000'000;
constexpr std::int64_t kMs = 1'000'000;

std::string row(std::int64_t ts, char type, int inst, char side, const char* px, const char* qty) {
  return std::to_string(ts) + "," + type + "," + std::to_string(inst) + "," + side + "," + px +
         "," + qty + "," + std::to_string(ts / kMs) + "\n";
}

// Target SOLFDUSD (0) 150.10 / 150.20, leader SOLUSDT (1) 150.00 / 150.02 and fx FDUSDUSDT (2)
// 0.9990 / 0.9992 (fair 150.1451). The leader and fx tick every 100 ms until 4 s, the target
// until 6 s; at 2 s a 5 SOL sell prints at the target's 150.10 bid. With leader_ticker, a
// SOLUSDT BookTicker at 2.55 s moves the leader to 149.95 / 149.97 (fair 150.0901, under the bid)
// until the next depth update at 2.6 s, whose newer update id restores it.
std::string tape(bool leader_ticker = false) {
  std::string t = "ts_ns,type,inst,side,price,qty,seq\n";
  t += row(kT0, 'S', 0, 'B', "150.10", "1") + row(kT0, 'S', 0, 'A', "150.20", "1");
  t += row(kT0, 'S', 1, 'B', "150.00", "5") + row(kT0, 'S', 1, 'A', "150.02", "5");
  t += row(kT0, 'S', 2, 'B', "0.9990", "100000") + row(kT0, 'S', 2, 'A', "0.9992", "100000");
  for (int k = 1; k <= 60; ++k) {
    const std::int64_t ts = kT0 + k * 100 * kMs;
    const char* q = k % 2 == 0 ? "5" : "6";
    if (k <= 40) {
      t += row(ts, 'D', 1, 'B', "150.00", q);
      t += row(ts, 'D', 2, 'B', "0.9990", k % 2 == 0 ? "100000" : "100001");
    }
    t += row(ts, 'D', 0, 'A', "150.21", q);
    if (k == 20) t += row(ts + 1, 'T', 0, 'A', "150.10", "5");
    if (leader_ticker && k == 25) {
      const std::int64_t tt = ts + 50 * kMs;
      t += row(tt, 'B', 1, 'B', "149.95", "3") + row(tt, 'B', 1, 'A', "149.97", "3");
    }
  }
  return t;
}

bt::BacktestConfig research_config() {
  const Config cfg = Config::load(
      (std::filesystem::path(FASTMM_CONFIGS_DIR) / "research" / "lead-mm-solfdusd.toml").string());
  bt::BacktestConfig b = bt::BacktestConfig::from_config(cfg);
  b.output_dir.clear();
  b.journal_out.clear();
  b.measure_wall_clock = false;
  return b;
}

}  // namespace

TEST_CASE("config.research: lead_mm backtests a three-instrument journal with one enabled") {
  const bt::BacktestConfig b = research_config();
  REQUIRE(b.instruments.size() == 3);
  CHECK(b.instruments.get(InstrumentId{0}).enabled());
  CHECK_FALSE(b.instruments.get(InstrumentId{1}).enabled());
  CHECK_FALSE(b.instruments.get(InstrumentId{2}).enabled());

  bt::CsvSource csv = bt::CsvSource::from_text(tape());
  const std::string path = (fastmm::test::tmp_dir() / "lead_mm_three_instruments.fmj").string();
  bt::write_md_journal(csv, path, b.instruments, 1);
  bt::JournalSource journal(path);
  const bt::BacktestResult r = bt::run_backtest(b, "lead_mm", &journal);

  CHECK(r.engine.risk_rejects == 0);
  CHECK(r.engine.venue_rejects == 0);
  bool bid = false;
  bool ask = false;
  bool pulled = false;
  const Price bid_px = Price::from_decimal("150.10").value();
  const Price ask_px = Price::from_decimal("150.20").value();
  for (std::size_t i = 0; i < r.orders.size(); ++i) {
    CAPTURE(i);
    CHECK(r.orders.instrument[i] == 0);
    const std::int64_t ts = r.orders.ts[i];
    if (r.orders.kind[i] == bt::kOrderKindNew) {
      CHECK(ts < kT0 + 4'500 * kMs);  // nothing new once the leader is stale
      bid = bid || (r.orders.side[i] == 0 && r.orders.price[i] == bid_px.raw);
      ask = ask || (r.orders.side[i] == 1 && r.orders.price[i] == ask_px.raw);
    } else if (r.orders.kind[i] == bt::kOrderKindCancel && ts > kT0 + 4'500 * kMs) {
      CHECK(ts < kT0 + 4'700 * kMs);  // the 100 ms timer after 500 ms of silence
      pulled = true;
    }
  }
  CHECK(bid);
  CHECK(ask);
  CHECK(pulled);
  REQUIRE(r.fills.size() >= 1);
  CHECK(r.fills.instrument[0] == 0);
  CHECK(r.fills.side[0] == 0);
  CHECK(r.fills.price[0] == bid_px.raw);
  CHECK(r.fills.fee[0] == 0);  // 0 bps maker
  // The outbound stream as it was before imb_bps existed: imb_bps = 0 changes nothing.
  CHECK(r.outbound_sha256 == "0de24e821579bad9c6b8dfeff4541931a44a589fff4505437ac6556998e2b909");
}

TEST_CASE("config.research: recorded BookTicker events reach lead_mm in a journal backtest") {
  const bt::BacktestConfig b = research_config();
  bt::CsvSource csv = bt::CsvSource::from_text(tape(true));
  const std::string path = (fastmm::test::tmp_dir() / "lead_mm_ticker.fmj").string();
  bt::write_md_journal(csv, path, b.instruments, 1);
  bt::JournalSource journal(path);
  const bt::BacktestResult r = bt::run_backtest(b, "lead_mm", &journal);
  CHECK(r.engine.risk_rejects == 0);
  // The ticker (2.55 s + 5 ms feed latency) cancels the bid; the 2.6 s depth update re-places it.
  bool cancelled = false;
  bool replaced = false;
  for (std::size_t i = 0; i < r.orders.size(); ++i) {
    const std::int64_t ts = r.orders.ts[i];
    if (r.orders.kind[i] == bt::kOrderKindCancel && ts > kT0 + 2'550 * kMs &&
        ts < kT0 + 2'580 * kMs)
      cancelled = true;
    if (r.orders.kind[i] == bt::kOrderKindNew && r.orders.side[i] == 0 && ts > kT0 + 2'600 * kMs &&
        ts < kT0 + 2'700 * kMs)
      replaced = true;
  }
  CHECK(cancelled);
  CHECK(replaced);
  CHECK(r.outbound_sha256 == "42c3e4709a9a8522572beb7ded880da79eb02b36b6a79a52e2ee793fc65dc437");
}
