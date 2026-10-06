// The store's side of a fill audit (Reader::booked_fills) and a venue's exported executions
// (parse_venue_fills: Binance myTrades / userTrades JSON, generic JSON and CSV).
#include "store_test_util.hpp"

#include "fastmm/core/fill_audit.hpp"
#include "fastmm/store/fill_audit_file.hpp"
#include "fastmm/store/sqlite_store.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

constexpr std::int64_t kMs = 1'000'000;

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
}

// One session of engine "test" on venues "spot" (0) and "perp" (1); BTCUSDT trades on both.
void write_session(const std::string& path,
                   std::uint64_t session,
                   const std::vector<std::pair<std::uint8_t, std::int64_t>>& fills,
                   const std::string& first_id) {
  GenericSection section;
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.engine_name = "test";
  auto backend = make_sqlite_backend();
  REQUIRE(backend->open(o));
  SessionOpen s = fastmm::test::store_session(session, kDay1Ns);
  s.venues = {"spot", "perp"};
  REQUIRE(backend->session_open(s));
  const InstrumentTable t = fastmm::test::store_table();
  REQUIRE(backend->instruments(session, std::span<const Instrument>(t.data(), t.size())));
  backend->begin();
  std::uint64_t seq = 0;
  long id = std::stol(first_id);
  for (const auto& [venue, exch_ms] : fills) {
    FillRecord r = fastmm::test::store_fill(
        session, ++seq, kDay1Ns, Side::Sell, 7'000'000, 150'000, 0, 21, std::to_string(id++));
    r.hdr.venue = VenueId{venue};
    r.hdr.exch_ts = Timestamp{exch_ms * kMs};
    r.fee_asset = FeeAsset::Base;
    backend->fill(r);
  }
  // An estimate: no trade id.
  FillRecord est =
      fastmm::test::store_fill(session, ++seq, kDay1Ns, Side::Buy, 7'000'000, 1, 0, 0, "");
  est.hdr.exch_ts = Timestamp{fills.front().second * kMs};
  backend->fill(est);
  backend->commit();
  backend->close();
}

}  // namespace

TEST_CASE("store.fill_audit: booked_fills reads one venue's executions by the venue's time") {
  const std::string path = fresh("fill_audit_booked.db");
  write_session(path, 1, {{0, 1000}, {1, 1500}, {0, 2000}}, "500");
  write_session(path, 2, {{0, 3000}}, "900");  // a later session of the engine
  GenericSection section;
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto reader = make_sqlite_reader();
  REQUIRE(reader->open(o));

  BookedFillQuery q;
  q.engine = "test";
  q.venue = "spot";
  auto all = reader->booked_fills(q);
  REQUIRE(all);
  REQUIRE(all->size() == 3);  // the estimate and the other venue are left out
  const AuditFill& f = (*all)[0];
  CHECK(f.venue == "spot");
  CHECK(f.symbol == "BTCUSDT");
  CHECK(f.exec_id == "500");
  CHECK(f.order_id == "V1");
  CHECK(f.side == Side::Sell);
  CHECK(f.price_raw == 7'000'000);
  CHECK(f.qty_raw == 150'000);
  CHECK(f.fee_raw == 21);
  CHECK(f.has_fee);
  CHECK(f.fee_asset == "base");
  CHECK(f.time_ms == 1000);
  CHECK(f.session_id == 1);
  CHECK((*all)[2].session_id == 2);

  q.from_ms = 1001;
  q.to_ms = 3000;  // inclusive, to the end of the millisecond
  auto window = reader->booked_fills(q);
  REQUIRE(window);
  REQUIRE(window->size() == 2);
  CHECK((*window)[0].exec_id == "502");
  CHECK((*window)[1].exec_id == "900");

  q.venue = "perp";
  q.from_ms = 0;
  q.to_ms = 0;
  auto perp = reader->booked_fills(q);
  REQUIRE(perp);
  REQUIRE(perp->size() == 1);
  CHECK(perp->front().exec_id == "501");

  q.engine = "another";
  auto none = reader->booked_fills(q);
  REQUIRE(none);
  CHECK(none->empty());
}

TEST_CASE("store.fill_audit: Binance Spot myTrades and USD-M userTrades as they come") {
  // rest-api.md "Account trade list" (Spot): isBuyer, numbers as strings, ids as numbers.
  const auto spot = parse_venue_fills(
      R"([{"symbol":"BTCUSDT","id":28457,"orderId":100234,"orderListId":-1,"price":"70000.12000000",)"
      R"("qty":"0.00150000","quoteQty":"105.00018","commission":"0.00000150","commissionAsset":"BTC",)"
      R"("time":1789295199990,"isBuyer":true,"isMaker":false,"isBestMatch":true}])");
  REQUIRE(spot);
  REQUIRE(spot->size() == 1);
  const AuditFill& s = spot->front();
  CHECK(s.symbol == "BTCUSDT");
  CHECK(s.exec_id == "28457");
  CHECK(s.order_id == "100234");
  CHECK(s.side == Side::Buy);
  CHECK(s.price_raw == 7'000'012'000'000);
  CHECK(s.qty_raw == 150'000);
  CHECK(s.fee_raw == 150);
  CHECK(s.has_fee);
  CHECK(s.fee_asset == "BTC");
  CHECK(s.time_ms == 1789295199990);

  // USD-M "Account Trade List": side and buyer, a nested value skipped, a JSON number price.
  const auto usdm = parse_venue_fills(
      R"([ {"buyer":false,"commission":"-0.07819010","commissionAsset":"USDT","id":698759,)"
      R"("maker":false,"orderId":25851813,"price":7819.01,"qty":"0.002","quoteQty":"15.63802",)"
      R"("realizedPnl":"-0.91539999","side":"SELL","positionSide":"SHORT","symbol":"BTCUSDT",)"
      R"("time":1569514978020,"extra":{"a":[1,2,"]"]}} ])");
  REQUIRE(usdm);
  REQUIRE(usdm->size() == 1);
  CHECK(usdm->front().side == Side::Sell);
  CHECK(usdm->front().price_raw == 781'901'000'000);
  CHECK(usdm->front().fee_raw == -7'819'010);
  CHECK(parse_venue_fills("[]")->empty());
}

TEST_CASE("store.fill_audit: generic CSV and JSON, and what is wrong with a row") {
  const auto csv = parse_venue_fills(
      "venue,symbol,exec_id,order_id,cl_ord_id,side,price,qty,fee,fee_asset,time_ms\r\n"
      "spot,BTC-USDT,1001,77,fm1,sell,70000.5,0.01,0.07,USDT,2026-01-02 03:04:05.678\r\n"
      "\n"
      "spot,ETHUSDT,1002,78,\"\",Buy,3000,\"1,5\",,,1767323045000\n");
  REQUIRE_FALSE(csv);  // "1,5" is not a decimal
  CHECK(csv.error().find("row 4") != std::string::npos);

  const auto ok = parse_venue_fills(
      "symbol,exec_id,side,price,qty,time_ms,fee,fee_asset\n"
      "BTC-USDT,1001,sell,70000.5,0.01,2026-01-02T03:04:05.678Z,0.07,USDT\n"
      "ETHUSDT,1002,Buy,3000,1.5,1767323045000,,\n");
  REQUIRE(ok);
  REQUIRE(ok->size() == 2);
  CHECK((*ok)[0].symbol == "BTC-USDT");
  CHECK((*ok)[0].side == Side::Sell);
  CHECK((*ok)[0].time_ms == 1767323045678);
  CHECK((*ok)[0].fee_raw == 7'000'000);
  CHECK((*ok)[1].time_ms == 1767323045000);
  CHECK_FALSE((*ok)[1].has_fee);

  const auto json = parse_venue_fills(
      R"([{"symbol":"BTCUSDT","exec_id":"a-1","side":"buy","price":"1","qty":"2",)"
      R"("time_ms":5,"cl_ord_id":"fm1"}])");
  REQUIRE(json);
  CHECK(json->front().exec_id == "a-1");
  CHECK(json->front().cl_ord_id == "fm1");

  CHECK(parse_venue_fills(R"([{"symbol":"BTCUSDT","id":1,"price":"1","qty":"1","time":1}])")
            .error()
            .find("no side") != std::string::npos);
  CHECK(parse_venue_fills(R"([{"symbol":"BTCUSDT","id":1,"side":"x","price":"1","qty":"1",)"
                          R"("time":1}])")
            .error()
            .find("row 1") != std::string::npos);
  CHECK_FALSE(parse_venue_fills("[{\"symbol\":\"BTCUSDT\""));
  CHECK_FALSE(parse_venue_fills(""));
  CHECK_FALSE(load_venue_fills((tmp_dir() / "no-such-file.json").string()));
}
