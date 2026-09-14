#include "fix_test_util.hpp"

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;
using namespace fastmm::codecs::fix::test;

namespace {
constexpr std::int64_t kT0 = 1'789'374'615'250'000'000;  // 20260914-08:30:15.250
}

TEST_CASE("codecs.fix.builder: standard header with BodyLength and CheckSum backfilled") {
  char buf[512];
  FixBuilder b{std::span<char>(buf)};
  b.begin_header(msg::kHeartbeat, "CLIENT", "VENUE", 7, kT0);
  const std::size_t n = b.finish();
  REQUIRE(n > 0);
  CHECK(std::string(buf, n) == wrap("35=0|49=CLIENT|56=VENUE|34=7|52=20260914-08:30:15.250|"));
  FixView v;
  CHECK(v.parse(std::string_view(buf, n)) == FixError::None);
  CHECK(std::string_view(buf, n).substr(b.header_end(), 3) == "10=");
}

TEST_CASE("codecs.fix.builder: BodyLength with one to four digits") {
  for (const std::size_t pad :
       {std::size_t{0}, std::size_t{5}, std::size_t{95}, std::size_t{900}, std::size_t{2000}}) {
    CAPTURE(pad);
    std::string buf(4096, '\0');
    FixBuilder b{std::span<char>(buf.data(), buf.size())};
    b.begin(msg::kHeartbeat);
    std::string fields = "35=0|";
    if (pad > 0) {
      const std::string text(pad, 'x');
      b.field(tag::kText, text);
      fields += "58=" + text + "|";
    }
    const std::size_t n = b.finish();
    REQUIRE(n > 0);
    CHECK(std::string_view(buf.data(), n) == wrap(fields));
    FixView v;
    CHECK(v.parse(std::string_view(buf.data(), n)) == FixError::None);
  }
}

TEST_CASE("codecs.fix.builder: exact decimals, integers and the PossDup header") {
  char buf[512];
  FixBuilder b{std::span<char>(buf)};
  b.begin_header(msg::kExecutionReport, "S", "T", 12, kT0, true, kT0 - 1'000'000)
      .field_decimal(tag::kPrice, Price::from_decimal("70000.50000000").value())
      .field_decimal(tag::kOrderQty, Qty::from_raw(1))
      .field_decimal(tag::kLastPx, Price::from_int(-3))
      .field_int(tag::kRefSeqNum, -42)
      .field_uint(tag::kNewSeqNo, 18'446'744'073'709'551'615ULL)
      .field_bool(tag::kGapFillFlag, false)
      .field_char(tag::kSide, kSideSell);
  const std::size_t n = b.finish();
  REQUIRE(n > 0);
  CHECK(std::string(buf, n) ==
        wrap("35=8|49=S|56=T|34=12|43=Y|52=20260914-08:30:15.250|122=20260914-08:30:15.249|"
             "44=70000.5|38=0.00000001|31=-3|45=-42|36=18446744073709551615|123=N|54=2|"));
}

TEST_CASE("codecs.fix.builder: overflow latches and finish returns zero") {
  std::string big(512, '\0');
  FixBuilder b{std::span<char>(big.data(), big.size())};
  b.begin_header(msg::kNewOrderSingle, "CLIENT", "VENUE", 1, kT0).field(tag::kClOrdID, "abc");
  const std::size_t n = b.finish();
  REQUIRE(n > 0);
  const std::string expected(big.data(), n);

  std::string exact(n, '\0');
  FixBuilder fits{std::span<char>(exact.data(), exact.size())};
  fits.begin_header(msg::kNewOrderSingle, "CLIENT", "VENUE", 1, kT0).field(tag::kClOrdID, "abc");
  CHECK(fits.finish() == n);
  CHECK(exact == expected);

  std::string small(n - 1, '\0');
  FixBuilder overflow{std::span<char>(small.data(), small.size())};
  overflow.begin_header(msg::kNewOrderSingle, "CLIENT", "VENUE", 1, kT0)
      .field(tag::kClOrdID, "abc");
  CHECK(overflow.finish() == 0);
  CHECK_FALSE(overflow.ok());

  FixBuilder tiny{std::span<char>(small.data(), 10)};
  tiny.begin(msg::kHeartbeat);
  CHECK_FALSE(tiny.ok());
  CHECK(tiny.finish() == 0);
}
