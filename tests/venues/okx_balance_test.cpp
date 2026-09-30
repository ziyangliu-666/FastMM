// OKX balances: GET /api/v5/account/balance (okx_rest_decoder) and the private account channel
// (okx_private_parser) into BalanceFields / BalanceMsg per account mode (okx_balance.hpp). The
// documented examples (https://www.okx.com/docs-v5/en/#trading-account-rest-api-get-balance and
// #trading-account-websocket-account-channel, read 2026-09-30) and a reply and a push recorded on
// the demo account (spot mode) on 2026-09-30.
#include "fastmm/venues/okx/okx_balance.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/okx/okx_order_encoder.hpp"
#include "fastmm/venues/okx/okx_private_parser.hpp"
#include "fastmm/venues/okx/okx_rest_decoder.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::okx;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kOkx{1};

Notional n(const char* s) {
  const auto v = Notional::from_decimal(s);
  REQUIRE(v.has_value());
  return v.value_or(Notional{});
}

// BTC-USDT on OKX: the kept assets are BTC and USDT.
struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  VenueAssets assets;
  Universe() {
    REQUIRE(instruments.add(make_instrument("BTC-USDT", 1, "BTC", "USDT")));
    REQUIRE(symbols.build(instruments));
    assets.build(instruments, kOkx);
  }
};

PrivateDecodeResult decode(OkxPrivateParser& p, const std::string& json, Scratch& s) {
  const PaddedJson j(json);
  return p.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
}

const BalanceMsg& nth(const Scratch& s, std::uint32_t i) {
  return *reinterpret_cast<const BalanceMsg*>(s.buf + i * sizeof(BalanceMsg));
}

const BalanceRow* row(const AccountBalance& b, std::string_view ccy) {
  for (const BalanceRow& r : b.rows) {
    if (r.ccy == ccy) return &r;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("okx.balance: the account mode comes from acctLv") {
  CHECK(parse_account_mode("1") == OkxAccountMode::Spot);
  CHECK(parse_account_mode("2") == OkxAccountMode::Futures);
  CHECK(parse_account_mode("3") == OkxAccountMode::MultiCurrency);
  CHECK(parse_account_mode("4") == OkxAccountMode::Portfolio);
  CHECK(parse_account_mode("") == OkxAccountMode::Unknown);
  CHECK_FALSE(has_account_row(OkxAccountMode::Spot));
  CHECK_FALSE(has_account_row(OkxAccountMode::Futures));
  CHECK(has_account_row(OkxAccountMode::MultiCurrency));
  CHECK(has_account_row(OkxAccountMode::Portfolio));
}

TEST_CASE("okx.rest_decoder: the documented account/balance reply per account mode") {
  const std::string doc = fastmm::test::fixture("okx/account_balance_docs.json");

  // Futures mode: free is the currency's availEq, locked frozenBal, total cashBal, no account row.
  AccountBalance fut;
  REQUIRE(decode_balance(doc, OkxAccountMode::Futures, fut).empty());
  CHECK(fut.u_time_ms == 1705474164160);
  CHECK_FALSE(fut.has_account);
  REQUIRE(fut.rows.size() == 1);
  const BalanceFields& f = fut.rows[0].fields;
  CHECK(fut.rows[0].ccy == "USDT");
  CHECK(f.free == n("4834.31709362"));  // availEq 4834.3170936228935, rounded
  CHECK(f.locked == n("158.573"));
  CHECK(f.total == n("4850.43569362"));  // cashBal 4850.435693622894
  CHECK(f.equity == n("4992.89009362"));
  CHECK(f.maintenance.is_zero());  // mmr ""

  // Multi-currency margin: availBal per currency, and the account's USD margin.
  AccountBalance mcm;
  REQUIRE(decode_balance(doc, OkxAccountMode::MultiCurrency, mcm).empty());
  REQUIRE(mcm.rows.size() == 1);
  CHECK(mcm.rows[0].fields.free == n("4834.31709362"));  // availBal 4834.317093622894
  CHECK(mcm.rows[0].fields.total == n("4850.43569362"));
  REQUIRE(mcm.has_account);
  CHECK(mcm.account.free == n("55415.62471983"));  // availEq - imr (0)
  CHECK(mcm.account.locked.is_zero());
  CHECK(mcm.account.total == n("55837.43556135"));   // totalEq 55837.43556134779, rounded up
  CHECK(mcm.account.equity == n("55415.62471983"));  // adjEq
  CHECK(mcm.account.maintenance.is_zero());

  AccountBalance err;
  CHECK_FALSE(decode_balance(R"({"code":"50011","msg":"Rate limit reached.","data":[]})",
                             OkxAccountMode::Spot,
                             err)
                  .empty());
  CHECK_FALSE(
      decode_balance(
          R"({"code":"0","msg":"","data":[{"uTime":"1","details":[{"ccy":"BTC","availBal":"x"}]}]})",
          OkxAccountMode::Spot,
          err)
          .empty());
}

TEST_CASE("okx.rest_decoder: a recorded demo account/balance in spot mode") {
  AccountBalance b;
  REQUIRE(decode_balance(
              fastmm::test::fixture("okx/account_balance_spot_demo.json"), OkxAccountMode::Spot, b)
              .empty());
  CHECK(b.u_time_ms == 1790742198676);
  CHECK_FALSE(b.has_account);  // adjEq, imr and mmr are "" in spot mode without borrowing
  CHECK(b.rows.size() == 6);   // BTC, SGD, USDT, USD, USDC, ETH
  const BalanceRow* btc = row(b, "BTC");
  const BalanceRow* usdt = row(b, "USDT");
  REQUIRE(btc != nullptr);
  REQUIRE(usdt != nullptr);
  CHECK(btc->fields.free == n("0.99991918"));
  CHECK(btc->fields.locked.is_zero());
  CHECK(btc->fields.total == n("0.99991918"));
  CHECK(btc->fields.equity == n("0.99991918"));
  CHECK(usdt->fields.free == n("5006.64453598"));  // availBal 5006.644535984, rounded
  CHECK(usdt->fields.total == n("5006.64453598"));
}

TEST_CASE("okx.private_parser: the documented account push, per account mode") {
  Universe u;
  OkxPrivateParser p(u.symbols, u.instruments, kOkx);
  Scratch s;
  const std::string doc = fastmm::test::fixture("okx/private_account_docs.json");

  // Without set_balances the channel is not read.
  CHECK(decode(p, doc, s).status == ParseStatus::Ignored);

  // Multi-currency margin: USDT (BTC is not in the push), then the account row.
  p.set_balances(&u.assets, OkxAccountMode::MultiCurrency);
  PrivateDecodeResult r = decode(p, doc, s);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);
  const BalanceMsg& usdt = nth(s, 0);
  CHECK(usdt.hdr.type == EventType::Balance);
  CHECK(usdt.hdr.venue == kOkx);
  CHECK_FALSE(usdt.hdr.instrument.valid());
  CHECK(usdt.asset.view() == "USDT");
  CHECK(usdt.flags == 0);
  CHECK(usdt.free == n("4734.37119069"));  // availBal 4734.371190691436
  CHECK(usdt.locked == n("158.57998"));
  CHECK(usdt.total == n("4750.42697069"));  // cashBal
  CHECK(usdt.equity == n("4892.95117069"));
  CHECK(usdt.maintenance.is_zero());
  CHECK(usdt.hdr.exch_ts.ns == 1705564213903LL * 1'000'000);  // the currency's uTime
  CHECK(usdt.hdr.recv_ts.ns == 11);
  const BalanceMsg& acct = nth(s, 1);
  CHECK(acct.asset.view() == "USD");
  CHECK(acct.flags == BalanceMsg::kAccount);
  CHECK(acct.free == n("55444.12216906"));  // availEq - imr (0)
  CHECK(acct.locked.is_zero());
  CHECK(acct.total == n("55868.06403502"));   // totalEq 55868.06403501676, rounded up
  CHECK(acct.equity == n("55444.12216906"));  // adjEq
  CHECK(acct.hdr.exch_ts.ns == 1705564223311LL * 1'000'000);  // the account's uTime

  // Spot mode: availBal and frozenBal, total their sum; no account row.
  p.set_balances(&u.assets, OkxAccountMode::Spot);
  r = decode(p, doc, s);
  REQUIRE(r.count == 1);
  CHECK(nth(s, 0).free == n("4734.37119069"));
  CHECK(nth(s, 0).locked == n("158.57998"));
  CHECK(nth(s, 0).total == n("4892.95117069"));
  CHECK(nth(s, 0).equity == n("4892.95117069"));

  // Futures mode: the currency's availEq.
  p.set_balances(&u.assets, OkxAccountMode::Futures);
  r = decode(p, doc, s);
  REQUIRE(r.count == 1);
  CHECK(nth(s, 0).free == n("4734.37119069"));  // availEq 4734.371190691435
  CHECK(nth(s, 0).total == n("4750.42697069"));
  CHECK(p.stats().malformed == 0);
}

TEST_CASE("okx.private_parser: a recorded demo account push keeps the instruments' currencies") {
  Universe u;
  OkxPrivateParser p(u.symbols, u.instruments, kOkx);
  p.set_balances(&u.assets, OkxAccountMode::Spot);
  Scratch s;
  const PrivateDecodeResult r =
      decode(p, fastmm::test::fixture("okx/private_account_spot_demo.json"), s);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);  // of BTC, SGD, USDT, USD, USDC and ETH
  CHECK(nth(s, 0).asset.view() == "BTC");
  CHECK(nth(s, 0).free == n("0.99991918"));
  CHECK(nth(s, 0).total == n("0.99991918"));
  CHECK(nth(s, 0).hdr.exch_ts.ns == 1790736818653LL * 1'000'000);
  CHECK(nth(s, 1).asset.view() == "USDT");
  CHECK(nth(s, 1).free == n("5006.64453598"));
  CHECK(nth(s, 1).hdr.exch_ts.ns == 1790736818653LL * 1'000'000);
}

TEST_CASE("okx.private_parser: an account update without a currency uTime takes the account's") {
  Universe u;
  OkxPrivateParser p(u.symbols, u.instruments, kOkx);
  p.set_balances(&u.assets, OkxAccountMode::Spot);
  Scratch s;
  // An event_update naming BTC only: the other currencies did not change.
  PrivateDecodeResult r = decode(
      p,
      R"({"arg":{"channel":"account","uid":"77"},"eventType":"event_update","data":[{"details":[{"ccy":"btc","availBal":"0.5","frozenBal":"0.25","cashBal":"0.75","eq":"0.75"}],"totalEq":"1","uTime":"1790000000000"}]})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  CHECK(nth(s, 0).asset.view() == "BTC");  // the instruments' spelling
  CHECK(nth(s, 0).free == n("0.5"));
  CHECK(nth(s, 0).locked == n("0.25"));
  CHECK(nth(s, 0).total == n("0.75"));
  CHECK(nth(s, 0).hdr.exch_ts.ns == 1790000000000LL * 1'000'000);

  r = decode(
      p,
      R"({"arg":{"channel":"account"},"data":[{"details":[{"ccy":"BTC","availBal":"1e3"}],"uTime":"1"}]})",
      s);
  CHECK(r.status == ParseStatus::Malformed);
}

TEST_CASE("okx.encoder: the account channel and account/balance") {
  char buf[1024];
  constexpr std::string_view kChannels[] = {"orders", "account"};
  const std::size_t len =
      OkxOrderEncoder::encode_private_subscribe("private", kChannels, "SPOT", buf);
  CHECK(
      std::string_view(buf, len) ==
      R"({"id":"private","op":"subscribe","args":[{"channel":"orders","instType":"SPOT"},{"channel":"account","extraParams":"{\"updateInterval\":\"0\"}"}]})");
  RestRequest rr;
  OkxOrderEncoder::encode_rest_balance(rr);
  CHECK(rr.method == "GET");
  CHECK(rr.path == "/api/v5/account/balance");
  CHECK(rr.body.empty());
}
