#pragma once
// Bybit unified-account balances -> BalanceFields (venues/balances.hpp), shared by the REST reply
// (GET /v5/account/wallet-balance?accountType=UNIFIED, bybit_rest_decoder) and the private `wallet`
// topic (bybit_private_parser). Both carry the same object: account-level fields in USD and coin[]
// per coin, every number a string (https://bybit-exchange.github.io/docs/v5/account/wallet-balance
// and .../v5/websocket/private/wallet, read 2026-09-30).
//
// Per coin:
//   free        walletBalance - locked - totalOrderIM - totalPositionIM - bonus (the page's
//               "derivatives available balance" of a coin; for spot walletBalance - locked)
//   locked      locked (spot open orders) + totalOrderIM + totalPositionIM
//   total       walletBalance
//   equity      equity (walletBalance - spotBorrow + unrealisedPnl + option value)
//   maintenance totalPositionMM
// totalOrderIM, totalPositionIM and totalPositionMM are "" in portfolio margin mode (zero here).
// The account row (BalanceMsg::kAccount, asset USD), which the derivatives of a cross or portfolio
// margin account draw on: free = totalAvailableBalance, locked = totalInitialMargin, total =
// totalWalletBalance, equity = totalEquity, maintenance = totalMaintenanceMargin. "All account
// wide fields are not applicable to isolated margin": an isolated-margin account sends none.
// Not read: free (deprecated, "there is no Spot wallet any more"), availableToWithdraw
// (deprecated for UNIFIED since 9 Jan 2025), availableToBorrow (deprecated, always ""),
// accountLTV (deprecated), the *ByMp fields ("you can ignore this field").
// Amounts are rounded half away from zero to 8 decimals.
#include "fastmm/venues/balances.hpp"
#include "fastmm/venues/decimal.hpp"

#include <string_view>

namespace fastmm::venues::bybit {

// One entry of coin[], as sent.
struct BybitCoinFields {
  std::string_view coin;
  std::string_view wallet_balance;
  std::string_view locked;
  std::string_view equity;
  std::string_view total_order_im;
  std::string_view total_position_im;
  std::string_view total_position_mm;
  std::string_view bonus;
  std::string_view spot_borrow;
};

// The account-level fields, as sent.
struct BybitAccountFields {
  std::string_view total_equity;
  std::string_view total_wallet_balance;
  std::string_view total_available_balance;
  std::string_view total_initial_margin;
  std::string_view total_maintenance_margin;
};

// "" is zero; false for anything that is not a decimal.
[[nodiscard]] inline bool bybit_amount(std::string_view s, Notional& out) noexcept {
  if (s.empty()) {
    out = Notional{};
    return true;
  }
  const auto v = parse_rounded<Notional>(s);
  if (!v) return false;
  out = *v;
  return true;
}

[[nodiscard]] inline bool bybit_coin_balance(const BybitCoinFields& c, BalanceFields& f) noexcept {
  Notional wallet;
  Notional locked;
  Notional equity;
  Notional order_im;
  Notional position_im;
  Notional position_mm;
  Notional bonus;
  if (!bybit_amount(c.wallet_balance, wallet) || !bybit_amount(c.locked, locked) ||
      !bybit_amount(c.equity, equity) || !bybit_amount(c.total_order_im, order_im) ||
      !bybit_amount(c.total_position_im, position_im) ||
      !bybit_amount(c.total_position_mm, position_mm) || !bybit_amount(c.bonus, bonus))
    return false;
  f.locked = locked + order_im + position_im;
  f.free = wallet - f.locked - bonus;
  f.total = wallet;
  f.equity = equity;
  f.maintenance = position_mm;
  return true;
}

[[nodiscard]] inline bool bybit_account_balance(const BybitAccountFields& a,
                                                BalanceFields& f) noexcept {
  return bybit_amount(a.total_available_balance, f.free) &&
         bybit_amount(a.total_initial_margin, f.locked) &&
         bybit_amount(a.total_wallet_balance, f.total) && bybit_amount(a.total_equity, f.equity) &&
         bybit_amount(a.total_maintenance_margin, f.maintenance);
}

}  // namespace fastmm::venues::bybit
