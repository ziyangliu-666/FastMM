// Step-1 foundation types: Result, FixedString, StrongId, Fixed, enums.
#include "test_support.hpp"

#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/strong_id.hpp"

#include <map>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::literals;

namespace {
std::string dec(Price p) {
  char buf[kMaxDecimalChars];
  return std::string(buf, p.to_decimal(buf));
}
}  // namespace

TEST_CASE("core.result: value / error / and_then / map") {
  Result<int, RejectReason> ok = 42;
  REQUIRE(ok.has_value());
  CHECK(*ok == 42);
  CHECK(ok.value_or(7) == 42);
  Result<int, RejectReason> bad = fail(RejectReason::RateLimit);
  CHECK_FALSE(bad);
  CHECK(bad.error() == RejectReason::RateLimit);
  CHECK(bad.value_or(7) == 7);

  auto twice = [](const int& v) -> Result<int, RejectReason> { return v * 2; };
  CHECK(ok.and_then(twice).value() == 84);
  CHECK(bad.and_then(twice).error() == RejectReason::RateLimit);
  CHECK(ok.map([](const int& v) { return std::to_string(v); }).value() == "42");

  Result<void, int> v;
  CHECK(v);
  Result<void, int> ve = fail(3);
  CHECK(ve.error() == 3);
  CHECK(ve.and_then([] { return Result<void, int>{}; }).error() == 3);

  // non-trivial payloads are destroyed correctly (ASan/valgrind would flag leaks)
  Result<std::string, std::string> s = std::string("hello");
  Result<std::string, std::string> s2 = s;
  CHECK(s2.value() == "hello");
  s2 = Result<std::string, std::string>(fail(std::string("nope")));
  CHECK(s2.error() == "nope");
  Result<std::string, std::string> s3 = std::move(s2);
  CHECK(s3.error() == "nope");
}

TEST_CASE("core.fixed_string: assign / compare / truncate") {
  FixedString<8> a("abc");
  CHECK(a.size() == 3);
  CHECK(a == "abc");
  CHECK(std::string_view(a) == "abc");
  FixedString<8> b = a;
  CHECK(a == b);
  CHECK_FALSE(a.assign("0123456789"));
  CHECK(a.size() == 8);
  CHECK(a.view() == "01234567");
  a.clear();
  CHECK(a.empty());
  CHECK(a.push_back('x'));
  CHECK(a.view() == "x");
  CHECK(FixedString<8>("a") < FixedString<8>("b"));
  CHECK(FixedString<8>("abc").hash() != FixedString<8>("abd").hash());
  static_assert(sizeof(FixedString<40>) == 41);
}

TEST_CASE("core.strong_id: distinct types and cl_ord_id round trip") {
  InstrumentId i{3};
  InstrumentId j{3};
  CHECK(i == j);
  CHECK_FALSE(InstrumentId{}.valid());
  CHECK(VenueId{0}.valid());
  CHECK_FALSE(ClientOrderId{}.valid());
  CHECK_FALSE(Handle<int>{}.valid());
  CHECK(Handle<int>{0}.valid());

  const ClientOrderId id = make_cl_ord_id(0x1234, 0xdeadbeef);
  CHECK(cl_ord_id_epoch(id) == 0x1234);
  CHECK(cl_ord_id_seq(id) == 0xdeadbeef);
  const auto s = encode_cl_ord_id(id);
  CHECK(s.view() == "fm1234deadbeef");
  CHECK(s.size() == 14);
  const auto back = decode_cl_ord_id(s.view());
  REQUIRE(back.has_value());
  CHECK(*back == id);
  CHECK_FALSE(decode_cl_ord_id("fm1234DEADBEEF").has_value());  // uppercase rejected
  CHECK_FALSE(decode_cl_ord_id("xx1234deadbeef").has_value());
  CHECK_FALSE(decode_cl_ord_id("fm1234deadbee").has_value());
  CHECK_FALSE(decode_cl_ord_id("fm000000000000").has_value());
  CHECK(encode_cl_ord_id(make_cl_ord_id(1, 1)).view() == "fm000100000001");
  // The SWAR encoder against a per-digit reference, bits above 48 ignored.
  std::uint64_t v = 0x9E37'79B9'7F4A'7C15ULL;
  for (int n = 0; n < 2000; ++n) {
    v = v * 6364136223846793005ULL + 1442695040888963407ULL;
    std::string want = "fm";
    for (int d = 11; d >= 0; --d) want += "0123456789abcdef"[(v >> (4 * d)) & 0xF];
    CAPTURE(v);
    REQUIRE(encode_cl_ord_id(ClientOrderId{v}).view() == want);
    char raw[16] = {};
    write_cl_ord_id(raw, ClientOrderId{v});
    CHECK(std::string_view(raw, 14) == want);
    CHECK(raw[14] == 0);
  }
}

TEST_CASE("core.fixed: decimal parse / format round trip") {
  const std::vector<std::pair<std::string, std::int64_t>> cases = {
      {"0", 0},
      {"1", 100000000},
      {"0.00000001", 1},
      {"123456789.12345678", 12345678912345678LL},
      {"92233720368.54775807", 9223372036854775807LL},
      {"-92233720368.54775807", -9223372036854775807LL},
      {"0.5", 50000000},
      {"100.25", 10025000000},
      {"-0.1", -10000000},
      {"1.5", 150000000},
      {"1.50000000", 150000000},
      {"1.500000000000", 150000000},
      {".5", 50000000},
      {"5.", 500000000},
      {"+3", 300000000},
  };
  for (const auto& [str, raw] : cases) {
    CAPTURE(str);
    const auto p = Price::from_decimal(str);
    REQUIRE(p.has_value());
    CHECK(p->raw == raw);
    // format→parse round trip is identity on the raw value
    const auto again = Price::from_decimal(dec(*p));
    REQUIRE(again.has_value());
    CHECK(again->raw == raw);
  }
  CHECK(dec(Price::from_decimal("1.50000000").value()) == "1.5");
  CHECK(dec(Price::from_decimal("100").value()) == "100");
  CHECK(dec(Price::from_decimal("0.00000001").value()) == "0.00000001");
  CHECK(dec(Price::from_decimal("-0.1").value()) == "-0.1");
  CHECK(dec(Price::from_decimal("123456789.12345678").value()) == "123456789.12345678");
  CHECK(dec(Price::min()) == "-92233720368.54775808");
  CHECK(dec(Price::max()) == "92233720368.54775807");
  CHECK(dec(Price{}) == "0");

  SUBCASE("rejections") {
    for (const char* bad : {"",
                            "-",
                            ".",
                            "1e5",
                            " 1",
                            "1 ",
                            "abc",
                            "1.2.3",
                            "92233720369",
                            "92233720368.54775808",
                            "0.000000001",
                            "1,5",
                            "--1"}) {
      CAPTURE(bad);
      CHECK_FALSE(Price::from_decimal(bad).has_value());
    }
  }
  SUBCASE("exhaustive small sweep") {
    for (std::int64_t r = -1'000; r <= 1'000; ++r) {
      const auto p = Price::from_raw(r);
      const auto again = Price::from_decimal(dec(p));
      REQUIRE(again.has_value());
      CHECK(again->raw == r);
    }
    for (std::int64_t r = 0; r < 20'000'000'000'000LL; r += 12'345'678'901LL) {
      const auto p = Price::from_raw(r);
      CHECK(Price::from_decimal(dec(p))->raw == r);
    }
  }
}

TEST_CASE("core.fixed: arithmetic, mul, div, bps") {
  const Price p = Price::from_decimal("50000.5").value();
  const Qty q = Qty::from_decimal("0.002").value();
  const Notional n = mul(p, q);
  CHECK(n.raw == Notional::from_decimal("100.001").value().raw);
  // large values do not overflow via __int128
  const Notional big = mul(Price::from_int(90'000'000'000LL), Qty::from_int(1));
  CHECK(big.raw == Notional::from_int(90'000'000'000LL).raw);
  CHECK(div(n, q).raw == p.raw);
  CHECK(apply_bps(p, 10).raw == Price::from_decimal("50.0005").value().raw);
  CHECK((p + p).raw == 2 * p.raw);
  CHECK((p * 3).raw == 3 * p.raw);
  CHECK((-p).raw == -p.raw);
  CHECK(p.abs() == p);
  CHECK((-p).abs() == p);
  CHECK(100_px == Price::from_int(100));
  CHECK(2_qty == Qty::from_int(2));
  CHECK(min(p, 100_px) == 100_px);
  CHECK(max(p, 100_px) == p);
  CHECK(Price::from_double(1.23456789).raw == 123456789);
  CHECK(Price::from_double(-1.5).raw == -150000000);
  CHECK(Price::from_int(7).to_double() == doctest::Approx(7.0));
}

TEST_CASE("core.fixed: tick and lot rounding per side") {
  const Price tick = Price::from_decimal("0.01").value();
  const Price p = Price::from_decimal("100.123").value();
  CHECK(dec(round_to_tick(p, tick, Side::Buy)) == "100.12");
  CHECK(dec(round_to_tick(p, tick, Side::Sell)) == "100.13");
  CHECK(dec(round_to_tick_nearest(p, tick)) == "100.12");
  CHECK(dec(round_to_tick_nearest(Price::from_decimal("100.125").value(), tick)) == "100.13");
  const Price exact = Price::from_decimal("100.12").value();
  CHECK(round_to_tick(exact, tick, Side::Sell) == exact);
  CHECK(round_to_tick(exact, tick, Side::Buy) == exact);
  CHECK(on_tick(exact, tick));
  CHECK_FALSE(on_tick(p, tick));
  // negative prices (spreads / PnL) floor correctly
  CHECK(dec(round_to_tick(Price::from_decimal("-0.015").value(), tick, Side::Buy)) == "-0.02");
  CHECK(dec(round_to_tick(Price::from_decimal("-0.015").value(), tick, Side::Sell)) == "-0.01");
  const Qty lot = Qty::from_decimal("0.001").value();
  CHECK(round_to_lot(Qty::from_decimal("1.23456").value(), lot).raw ==
        Qty::from_decimal("1.234").value().raw);
  CHECK(on_lot(Qty::from_decimal("1.234").value(), lot));
  CHECK_FALSE(on_lot(Qty::from_decimal("1.2345").value(), lot));
}

TEST_CASE("core.enums: to_string covers every enumerator") {
  CHECK(to_string(Side::Buy) == "Buy");
  CHECK(opposite(Side::Buy) == Side::Sell);
  CHECK(sign(Side::Sell) == -1);
  CHECK(to_string(OrderType::PostOnly) == "PostOnly");
  CHECK(to_string(TimeInForce::Ioc) == "IOC");
  CHECK(to_string(OrderState::PendingReplace) == "PendingReplace");
  CHECK(is_terminal(OrderState::Filled));
  CHECK_FALSE(is_terminal(OrderState::PendingCancel));
  CHECK(is_pending(OrderState::PendingNew));
  CHECK(to_string(RejectReason::SelfTradePrevention) == "SelfTradePrevention");
  for (int i = 0; i < static_cast<int>(EventType::Count); ++i) {
    CHECK(to_string(static_cast<EventType>(i)) != "?");
  }
  CHECK(to_string(ConnState::Resyncing) == "Resyncing");
  CHECK(to_string(LogLevel::Warn) == "WARN");
  CHECK(to_string(ControlCommand::TripKill) == "TripKill");
  CHECK(to_string(Liquidity::Maker) == "Maker");
}
