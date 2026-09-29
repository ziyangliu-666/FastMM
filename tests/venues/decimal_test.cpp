#include "fastmm/venues/decimal.hpp"

#include "test_support.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;

TEST_CASE("venues.decimal: exact parse of venue price/qty strings") {
  CHECK(parse_price("76745.18000000")->raw == 7'674'518'000'000LL);
  CHECK(parse_qty("0.00065000")->raw == 65'000);
  CHECK(parse_qty("8.75160000")->raw == 875'160'000);
  CHECK(parse_price("0.1")->raw == 10'000'000);
  CHECK(parse_price("77171.6")->raw == 7'717'160'000'000LL);
  CHECK(parse_qty("0")->raw == 0);
  CHECK(parse_qty("0.00000000")->raw == 0);
  CHECK(parse_notional("-12.5")->raw == -1'250'000'000LL);
  CHECK(parse_price("1e5").error() == DecimalError::Malformed);
  CHECK(parse_price("").error() == DecimalError::Empty);
  CHECK(parse_price("12.123456789").error() == DecimalError::Malformed);
  CHECK(parse_price("12.12345678000").has_value());  // trailing zeros beyond 8 places ok
  CHECK(parse_price("999999999999").error() == DecimalError::Overflow);
  CHECK(parse_price(" 1").error() == DecimalError::Malformed);
}

// Binance USD-M's positionRisk and ACCOUNT_UPDATE give an entry price with more than 8 fraction
// digits; parse_price refuses it, and the position was then valued at an average of 0.
TEST_CASE("venues.decimal: parse_avg_price rounds an average to 8 decimals") {
  CHECK(parse_avg_price("83954.61851851852")->raw == 8'395'461'851'852LL);
  CHECK(parse_avg_price("83954.618518514")->raw == 8'395'461'851'851LL);
  CHECK(parse_avg_price("83954.618518515")->raw == 8'395'461'851'852LL);
  CHECK(parse_avg_price("-1.000000005")->raw == -100'000'001LL);
  CHECK(parse_avg_price("-0.000000005")->raw == -1);
  CHECK(parse_avg_price("0.999999995")->raw == 100'000'000LL);
  CHECK(parse_avg_price("76980.0")->raw == 7'698'000'000'000LL);
  CHECK(parse_avg_price("0")->raw == 0);
  CHECK(parse_avg_price("").error() == DecimalError::Empty);
  CHECK(parse_avg_price("1.1234567891x").error() == DecimalError::Malformed);
  CHECK(parse_avg_price("1e5").error() == DecimalError::Malformed);
  CHECK(parse_avg_price("999999999999.123456789").error() == DecimalError::Overflow);
}

TEST_CASE("venues.decimal: parse_int64 and timestamps") {
  CHECK(*parse_int64("1789295134226") == 1789295134226LL);
  CHECK(*parse_int64("-1") == -1);
  CHECK(*parse_int64("0") == 0);
  CHECK(parse_int64("").error() == DecimalError::Empty);
  CHECK(parse_int64("-").error() == DecimalError::Malformed);
  CHECK(parse_int64("12a").error() == DecimalError::Malformed);
  CHECK(parse_int64("99999999999999999999").error() == DecimalError::Overflow);
  CHECK(*parse_int64("9223372036854775807") == INT64_MAX);
  CHECK(parse_ts_ms("1789295134226")->ns == 1789295134226LL * 1'000'000);
  CHECK(ts_from_ms(1).ns == 1'000'000);
}

TEST_CASE("venues.decimal: DecimalText formats without heap") {
  DecimalText t(Price::from_raw(7'674'518'000'000LL));
  CHECK(t.view() == "76745.18");
  CHECK(std::string(t.c_str()) == "76745.18");
  DecimalText q(Qty::from_raw(65'000));
  CHECK(q.view() == "0.00065");
  char buf[24];
  CHECK(std::string_view(buf, format_int64(-1234, buf)) == "-1234");
  CHECK(std::string_view(buf, format_int64(0, buf)) == "0");
}
