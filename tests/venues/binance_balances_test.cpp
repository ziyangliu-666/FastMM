// Balances on Binance Spot and Binance USDⓈ-M: the account requests, the account replies and the
// user-stream events, from the documented examples (tests/fixtures/*/fixtures.meta.json).
#include "venue_test_util.hpp"

#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/binance/binance_rest_decoder.hpp"
#include "fastmm/venues/binance/binance_user_parser.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_order_encoder.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_rest_decoder.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_user_parser.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {

const Timestamp kRecv{1'700'000'000'000'000'000LL};
const Cycles kT0{42};
constexpr std::int64_t kTs = 1789295199000;

Notional n(const char* s) {
  return Notional::from_decimal(s).value();
}

binance::Signer signer() {
  binance::Credentials c;
  c.api_key = "k";
  c.secret.value = "s";
  return binance::Signer(c);
}

// The messages a decode wrote, in order.
std::vector<const EventHeader*> messages(const Scratch& s, std::uint32_t count) {
  std::vector<const EventHeader*> out;
  std::uint32_t off = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto* h = reinterpret_cast<const EventHeader*>(s.buf + off);
    out.push_back(h);
    off += h->len;
  }
  return out;
}

}  // namespace

// ---- Binance Spot ------------------------------------------------------------------------------

TEST_CASE("binance.balances: the account requests ask for the non-zero balances only") {
  TestUniverse u;
  const binance::Signer s = signer();
  binance::BinanceOrderEncoder enc(s, u.symbols, 5000);
  binance::RestRequest rr;
  REQUIRE(enc.encode_rest_account(kTs, rr));
  CHECK(rr.method == "GET");
  CHECK(rr.path == "/api/v3/account");
  CHECK(rr.weight == 20);
  CHECK(
      std::string(rr.query.view())
          .starts_with("omitZeroBalances=true&recvWindow=5000&timestamp=1789295199000&signature="));

  char buf[binance::kMaxRequestBytes];
  const std::size_t len = enc.encode_ws_account_status("bal", kTs, buf);
  REQUIRE(len > 0);
  const std::string frame(buf, len);
  CHECK(frame.find(R"("method":"account.status")") != std::string::npos);
  CHECK(frame.find(R"("id":"bal")") != std::string::npos);
  CHECK(frame.find(R"("omitZeroBalances":true)") != std::string::npos);
  CHECK(frame.find(R"("signature":")") != std::string::npos);
}

TEST_CASE("binance.balances: the documented account replies, REST and WebSocket API") {
  for (const char* fixture : {"binance/account_rest.json", "binance/ws_api_account_status.json"}) {
    CAPTURE(fixture);
    std::vector<binance::AccountBalance> rows;
    std::int64_t update_ms = -1;
    REQUIRE(binance::decode_account_balances(fastmm::test::fixture(fixture), rows, update_ms) ==
            "");
    const bool ws = std::string_view(fixture).find("ws_api") != std::string_view::npos;
    if (ws) {
      CHECK(update_ms == 1660801833000);
      REQUIRE(rows.size() == 3);
      CHECK(rows[0].asset == "BNB");
      CHECK(rows[0].free.is_zero());
      CHECK(rows[1].asset == "BTC");
      CHECK(rows[1].free == n("1.3447112"));
      CHECK(rows[1].locked == n("0.086"));
      CHECK(rows[2].asset == "USDT");
      CHECK(rows[2].free == n("1021.21"));
    } else {
      CHECK(update_ms == 123456789);
      REQUIRE(rows.size() == 2);
      CHECK(rows[0].asset == "BTC");
      CHECK(rows[0].free == n("4723846.89208129"));
      CHECK(rows[0].locked.is_zero());
      CHECK(rows[1].asset == "LTC");
      CHECK(rows[1].free == n("4763368.68006011"));
    }
  }
  std::vector<binance::AccountBalance> rows;
  std::int64_t t = 0;
  CHECK(binance::decode_account_balances(R"({"code":-2015,"msg":"Invalid API-key"})", rows, t) !=
        "");
  CHECK(binance::decode_account_balances("{", rows, t) != "");
}

TEST_CASE("binance.balances: the documented outboundAccountPosition -> BalanceMsg") {
  // user-data-stream.md "Account Update" (read 2026-09-30), the comments removed.
  const PaddedJson frame(R"({
    "subscriptionId": 0,
    "event": {
        "e": "outboundAccountPosition",
        "E": 1564034571105,
        "u": 1564034571073,
        "B": [
            {
                "a": "ETH",
                "f": "10000.000000",
                "l": "0.000000"
            }
        ]
    }
})");
  TestUniverse u;
  const VenueAssets assets(u.instruments, VenueId{0});
  binance::BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  p.set_balance_assets(&assets);
  Scratch s;
  const auto r = p.decode(frame.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);  // ETH's balance, then ETHUSDT's position
  const auto msgs = messages(s, r.count);
  REQUIRE(msgs[0]->type == EventType::Balance);
  const auto& b = msg_cast<BalanceMsg>(msgs[0]);
  CHECK(b.asset.view() == "ETH");
  CHECK(b.free == n("10000"));
  CHECK(b.locked.is_zero());
  CHECK(b.total == n("10000"));
  CHECK(b.equity == n("10000"));
  CHECK(b.maintenance.is_zero());
  CHECK(b.flags == 0);
  CHECK_FALSE(b.hdr.instrument.valid());
  CHECK(b.hdr.venue == VenueId{0});
  CHECK(b.hdr.exch_ts.ns == 1564034571073LL * 1'000'000);  // u, the account update
  CHECK(b.hdr.recv_ts == kRecv);
  CHECK(b.hdr.t0_cycles == kT0);
  CHECK(msgs[1]->type == EventType::PositionUpdate);
  CHECK(msgs[1]->instrument == InstrumentId{1});
  CHECK(p.stats().balances == 1);
}

TEST_CASE("binance.balances: outboundAccountPosition forwards only the assets the engine keeps") {
  TestUniverse u;
  binance::BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  // BTC and USDT kept (BTCUSDT on venue 0); BNB is not an asset of any instrument.
  InstrumentTable only_btc;
  REQUIRE(only_btc.add(fastmm::venues::test::make_instrument("BTCUSDT", 0, "BTC", "USDT")));
  const VenueAssets assets(only_btc, VenueId{0});
  p.set_balance_assets(&assets);
  const auto fx = padded_fixture("binance/outbound_account_position.json");
  const auto r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 3);
  const auto msgs = messages(s, r.count);
  CHECK(msgs[0]->type == EventType::Balance);
  CHECK(msg_cast<BalanceMsg>(msgs[0]).asset.view() == "BTC");
  CHECK(msg_cast<BalanceMsg>(msgs[0]).free == n("1.2345"));
  CHECK(msg_cast<BalanceMsg>(msgs[0]).locked == n("0.001"));
  CHECK(msg_cast<BalanceMsg>(msgs[0]).total == n("1.2355"));
  CHECK(msg_cast<BalanceMsg>(msgs[0]).hdr.exch_ts.ns == 1789295200990LL * 1'000'000);
  CHECK(msgs[1]->type == EventType::PositionUpdate);
  CHECK(msgs[2]->type == EventType::Balance);
  CHECK(msg_cast<BalanceMsg>(msgs[2]).asset.view() == "USDT");
  CHECK(msg_cast<BalanceMsg>(msgs[2]).free == n("9500"));
  CHECK(msg_cast<BalanceMsg>(msgs[2]).locked == n("70"));

  const PaddedJson bnb(
      R"({"e":"outboundAccountPosition","E":2,"u":1,"B":[{"a":"BNB","f":"1.00000000","l":"0.00000000"}]})");
  CHECK(p.decode(bnb.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
}

TEST_CASE("binance.balances: the recorded Demo Mode account.status and outboundAccountPosition") {
  std::vector<binance::AccountBalance> rows;
  std::int64_t update_ms = 0;
  REQUIRE(binance::decode_account_balances(
              fastmm::test::fixture("binance/demo_account_status.json"), rows, update_ms) == "");
  REQUIRE(rows.size() == 3);  // omitZeroBalances: BTC, USDT, USDC
  CHECK(rows[0].asset == "BTC");
  CHECK(rows[0].free == n("0.01485691"));
  CHECK(rows[1].asset == "USDT");
  CHECK(rows[1].free == n("3689.34023246"));
  CHECK(rows[2].asset == "USDC");
  CHECK(update_ms == 1790686454570);

  // After a buy of 0.00009 BTC at 66584 was placed: 5.99256 USDT locked.
  TestUniverse u;
  const VenueAssets assets(u.instruments, VenueId{0});
  binance::BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  p.set_balance_assets(&assets);
  Scratch s;
  const auto fx = padded_fixture("binance/demo_outbound_account_position.json");
  const auto r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 3);  // BTC balance, BTCUSDT position, USDT balance (BNB is not kept)
  const auto msgs = messages(s, r.count);
  CHECK(msg_cast<BalanceMsg>(msgs[0]).asset.view() == "BTC");
  CHECK(msgs[1]->type == EventType::PositionUpdate);
  const auto& usdt = msg_cast<BalanceMsg>(msgs[2]);
  CHECK(usdt.asset.view() == "USDT");
  CHECK(usdt.free == n("3683.34767246"));
  CHECK(usdt.locked == n("5.99256"));
  CHECK(usdt.free + usdt.locked == rows[1].free);
  CHECK(usdt.hdr.exch_ts.ns == 1790743940971LL * 1'000'000);
}

// ---- Binance USDⓈ-M ----------------------------------------------------------------------------

TEST_CASE("binance_usdm.balances: the account request") {
  TestUniverse u;
  const binance::Signer s = signer();
  binance_usdm::BinanceUsdmOrderEncoder enc(s, u.symbols, 5000);
  binance::RestRequest rr;
  REQUIRE(enc.encode_rest_account(kTs, rr));
  CHECK(rr.method == "GET");
  CHECK(rr.path == "/fapi/v3/account");
  CHECK(rr.weight == 5);
  CHECK(std::string(rr.query.view()).starts_with("recvWindow=5000&timestamp=1789295199000"));
}

TEST_CASE("binance_usdm.balances: the documented account replies, single-asset and multi-assets") {
  {
    binance_usdm::FuturesAccount a;
    REQUIRE(binance_usdm::decode_account(
                fastmm::test::fixture("binance_usdm/account_v3_single.json"), a) == "");
    CHECK(a.total.available == n("103.12345678"));
    CHECK(a.total.wallet == n("103.12345678"));
    CHECK(a.total.margin == n("103.12345678"));
    CHECK(a.total.initial.is_zero());
    CHECK(a.total.maintenance.is_zero());
    REQUIRE(a.assets.size() == 2);
    CHECK(a.assets[0].asset == "USDT");
    CHECK(a.assets[0].m.available == n("23.72469206"));
    CHECK(a.assets[0].m.wallet == n("23.72469206"));
    CHECK(a.assets[0].m.margin == n("23.72469206"));
    CHECK(a.assets[0].update_time_ms == 1625474304765);
    CHECK(a.assets[1].asset == "USDC");
    CHECK(a.assets[1].m.available == n("126.72469206"));
    CHECK(a.assets[1].m.max_withdraw == n("103.12345678"));
  }
  {
    binance_usdm::FuturesAccount a;
    REQUIRE(binance_usdm::decode_account(
                fastmm::test::fixture("binance_usdm/account_v3_multi.json"), a) == "");
    CHECK(a.total.available == n("126.72469206"));
    CHECK(a.total.wallet == n("126.72469206"));
    REQUIRE(a.assets.size() == 2);
    CHECK(a.assets[0].m.available == n("126.72469206"));  // the account's, in USD
    CHECK(a.assets[0].m.max_withdraw == n("23.72469206"));
    CHECK(a.assets[1].asset == "BUSD");
  }
  {
    // Recorded on Demo Trading, single-asset mode: every margin asset listed, zero or not; the
    // totals are USDT's alone (USDC holds 5000).
    binance_usdm::FuturesAccount a;
    REQUIRE(binance_usdm::decode_account(fastmm::test::fixture("binance_usdm/demo_account_v3.json"),
                                         a) == "");
    CHECK(a.assets.size() == 8);
    CHECK(a.total.wallet == n("4976.05154461"));
    const auto usdt = std::find_if(
        a.assets.begin(), a.assets.end(), [](const auto& x) { return x.asset == "USDT"; });
    REQUIRE(usdt != a.assets.end());
    CHECK(usdt->m.available == n("4976.05154461"));
    CHECK(usdt->m.wallet == a.total.wallet);
    CHECK(usdt->update_time_ms == 1790686551779);
  }
  binance_usdm::FuturesAccount a;
  CHECK(binance_usdm::decode_account(R"({"assets":[]})", a) != "");
  bool multi = false;
  REQUIRE(binance_usdm::decode_multi_assets_mode(R"({"multiAssetsMargin":true})", multi) == "");
  CHECK(multi);
  CHECK(binance_usdm::decode_multi_assets_mode("{}", multi) != "");
}

TEST_CASE("binance_usdm.balances: ACCOUNT_UPDATE with balances asks for the account") {
  TestUniverse u;
  binance_usdm::BinanceUsdmUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  {
    // The documented example: B[] names USDT and BUSD; the one-way BTCUSDT position is flat.
    const auto fx = padded_fixture("binance_usdm/account_update_docs.json");
    const auto r = p.decode(fx.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.balances);
    CHECK(r.count == 1);
    CHECK(s.as<PositionUpdateMsg>().qty.is_zero());
  }
  {
    // Balance only (a crossed funding payment).
    const PaddedJson funding(
        R"({"e":"ACCOUNT_UPDATE","E":1789500000001,"T":1789500000000,"a":{"m":"FUNDING_FEE","S":"BTCUSDT","B":[{"a":"USDT","wb":"4999.625","cw":"4999.625","bc":"-0.375"}]}})");
    const auto r = p.decode(funding.view(), kRecv, kT0, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.balances);
    CHECK(r.funding);
  }
  {
    const PaddedJson none(
        R"({"e":"ACCOUNT_UPDATE","E":1,"T":1,"a":{"m":"ORDER","B":[],"P":[{"s":"BTCUSDT","pa":"0.001","ep":"70000","bep":"0","cr":"0","up":"0","mt":"cross","iw":"0","ps":"BOTH"}]}})");
    const auto r = p.decode(none.view(), kRecv, kT0, s.span());
    CHECK(r.status == ParseStatus::Ok);
    CHECK_FALSE(r.balances);
  }
  CHECK(p.stats().balance_events == 2);
}

TEST_CASE("binance_usdm.balances: ACCOUNT_CONFIG_UPDATE says the multi-assets mode") {
  TestUniverse u;
  binance_usdm::BinanceUsdmUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance_usdm/account_config_update_multi_assets.json");
  const auto on = p.decode(fx.view(), kRecv, kT0, s.span());
  CHECK(on.status == ParseStatus::Ignored);
  CHECK(on.multi_assets == 1);
  const PaddedJson off(R"({"e":"ACCOUNT_CONFIG_UPDATE","E":1,"T":1,"ai":{"j":false}})");
  CHECK(p.decode(off.view(), kRecv, kT0, s.span()).multi_assets == 0);
  // The leverage one (the first documented payload) says nothing about the mode.
  const PaddedJson leverage(
      R"({"e":"ACCOUNT_CONFIG_UPDATE","E":1611646737479,"T":1611646737476,"ac":{"s":"BTCUSDT","l":25}})");
  CHECK(p.decode(leverage.view(), kRecv, kT0, s.span()).multi_assets == -1);
}
