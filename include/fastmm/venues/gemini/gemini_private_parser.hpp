#pragma once
// Gemini order-connection decoder: `orders@account` events and the replies to order.place,
// order.cancel and subscribe on the authenticated wss://ws.gemini.com connection
// (https://developer.gemini.com/websocket/streams.md#order-events and the AsyncAPI spec, read
// 2026-09-30). Fields with empty or zero values may be left out of an event.
//
//   {"e":"orderUpdate","E","s","i","c","S","o","X","p","q","z","Z","L","a","t","n","m","r","T"}
//     X NEW or OPEN          -> OrderAckMsg (venue id `i`)
//     X PARTIALLY_FILLED or FILLED
//                            -> OrderFillMsg: exec id `t`, price `L`, quantity `Z` ("the quantity
//                               filled in the last execution"), leaves `z`, cum `q` - `z`, fee `n`
//                               (in the instrument's quote), maker `m`
//     X CANCELED             -> OrderExpiredMsg when `r` says the venue ended it for its terms
//                               (MakerOrCancelWouldTake, ImmediateOrCancelWouldPost,
//                               FillOrKillWouldNotFill, SelfCrossPrevented, ExceedsPriceLimits),
//                               else OrderCancelAckMsg; cum `Z` ("the cumulative quantity filled
//                               over the lifetime of the order" on a CANCELED event)
//     X REJECTED             -> OrderRejectMsg (reason from `r`)
//     X MODIFIED             -> ignored (no method amends an order)
//   {"id":..,"status":200,"result":{..}} / {"id","status","error":{"code","msg"}}
//                            -> control: the id, the status, the error, result.orderId
//
// `balances@account` (streams.md "Balance Updates" and AsyncAPI BalanceUpdate, read 2026-09-30):
//   {"e":"balanceUpdate","E":ns,"u":ns,"B":[{"a":asset,"f":available,"c":confirmed}]}
//     "pushes updates in real time whenever a balance change occurs, and only includes the assets
//     that changed"; f "Available balance (amount available to trade)", c "Confirmed balance
//     (total balance including pending)", u "Time of the last account update", E "Event time".
//     -> PrivateDecodeResult::balance and balance_rows(): absolute amounts per asset, so each row
//        stands on its own (an asset the update leaves out did not change).
//
// Post-only, IOC and FOK orders "are accepted, then cancelled — they are never REJECTED". A
// fully filled IOC also ends CANCELED: the cum on the cancel is what filled.
//
// Client ids come from `c`; ids that are not FastMM ids yield an invalid ClientOrderId (the OMS
// treats them as unknown). An event naming no symbol carries an invalid instrument; the venue
// fills it in from its order shadow.
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/gemini/gemini_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::gemini {

struct PrivateParserStats {
  std::uint64_t frames = 0;
  std::uint64_t acks = 0;
  std::uint64_t fills = 0;
  std::uint64_t cancels = 0;
  std::uint64_t rejects = 0;
  std::uint64_t balances = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t foreign_ids = 0;
};

struct PrivateControl : MdControl {
  std::string_view order_id;  // result.orderId (text or digits)
};

// One asset of a balanceUpdate.
struct BalanceUpdateRow {
  std::string_view asset;  // valid until the next decode
  Notional available{};    // f
  Notional confirmed{};    // c
};
// A realtime update names the assets that changed, typically one or two.
inline constexpr std::size_t kMaxBalanceRows = 32;

struct BalanceUpdate {
  bool present = false;
  bool truncated = false;    // more rows than kMaxBalanceRows, or one unreadable: some left out
  std::uint32_t rows = 0;    // GeminiPrivateParser::balance_rows()
  std::int64_t time_ns = 0;  // u, else E
};

struct PrivateDecodeResult : DecodeResult {
  std::uint32_t count = 0;
  PrivateControl control;
  BalanceUpdate balance;  // a balanceUpdate event (status Ok, count 0)
};

class GeminiPrivateParser {
 public:
  GeminiPrivateParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity = 1U << 20);
  ~GeminiPrivateParser();
  GeminiPrivateParser(const GeminiPrivateParser&) = delete;
  GeminiPrivateParser& operator=(const GeminiPrivateParser&) = delete;

  // `out` must hold kDecoderScratchBytes; messages are written back to back (`count`).
  PrivateDecodeResult decode(std::string_view json,
                             Timestamp recv_ts,
                             Cycles t0,
                             std::span<std::byte> out) noexcept;

  [[nodiscard]] const PrivateParserStats& stats() const noexcept { return stats_; }
  // The rows of the last decode's balanceUpdate.
  [[nodiscard]] std::span<const BalanceUpdateRow> balance_rows(
      const BalanceUpdate& b) const noexcept {
    return {balance_rows_.data(), b.rows};
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::array<BalanceUpdateRow, kMaxBalanceRows> balance_rows_{};
  const SymbolTable& symbols_;
  VenueId venue_;
  PrivateParserStats stats_;
};

}  // namespace fastmm::venues::gemini
