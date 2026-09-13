#pragma once
// Binance Spot user-data-stream decoder (6.4). Accepts the three envelopes Binance uses:
//   * WebSocket API subscription: {"subscriptionId":N,"event":{...}}   (web-socket-api.md
//     "Event format" / user-data-stream.md payload examples),
//   * legacy listenKey raw stream: {...} (the sim exchange still speaks this),
//   * combined stream wrapper: {"stream":"<listenKey>","data":{...}}.
// and maps (user-data-stream.md "Order Update" executionReport fields, enums.md
// "Execution types"):
//   x=NEW       -> OrderAckMsg          (c = clientOrderId, i = orderId)
//   x=REJECTED  -> OrderRejectMsg       (r = reject reason text)
//   x=CANCELED  -> OrderCancelAckMsg    (C = original client id, z = cumulative filled)
//   x=REPLACED  -> OrderAckMsg          (amend-keep-priority; same client id)
//   x=TRADE     -> OrderFillMsg         (t = trade id as exec id, L/l/z, n fee, m maker)
//   x=EXPIRED / TRADE_PREVENTION -> OrderExpiredMsg
//   outboundAccountPosition -> one PositionUpdateMsg per instrument whose base asset
//                              appears in B[] (qty = free + locked, avg_px unknown = 0)
// Client ids that do not decode as FastMM ids (manual orders) are reported with an invalid
// ClientOrderId so the OMS classifies them as unknown.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::binance {

struct UserParserStats {
  std::uint64_t frames = 0;
  std::uint64_t exec_reports = 0;
  std::uint64_t positions = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t foreign_ids = 0;  // client ids not minted by FastMM
};

struct UserDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back in `out`
};

class BinanceUserParser {
 public:
  BinanceUserParser(const SymbolTable& symbols,
                    const InstrumentTable& instruments,
                    VenueId venue,
                    std::size_t capacity = 1U << 20);
  ~BinanceUserParser();
  BinanceUserParser(const BinanceUserParser&) = delete;
  BinanceUserParser& operator=(const BinanceUserParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Ignored for responses / unknown events.
  UserDecodeResult decode(std::string_view json,
                          Timestamp recv_ts,
                          Cycles t0,
                          std::span<std::byte> out) noexcept;

  [[nodiscard]] const UserParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  const InstrumentTable& instruments_;
  VenueId venue_;
  UserParserStats stats_;
};

}  // namespace fastmm::venues::binance
