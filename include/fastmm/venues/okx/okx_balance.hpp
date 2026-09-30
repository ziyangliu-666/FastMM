#pragma once
// OKX balances -> BalanceFields (venues/balances.hpp), shared by the REST reply (GET
// /api/v5/account/balance, okx_rest_decoder) and the private `account` channel
// (okx_private_parser). Both carry the same object: account-level fields in USD and details[] per
// currency, every number a string, "" for a field the account mode does not fill
// (https://www.okx.com/docs-v5/en/#trading-account-rest-api-get-balance and
// #trading-account-websocket-account-channel, read 2026-09-30).
//
// The account mode is acctLv of GET /api/v5/account/config (1 spot, 2 futures, 3 multi-currency
// margin, 4 portfolio margin). Per currency:
//   spot mode            free = availBal, locked = frozenBal, total = free + locked, equity = total
//   futures mode         free = availEq (the currency's available margin, availBal when ""),
//                        locked = frozenBal (what orders and positions hold), total = cashBal,
//                        equity = eq, maintenance = mmr (cross, per currency)
//   multi-currency, PM   free = availBal, locked = frozenBal, total = cashBal, equity = eq
// and, in multi-currency and portfolio margin mode only, the account row (BalanceMsg::kAccount,
// asset USD), which the derivatives draw on: free = availEq - imr (availEq: "account level
// available equity", adjEq when ""), locked = imr, total = totalEq (OKX has no USD wallet
// balance), equity = adjEq, maintenance = mmr. Spot mode fills adjEq, imr and mmr only with spot
// borrowing on; the connector trades spot there without it, so it sends no account row.
// Amounts are rounded half away from zero to 8 decimals (availBal "5006.644535984").
#include "fastmm/venues/balances.hpp"
#include "fastmm/venues/decimal.hpp"

#include <cstdint>
#include <string_view>

namespace fastmm::venues::okx {

enum class OkxAccountMode : std::uint8_t {
  Unknown = 0,
  Spot = 1,
  Futures = 2,
  MultiCurrency = 3,
  Portfolio = 4,
};

[[nodiscard]] constexpr OkxAccountMode parse_account_mode(std::string_view acct_lv) noexcept {
  if (acct_lv == "1") return OkxAccountMode::Spot;
  if (acct_lv == "2") return OkxAccountMode::Futures;
  if (acct_lv == "3") return OkxAccountMode::MultiCurrency;
  if (acct_lv == "4") return OkxAccountMode::Portfolio;
  return OkxAccountMode::Unknown;
}

// The derivatives draw on the account's USD margin (the kAccount row).
[[nodiscard]] constexpr bool has_account_row(OkxAccountMode m) noexcept {
  return m == OkxAccountMode::MultiCurrency || m == OkxAccountMode::Portfolio;
}

// One entry of details[], as sent.
struct OkxCcyFields {
  std::string_view ccy;
  std::string_view avail_bal;
  std::string_view frozen_bal;
  std::string_view cash_bal;
  std::string_view eq;
  std::string_view avail_eq;
  std::string_view mmr;
  std::string_view u_time;
};

// The account-level fields, as sent.
struct OkxAccountFields {
  std::string_view total_eq;
  std::string_view adj_eq;
  std::string_view avail_eq;
  std::string_view imr;
  std::string_view mmr;
  std::string_view u_time;
};

// "" is zero; false for anything that is not a decimal.
[[nodiscard]] inline bool okx_amount(std::string_view s, Notional& out) noexcept {
  if (s.empty()) {
    out = Notional{};
    return true;
  }
  const auto v = parse_rounded<Notional>(s);
  if (!v) return false;
  out = *v;
  return true;
}

[[nodiscard]] inline bool okx_ccy_balance(const OkxCcyFields& c,
                                          OkxAccountMode mode,
                                          BalanceFields& f) noexcept {
  Notional avail;
  Notional frozen;
  Notional cash;
  Notional eq;
  Notional avail_eq;
  Notional mmr;
  if (!okx_amount(c.avail_bal, avail) || !okx_amount(c.frozen_bal, frozen) ||
      !okx_amount(c.cash_bal, cash) || !okx_amount(c.eq, eq) || !okx_amount(c.avail_eq, avail_eq) ||
      !okx_amount(c.mmr, mmr))
    return false;
  if (mode == OkxAccountMode::Spot || mode == OkxAccountMode::Unknown) {
    f = BalanceFields::spot(avail, frozen);
    return true;
  }
  f.free = mode == OkxAccountMode::Futures && !c.avail_eq.empty() ? avail_eq : avail;
  f.locked = frozen;
  f.total = cash;
  f.equity = eq;
  f.maintenance = mmr;
  return true;
}

[[nodiscard]] inline bool okx_account_balance(const OkxAccountFields& a,
                                              BalanceFields& f) noexcept {
  Notional total_eq;
  Notional adj_eq;
  Notional avail_eq;
  Notional imr;
  Notional mmr;
  if (!okx_amount(a.total_eq, total_eq) || !okx_amount(a.adj_eq, adj_eq) ||
      !okx_amount(a.avail_eq, avail_eq) || !okx_amount(a.imr, imr) || !okx_amount(a.mmr, mmr))
    return false;
  f.free = (a.avail_eq.empty() ? adj_eq : avail_eq) - imr;
  f.locked = imr;
  f.total = total_eq;
  f.equity = adj_eq;
  f.maintenance = mmr;
  return true;
}

}  // namespace fastmm::venues::okx
