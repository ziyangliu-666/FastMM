#pragma once
// Hyperliquid account streams, subscribed by address on a WebSocket of their own (no auth; the
// WebSocket docs' "Subscriptions", read 2026-10-12; shapes recorded from production for a public
// address, tests/fixtures/hyperliquid):
//
//   orderUpdates  {"channel":"orderUpdates","data":[{"order":{coin, side, limitPx, sz (left),
//                 oid, timestamp, origSz, cloid?, ...},"status":..,"statusTimestamp":ms}]}
//                   open                       -> OrderAckMsg (venue id = oid)
//                   canceled                   -> OrderCancelAckMsg (cum = origSz - sz)
//                   *Canceled, scheduledCancel -> OrderExpiredMsg (cum)
//                   *Rejected                  -> OrderRejectMsg (hl_error_map.hpp)
//                   filled, triggered          -> nothing (the fills come on userFills)
//   userFills     {"channel":"userFills","data":{"isSnapshot":true?,"user","fills":[{coin, px, sz,
//                 side "B"|"A", time, oid, crossed (taker), fee (signed, feeToken), tid, cloid?,
//                 ...}]}} -> OrderFillMsg (exec id = tid; cum_qty and leaves_qty are left zero, the
//                 connector fills them from its order shadow); the snapshot sent on subscribing is
//                 counted, not decoded: the execution replay books what the stream missed
//   userFundings  {"channel":"userFundings","data":{"isSnapshot"?,"user","fundings":[{time, coin,
//                 usdc, szi, fundingRate}]}} -> FundingMsg (id "<time>:<coin>", asset USDC); the
//                 snapshot is skipped the same way
//   subscriptionResponse, pong, error -> control
//
// Every message's exch_ts is the venue's time (the status time, the fill's block time). Client ids
// come from the cloid (hl_order_encoder.hpp); another client's or none gives an invalid id.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/hyperliquid/hl_md_parser.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::hyperliquid {

struct PrivateParserStats {
  std::uint64_t frames = 0;
  std::uint64_t order_updates = 0;
  std::uint64_t fills = 0;
  std::uint64_t fundings = 0;
  std::uint64_t snapshots_skipped = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
};

struct PrivateDecodeResult : MdDecodeResult {
  bool snapshot = false;  // a userFills / userFundings snapshot, skipped
};

class HlPrivateParser {
 public:
  HlPrivateParser(const SymbolTable& symbols,
                  VenueId venue,
                  std::string dex = {},
                  std::size_t capacity = 1U << 20);
  ~HlPrivateParser();
  HlPrivateParser(const HlPrivateParser&) = delete;
  HlPrivateParser& operator=(const HlPrivateParser&) = delete;

  // `json` needs kJsonPadding readable bytes behind it; `out` must hold kDecoderScratchBytes.
  PrivateDecodeResult decode(std::string_view json,
                             Timestamp recv_ts,
                             Cycles t0,
                             std::span<std::byte> out) noexcept;

  [[nodiscard]] const PrivateParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  CoinLookup coins_;
  VenueId venue_;
  PrivateParserStats stats_;
};

}  // namespace fastmm::venues::hyperliquid
