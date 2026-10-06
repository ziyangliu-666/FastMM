#pragma once
// A venue's executions exported to a file, for an offline fill audit (fastmm-pnl audit, against
// the store; core/fill_audit.hpp). Two forms:
//   * a JSON array of objects, as Binance returns GET /api/v3/myTrades and GET /fapi/v1/userTrades
//     (id, orderId, price, qty, commission, commissionAsset, time, isBuyer / buyer / side), or with
//     the generic names below;
//   * CSV with a header row naming the columns, in any order.
// Generic names (JSON keys or CSV columns), aliases after the slash:
//   symbol        the venue's symbol (BTCUSDT, BTC-USDT)                       required
//   exec_id       the venue's trade id / id, trade_id, tradeId                  required
//   side          Buy or Sell, any case / isBuyer, buyer (true: Buy)           required
//   price, qty    decimals / quantity                                          required
//   time_ms       Unix ms, or a UTC time "2024-03-04 09:14:02.123" / time      required
//   order_id      the venue's order id / orderId
//   cl_ord_id     the client order id / clientOrderId
//   fee           the commission as charged / commission
//   fee_asset     the asset it was charged in / commissionAsset
//   venue         [venues.<name>]
// Unknown keys and columns are ignored. Numbers may be JSON numbers or strings; decimals are exact
// to 1e-8.
#include "fastmm/core/fill_audit.hpp"
#include "fastmm/core/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fastmm::store {

// The executions in `text`: JSON when its first non-blank character is '[', CSV otherwise. The
// error names the row (1-based, the header being row 1 of a CSV) and what is wrong with it.
[[nodiscard]] Result<std::vector<AuditFill>, std::string> parse_venue_fills(std::string_view text);
// The same from a file.
[[nodiscard]] Result<std::vector<AuditFill>, std::string> load_venue_fills(const std::string& path);

}  // namespace fastmm::store
