// Bybit unified-account balances: GET /v5/account/wallet-balance and /v5/account/info
// (bybit_rest_decoder) and the private wallet topic (bybit_private_parser) into BalanceFields /
// BalanceMsg (bybit_balance.hpp). The documented examples
// (https://bybit-exchange.github.io/docs/v5/account/wallet-balance, .../account/account-info and
// .../websocket/private/wallet, read 2026-09-30); nothing here has met the real venue (no keys).
#include "fastmm/venues/bybit/bybit_balance.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/bybit/bybit_order_encoder.hpp"
#include "fastmm/venues/bybit/bybit_private_parser.hpp"
#include "fastmm/venues/bybit/bybit_rest_decoder.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {

constexpr VenueId kBybit{1};
const InstrumentId kBtc{2};

Notional n(const char* s) {
  const auto v = Notional::from_decimal(s);
  REQUIRE(v.has_value());
  return v.value_or(Notional{});
}

MdDecodeResult decode(BybitPrivateParser& p, const char* fixture, Scratch& s) {
  const auto j = padded_fixture(fixture);
  return p.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
}

const EventHeader& nth(const Scratch& s, std::uint32_t i) {
  std::uint32_t off = 0;
  for (std::uint32_t k = 0; k < i; ++k)
    off += reinterpret_cast<const EventHeader*>(s.buf + off)->len;
  return *reinterpret_cast<const EventHeader*>(s.buf + off);
}

const BalanceMsg& balance(const Scratch& s, std::uint32_t i) {
  const EventHeader& h = nth(s, i);
  REQUIRE(h.type == EventType::Balance);
  return reinterpret_cast<const BalanceMsg&>(h);
}

}  // namespace

TEST_CASE("bybit.balance: a coin's free balance is net of spot orders, margin and bonus") {
  BalanceFields f;
  REQUIRE(bybit_coin_balance(BybitCoinFields{"USDT", "1000", "10", "999", "5", "20", "2", "1", "0"},
                             f));
  CHECK(f.locked == n("35"));  // locked + totalOrderIM + totalPositionIM
  CHECK(f.free == n("964"));   // walletBalance - locked - bonus
  CHECK(f.total == n("1000"));
  CHECK(f.equity == n("999"));
  CHECK(f.maintenance == n("2"));
  // Portfolio margin leaves the margin fields "".
  REQUIRE(bybit_coin_balance(BybitCoinFields{"BTC", "1.5", "0.5", "1.5", "", "", "", "0", ""}, f));
  CHECK(f.free == n("1"));
  CHECK(f.locked == n("0.5"));
  CHECK(f.maintenance.is_zero());
  CHECK_FALSE(bybit_coin_balance(BybitCoinFields{"BTC", "abc", "0", "0", "", "", "", "", ""}, f));
}

TEST_CASE("bybit.rest_decoder: the documented wallet-balance and account info replies") {
  WalletBalance w;
  REQUIRE(
      decode_wallet_balance(fastmm::test::fixture("bybit/wallet_balance_docs.json"), w).empty());
  CHECK(w.time_ms == 1690872862481);
  REQUIRE(w.coins.size() == 1);
  CHECK(w.coins[0].coin == "BTC");
  CHECK(w.coins[0].fields.free.is_zero());
  CHECK(w.coins[0].fields.total.is_zero());
  CHECK(w.account.free == n("3.00326056"));    // totalAvailableBalance
  CHECK(w.account.locked.is_zero());           // totalInitialMargin
  CHECK(w.account.total == n("3.00326056"));   // totalWalletBalance
  CHECK(w.account.equity == n("3.31216591"));  // totalEquity
  CHECK(w.account.maintenance.is_zero());

  WalletBalance u;
  REQUIRE(
      decode_wallet_balance(fastmm::test::fixture("bybit/wallet_balance_unified.json"), u).empty());
  REQUIRE(u.coins.size() == 3);
  CHECK(u.coins[1].coin == "USDT");
  CHECK(u.coins[1].fields.locked == n("70.0001"));
  CHECK(u.coins[1].fields.free == n("9852.7999"));
  CHECK(u.coins[1].fields.maintenance == n("5"));
  CHECK(u.account.locked == n("10"));
  CHECK(u.account.maintenance == n("5"));

  WalletBalance err;
  CHECK_FALSE(
      decode_wallet_balance(
          R"({"retCode":10006,"retMsg":"Too many visits!","result":{},"retExtInfo":{},"time":1})",
          err)
          .empty());

  std::string mode;
  REQUIRE(decode_margin_mode(fastmm::test::fixture("bybit/account_info_docs.json"), mode).empty());
  CHECK(mode == "REGULAR_MARGIN");

  RestRequest rr;
  BybitOrderEncoder::encode_rest_wallet_balance(rr);
  CHECK(rr.target() == "/v5/account/wallet-balance?accountType=UNIFIED");
  BybitOrderEncoder::encode_rest_account_info(rr);
  CHECK(rr.target() == "/v5/account/info");
}

TEST_CASE("bybit.private_parser: the documented wallet push, spot") {
  TestUniverse u;
  BybitPrivateParser p(u.symbols, u.instruments, kBybit);
  Scratch s;
  // Without set_balances: the position only, as before.
  MdDecodeResult r = decode(p, "bybit/private_wallet_docs.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);
  CHECK(nth(s, 0).type == EventType::PositionUpdate);

  VenueAssets assets(u.instruments, kBybit);
  p.set_balances(&assets, false);
  r = decode(p, "bybit/private_wallet_docs.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);
  const auto& pos = reinterpret_cast<const PositionUpdateMsg&>(nth(s, 0));
  CHECK(pos.hdr.type == EventType::PositionUpdate);
  CHECK(pos.hdr.instrument == kBtc);
  CHECK(pos.qty == Qty::from_raw(n("0.00102964").raw));
  const BalanceMsg& btc = balance(s, 1);
  CHECK(btc.asset.view() == "BTC");
  CHECK(btc.hdr.venue == kBybit);
  CHECK(btc.flags == 0);
  CHECK(btc.free == n("0.00102964"));
  CHECK(btc.locked.is_zero());
  CHECK(btc.total == n("0.00102964"));
  CHECK(btc.equity == n("0.00102964"));
  CHECK(btc.maintenance.is_zero());                          // totalPositionMM ""
  CHECK(btc.hdr.exch_ts.ns == 1700034722104LL * 1'000'000);  // creationTime
  CHECK(btc.hdr.recv_ts.ns == 11);

  // Two coins: BTC and USDT, each after the positions it moves.
  r = decode(p, "bybit/private_wallet.json", s);
  REQUIRE(r.count == 3);
  CHECK(nth(s, 0).type == EventType::PositionUpdate);
  CHECK(balance(s, 1).asset.view() == "BTC");
  CHECK(balance(s, 2).asset.view() == "USDT");
  CHECK(balance(s, 2).free == n("9862.7999"));
  CHECK(balance(s, 2).locked == n("60.0001"));
  CHECK(p.stats().malformed == 0);
}

TEST_CASE("bybit_linear.private_parser: the documented wallet push, with the account row") {
  TestUniverse u;
  BybitPrivateParser p(u.symbols, u.instruments, kBybit, 1U << 20, BybitCategory::Linear);
  VenueAssets assets(u.instruments, kBybit);
  p.set_balances(&assets, true);
  Scratch s;
  const MdDecodeResult r = decode(p, "bybit/private_wallet_docs.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);  // no position from the wallet: linear has the position topic
  CHECK(balance(s, 0).asset.view() == "BTC");
  const BalanceMsg& acct = balance(s, 1);
  CHECK(acct.asset.view() == "USD");
  CHECK(acct.flags == BalanceMsg::kAccount);
  CHECK(acct.free == n("9556.6056555"));      // totalAvailableBalance
  CHECK(acct.locked.is_zero());               // totalInitialMargin
  CHECK(acct.total == n("9684.46297164"));    // totalWalletBalance
  CHECK(acct.equity == n("10262.91335023"));  // totalEquity
  CHECK(acct.maintenance.is_zero());          // totalMaintenanceMargin
  CHECK(acct.hdr.exch_ts.ns == 1700034722104LL * 1'000'000);

  const venues::PaddedJson bad(
      R"({"topic":"wallet","creationTime":1,"data":[{"accountType":"UNIFIED","coin":[{"coin":"BTC","walletBalance":"x"}]}]})");
  CHECK(p.decode(bad.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Malformed);
}
