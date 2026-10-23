// fastmm-data events (backtest/order_events.hpp): a session's own orders, the venue's answers, its
// balances and the market data of the symbols asked for, as CSV rows.
#include "fastmm/backtest/order_events.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kV0 = 1'700'000'000'000 * kMs;

std::string session(const char* name) {
  const std::string path = tmp_path(name);
  JournalBuilder j(path, true);
  j.now = kV0;
  j.venue = kV0;
  j.book(true, {{"100.00", "1"}}, {{"100.05", "1"}});
  j.out_new(1, Side::Buy, "100.01", "0.5");
  j.now = kV0 + 2 * kMs;  // acked 1 ms after it went out (venue time, a whole millisecond)
  j.venue = kV0 + 1 * kMs;
  j.ack(1);
  j.now = kV0 + 10 * kMs;
  j.out_cancel(1);
  j.now = kV0 + 60 * kMs;  // a burst: the cancel took 30 ms to the book, its ack 20 ms back
  j.venue = kV0 + 40 * kMs;
  j.cancel_ack(1);
  j.venue = kV0 + 41 * kMs;
  j.trade("100.01", "0.2", Side::Sell, 77);
  j.balance("USDT", "1000", 0);
  return path;
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) out.push_back(l);
  return out;
}

std::string dump(const std::string& path, const OrderEventsOptions& opt) {
  const std::string csv = path + ".csv";
  std::FILE* f = std::fopen(csv.c_str(), "w");
  REQUIRE(f != nullptr);
  static_cast<void>(write_order_events({path}, f, opt));
  std::fclose(f);
  std::ifstream in(csv);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

}  // namespace

TEST_CASE("backtest.order_events: own orders, answers and balances, and market data when asked") {
  const std::string path = session("order_events.fmj");
  const std::vector<std::string> rows = lines(dump(path, {}));
  REQUIRE(rows.size() == 6);
  CHECK(rows[0].rfind("type,time,venue_time,venue,symbol,id,side,price,qty,", 0) == 0);
  CHECK(rows[1].rfind("new,1700000000000000000,0,255,BTCUSDT,1,B,100.01000000,0.50000000,", 0) ==
        0);
  CHECK(rows[2].rfind("ack,1700000000002000000,1700000000001000000,255,BTCUSDT,1,", 0) == 0);
  CHECK(rows[3].rfind("cancel,1700000000010000000,", 0) == 0);
  CHECK(rows[4].rfind("cancel_ack,1700000000060000000,1700000000040000000,", 0) == 0);
  CHECK(rows[5].rfind("balance,", 0) == 0);
  CHECK(rows[5].find(",USDT,1000.00000000,") != std::string::npos);
  for (const std::string& r : rows) CHECK(std::count(r.begin(), r.end(), ',') == 16);

  OrderEventsOptions opt;
  opt.symbols = {"*"};
  opt.md_books = false;
  opt.from_ns = kV0 + 5 * kMs;
  const std::vector<std::string> some = lines(dump(path, opt));
  REQUIRE(some.size() == 5);  // the header, the cancel, its ack, the trade, the balance
  CHECK(some[3].rfind("trade,1700000000060000000,1700000000041000000,255,BTCUSDT,,S,100.01000000,"
                      "0.20000000,77,",
                      0) == 0);
}

TEST_CASE("backtest.order_events: money in and out of an account is a flow row") {
  const std::string path = tmp_path("order_events_flows.fmj");
  {
    JournalBuilder j(path, true);
    j.venue = kV0;
    j.flow("USDT", "25", 1);
    j.now += kMs;
    j.venue = kV0 + kMs;
    j.flow("BTC", "-0.5", 2);
  }
  const std::vector<std::string> rows = lines(dump(path, {}));
  REQUIRE(rows.size() == 3);
  CHECK(rows[1].rfind("flow,", 0) == 0);
  CHECK(rows[1].find(",1,,,,,,USDT,25.00000000,") != std::string::npos);
  CHECK(rows[2].find(",2,,,,,,BTC,-0.50000000,") != std::string::npos);
  for (const std::string& r : rows) CHECK(std::count(r.begin(), r.end(), ',') == 16);
}
