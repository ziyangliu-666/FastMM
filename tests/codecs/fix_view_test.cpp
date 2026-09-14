#include "fix_test_util.hpp"

#include <utility>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;
using namespace fastmm::codecs::fix::test;

TEST_CASE("codecs.fix.view: known-answer BodyLength and CheckSum (public FIX 4.2 example)") {
  // The ExecutionReport example from the Wikipedia "Financial Information eXchange" article:
  // BodyLength 178, CheckSum 128.
  const std::string m =
      soh("8=FIX.4.2|9=178|35=8|49=PHLX|56=PERS|52=20071123-05:30:00.000|11=ATOMNOCCC9990900|20=3|"
          "150=E|39=E|55=MSFT|167=CS|54=1|38=15|40=2|44=15|58=PHLX EQUITY TESTING|59=0|47=C|32=0|"
          "31=0|151=15|14=0|6=0|10=128|");
  FixView v;
  CHECK(v.parse(m) == FixError::BadBeginString);  // FIX.4.4 expected by default
  REQUIRE(v.parse(m, "FIX.4.2") == FixError::None);
  CHECK(v.valid());
  CHECK(v.size() == 25);
  CHECK(v.begin_string() == "FIX.4.2");
  CHECK(v.msg_type() == "8");
  CHECK(v.get(tag::kSymbol) == "MSFT");
  CHECK(v.get_char(tag::kExecType) == 'E');
  CHECK(v.get_int(tag::kLeavesQty) == 15);
  CHECK(v.get(tag::kText) == "PHLX EQUITY TESTING");
  CHECK(v.get_int(tag::kBodyLength) == 178);
  CHECK(v.get_int(tag::kCheckSum) == 128);
  CHECK(v.field(v.size() - 1).tag == tag::kCheckSum);
  CHECK(v.raw().substr(v.trailer_offset(), 3) == "10=");
  // The test helper computes the same framing.
  CHECK(wrap("35=8|49=PHLX|56=PERS|52=20071123-05:30:00.000|11=ATOMNOCCC9990900|20=3|150=E|39=E|"
             "55=MSFT|167=CS|54=1|38=15|40=2|44=15|58=PHLX EQUITY TESTING|59=0|47=C|32=0|31=0|"
             "151=15|14=0|6=0|",
             "FIX.4.2") == m);
  CHECK(v.parse(m, "") == FixError::None);  // any BeginString
}

TEST_CASE("codecs.fix.view: framing errors are detected") {
  const std::string good = wrap("35=0|49=A|56=B|34=1|52=20260914-08:00:00.000|");
  FixView v;
  REQUIRE(v.parse(good) == FixError::None);

  std::string bad_cks = good;
  char& digit = bad_cks[bad_cks.size() - 2];
  digit = digit == '0' ? '1' : '0';
  CHECK(v.parse(bad_cks) == FixError::BadCheckSum);
  CHECK_FALSE(v.valid());
  CHECK(v.parse(bad_cks, kBeginString44, false) == FixError::None);

  std::string flipped = good;
  flipped[good.find("49=A") + 3] = 'C';
  CHECK(v.parse(flipped) == FixError::BadCheckSum);

  std::string wrong_len = good;
  const std::size_t p = wrong_len.find("9=") + 2;
  wrong_len[p] = static_cast<char>(wrong_len[p] + 1);
  CHECK(v.parse(wrong_len) == FixError::BadBodyLength);
  CHECK(v.parse(std::string_view(good).substr(0, good.size() - 1)) == FixError::BadBodyLength);
  CHECK(v.parse(good + "x") == FixError::BadBodyLength);
  CHECK(v.parse(std::string_view{}) == FixError::Truncated);
  CHECK(v.parse(soh("9=5|8=FIX.4.4|35=0|10=000|")) == FixError::BadBeginString);
  CHECK(v.parse(wrap("35=0|49=A|", "FIX.4.2")) == FixError::BadBeginString);
  CHECK(v.parse(wrap("49=A|35=0|56=B|34=1|")) == FixError::BadMsgType);
  CHECK(v.parse(wrap("35=0|49=|56=B|34=1|")) == FixError::BadField);
  CHECK(v.parse(wrap("35=0|4x9=A|56=B|34=1|")) == FixError::BadField);
  CHECK(v.parse(wrap("35=0|049=A|56=B|34=1|")) == FixError::BadField);
  CHECK(v.parse(wrap("35=0|0=A|56=B|34=1|")) == FixError::BadField);
  CHECK(v.parse(wrap("35=0|49A|56=B|34=1|")) == FixError::BadField);
  // A valid message after errors parses again (index fully reset).
  REQUIRE(v.parse(good) == FixError::None);
  CHECK(v.get(tag::kSenderCompID) == "A");
  CHECK_FALSE(v.has(tag::kSymbol));
}

TEST_CASE("codecs.fix.view: typed getters are exact") {
  const std::string m = wrap(
      "35=8|34=7|44=70000.12345678|38=0.00000001|31=-1.5|14=100|32=1.000000000|"
      "52=20260914-08:30:15.250|60=20260914-08:30:15|122=20260914-08:30:15.123456|"
      "43=Y|97=N|58=a=b c|99=+1|100=1e5|101=0.000000001|102=2026-09-14|");
  FixView v;
  REQUIRE(v.parse(m) == FixError::None);
  CHECK(v.get_price(tag::kPrice)->raw == 7'000'012'345'678);
  CHECK(v.get_qty(tag::kOrderQty)->raw == 1);
  CHECK(v.get_price(tag::kLastPx)->raw == -150'000'000);
  CHECK(v.get_qty(tag::kCumQty)->raw == 100 * kFixedScale);
  CHECK(v.get_qty(tag::kLastQty)->raw == kFixedScale);
  CHECK(v.get_bool(tag::kPossDupFlag) == true);
  CHECK(v.get_bool(tag::kPossResend) == false);
  CHECK(v.get(tag::kText) == "a=b c");
  CHECK_FALSE(v.get_price(99).has_value());   // '+' is not a FIX float
  CHECK_FALSE(v.get_price(100).has_value());  // no exponent
  CHECK_FALSE(v.get_qty(101).has_value());    // below 1e-8
  CHECK_FALSE(v.get_timestamp_ns(102).has_value());
  CHECK_FALSE(v.get_int(tag::kText).has_value());
  CHECK_FALSE(v.get_char(tag::kPrice).has_value());
  CHECK_FALSE(v.get_int(999).has_value());
  CHECK_FALSE(v.has(999));
  CHECK(v.find(tag::kMsgSeqNum) == 3);
  CHECK(v.find(tag::kMsgSeqNum, 4) == FixView::npos);

  // 2026-09-14T08:30:15Z == 1789374615 s since the epoch.
  CHECK(v.get_timestamp_ns(tag::kSendingTime) == 1'789'374'615'250'000'000);
  CHECK(v.get_timestamp_ns(tag::kTransactTime) == 1'789'374'615'000'000'000);
  CHECK(v.get_timestamp_ns(tag::kOrigSendingTime) == 1'789'374'615'123'456'000);
}

TEST_CASE("codecs.fix.view: UTCTimestamp parse and format") {
  CHECK(parse_utc_timestamp("19700101-00:00:00") == 0);
  CHECK(parse_utc_timestamp("20000301-00:00:00.000") == 951'868'800'000'000'000);
  CHECK(parse_utc_timestamp("20240229-00:00:00") == 1'709'164'800'000'000'000);
  CHECK(parse_utc_timestamp("20260914-08:30:15.123456789") == 1'789'374'615'123'456'789);
  CHECK_FALSE(parse_utc_timestamp("20261314-08:30:15").has_value());
  CHECK_FALSE(parse_utc_timestamp("20260914 08:30:15").has_value());
  CHECK_FALSE(parse_utc_timestamp("20260914-24:00:00").has_value());
  CHECK_FALSE(parse_utc_timestamp("20260914-08:30:15.25").has_value());
  CHECK_FALSE(parse_utc_timestamp("20260914-08:30:15,250").has_value());
  char buf[kUtcTimestampMillisChars];
  CHECK(std::string_view(buf, format_utc_timestamp(1'789'374'615'250'999'999, buf)) ==
        "20260914-08:30:15.250");
  CHECK(std::string_view(buf, format_utc_timestamp(951'868'799'999'000'000, buf)) ==
        "20000229-23:59:59.999");
  // Round trip across ~150 years in 37-day steps with millisecond precision.
  for (std::int64_t days = 0; days < 55'000; days += 37) {
    const std::int64_t ns = days * 86'400'000'000'000 + 45'296'789'000'000;
    const std::size_t n = format_utc_timestamp(ns, buf);
    REQUIRE(parse_utc_timestamp(std::string_view(buf, n)) == ns);
  }
}

TEST_CASE("codecs.fix.view: repeating groups and data fields") {
  FixView v;
  const std::string m =
      wrap("35=W|55=BTCUSDT|268=3|269=0|270=100|271=1|269=1|270=101|271=2|269=0|270=99|271=3|");
  REQUIRE(v.parse(m) == FixError::None);
  FixGroupReader g(v, tag::kNoMDEntries, tag::kMDEntryType);
  FixRange e;
  std::vector<std::pair<char, std::int64_t>> seen;
  while (g.next(e)) {
    seen.emplace_back(e.get_char(tag::kMDEntryType).value(), e.get_price(tag::kMDEntryPx)->units());
    CHECK(e.size() == 3);
    CHECK_FALSE(e.has(tag::kSymbol));  // lookups stay inside the entry
  }
  CHECK(g.present());
  CHECK(g.complete());
  CHECK(g.declared() == 3);
  CHECK(seen == std::vector<std::pair<char, std::int64_t>>{{'0', 100}, {'1', 101}, {'0', 99}});

  // The view refers to the message bytes: keep each message alive while the view is used.
  const std::string short_msg = wrap("35=W|268=3|269=0|270=100|269=1|270=101|");
  REQUIRE(v.parse(short_msg) == FixError::None);
  FixGroupReader short_group(v, tag::kNoMDEntries, tag::kMDEntryType);
  while (short_group.next(e)) {
  }
  CHECK(short_group.read() == 2);
  CHECK_FALSE(short_group.complete());

  const std::string members_msg = wrap("35=X|268=2|279=0|269=0|270=1|279=2|269=1|270=2|58=done|");
  REQUIRE(v.parse(members_msg) == FixError::None);
  const std::uint32_t members[] = {tag::kMDUpdateAction, tag::kMDEntryType, tag::kMDEntryPx};
  FixGroupReader with_members(v, tag::kNoMDEntries, tag::kMDUpdateAction, members);
  REQUIRE(with_members.next(e));
  REQUIRE(with_members.next(e));
  CHECK(e.size() == 3);
  CHECK_FALSE(e.has(tag::kText));
  CHECK_FALSE(with_members.next(e));
  CHECK(with_members.complete());
  CHECK(v.get(tag::kText) == "done");

  FixGroupReader absent(v, 146, 55);
  CHECK_FALSE(absent.present());
  CHECK_FALSE(absent.next(e));

  // EncodedText(355) may contain SOH: its length comes from EncodedTextLen(354).
  const std::string body = soh("35=B|354=5|355=") +
                           std::string(
                               "a\x01"
                               "b\x01"
                               "c",
                               5) +
                           soh("|58=x|");
  const std::string data_msg = wrap(body, kBeginString44, false);
  REQUIRE(v.parse(data_msg) == FixError::None);
  CHECK(v.get(355) == std::string_view("a\x01"
                                       "b\x01"
                                       "c",
                                       5));
  CHECK(v.get(tag::kText) == "x");
}
