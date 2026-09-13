#pragma once
// Bybit v5 private-stream decoder (6.5), wss://stream[-testnet].bybit.com/v5/private.
// Topics (https://bybit-exchange.github.io/docs/v5/websocket/private/order, .../execution,
// .../wallet; enums from https://bybit-exchange.github.io/docs/v5/enum):
//
//   order      data[] orderStatus New                      -> OrderAckMsg
//                                 Rejected                 -> OrderRejectMsg (rejectReason)
//                                 Cancelled /
//                                 PartiallyFilledCanceled  -> OrderCancelAckMsg (cumExecQty)
//                                 Deactivated              -> OrderExpiredMsg
//                                 PartiallyFilled / Filled -> nothing (fills come from
//                                                             `execution`)
//   execution  data[] execType Trade                       -> OrderFillMsg (execId dedupe,
//                                 cum = orderQty - leavesQty, isMaker -> liquidity)
//   wallet     data[].coin[]                               -> PositionUpdateMsg for each
//                                 instrument whose base coin matches (qty = walletBalance)
//   {"success":..,"op":"auth"|"subscribe"|"pong"} / {"retCode":..,"op":"auth"} -> control
//
// Only category == "spot" items are decoded. Client ids come from orderLinkId; ids that are
// not FastMM ids yield an invalid ClientOrderId (the OMS treats them as unknown).
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/bybit/bybit_md_parser.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::bybit {

struct PrivateParserStats {
  std::uint64_t frames = 0;
  std::uint64_t orders = 0;
  std::uint64_t executions = 0;
  std::uint64_t wallets = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign_ids = 0;
  std::uint64_t overflow = 0;
};

class BybitPrivateParser {
 public:
  BybitPrivateParser(const SymbolTable& symbols,
                     const InstrumentTable& instruments,
                     VenueId venue,
                     std::size_t capacity = 1U << 20);
  ~BybitPrivateParser();
  BybitPrivateParser(const BybitPrivateParser&) = delete;
  BybitPrivateParser& operator=(const BybitPrivateParser&) = delete;

  // `out` must hold kDecoderScratchBytes; messages are written back to back (`count`).
  MdDecodeResult decode(std::string_view json,
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

}  // namespace fastmm::venues::bybit
