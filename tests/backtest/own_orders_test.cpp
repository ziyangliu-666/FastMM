// Own orders (backtest/own_orders.hpp) and JournalSource strip_own: a live session's feed shows our
// resting orders; the stripped feed does not.
#include "fastmm/backtest/own_orders.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kV0 = 1'700'000'000'000 * kMs;

// Others bid 1 at 100.00; we join there with 1 and improve to 100.01 with 1, then cancel both.
std::string live_journal(const char* name, bool live = true) {
  const std::string path = tmp_path(name);
  JournalBuilder j(path, live);
  j.venue = kV0;
  j.book(true, {{"100.00", "1"}}, {{"100.05", "1"}});
  j.out_new(1, Side::Buy, "100.00", "1");
  j.out_new(2, Side::Buy, "100.01", "1");
  j.now += kMs;
  j.venue = kV0 + 1 * kMs;
  j.ack(1);
  j.ack(2);
  j.venue = kV0 + 2 * kMs + 300'000;  // the feed shows both
  j.book(false, {{"100.01", "1"}, {"100.00", "2"}}, {});
  j.ticker("100.01", "1", "100.05", "1");  // our improved bid is the whole best bid
  j.book(false, {{"100.00", "2.5"}}, {});  // someone joins at 100.00
  j.ticker("100.00", "2.5", "100.05", "1");
  j.trade("100.05", "0.1", Side::Buy, 900);  // trades pass as they are
  j.out_cancel(1);
  j.out_cancel(2);
  j.now += kMs;
  j.venue = kV0 + 4 * kMs;
  j.cancel_ack(1);
  j.cancel_ack(2);
  // A throttled update, still showing both; stamped before the cancels.
  j.venue = kV0 + 3 * kMs + 500'000;
  j.book(false, {{"100.01", "1"}, {"100.00", "2.5"}}, {});
  j.venue = kV0 + 5 * kMs + 100'000;  // gone
  j.book(false, {{"100.01", "0"}, {"100.00", "1.5"}}, {});
  j.ticker("100.00", "1.5", "100.05", "1");
  j.now += 1000;
  j.close();
  return path;
}

std::vector<Level> bids(const EventHeader* h) {
  const auto& d = msg_cast<BookDeltaMsg>(h);
  return {d.bids().begin(), d.bids().end()};
}

}  // namespace

TEST_CASE("backtest.own_orders: the order log is in venue time") {
  JournalReader r;
  REQUIRE(r.open(live_journal("own_orders_log.fmj")));
  const OwnOrderLog log = collect_own_orders(r);
  CHECK(log.live);
  CHECK(log.ms_order_times);
  REQUIRE(log.orders.size() == 2);
  const OwnOrder& o = log.orders[1];
  CHECK(o.ack.ts.ns == kV0 + kMs);
  CHECK(o.end.ts.ns == kV0 + 4 * kMs);
  CHECK(o.why == OrderEnd::Canceled);
  CHECK(log.gone(o).ns == kV0 + 5 * kMs - 1);  // the rest of the cancel's millisecond
  const OwnOrderStripper s(log);
  CHECK(s.own_at(InstrumentId{0}, Side::Buy, px("100.00"), Timestamp{kV0 + kMs - 1}).is_zero());
  CHECK(s.own_at(InstrumentId{0}, Side::Buy, px("100.00"), Timestamp{kV0 + kMs}) == qt("1"));
  CHECK(s.own_at(InstrumentId{0}, Side::Buy, px("100.00"), Timestamp{kV0 + 5 * kMs}).is_zero());
  CHECK(s.own_at(InstrumentId{0}, Side::Sell, px("100.00"), Timestamp{kV0 + kMs}).is_zero());
}

TEST_CASE("backtest.journal_source: strip_own takes our orders out of depth and tickers") {
  const std::string path = live_journal("own_orders_strip.fmj");
  JournalSource plain(path);
  JournalSource js(path, true);
  REQUIRE(js.stripper() != nullptr);

  const auto next = [](JournalSource& s) {
    const EventHeader* h = s.next();
    REQUIRE(h != nullptr);
    return h;
  };
  next(plain);
  next(js);  // the snapshot before our orders: unchanged
  // Both of ours shown: 100.01 was only ours (deleted), 100.00 keeps the other 1.
  const EventHeader* h = next(js);
  REQUIRE(h->type == EventType::BookDelta);
  CHECK(bids(h) == std::vector<Level>{{px("100.01"), Qty{}}, {px("100.00"), qt("1")}});
  CHECK(bids(next(plain)) == std::vector<Level>{{px("100.01"), qt("1")}, {px("100.00"), qt("2")}});
  // The ticker whose best bid was only ours is dropped; the next delta comes instead.
  h = next(js);
  REQUIRE(h->type == EventType::BookDelta);
  CHECK(bids(h) == std::vector<Level>{{px("100.00"), qt("1.5")}});
  h = next(js);
  REQUIRE(h->type == EventType::BookTicker);
  CHECK(msg_cast<BookTickerMsg>(h).bid_qty == qt("1.5"));
  CHECK(msg_cast<BookTickerMsg>(h).ask_qty == qt("1"));
  h = next(js);
  REQUIRE(h->type == EventType::Trade);
  CHECK(msg_cast<TradeMsg>(h).qty == qt("0.1"));
  // The throttled update stamped before the cancels still shows ours: stripped.
  CHECK(bids(next(js)) == std::vector<Level>{{px("100.01"), Qty{}}, {px("100.00"), qt("1.5")}});
  // After the cancels: as published.
  CHECK(bids(next(js)) == std::vector<Level>{{px("100.01"), Qty{}}, {px("100.00"), qt("1.5")}});
  CHECK(msg_cast<BookTickerMsg>(next(js)).bid_qty == qt("1.5"));
  CHECK(js.next() == nullptr);

  const OwnOrderStripper::Stats& st = js.stripper()->stats();
  CHECK(st.levels_adjusted == 3);
  CHECK(st.levels_removed == 2);
  CHECK(st.tickers_adjusted == 1);
  CHECK(st.tickers_dropped == 1);
  CHECK(js.note().find("1 dropped") != std::string::npos);
  CHECK(plain.note().empty());
  js.reset();
  CHECK(js.next()->type == EventType::BookSnapshot);
}

TEST_CASE("backtest.journal_source: strip_own through the data spec, live journals only") {
  const std::string path = live_journal("own_orders_spec.fmj");
  const auto src = open_data("journal:" + path + ",strip_own=1");
  REQUIRE(src != nullptr);
  CHECK(src->note().find("own orders stripped (2 orders)") != std::string::npos);
  CHECK(open_data("journal:" + path)->note().empty());
  const std::string sim = live_journal("own_orders_not_live.fmj", false);
  CHECK_THROWS_WITH_AS(JournalSource(sim, true),
                       doctest::Contains("was not recorded by a live session"),
                       std::runtime_error);
}
