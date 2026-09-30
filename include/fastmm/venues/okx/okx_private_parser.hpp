#pragma once
// OKX v5 private-stream decoder, wss://ws.okx.com:8443/ws/v5/private
// (https://www.okx.com/docs-v5/en/#order-book-trading-trade-ws-order-channel,
// #trading-account-websocket-positions-channel, #trading-account-websocket-balance-and-position-
// channel; read 2026-09-26). Every field is a string unless noted.
//
//   orders   data[] one per order change:
//              fillSz > 0 with tradeId        -> OrderFillMsg (exec id tradeId, fillPx, fillSz,
//                                                cum accFillSz, leaves sz - accFillSz, fee
//                                                -fillFee in fillFeeCcy, execType M = maker,
//                                                fillTime)
//              amendResult "0" (reqId)        -> OrderAckMsg for the id in reqId, kAmendedInPlace
//              amendResult "-1"               -> OrderRejectMsg for the id in reqId (code, msg)
//              state live, no fill, no amend  -> OrderAckMsg
//              state canceled / mmp_canceled  -> OrderExpiredMsg when the venue ended it for the
//                                                order's own terms (cancelSource 13 FOK, 14 IOC,
//                                                31 post-only would take), else OrderCancelAckMsg
//                                                (cum = accFillSz)
//              partially_filled / filled      -> the fill only
//   positions data[] posSide net              -> PositionUpdateMsg (qty = pos, signed; avgPx)
//              posSide long / short           -> counted as a hedge-mode position, not decoded
//   balance_and_position data[] eventType funding_fee -> no message; funding_event is set so the
//              venue reads the bills (the push has the balance change but no id)
//   account  data[0] (#trading-account-websocket-account-channel, read 2026-09-30), once
//              set_balances() named the assets:
//              details[] of a kept currency   -> BalanceMsg (okx_balance.hpp), stamped with its
//                                                uTime (the account's uTime when it has none)
//              multi-currency / PM mode       -> BalanceMsg kAccount, asset USD, the account's
//                                                uTime
//              Every amount is absolute. The initial push (eventType snapshot) lists every
//              currency with a balance, an event_update only the currencies that changed: each
//              is a full update of that currency. OKX pushes only currencies "with non-zero
//              balance", so one that went to zero may never be named: its row then moves with
//              the engine's own fills until the next REST snapshot (every reconciliation) sets it.
//   {"event":"login"|"subscribe"|"error"|"channel-conn-count"|"notice",..}, "pong" -> control
//
// Client ids come from clOrdId; ids that are not FastMM ids yield an invalid ClientOrderId (the
// OMS treats them as unknown). A fee in the instrument's quote (the settlement currency, USDT) is
// FeeAsset::Quote, in its base FeeAsset::Base, in anything else FeeAsset::Other.
//
// The orders channel may deliver a message twice (changelog 2025-07-08): a fill carries its
// tradeId, which the OMS deduplicates, and a second cancel or amend result finds the order no
// longer pending and is ignored.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/balances.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/okx/okx_balance.hpp"
#include "fastmm/venues/okx/okx_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::okx {

struct PrivateParserStats {
  std::uint64_t frames = 0;
  std::uint64_t orders = 0;
  std::uint64_t fills = 0;
  std::uint64_t amends = 0;
  std::uint64_t positions = 0;
  std::uint64_t hedge_positions = 0;  // posSide long / short: not decoded
  std::uint64_t funding_events = 0;
  std::uint64_t balances = 0;  // account-channel items decoded
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign_ids = 0;
  std::uint64_t overflow = 0;
};

struct PrivateDecodeResult : MdDecodeResult {
  bool funding_event = false;  // balance_and_position eventType funding_fee
  bool positions_snapshot = false;
};

class OkxPrivateParser {
 public:
  OkxPrivateParser(const SymbolTable& symbols,
                   const InstrumentTable& instruments,
                   VenueId venue,
                   std::size_t capacity = 1U << 20);
  ~OkxPrivateParser();
  OkxPrivateParser(const OkxPrivateParser&) = delete;
  OkxPrivateParser& operator=(const OkxPrivateParser&) = delete;

  // `out` must hold kDecoderScratchBytes; messages are written back to back (`count`).
  PrivateDecodeResult decode(std::string_view json,
                             Timestamp recv_ts,
                             Cycles t0,
                             std::span<std::byte> out) noexcept;

  // The account channel is decoded for these assets (nullptr: ignored), mapped for `mode`.
  // `assets` must outlive the parser.
  void set_balances(const VenueAssets* assets, OkxAccountMode mode) noexcept {
    assets_ = assets;
    mode_ = mode;
  }

  [[nodiscard]] const PrivateParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  const VenueAssets* assets_ = nullptr;
  OkxAccountMode mode_ = OkxAccountMode::Unknown;
  PrivateParserStats stats_;
};

}  // namespace fastmm::venues::okx
