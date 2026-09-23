// The data-source registry and the archive readers: spec parsing, the Binance and Tardis
// decoders on small written-out files, the merge order between trades and book, and the
// journal converter.
#include "fastmm/backtest/data_registry.hpp"

#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/binance_source.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/tardis_source.hpp"

#include <fstream>
#include <stdexcept>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// A cache root of this test's own, holding one day of each archive's layout.
std::string write_cache(const std::string& name,
                        const std::string& key,
                        const std::string& contents) {
  const std::filesystem::path root = test::tmp_dir() / name;
  const std::filesystem::path path = root / key;
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
  return root.string();
}

struct Event {
  EventType type;
  std::int64_t ts;
  std::uint32_t bids;
  std::uint32_t asks;
  Price px;
  Qty qty;
  Side side;
  bool snapshot;
};

std::vector<Event> drain_events(MdSource& s) {
  std::vector<Event> out;
  while (const EventHeader* h = s.next()) {
    Event e{};
    e.type = h->type;
    e.ts = h->exch_ts.ns;
    if (h->type == EventType::Trade) {
      const auto& t = msg_cast<TradeMsg>(h);
      e.px = t.price;
      e.qty = t.qty;
      e.side = t.aggressor;
    } else {
      const auto& d = msg_cast<BookDeltaMsg>(h);
      e.bids = d.bid_count;
      e.asks = d.ask_count;
      e.snapshot = d.is_snapshot();
      if (d.bid_count > 0) {
        e.px = d.bids()[0].price;
        e.qty = d.bids()[0].qty;
      }
    }
    out.push_back(e);
  }
  return out;
}

const char* kBookTicker =
    "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_"
    "time\n"
    "1,70000.0,1.5,70000.1,2.0,1711497600000,1711497600002\n"   // snapshot
    "2,70000.0,1.5,70000.1,2.0,1711497600001,1711497600003\n"   // nothing changed: skipped
    "3,70000.0,1.0,70000.1,2.0,1711497600002,1711497600004\n"   // bid size only
    "4,69999.9,3.0,70000.1,2.0,1711497600003,1711497600005\n";  // bid price moves

const char* kAggTrades =
    "agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker\n"
    "10,70000.1,0.5,1,1,1711497600002,false\n"
    "11,70000.0,0.25,2,2,1711497600003,true\n";

const char* kTardisBook =
    "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
    "binance,X,1788220800000000,1788220800001000,true,ask,100.2,3\n"
    "binance,X,1788220800000000,1788220800001000,true,ask,100.3,4\n"
    "binance,X,1788220800000000,1788220800001000,true,bid,100.1,5\n"
    "binance,X,1788220800100000,1788220800101000,false,bid,100.1,0\n"
    "binance,X,1788220800100000,1788220800101000,false,bid,100.0,7\n";

const char* kTardisTrades =
    "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n"
    "binance,X,1788220800050000,1788220800051000,900,sell,100.1,1.5\n";

}  // namespace

TEST_CASE("backtest.data: a spec is a name, positional arguments and key=value options") {
  const std::string_view positional[] = {"symbol", "date"};
  const DataSourceOptions o =
      DataSourceOptions::parse("binance:BTCUSDT,2024-03-27,market=spot", positional);
  CHECK(o.name() == "binance");
  CHECK(o.get("symbol") == "BTCUSDT");
  CHECK(o.get("date") == "2024-03-27");
  CHECK(o.get("market") == "spot");
  CHECK(o.get("dir", "fallback") == "fallback");
  CHECK(o.get_bool("book", true));
  CHECK(o.get_int("venue", 7) == 7);

  CHECK(DataSourceOptions::parse("synthetic").name() == "synthetic");
  // A time keeps its colons: only the first one separates the name.
  CHECK(DataSourceOptions::parse("x:start=09:30").get("start") == "09:30");
  CHECK_THROWS_AS(static_cast<void>(DataSourceOptions::parse("x:a=1,a=2")), std::runtime_error);
  CHECK_THROWS_AS(static_cast<void>(DataSourceOptions::parse("x:bare")), std::runtime_error);
  CHECK_THROWS_AS(static_cast<void>(DataSourceOptions::parse("x:=1")), std::runtime_error);
  CHECK_THROWS_AS(static_cast<void>(DataSourceOptions::parse("x:n=zzz").get_int("n", 0)),
                  std::runtime_error);
  CHECK_THROWS_AS(static_cast<void>(DataSourceOptions::parse("x:b=maybe").get_bool("b", false)),
                  std::runtime_error);
  CHECK_THROWS_AS(DataSourceOptions::parse("x:q=1").reject_unknown({"p"}), std::runtime_error);
}

TEST_CASE("backtest.data: every built-in source is registered with its capabilities") {
  register_builtin_data_sources();
  const DataSourceRegistry& r = DataSourceRegistry::instance();
  for (std::string_view name : {"synthetic", "journal", "csv", "binance", "tardis"})
    REQUIRE_MESSAGE(r.find(name) != nullptr, name);
  // Binance's dumps are the top of book and nothing deeper; Tardis publishes the whole L2.
  CHECK_FALSE(r.find("binance")->caps.depth);
  CHECK(r.find("binance")->caps.top_of_book);
  CHECK(r.find("binance")->caps.trades);
  CHECK(r.find("tardis")->caps.depth);
  // Registering the same entry again is a no-op, a different opener is refused.
  CHECK(DataSourceRegistry::instance().try_add(*r.find("csv")));
  DataSourceEntry clash = *r.find("csv");
  clash.open = r.find("journal")->open;
  CHECK_FALSE(DataSourceRegistry::instance().try_add(clash));
  CHECK(format_data_sources().find("binance") != std::string::npos);
}

TEST_CASE("backtest.data: bookTicker becomes a one-level book, unchanged rows are dropped") {
  const std::string dir = write_cache(
      "bt_bin",
      binance_daily_key(BinanceMarket::UsdmFutures, "bookTicker", "BTCUSDT", "2024-03-27", ".csv"),
      kBookTicker);
  BinanceSourceConfig cfg;
  cfg.dir = dir;
  cfg.symbol = "BTCUSDT";
  cfg.dates = {"2024-03-27"};
  cfg.trades = false;
  BinanceDataset src(cfg);
  const std::vector<Event> e = drain_events(src);

  REQUIRE(e.size() == 3);  // row 2 repeats the top of book and yields nothing
  CHECK(e[0].type == EventType::BookSnapshot);
  CHECK(e[0].snapshot);
  CHECK(e[0].bids == 1);
  CHECK(e[0].asks == 1);
  CHECK(e[0].ts == 1'711'497'600'000'000'000);  // milliseconds scaled to ns
  // Only the bid size changed: one level, no ask.
  CHECK(e[1].type == EventType::BookDelta);
  CHECK(e[1].bids == 1);
  CHECK(e[1].asks == 0);
  CHECK(e[1].qty == qt("1"));
  // The bid moved: the old level is deleted and the new one added.
  CHECK(e[2].bids == 2);
  CHECK(e[2].asks == 0);
  CHECK(e[2].px == px("70000.0"));
  CHECK(e[2].qty == qt("0"));

  src.reset();
  CHECK(drain_events(src).size() == 3);
  CHECK(src.start_ts() == Timestamp{1'711'497'600'000'000'000});
}

TEST_CASE("backtest.data: aggTrades carry the aggressor, and a trade wins a tie with the book") {
  const std::string key_book =
      binance_daily_key(BinanceMarket::UsdmFutures, "bookTicker", "BTCUSDT", "2024-03-27", ".csv");
  const std::string key_trades =
      binance_daily_key(BinanceMarket::UsdmFutures, "aggTrades", "BTCUSDT", "2024-03-27", ".csv");
  const std::string dir = write_cache("bt_bin2", key_book, kBookTicker);
  write_cache("bt_bin2", key_trades, kAggTrades);

  BinanceSourceConfig cfg;
  cfg.dir = dir;
  cfg.symbol = "BTCUSDT";
  cfg.dates = {"2024-03-27"};
  BinanceDataset src(cfg);
  const std::vector<Event> e = drain_events(src);

  REQUIRE(e.size() == 5);
  CHECK(e[0].type == EventType::BookSnapshot);
  // is_buyer_maker=false means the buyer took: the aggressor bought.
  CHECK(e[1].type == EventType::Trade);
  CHECK(e[1].side == Side::Buy);
  CHECK(e[1].ts == 1'711'497'600'002'000'000);
  // Same millisecond as the book update at 1711497600002: the trade comes first, so it
  // consumes the queue before the level is recorded as smaller.
  CHECK(e[2].type == EventType::BookDelta);
  CHECK(e[2].ts == e[1].ts);
  CHECK(e[3].type == EventType::Trade);
  CHECK(e[3].side == Side::Sell);  // is_buyer_maker=true
  CHECK(e[4].type == EventType::BookDelta);
}

TEST_CASE("backtest.data: a missing file names the fetch command") {
  BinanceSourceConfig cfg;
  cfg.dir = (test::tmp_dir() / "bt_absent").string();
  cfg.symbol = "BTCUSDT";
  cfg.dates = {"2024-03-27"};
  try {
    BinanceDataset src(cfg);
    FAIL("expected a missing-file error");
  } catch (const std::runtime_error& e) {
    const std::string what = e.what();
    CHECK(what.find("fastmm.data fetch") != std::string::npos);
    CHECK(what.find("2024-03-27") != std::string::npos);
  }
  // The archive has no spot book at all, and says so instead of looking for a file.
  cfg.market = BinanceMarket::Spot;
  CHECK_THROWS_AS(BinanceDataset{cfg}, std::runtime_error);
}

TEST_CASE("backtest.data: tardis rows of one timestamp form one L2 message") {
  const std::string dir =
      write_cache("bt_tardis",
                  tardis_key("binance", "incremental_book_L2", "X", "2026-09-01", ".csv"),
                  kTardisBook);
  write_cache(
      "bt_tardis", tardis_key("binance", "trades", "X", "2026-09-01", ".csv"), kTardisTrades);
  TardisSourceConfig cfg;
  cfg.dir = dir;
  cfg.exchange = "binance";
  cfg.symbol = "X";
  cfg.dates = {"2026-09-01"};
  TardisDataset src(cfg);
  const std::vector<Event> e = drain_events(src);

  REQUIRE(e.size() == 3);
  CHECK(e[0].type == EventType::BookSnapshot);
  CHECK(e[0].bids == 1);
  CHECK(e[0].asks == 2);
  CHECK(e[0].ts == 1'788'220'800'000'000'000);  // microseconds scaled to ns
  CHECK(e[1].type == EventType::Trade);
  CHECK(e[1].side == Side::Sell);
  CHECK(e[2].type == EventType::BookDelta);
  CHECK(e[2].bids == 2);  // the delete and the new level, one message
  CHECK(e[2].qty == qt("0"));
}

TEST_CASE("backtest.data: open_data resolves specs, paths and unknown names") {
  const std::string dir = write_cache(
      "bt_open",
      binance_daily_key(BinanceMarket::UsdmFutures, "bookTicker", "BTCUSDT", "2024-03-27", ".csv"),
      kBookTicker);
  CHECK(open_data("synthetic") == nullptr);
  CHECK(open_data("") == nullptr);
  std::unique_ptr<MdSource> s = open_data("binance:BTCUSDT,2024-03-27,trades=false,dir=" + dir);
  REQUIRE(s != nullptr);
  CHECK(drain_events(*s).size() == 3);
  CHECK_THROWS_AS(static_cast<void>(open_data("nosuchsource:x")), std::runtime_error);
  CHECK_THROWS_AS(static_cast<void>(open_data("data.parquet")), std::runtime_error);
}

TEST_CASE("backtest.data: convert writes a journal that replays the same events") {
  const std::string dir = write_cache(
      "bt_conv",
      binance_daily_key(BinanceMarket::UsdmFutures, "bookTicker", "BTCUSDT", "2024-03-27", ".csv"),
      kBookTicker);
  const BacktestConfig cfg = BacktestConfig::single_instrument("BTCUSDT", px("0.1"), qt("0.001"));
  const std::string spec = "binance:BTCUSDT,2024-03-27,trades=false,dir=" + dir;
  const std::string out = (test::tmp_dir() / "bt_conv.fmj").string();
  CHECK(convert_data(spec, out, cfg.instruments) == 3);

  std::unique_ptr<MdSource> from_csv = open_data(spec);
  JournalSource from_journal(out);
  const std::vector<Event> a = drain_events(*from_csv);
  const std::vector<Event> b = drain_events(from_journal);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].type == b[i].type);
    CHECK(a[i].ts == b[i].ts);
    CHECK(a[i].bids == b[i].bids);
    CHECK(a[i].asks == b[i].asks);
    CHECK(a[i].px == b[i].px);
    CHECK(a[i].qty == b[i].qty);
  }
}

TEST_CASE("backtest.data: date_range and parse_utc_time") {
  const std::vector<std::string> d = date_range("2024-02-28", "2024-03-01");
  REQUIRE(d.size() == 3);
  CHECK(d[0] == "2024-02-28");
  CHECK(d[1] == "2024-02-29");  // a leap year
  CHECK(d[2] == "2024-03-01");
  CHECK(date_range("2024-03-27", "2024-03-27").size() == 1);
  CHECK_THROWS_AS(static_cast<void>(date_range("2024-03-27", "2024-03-26")), std::runtime_error);
  CHECK_THROWS_AS(static_cast<void>(date_range("27-03-2024", "27-03-2024")), std::runtime_error);

  CHECK(parse_utc_time("1970-01-01T00:00:00", "1970-01-01") == Timestamp{0});
  CHECK(parse_utc_time("00:00:01", "1970-01-01") == Timestamp{1'000'000'000});
  CHECK(parse_utc_time("2024-03-27", "2024-03-27") == Timestamp{1'711'497'600'000'000'000});
  CHECK(parse_utc_time("12:30", "2024-03-27") == Timestamp{1'711'542'600'000'000'000});
  CHECK_THROWS_AS(static_cast<void>(parse_utc_time("noon", "2024-03-27")), std::runtime_error);
}
