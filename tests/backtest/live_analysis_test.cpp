// fastmm-data analyze: a live session's fills marked against its own BookTicker mids.
#include "fastmm/backtest/live_analysis.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include <array>
#include <cmath>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

TEST_CASE("backtest.live_analysis: capture, markout and adverse selection of the session's fills") {
  const std::string path = tmp_path("live_analysis.fmj");
  {
    JournalBuilder j(path, true);
    j.ticker("100.00", "1", "100.02", "1");  // mid 100.01
    j.out_new(1, Side::Buy, "100.00", "1");
    j.ack(1);
    j.fill(1, "100.00", "1", "0", "E1");  // 1 bps captured against the mid
    j.fill(1, "100.00", "1", "0", "E1");  // the same execution again: counted once
    j.out_new(2, Side::Buy, "99.90", "1");
    j.out_cancel(2);
    j.now += 1'000'000'000;
    j.ticker("99.98", "1", "100.00", "1");  // mid 99.99 a second later: 1 bps below the price
  }
  const std::array<std::int64_t, 2> horizons{1'000'000'000, 10'000'000'000};
  const LiveAnalysis a = analyze_live(path, horizons);
  CHECK(a.strategy == "fill_check_test");
  CHECK(a.orders == 2);
  CHECK(a.cancels == 1);
  CHECK(a.fills == 1);
  CHECK(a.maker_fills == 1);
  CHECK(a.repeated_fills == 1);
  CHECK(a.mids);
  REQUIRE(a.instruments.size() == 1);
  CHECK(a.instruments[0].symbol == "BTCUSDT");
  CHECK(a.instruments[0].bought == 1.0);
  REQUIRE(a.horizons.size() == 2);
  const MarkoutBucket& one = a.horizons[0].total;
  CHECK(one.fills == 1);
  CHECK(std::abs(one.capture_bps() - 1.0) < 1e-6);
  CHECK(std::abs(one.markout_bps() + 1.0) < 1e-6);
  CHECK(std::abs(one.adverse_selection_bps() - 2.0) < 1e-6);
  // Ten seconds on is past the last mid: left out, not marked at a stale one.
  CHECK(a.horizons[1].total.fills == 0);
  CHECK(a.horizons[1].excluded_past_end == 1);

  const std::string text = format_live_analysis(a);
  CHECK(text.find("fills    1 (1 maker, 100.0%), 1 repeated report(s) counted once") !=
        std::string::npos);
  CHECK(text.find("1s       all") != std::string::npos);
  const std::string json = live_analysis_json(a);
  CHECK(json.find(R"("fills": 1, "maker_fills": 1, "repeated_fills": 1, "mids": true)") !=
        std::string::npos);
  CHECK(json.find(R"("horizon_ns": 1000000000, "excluded_no_mid": 0, "excluded_past_end": 0, )"
                  R"("all": {"fills": 1, "notional": 100, "capture_bps": 1, "markout_bps": -1, )"
                  R"("adverse_selection_bps": 2, )") != std::string::npos);
}
