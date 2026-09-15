#pragma once
// Binance USDⓈ-M futures market-data decoder. One WebSocket text frame from a combined stream
// (`{"stream":"btcusdt@depth@100ms","data":{...}}`, or a raw `/ws/<stream>` payload) becomes one
// normalised message in the caller's scratch buffer:
//
//   <symbol>@depth@100ms  depthUpdate {E,T,s,U,u,pu,b,a}   -> BookDeltaMsg (first=U, last=u,
//   prev=pu) <symbol>@bookTicker   bookTicker {u,s,b,B,a,A,T,E}     -> BookTickerMsg
//   <symbol>@aggTrade     aggTrade {E,a,s,p,q,f,l,T,m}     -> TradeMsg (aggressor = m ? Sell : Buy)
//   <symbol>@markPrice*   markPriceUpdate                  -> Ignored (no engine message)
//
// Streams:
// https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams
// ("Diff. Book Depth Streams", "Individual Symbol Book Ticker Streams", "Aggregate Trade Streams").
// Since 2026-03-05 depth and bookTicker are served under /public and aggTrade under /market
// ("Important WebSocket Change Notice"). The payloads also carry "ps" and "st", which are not read.
//
// Same rules as the Spot parser: simdjson On-Demand in the .cpp, kJsonPadding readable bytes after
// the frame, no allocation after construction.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/binance/binance_md_parser.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::binance_usdm {

using binance::MdParserStats;

class BinanceUsdmMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 4U << 20;

  BinanceUsdmMdParser(const SymbolTable& symbols,
                      VenueId venue,
                      std::size_t capacity = kDefaultCapacity);
  ~BinanceUsdmMdParser();
  BinanceUsdmMdParser(const BinanceUsdmMdParser&) = delete;
  BinanceUsdmMdParser& operator=(const BinanceUsdmMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. On Ok, out holds one message of `len` bytes.
  DecodeResult decode(std::string_view json,
                      Timestamp recv_ts,
                      Cycles t0,
                      std::span<std::byte> out) noexcept;

  // REST `GET /fapi/v1/depth` body `{"lastUpdateId":L,"E":ms,"T":ms,"bids":[..],"asks":[..]}` ->
  // BookSnapshotMsg (kSnapshot, last_update_id = L) for the given instrument.
  DecodeResult decode_depth_snapshot(std::string_view json,
                                     InstrumentId instrument,
                                     Timestamp recv_ts,
                                     Cycles t0,
                                     std::span<std::byte> out) noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
};

}  // namespace fastmm::venues::binance_usdm
