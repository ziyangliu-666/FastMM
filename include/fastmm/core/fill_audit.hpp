#pragma once
// Fill audit: the venue's record of an account's executions against the engine's, execution by
// execution. An execution is the venue's trade id on one symbol; the two halves of a self-trade
// share the id and differ in side. audit_fills() pairs the two lists by (symbol, trade id, side)
// and sorts every row inside the window into
//   * matched     - both have it, and quantity, price, fee, side and order agree;
//   * mismatched  - both have it, a field differs (AuditField);
//   * missing     - the venue has it, the engine never booked it;
//   * phantom     - the engine booked it, the venue has no such execution;
//   * duplicates  - the engine booked it more than once (sessions that each stored it).
// fastmm-live runs it periodically against the venue's trade history ([venues.<name>]
// fill_audit_interval_s, live/fill_auditor.hpp); fastmm-pnl audit runs it against a file the
// venue exported. Control path: strings, vectors.
#include "fastmm/core/enums.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

// One execution, as the venue reports it or as the engine stored it. Amounts are fixed point,
// 1e-8 (Price, Qty, Notional raw).
struct AuditFill {
  std::string venue;      // [venues.<name>]; empty when the source names none
  std::string symbol;     // compared with normalize_symbol()
  std::string exec_id;    // the venue's trade id
  std::string order_id;   // the venue's order id; empty: not known
  std::string cl_ord_id;  // the client order id; empty: not known
  Side side = Side::Buy;
  std::int64_t price_raw = 0;
  std::int64_t qty_raw = 0;
  std::int64_t fee_raw = 0;  // the commission as reported, in fee_asset units
  bool has_fee = false;      // false: the source does not report the commission
  std::string fee_asset;  // as the source names it ("USDT", "BNB"; the store: quote, base, other)
  std::int64_t time_ms = 0;      // the venue's time of the trade, Unix ms
  std::uint64_t session_id = 0;  // the session that stored it (the engine's side)
};

// The fields of a mismatch, a bit each.
enum AuditField : std::uint8_t {
  kAuditQty = 1U << 0,
  kAuditPrice = 1U << 1,
  kAuditFee = 1U << 2,
  kAuditSide = 1U << 3,
  kAuditOrder = 1U << 4,  // the venue order id, else the client order id, where both name one
};
// "qty,price" for kAuditQty | kAuditPrice.
[[nodiscard]] std::string audit_fields_text(std::uint8_t fields);

struct AuditMismatch {
  AuditFill venue;
  AuditFill booked;
  std::uint8_t fields = 0;  // AuditField bits
};

struct FillAuditReport {
  std::int64_t from_ms = 0;  // the window, inclusive; 0: open
  std::int64_t to_ms = 0;
  std::size_t venue_rows = 0;   // in the window
  std::size_t booked_rows = 0;  // in the window, one per execution
  std::size_t matched = 0;
  std::vector<AuditFill> missing;
  std::vector<AuditFill> phantom;
  std::vector<AuditMismatch> mismatched;
  std::vector<AuditFill> duplicates;  // the copies after the first
  [[nodiscard]] bool clean() const noexcept {
    return missing.empty() && phantom.empty() && mismatched.empty() && duplicates.empty();
  }
  // The earliest venue time of a missing execution; 0: none.
  [[nodiscard]] std::int64_t first_missing_ms() const noexcept;
};

// Upper case without separators: "btc-usdt", "BTC/USDT" and "BTCUSDT" are one symbol.
[[nodiscard]] std::string normalize_symbol(std::string_view symbol);

// Pairs `venue` with `booked` and classifies what falls in [from_ms, to_ms] (venue time; 0 leaves
// that end open). A pair counts when either row is inside, so a fill stamped a little differently
// on the two sides is compared rather than reported twice; give both lists a margin beyond the
// window. Rows with an empty trade id (the engine's estimates) are ignored, as are copies of one
// venue row.
[[nodiscard]] FillAuditReport audit_fills(std::span<const AuditFill> venue,
                                          std::span<const AuditFill> booked,
                                          std::int64_t from_ms = 0,
                                          std::int64_t to_ms = 0);

// One line per execution: "BTCUSDT 123456 Buy 0.001 @ 70000 fee 0.07 USDT order 42 at <ms>".
[[nodiscard]] std::string describe(const AuditFill& f);

}  // namespace fastmm
