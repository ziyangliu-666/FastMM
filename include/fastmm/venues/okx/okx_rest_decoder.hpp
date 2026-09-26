#pragma once
// Control-path decoders for OKX v5 REST bodies (allocation allowed). Every reply is
// {"code":"0","msg":"","data":[...]}, numbers as strings unless noted
// (https://www.okx.com/docs-v5/en/, read 2026-09-26):
//   GET /api/v5/public/instruments?instType=SWAP&instId=I
//       data[] {instId, instIdCode (integer, may be null), instType, ctType linear | inverse,
//               ctVal, ctMult, ctValCcy, settleCcy, tickSz, lotSz, minSz, maxLmtSz, state live |
//               suspend | preopen | test | rebase | post_only}
//   GET /api/v5/public/time            data[0].ts (ms)
//   GET /api/v5/account/config         data[0] {posMode net_mode | long_short_mode, acctLv}
//   GET /api/v5/account/positions      data[] {instId, posSide net | long | short, pos, avgPx}
//   GET /api/v5/trade/orders-pending   data[] {instId, ordId, clOrdId, px, sz, side, state,
//                                      accFillSz}
//   GET /api/v5/trade/fills[-history]  data[] {instId, tradeId, ordId, clOrdId, billId, fillPx,
//                                      fillSz, side, execType, fee, feeCcy, ts, fillTime}
//   GET /api/v5/account/bills[-archive] data[] {billId, instId, ccy, balChg, type, subType, ts}
//   POST /api/v5/trade/cancel-batch-orders  data[] {ordId, sCode, sMsg}
//   POST /api/v5/trade/cancel-all-after     data[0] {triggerTime, ts}
#include "fastmm/core/fixed_point.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::okx {

struct InstrumentInfo {
  std::string inst_id;
  std::int64_t inst_id_code = -1;  // null: none
  std::string inst_type;           // SWAP
  std::string ct_type;             // linear | inverse
  std::string ct_val_ccy;          // BTC
  std::string settle_ccy;          // USDT
  std::string state;               // live
  Qty ct_val{};
  Qty ct_mult{};
  Price tick{};
  Qty lot{};
  Qty min_sz{};
  Qty max_limit_sz{};
};

struct AccountConfig {
  std::string pos_mode;  // net_mode | long_short_mode
  std::string acct_lv;   // 1 spot, 2 futures, 3 multi-currency margin, 4 portfolio margin
};

struct PositionRecord {
  std::string inst_id;
  std::string pos_side;  // net | long | short
  Qty qty{};             // signed in net mode
  Price avg_px{};
};

struct PendingOrder {
  std::string inst_id;
  std::string ord_id;
  std::string cl_ord_id;
  std::string side;
  std::string state;  // live | partially_filled
  Price px{};
  Qty sz{};
  Qty acc_fill_sz{};
};

struct FillRecord {
  std::string inst_id;
  std::string trade_id;
  std::string ord_id;
  std::string cl_ord_id;
  std::string bill_id;
  std::string side;
  std::string exec_type;  // T | M
  std::string fee_ccy;
  Price px{};
  Qty sz{};
  Notional fee{};  // as sent: negative charged
  std::int64_t ts_ms = 0;
  std::int64_t fill_time_ms = 0;
};

struct BillRecord {
  std::string bill_id;
  std::string inst_id;
  std::string ccy;
  std::string type;
  std::string sub_type;
  Notional bal_chg{};  // signed: positive received
  std::int64_t ts_ms = 0;
};

// Empty string on success, else an error description (a non-zero code included).
std::string decode_instruments(std::string_view json, std::vector<InstrumentInfo>& out);
std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms);
std::string decode_account_config(std::string_view json, AccountConfig& out);
std::string decode_positions(std::string_view json, std::vector<PositionRecord>& out);
std::string decode_pending_orders(std::string_view json, std::vector<PendingOrder>& out);
std::string decode_fills(std::string_view json, std::vector<FillRecord>& out);
std::string decode_bills(std::string_view json, std::vector<BillRecord>& out);
// cancel-batch-orders: the orders the venue did not cancel (sCode not 0 and not 51400, "filled,
// canceled or does not exist"), as "ordId: sCode sMsg".
std::string decode_cancel_batch(std::string_view json, std::vector<std::string>& failed);
// {"code":..,"msg":..}; false when the body is not an envelope.
bool decode_envelope(std::string_view json, int& code, std::string& msg);

}  // namespace fastmm::venues::okx
