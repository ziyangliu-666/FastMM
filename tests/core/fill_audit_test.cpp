// audit_fills: the venue's executions against the engine's, by symbol, trade id and side.
#include "fastmm/core/fill_audit.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

using namespace fastmm;

namespace {

AuditFill fill(const std::string& symbol,
               const std::string& id,
               Side side,
               std::int64_t qty_raw,
               std::int64_t price_raw,
               std::int64_t time_ms,
               const std::string& order = "1") {
  AuditFill f;
  f.symbol = symbol;
  f.exec_id = id;
  f.side = side;
  f.qty_raw = qty_raw;
  f.price_raw = price_raw;
  f.time_ms = time_ms;
  f.order_id = order;
  f.fee_raw = qty_raw / 1000;
  f.has_fee = true;
  return f;
}

}  // namespace

TEST_CASE("core.fill_audit: matched, missing, phantom and differing executions") {
  const std::vector<AuditFill> venue = {
      fill("BTCUSDT", "100", Side::Buy, 10, 7'000'000, 1000),
      fill("BTCUSDT", "101", Side::Sell, 10, 7'000'100, 2000),  // never booked
      fill("ETHUSDT", "100", Side::Buy, 20, 300'000, 3000),     // same id, other symbol
      fill("BTCUSDT", "102", Side::Buy, 10, 7'000'000, 4000),   // booked with another qty
  };
  std::vector<AuditFill> booked = {
      fill("BTCUSDT", "100", Side::Buy, 10, 7'000'000, 1000),
      fill("ETHUSDT", "100", Side::Buy, 20, 300'000, 3000),
      fill("BTCUSDT", "102", Side::Buy, 12, 7'000'000, 4000),
      fill("BTCUSDT", "103", Side::Sell, 5, 7'000'000, 5000),  // not at the venue
  };
  booked[2].fee_raw = venue[3].fee_raw;  // only the quantity differs
  const FillAuditReport r = audit_fills(venue, booked);
  CHECK(r.venue_rows == 4);
  CHECK(r.booked_rows == 4);
  CHECK(r.matched == 2);
  REQUIRE(r.missing.size() == 1);
  CHECK(r.missing[0].exec_id == "101");
  REQUIRE(r.phantom.size() == 1);
  CHECK(r.phantom[0].exec_id == "103");
  REQUIRE(r.mismatched.size() == 1);
  CHECK(r.mismatched[0].venue.exec_id == "102");
  CHECK(r.mismatched[0].fields == kAuditQty);
  CHECK(audit_fields_text(r.mismatched[0].fields) == "qty");
  CHECK(r.duplicates.empty());
  CHECK_FALSE(r.clean());
  CHECK(r.first_missing_ms() == 2000);
}

TEST_CASE("core.fill_audit: every field that differs is named") {
  AuditFill v = fill("BTCUSDT", "7", Side::Buy, 10, 100, 1000, "42");
  AuditFill b = v;
  b.price_raw = 101;
  b.fee_raw = v.fee_raw + 1;
  b.order_id = "43";
  b.side = Side::Sell;
  const FillAuditReport r = audit_fills(std::vector{v}, std::vector{b});
  REQUIRE(r.mismatched.size() == 1);
  CHECK(r.mismatched[0].fields == (kAuditPrice | kAuditFee | kAuditSide | kAuditOrder));
  CHECK(audit_fields_text(r.mismatched[0].fields) == "price,fee,side,order");
  // A source that does not report the fee, or names no order, is not a difference there.
  b = v;
  b.has_fee = false;
  b.fee_raw = 999;
  b.order_id.clear();
  b.cl_ord_id = "fm1";
  CHECK(audit_fills(std::vector{v}, std::vector{b}).matched == 1);
  // The client order id decides when neither side has the venue's order id.
  v.order_id.clear();
  v.cl_ord_id = "fm2";
  CHECK(audit_fills(std::vector{v}, std::vector{b}).mismatched.size() == 1);
}

TEST_CASE("core.fill_audit: the two halves of a self-trade are two executions") {
  const std::vector<AuditFill> venue = {fill("BTCUSDT", "9", Side::Buy, 10, 100, 1000),
                                        fill("BTCUSDT", "9", Side::Sell, 10, 100, 1000)};
  const std::vector<AuditFill> booked = {fill("BTCUSDT", "9", Side::Sell, 10, 100, 1000)};
  const FillAuditReport r = audit_fills(venue, booked);
  CHECK(r.matched == 1);
  REQUIRE(r.missing.size() == 1);
  CHECK(r.missing[0].side == Side::Buy);
}

TEST_CASE("core.fill_audit: a fill stored twice is a duplicate, a venue row twice is one") {
  const AuditFill f = fill("BTCUSDT", "5", Side::Buy, 10, 100, 1000);
  AuditFill again = f;
  again.session_id = 2;
  const FillAuditReport r = audit_fills(std::vector{f, f}, std::vector{f, again});
  CHECK(r.matched == 1);
  CHECK(r.missing.empty());
  REQUIRE(r.duplicates.size() == 1);
  CHECK(r.duplicates[0].session_id == 2);
}

TEST_CASE("core.fill_audit: the window, symbols and estimates") {
  // Outside [2000, 3000] nothing counts; a pair with one side inside is compared.
  const std::vector<AuditFill> venue = {
      fill("BTC-USDT", "1", Side::Buy, 10, 100, 1000),  // before the window, never booked
      fill("btcusdt", "2", Side::Buy, 10, 100, 2999),   // stamped later by the engine
      fill("BTCUSDT", "3", Side::Buy, 10, 100, 2500),
      fill("BTCUSDT", "", Side::Buy, 10, 100, 2500),  // no trade id: not an execution to audit
  };
  const std::vector<AuditFill> booked = {
      fill("BTCUSDT", "2", Side::Buy, 10, 100, 3001),
      fill("BTCUSDT", "", Side::Buy, 10, 100, 2500),   // an estimate
      fill("BTCUSDT", "4", Side::Buy, 10, 100, 3500),  // after the window
  };
  const FillAuditReport r = audit_fills(venue, booked, 2000, 3000);
  CHECK(r.matched == 1);
  REQUIRE(r.missing.size() == 1);
  CHECK(r.missing[0].exec_id == "3");
  CHECK(r.phantom.empty());
  CHECK(r.venue_rows == 2);
  CHECK(r.booked_rows == 0);
  CHECK(normalize_symbol("btc/usdt") == "BTCUSDT");
}

TEST_CASE("core.fill_audit: describe names the execution") {
  AuditFill f = fill("BTCUSDT", "77", Side::Sell, 150'000, 7'000'012'000'000, 1234, "42");
  f.fee_raw = 10'000;
  f.fee_asset = "USDT";
  CHECK(describe(f) == "BTCUSDT 77 Sell 0.0015 @ 70000.12 fee 0.0001 USDT order 42 at 1234");
}
