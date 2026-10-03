#pragma once
// Gate futures private-channel decoder, on the same stream as the public data
// (wss://fx-ws.gateio.ws/v4/ws/usdt, subscribed with the auth block). Channels (futures WS docs
// "Orders API", "User trades API", "Positions API", "Balances API", read 2026-10-03):
//
//   futures.orders      result[] {id, contract, size (signed), left (signed), price, status
//                       open|finished, finish_as, text, tif, is_reduce_only, create_time_ms,
//                       finish_time_ms, update_time, fill_price, ...}
//                         status open, nothing filled, no finish_as  -> OrderAckMsg
//                         finished, finish_as filled                 -> nothing (usertrades)
//                         finished, finish_as cancelled              -> OrderCancelAckMsg (cum)
//                         finished, finish_as ioc/stp/reduce_only/position_closed/reduce_out/
//                                   liquidated/auto_deleveraged      -> OrderExpiredMsg (cum)
//                         open with a part filled (a fill's echo)    -> nothing
//   futures.usertrades  result[] {id, create_time_ms, contract, order_id, size (signed), price,
//                       role maker|taker, text, fee, point_fee}      -> OrderFillMsg (exec id =
//                       id; fee in the settle currency, FeeAsset::Quote; cum_qty and leaves_qty
//                       are left zero: the connector fills them from its order shadow)
//   futures.positions   result[] {contract, size (signed), entry_price, mode, update_id,
//                       time_ms, ...}                                 -> PositionUpdateMsg (mode
//                       other than "single" is counted as dual-mode, not decoded)
//   futures.balances    result[] {balance, change, text, time_ms, type, currency}
//                                                                     -> type "fund" with a
//                       contract in `text` becomes a FundingMsg; any balance item sets
//                       MdDecodeResult::balance_changed so the connector refreshes the balances
//   subscribe / unsubscribe replies, futures.pong, futures.system      -> control
//
// Client ids come from `text` ("t-" + FastMM's id); other texts ("api", "web") give an invalid
// ClientOrderId, which the OMS treats as unknown. Sizes are contracts, as numbers or strings.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/gate/gate_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::gate {

struct PrivateParserStats {
  std::uint64_t frames = 0;
  std::uint64_t orders = 0;
  std::uint64_t trades = 0;
  std::uint64_t positions = 0;
  std::uint64_t dual_positions = 0;  // mode != single: not decoded
  std::uint64_t balances = 0;
  std::uint64_t funding = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign_ids = 0;
  std::uint64_t overflow = 0;
};

struct PrivateDecodeResult : MdDecodeResult {
  bool balance_changed = false;  // a futures.balances item: ask the venue for the balances
};

class GatePrivateParser {
 public:
  GatePrivateParser(const SymbolTable& symbols,
                    const InstrumentTable& instruments,
                    VenueId venue,
                    std::size_t capacity = 1U << 20);
  ~GatePrivateParser();
  GatePrivateParser(const GatePrivateParser&) = delete;
  GatePrivateParser& operator=(const GatePrivateParser&) = delete;

  // `out` must hold kDecoderScratchBytes; messages are written back to back (`count`).
  PrivateDecodeResult decode(std::string_view json,
                             Timestamp recv_ts,
                             Cycles t0,
                             std::span<std::byte> out) noexcept;

  [[nodiscard]] const PrivateParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  PrivateParserStats stats_;
};

// "t-fm.." -> the client id; nullopt for any other text.
[[nodiscard]] std::optional<ClientOrderId> cl_ord_id_of_text(std::string_view text) noexcept;

}  // namespace fastmm::venues::gate
