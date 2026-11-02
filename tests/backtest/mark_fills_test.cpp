// fastmm-data mark (backtest/mark_fills.hpp): the recorded mid at each row's time and horizons.
#include "fastmm/backtest/mark_fills.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

constexpr std::int64_t kS = 1'000'000'000;
constexpr std::int64_t kV0 = 1'700'000'000 * kS;

std::vector<std::string> lines(const std::string& path) {
  std::ifstream in(path);
  std::vector<std::string> out;
  for (std::string l; std::getline(in, l);) out.push_back(l);
  return out;
}

std::string session(const char* name) {
  const std::string path = tmp_path(name);
  JournalBuilder j(path, true);
  j.now = kV0;
  j.venue = kV0;
  j.ticker("100", "1", "102", "1");  // mid 101
  j.now = kV0 + 10 * kS;
  j.venue = kV0 + 10 * kS;
  j.ticker("102", "1", "104", "1");  // mid 103
  j.now = kV0 + 20 * kS;
  j.venue = kV0 + 20 * kS;
  j.ticker("104", "1", "106", "1");  // mid 105
  return path;
}

}  // namespace

TEST_CASE("backtest.mark_fills: the mid at each row's time and horizons, in the rows' order") {
  const std::string journal = session("mark_fills.fmj");
  const std::string csv = tmp_path("mark_fills_rows.csv");
  {
    std::ofstream f(csv);
    f << "x,t_ns,symbol\n";
    f << "a," << kV0 + 15 * kS << ",BTCUSDT\n";
    f << "b," << kV0 + 1 * kS << ",BTCUSDT\n";
    f << "c," << kV0 - 1 * kS << ",BTCUSDT\n";    // before the first ticker
    f << "d," << kV0 + 1 * kS << ",NOSUCHSYM\n";  // not in the journal
    f << "e," << kV0 + 19 * kS << ",BTCUSDT\n";   // 22 s is past the last ticker
  }
  MarkOptions opt;
  opt.horizons_ns = {0, 3 * kS};
  CHECK(mark_fills(journal, {csv}, opt) == 5);
  const std::vector<std::string> got = lines(csv + ".marks");
  REQUIRE(got.size() == 5);
  CHECK(got[0] == "103,103");
  CHECK(got[1] == "101,101");
  CHECK(got[2] == ",101");
  CHECK(got[3] == ",");
  CHECK(got[4] == "103,");
}

TEST_CASE("backtest.mark_fills: bars hold the last mid of each bar, a row only when it changed") {
  const std::string journal = session("mark_fills_bars.fmj");
  const std::string csv = tmp_path("mark_fills_bars.csv");
  {
    std::ofstream f(csv);
    f << "t_ns,symbol\n";
  }
  MarkOptions opt;
  opt.bars_out = tmp_path("mark_fills_bars_out.csv");
  opt.bar_ns = 5 * kS;
  CHECK(mark_fills(journal, {csv}, opt) == 0);
  const std::vector<std::string> got = lines(opt.bars_out);
  REQUIRE(got.size() == 3);
  CHECK(got[0] == "bar_ns,symbol,mid");
  CHECK(got[1] == std::to_string(kV0 + 5 * kS) + ",BTCUSDT,101");
  CHECK(got[2] == std::to_string(kV0 + 15 * kS) + ",BTCUSDT,103");
}

TEST_CASE("backtest.mark_fills: a CSV without the columns is an error") {
  const std::string journal = session("mark_fills_bad.fmj");
  const std::string csv = tmp_path("mark_fills_bad.csv");
  {
    std::ofstream f(csv);
    f << "time,symbol\n1,BTCUSDT\n";
  }
  CHECK_THROWS_WITH_AS(static_cast<void>(mark_fills(journal, {csv}, MarkOptions{})),
                       doctest::Contains("no column t_ns"),
                       std::runtime_error);
}
