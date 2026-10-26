// The itch source: a TotalView-ITCH file as the order-by-order events the venue keeps and the L2
// view the engine trades on, the queues at `start`, and New York's midnight.
#include "fastmm/backtest/itch_source.hpp"

#include "backtest_test_util.hpp"
#include "journal_builder.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/multiverse.hpp"
#include "fastmm/codecs/itch/itch_encoder.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;
constexpr std::uint64_t hm(int h, int m) {
  return (static_cast<std::uint64_t>(h) * 3600 + static_cast<std::uint64_t>(m) * 60) * kSecond;
}
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

// A file of ITCH messages, each behind its 2-byte big-endian length, as NASDAQ's are.
class ItchFile {
 public:
  template <class F>
  void put(F&& write) {
    std::array<std::byte, 64> buf{};
    const std::size_t n = write(std::span<std::byte>(buf));
    REQUIRE(n > 0);
    bytes_.push_back(static_cast<char>((n >> 8U) & 0xFFU));
    bytes_.push_back(static_cast<char>(n & 0xFFU));
    bytes_.append(reinterpret_cast<const char*>(buf.data()), n);
  }
  std::string save(const char* name) const {
    const std::string path = testutil::tmp_path(name);
    std::ofstream(path, std::ios::binary)
        .write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
    return path;
  }

 private:
  std::string bytes_;
};

// Each test case runs in a process of its own, in parallel: each writes its own file.
std::string day_file(const char* name) {
  codecs::itch::ItchEncoder e;
  ItchFile f;
  constexpr std::uint16_t L = 7;
  f.put([&](auto out) { return e.stock_directory(out, L, hm(3, 0), "AAPL"); });
  f.put([&](auto out) { return e.stock_directory(out, 8, hm(3, 0), "MSFT"); });
  f.put([&](auto out) {
    return e.add_order(out, L, hm(9, 0), 1, Side::Buy, qt("100"), "AAPL", px("100.00"));
  });
  f.put([&](auto out) {
    return e.add_order(out, L, hm(9, 0), 2, Side::Sell, qt("100"), "AAPL", px("100.02"));
  });
  f.put([&](auto out) {
    return e.add_order(out, 8, hm(9, 0), 9, Side::Buy, qt("100"), "MSFT", px("50.00"));
  });
  f.put([&](auto out) {
    return e.add_order(out, L, hm(9, 10), 3, Side::Buy, qt("50"), "AAPL", px("100.00"));
  });
  // From 09:30
  f.put([&](auto out) { return e.order_executed(out, L, hm(9, 31), 1, qt("30"), 70); });
  f.put([&](auto out) {
    return e.trade(out, L, hm(9, 32), Side::Buy, qt("200"), "AAPL", px("100.01"), 71);
  });
  f.put([&](auto out) { return e.order_cancel(out, L, hm(9, 33), 3, qt("10")); });
  f.put(
      [&](auto out) { return e.order_replace(out, L, hm(9, 34), 2, 4, qt("100"), px("100.03")); });
  f.put([&](auto out) { return e.order_delete(out, L, hm(10, 1), 4); });
  return f.save(name);
}

ItchSourceConfig config(const std::string& path) {
  ItchSourceConfig c;
  c.path = path;
  c.date = "2019-07-30";
  c.symbols = {{"AAPL", InstrumentId{0}}};
  c.start_ns = static_cast<std::int64_t>(hm(9, 30));
  c.end_ns = static_cast<std::int64_t>(hm(10, 0));
  c.depth = 4;
  c.max_orders = 1024;
  return c;
}

std::vector<std::vector<std::byte>> drain(ItchSource& s) {
  std::vector<std::vector<std::byte>> out;
  while (const EventHeader* h = s.next()) {
    std::vector<std::byte> b(h->len);
    std::memcpy(b.data(), h, h->len);
    out.push_back(std::move(b));
  }
  return out;
}
const EventHeader& hdr(const std::vector<std::byte>& b) {
  return *reinterpret_cast<const EventHeader*>(b.data());
}
template <class M>
const M& as(const std::vector<std::byte>& b) {
  return *reinterpret_cast<const M*>(b.data());
}

}  // namespace

TEST_CASE("itch: midnight in New York follows daylight saving time") {
  // 2019-07-30 is daylight time (UTC-4), 2019-01-15 standard time (UTC-5); 2019's clocks changed
  // on March 10 and November 3.
  constexpr std::int64_t kDay = 86'400 * static_cast<std::int64_t>(kSecond);
  CHECK(new_york_midnight("2019-07-30").ns ==
        18'107 * kDay + 4 * 3600 * static_cast<std::int64_t>(kSecond));
  CHECK(new_york_midnight("2019-01-15").ns ==
        17'911 * kDay + 5 * 3600 * static_cast<std::int64_t>(kSecond));
  // The clocks change at 02:00, after midnight.
  constexpr std::int64_t kHour = 3600 * static_cast<std::int64_t>(kSecond);
  CHECK(new_york_midnight("2019-03-10").ns % kDay == 5 * kHour);
  CHECK(new_york_midnight("2019-03-11").ns % kDay == 4 * kHour);
  CHECK(new_york_midnight("2019-11-03").ns % kDay == 4 * kHour);
  CHECK(new_york_midnight("2019-11-04").ns % kDay == 5 * kHour);
  CHECK(parse_time_of_day("09:30") == static_cast<std::int64_t>(hm(9, 30)));
}

TEST_CASE("itch: the queues at start, then the venue's events ahead of the engine's view") {
  ItchSource s(config(day_file("itch_source_queues.itch")));
  const Timestamp midnight = new_york_midnight("2019-07-30");
  CHECK(s.start_ts() == Timestamp{midnight.ns + static_cast<std::int64_t>(hm(9, 30))});
  const auto ev = drain(s);
  std::size_t i = 0;
  // The resting orders in queue order (bids, then asks), stamped at the first message at or after
  // start; MSFT is not configured.
  const std::uint64_t refs[3] = {1, 3, 2};
  for (const std::uint64_t ref : refs) {
    REQUIRE(i < ev.size());
    REQUIRE(hdr(ev[i]).type == EventType::OrderAddL3);
    CHECK(as<OrderAddL3Msg>(ev[i]).order_ref == ref);
    CHECK(hdr(ev[i]).instrument == InstrumentId{0});
    CHECK(hdr(ev[i]).exch_ts == Timestamp{midnight.ns + static_cast<std::int64_t>(hm(9, 31))});
    ++i;
  }
  // The book the engine starts from.
  REQUIRE(hdr(ev[i]).type == EventType::BookSnapshot);
  {
    const auto& d = as<BookDeltaMsg>(ev[i]);
    REQUIRE(d.bid_count == 1);
    REQUIRE(d.ask_count == 1);
    CHECK(d.levels()[0].price == px("100.00"));
    CHECK(d.levels()[0].qty == qt("150"));
    CHECK(d.levels()[1].price == px("100.02"));
  }
  ++i;
  std::vector<EventType> rest;
  for (; i < ev.size(); ++i) {
    if (hdr(ev[i]).type == EventType::ConnectionState) continue;
    rest.push_back(hdr(ev[i]).type);
    CHECK(hdr(ev[i]).exch_ts <= Timestamp{midnight.ns + static_cast<std::int64_t>(hm(10, 0))});
  }
  // E: the venue's execution, its print, the level after it; P: a print alone, with no side; X:
  // the cancel and the level; U: the replace and the levels. The delete at 10:01 is past the end.
  const std::vector<EventType> want = {EventType::OrderExecL3,
                                       EventType::Trade,
                                       EventType::BookDelta,
                                       EventType::Trade,
                                       EventType::OrderCancelL3,
                                       EventType::BookDelta,
                                       EventType::OrderReplaceL3,
                                       EventType::BookDelta};
  CHECK(rest == want);
  for (const auto& b : ev) {
    if (hdr(b).type != EventType::Trade) continue;
    const auto& t = as<TradeMsg>(b);
    if (t.trade_id == 70) {
      CHECK(t.has_aggressor());
      CHECK(t.aggressor == Side::Sell);
      CHECK(t.price == px("100.00"));
    } else {
      CHECK(t.trade_id == 71);
      CHECK_FALSE(t.has_aggressor());
    }
  }
  // reset() reads the file again.
  s.reset();
  CHECK(drain(s).size() == ev.size());
}

TEST_CASE("itch: the day comes from the file name or date=, and a symbol is required") {
  ItchSourceConfig c = config(day_file("itch_source_names_a.itch"));
  c.date.clear();
  CHECK_THROWS_AS(ItchSource{c}, std::runtime_error);  // itch_source_test.itch names no day
  c = config(day_file("itch_source_names_b.itch"));
  c.symbols.clear();
  CHECK_THROWS_AS(ItchSource{c}, std::runtime_error);
}

TEST_CASE("itch: l3_queue runs only over a source that carries every order") {
  ItchSource itch(config(day_file("itch_source_l3.itch")));
  CHECK(itch.order_by_order());
  MergedSource merged({&itch});
  CHECK(merged.order_by_order());
  BacktestConfig c = testutil::synthetic_config(1, seconds(1));
  c.transport.fill_model = sim::FillModel::L3Queue;
  CHECK_THROWS_AS(run_backtest(c, "basic_mm"), std::invalid_argument);  // the synthetic market
}

TEST_CASE("itch: the runs of a multiverse over the file are their backtests alone") {
  // l2_queue: the order-by-order events pass the runs by, the L2 view fills them.
  const std::string path = day_file("itch_source_multiverse.itch");
  BacktestConfig base = BacktestConfig::single_instrument("AAPL", px("0.01"), qt("1"));
  base.transport.fill_model = sim::FillModel::L2Queue;
  base.measure_wall_clock = false;
  std::vector<BacktestConfig> cfgs;
  for (const char* half : {"0.5", "1", "2"}) {
    BacktestConfig c = base;
    c.params = {{"half_spread_bps", half}, {"quote_qty", "10"}, {"max_inventory", "100"}};
    cfgs.push_back(c);
  }
  const SourceFactory factory = [&]() -> std::unique_ptr<MdSource> {
    return std::make_unique<ItchSource>(config(path));
  };
  const std::vector<BacktestResult> together = run_multiverse(cfgs, "basic_mm", factory, 1);
  REQUIRE(together.size() == cfgs.size());
  for (std::size_t i = 0; i < cfgs.size(); ++i) {
    CAPTURE(i);
    ItchSource alone_source(config(path));
    const BacktestResult alone = run_backtest(cfgs[i], "basic_mm", &alone_source);
    CHECK(alone.md_events > 0);
    CHECK(together[i].outbound_sha256 == alone.outbound_sha256);
    CHECK(together[i].md_events == alone.md_events);
    CHECK(together[i].fills.size() == alone.fills.size());
    CHECK(together[i].metrics.net_pnl == alone.metrics.net_pnl);
  }
}
