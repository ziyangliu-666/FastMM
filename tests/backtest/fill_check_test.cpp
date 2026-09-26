// Fill check (backtest/fill_check.hpp): the l2_queue model run over the orders a journal had
// resting. Hand-built journals with a known answer, a backtest session journal (where the "live"
// fills are the model's own, so they must agree), and the committed fixtures.
#include "fastmm/backtest/fill_check.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/core/journal.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

constexpr std::array<double, 3> kC{0.0, 0.5, 1.0};
constexpr std::int64_t kTradeTs = 1'000'000'000 + 4000;

const FillCheckOrder& order(const FillCheckResult& r, std::uint64_t id) {
  for (const FillCheckOrder& o : r.orders) {
    if (o.cl_ord_id.value == id) return o;
  }
  FAIL("no row for order " << id);
  return r.orders.front();
}

}  // namespace

TEST_CASE("backtest.fill_check: a trade past the queue fills, a short or later one does not") {
  const std::string path = tmp_path("fill_check_basic.fmj");
  {
    JournalBuilder j(path);
    j.book(true, {{"100.00", "5"}, {"99.99", "3"}}, {{"100.01", "4"}, {"100.02", "2"}});
    j.now += 1000;
    j.out_new(1, Side::Buy, "100.00", "1");   // A: 5 ahead, the trade of 7 reaches it
    j.out_new(2, Side::Buy, "99.99", "1");    // B: no trade at or through 99.99
    j.out_new(3, Side::Sell, "100.01", "1");  // C: cancelled before the buy of 10
    j.out_new(4, Side::Sell, "100.02", "1");  // D: its fill depends on the conservatism
    j.now += 1000;
    j.ack(1);
    j.ack(2);
    j.ack(3);
    j.ack(4);
    j.now += 1000;
    j.out_cancel(3);
    j.cancel_ack(3);
    // 1.5 of the 2 at 100.02 cancelled: at c = 0 half of it was ahead of D (1 of 2 left),
    // at c = 0.5 a quarter, at c = 1 none.
    j.book(false, {}, {{"100.02", "0.5"}});
    j.now += 1000;
    CHECK(j.now == kTradeTs);
    j.trade("100.00", "7", Side::Sell);
    j.trade("100.01", "10", Side::Buy);
    j.trade("100.02", "1.5", Side::Buy);
    j.now += 500;
    j.fill(1, "100.00", "1", "0");
    j.fill(4, "100.02", "0.5", "0.5");
    j.now += 1000;
  }
  const FillCheckResult r = fill_check(path, kC);
  REQUIRE(r.orders.size() == 4);
  CHECK(r.orders_sent == 4);
  CHECK(r.not_acked == 0);
  CHECK(r.crossed_at_ack == 0);
  CHECK(r.md_events == 5);

  const FillCheckOrder& a = order(r, 1);
  CHECK(a.queue_ahead == qt("5"));
  CHECK(a.end == FillCheckEnd::Filled);
  CHECK(a.live_filled == qt("1"));
  for (std::size_t k = 0; k < kC.size(); ++k) {
    CHECK(a.model_filled[k] == qt("1"));
    CHECK(a.model_first_fill_ts[k].ns == kTradeTs);
  }
  CHECK((a.live_first_fill_ts.ns - kTradeTs) == 500);

  const FillCheckOrder& b = order(r, 2);
  CHECK(b.end == FillCheckEnd::Open);
  CHECK(b.end_ts.ns == 1'000'000'000 + 4500);  // the last event of the journal
  for (std::size_t k = 0; k < kC.size(); ++k) CHECK(b.model_filled[k].is_zero());

  const FillCheckOrder& c = order(r, 3);
  CHECK(c.end == FillCheckEnd::Canceled);
  CHECK(c.resting().ns == 1000);
  for (std::size_t k = 0; k < kC.size(); ++k) CHECK(c.model_filled[k].is_zero());

  // D: 2 ahead; c = 0 leaves 0.5 ahead, so the buy of 1.5 fills the whole 1; c = 0.5 leaves
  // 1.25 (fill 0.25); c = 1 leaves 2 (no fill).
  const FillCheckOrder& d = order(r, 4);
  CHECK(d.live_filled == qt("0.5"));
  CHECK(d.model_filled[0] == qt("1"));
  CHECK(d.model_filled[1] == qt("0.25"));
  CHECK(d.model_filled[2].is_zero());

  const FillCheckSummary s0 = r.summary(0);
  CHECK(s0.both == 2);
  CHECK(s0.live_only == 0);
  CHECK(s0.model_only == 0);
  CHECK(s0.neither == 2);
  CHECK(s0.live_qty == qt("1.5"));
  CHECK(s0.model_qty == qt("2"));
  CHECK(s0.median_abs_dt_ns == 500);
  const FillCheckSummary s1 = r.summary(2);
  CHECK(s1.both == 1);
  CHECK(s1.live_only == 1);
  CHECK(s1.model_qty == qt("1"));
  CHECK(s1.qty_ratio() == doctest::Approx(1.0 / 1.5));

  const std::string report = format_fill_check(r);
  CHECK(report.find("4 sent, 4 resting after the ack") != std::string::npos);
  const std::string csv = fill_check_csv(r);
  CHECK(std::count(csv.begin(), csv.end(), '\n') == 5);
  CHECK(csv.find("model_filled_c0.50") != std::string::npos);
}

TEST_CASE("backtest.fill_check: a replace at the same price keeps the queue position") {
  const std::string path = tmp_path("fill_check_replace.fmj");
  {
    JournalBuilder j(path);
    j.book(true, {{"100.00", "5"}}, {{"100.01", "5"}});
    j.out_new(1, Side::Buy, "100.00", "1");
    j.out_new(10, Side::Sell, "100.01", "1");
    j.now += 1000;
    j.ack(1);
    j.ack(10);
    j.trade("100.00", "4", Side::Sell);  // 1 left ahead of order 1
    j.trade("100.01", "4", Side::Buy);   // 1 left ahead of order 10
    j.now += 1000;
    j.out_replace(1, 2, "100.00", "1");    // same price, same size: keeps its place
    j.out_replace(10, 11, "100.01", "2");  // larger: back of the queue, 5 ahead
    j.now += 1000;
    j.cancel_ack(1);
    j.ack(2);
    j.ack(11);  // no cancel ack for 10: the ack of the new leg ends it
    j.ack(2);   // a second ack of the same order changes nothing
    j.now += 1000;
    j.trade("100.00", "1.5", Side::Sell);  // fills 0.5 of order 2
    j.trade("100.01", "4", Side::Buy);     // does not reach order 11
    j.now += 1000;
  }
  const FillCheckResult r = fill_check(path, kC);
  REQUIRE(r.orders.size() == 4);
  CHECK(order(r, 1).end == FillCheckEnd::Replaced);
  CHECK(order(r, 10).end == FillCheckEnd::Replaced);
  for (std::size_t k = 0; k < kC.size(); ++k) {
    CHECK(order(r, 1).model_filled[k].is_zero());
    CHECK(order(r, 2).model_filled[k] == qt("0.5"));
    CHECK(order(r, 11).model_filled[k].is_zero());
  }
  CHECK(order(r, 2).end == FillCheckEnd::Open);
}

TEST_CASE("backtest.fill_check: on a backtest session journal the model agrees with the fills") {
  // The session's fills come from the same model at conservatism 0.5, and the check runs on the
  // simulated venue's times.
  BacktestConfig cfg = synthetic_config(31, seconds(120));
  cfg.strategy = "basic_mm";
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 5000;
  cfg.journal_out = tmp_path("fill_check_session.fmj");
  const BacktestResult rec = run_backtest(cfg, "basic_mm");
  REQUIRE(rec.metrics.fills > 10);

  const FillCheckResult r = fill_check(cfg.journal_out, kC);
  REQUIRE(r.orders.size() > 10);
  CHECK(r.unknown_acks == 0);
  CHECK(r.model_full == 0);
  // Every order sent lands in exactly one bucket.
  CHECK(r.orders.size() + r.rejected + r.not_acked + r.ended_before_ack + r.not_resting +
            r.crossed_at_ack ==
        r.orders_sent);
  const FillCheckSummary s = r.summary(1);
  INFO(format_fill_check(r));
  const std::uint64_t live = s.both + s.live_only;
  REQUIRE(live > 10);
  CHECK(s.both * 10 >= live * 9);
  CHECK(s.model_only * 10 <= live);
  CHECK(s.qty_ratio() == doctest::Approx(1.0).epsilon(0.1));
  // Less conservative never fills less.
  CHECK(r.summary(0).model_qty >= s.model_qty);
  CHECK(s.model_qty >= r.summary(2).model_qty);
}

TEST_CASE("backtest.fill_check: runs on the committed journal fixtures") {
  const auto dir = std::filesystem::path(FASTMM_FIXTURES_DIR) / "journals";
  std::size_t n = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() != ".fmj") continue;
    ++n;
    const FillCheckResult r = fill_check(e.path().string(), kC);
    CHECK(r.md_events > 0);
    // Market-data-only fixtures have no orders; every summary is then empty.
    for (std::size_t k = 0; k < kC.size(); ++k) {
      const FillCheckSummary s = r.summary(k);
      CHECK(s.both + s.live_only + s.model_only + s.neither == r.orders.size());
    }
  }
  CHECK(n > 0);
}

namespace {
constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kV0 = 1'700'000'000'000 * kMs;  // a venue clock in whole ms
}  // namespace

TEST_CASE(
    "backtest.fill_check: the execution report arrives before the public trade that filled "
    "the order") {
  // Binance: transactTime and the execution report's T are whole ms; SBE trades carry us. The
  // report is received 3 ms before the trade; by receive time the trade falls after the order's
  // end and the model would never see it.
  const std::string path = tmp_path("fill_check_report_first.fmj");
  {
    JournalBuilder j(path, true);
    j.venue = kV0;
    j.book(true, {{"100.00", "1"}}, {{"100.02", "1"}});
    j.now += 1000;
    j.out_new(1, Side::Buy, "100.00", "1");
    j.now += 1'000'000;
    j.venue = kV0 + 5 * kMs;
    j.ack(1);
    j.venue = kV0 + 6 * kMs + 100'000;  // the depth feed now shows our order: 1 + 1
    j.book(false, {{"100.00", "2"}}, {});
    j.now += 5 * kMs;
    j.venue = kV0 + 8 * kMs;  // T of the execution report: 8.3 ms, truncated
    j.fill(1, "100.00", "1", "0", "501");
    j.now += 3 * kMs;
    j.venue = kV0 + 8 * kMs + 300'000;
    j.trade("100.00", "2", Side::Sell, 501);
    j.venue = kV0 + 8 * kMs + 600'000;  // same ms, later trade id: not ours
    j.trade("100.00", "5", Side::Sell, 502);
    j.now += 1000;
  }
  const FillCheckResult r = fill_check(path, kC);
  REQUIRE(r.orders.size() == 1);
  CHECK(r.own_in_depth);
  CHECK(r.ms_order_times);
  CHECK(r.order_recv_times == 0);
  const FillCheckOrder& o = order(r, 1);
  CHECK(o.queue_ahead == qt("1"));
  CHECK(o.ack_ts.ns == kV0 + 5 * kMs);
  CHECK(o.end == FillCheckEnd::Filled);
  CHECK(o.end_ts.ns == kV0 + 8 * kMs);
  CHECK(o.live_first_fill_ts.ns == kV0 + 8 * kMs);
  for (std::size_t k = 0; k < kC.size(); ++k) {
    CAPTURE(k);
    // The depth update of 1 -> 2 at our price was our own order: nothing ahead left the queue,
    // and the trade of 2 fills 1 behind the 1 ahead at every conservatism.
    CHECK(o.model_filled[k] == qt("1"));
    CHECK(o.model_first_fill_ts[k].ns == kV0 + 8 * kMs + 300'000);
  }
  CHECK(r.ack_ties == 0);
  CHECK(r.end_ties == 0);
  CHECK(r.summary(0).median_abs_dt_ns == 300'000);
}

TEST_CASE("backtest.fill_check: an order filled at placement, before its ack was received") {
  const std::string path = tmp_path("fill_check_at_placement.fmj");
  {
    JournalBuilder j(path, true);
    j.venue = kV0;
    j.book(true, {{"99.99", "1"}}, {{"100.02", "1"}});
    j.now += 1000;
    j.out_new(1, Side::Buy, "100.00", "0.5");  // improves the bid: nothing ahead
    j.now += 1'000'000;
    j.venue = kV0 + 5 * kMs + 400'000;  // an aggressor hits it in the ack's millisecond
    j.trade("100.00", "0.5", Side::Sell, 700);
    j.now += 200'000;
    j.venue = kV0 + 5 * kMs;
    j.fill(1, "100.00", "0.5", "0", "700");  // the user stream is faster than the API response
    j.now += 200'000;
    j.ack(1);
    j.now += 1000;
  }
  const FillCheckResult r = fill_check(path, kC);
  REQUIRE(r.orders.size() == 1);
  CHECK(r.ended_before_ack == 0);
  const FillCheckOrder& o = order(r, 1);
  CHECK(o.queue_ahead.is_zero());
  CHECK(o.end == FillCheckEnd::Filled);
  CHECK(o.resting().ns == 0);
  for (std::size_t k = 0; k < kC.size(); ++k) CHECK(o.model_filled[k] == qt("0.5"));
  CHECK(r.ack_ties == 1);  // the trade shares the ack's millisecond: counted, and it counts
}

TEST_CASE("backtest.fill_check: our cancelled order still in the depth feed is not ahead of us") {
  // Cancel-then-new at the same price 2 ms apart; the throttled depth feed still shows the old
  // order when the new one is acknowledged.
  const std::string path = tmp_path("fill_check_own_ghost.fmj");
  {
    JournalBuilder j(path, true);
    j.venue = kV0;
    j.book(true, {{"99.99", "3"}}, {{"100.02", "1"}});
    j.out_new(1, Side::Buy, "100.00", "1");
    j.now += kMs;
    j.venue = kV0 + 1 * kMs;
    j.ack(1);
    j.venue = kV0 + 1 * kMs + 500'000;
    j.book(false, {{"100.00", "1"}}, {});  // our order, alone at 100.00
    j.out_cancel(1);
    j.now += kMs;
    j.venue = kV0 + 3 * kMs;
    j.cancel_ack(1);
    j.out_new(2, Side::Buy, "100.00", "1");
    j.now += kMs;
    j.venue = kV0 + 5 * kMs;
    j.ack(2);
    j.now += kMs;
    j.venue = kV0 + 7 * kMs + 1000;
    j.trade("100.00", "1", Side::Sell, 800);
    j.now += 1000;
  }
  const FillCheckResult r = fill_check(path, kC);
  REQUIRE(r.orders.size() == 2);
  const FillCheckOrder& o = order(r, 2);
  CHECK(o.queue_ahead.is_zero());
  for (std::size_t k = 0; k < kC.size(); ++k) CHECK(o.model_filled[k] == qt("1"));
  CHECK(order(r, 1).end == FillCheckEnd::Canceled);
  CHECK(order(r, 1).model_filled[0].is_zero());
}

TEST_CASE("backtest.fill_check: a BookTicker newer than the depth caps the queue at the ack") {
  const std::string path = tmp_path("fill_check_ticker.fmj");
  {
    JournalBuilder j(path);
    j.book(true, {{"100.00", "5"}, {"99.99", "3"}}, {{"100.01", "4"}});
    j.now += 1000;
    j.out_new(1, Side::Buy, "100.00", "1");  // 5 ahead by the depth, 2 by the newer ticker
    j.out_new(2, Side::Buy, "99.99", "1");   // behind the touch: the depth's 3
    j.ticker("100.00", "2", "100.01", "4");
    j.now += 1000;
    j.ack(1);
    j.ack(2);
    j.now += 1000;
    j.trade("100.00", "2.5", Side::Sell);  // reaches order 1 only through the ticker's 2
    j.now += 1000;
  }
  const FillCheckResult r = fill_check(path, kC);
  REQUIRE(r.orders.size() == 2);
  CHECK(r.tickers == 1);
  CHECK(r.tickers_used == 1);
  CHECK(order(r, 1).queue_ahead == qt("2"));
  CHECK(order(r, 2).queue_ahead == qt("3"));
  for (std::size_t k = 0; k < kC.size(); ++k) CHECK(order(r, 1).model_filled[k] == qt("0.5"));
}
