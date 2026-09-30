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
//                                 Funding (linear)         -> FundingMsg (amount = -execFee,
//                                 execId, feeCurrency or the settle coin, execTime)
//   wallet     data[].coin[]                               -> spot: PositionUpdateMsg for each
//                                 instrument whose base coin matches (qty = walletBalance -
//                                 spotBorrow); once set_balances() named the assets, a
//                                 BalanceMsg per kept coin and, with the account row, one
//                                 kAccount (asset USD) from the account-level fields
//                                 (bybit_balance.hpp), stamped with creationTime: the push has
//                                 no other time. Every amount is absolute and each coin named is
//                                 complete; there is no snapshot on subscribing, and an
//                                 unrealised PnL change alone sends nothing (.../private/wallet)
//   position   data[] positionIdx 0 (linear)               -> PositionUpdateMsg (qty = size,
//                                 negative for side Sell; avg_px = entryPrice); a positionIdx
//                                 of 1 or 2 is a hedge-mode position: counted, not decoded
//                                 (.../websocket/private/position)
//   {"success":..,"op":"auth"|"subscribe"|"pong"} / {"retCode":..,"op":"auth"} -> control
//
// Only items of the parser's category are decoded. Client ids come from orderLinkId; ids that are
// not FastMM ids yield an invalid ClientOrderId (the OMS treats them as unknown). Fees: spot
// follows the "Spot Fee Currency Instruction"; a linear fee is in the settlement coin, which is the
// instrument's quote (feeCurrency, when present, names another coin as FeeAsset::Other).
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/balances.hpp"
#include "fastmm/venues/bybit/bybit_category.hpp"
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
  std::uint64_t funding = 0;  // execType Funding
  std::uint64_t wallets = 0;
  std::uint64_t balances = 0;  // BalanceMsg written from the wallet topic
  std::uint64_t positions = 0;
  std::uint64_t hedge_positions = 0;  // positionIdx 1 or 2: not decoded
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
                     std::size_t capacity = 1U << 20,
                     BybitCategory category = BybitCategory::Spot);
  ~BybitPrivateParser();
  BybitPrivateParser(const BybitPrivateParser&) = delete;
  BybitPrivateParser& operator=(const BybitPrivateParser&) = delete;

  // `out` must hold kDecoderScratchBytes; messages are written back to back (`count`).
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;

  // The wallet topic reports the balances of these assets (nullptr: none), and the account row
  // when `account_row` (derivatives on a cross or portfolio margin account). `assets` must
  // outlive the parser.
  void set_balances(const VenueAssets* assets, bool account_row) noexcept {
    assets_ = assets;
    account_row_ = account_row;
  }

  [[nodiscard]] const PrivateParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  BybitCategory category_;
  const VenueAssets* assets_ = nullptr;
  bool account_row_ = false;
  PrivateParserStats stats_;
};

}  // namespace fastmm::venues::bybit
