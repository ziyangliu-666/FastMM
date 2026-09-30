#pragma once
// Binance USDⓈ-M futures market-data decoder. One WebSocket text frame from a combined stream
// (`{"stream":"btcusdt@depth@100ms","data":{...}}`, or a raw `/ws/<stream>` payload) becomes one
// normalised message in the caller's scratch buffer:
//
//   <symbol>@depth@100ms   depthUpdate {E,T,s,U,u,pu,b,a}  -> BookDeltaMsg (first=U, last=u,
//                                                            prev=pu)
//   <symbol>@bookTicker    bookTicker {u,s,b,B,a,A,T,E}    -> BookTickerMsg
//   <symbol>@aggTrade      aggTrade {E,a,s,p,q,f,l,T,m}    -> TradeMsg (aggressor = m ? Sell : Buy)
//   <symbol>@markPrice@1s  markPriceUpdate {E,s,p,i,r,T}   -> PerpStateMsg (mark p, index i,
//                          funding rate r per funding interval, next funding T, venue time E)
//
// Streams:
// https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams
// ("Diff. Book Depth Streams", "Individual Symbol Book Ticker Streams", "Aggregate Trade Streams",
// "Mark Price Stream"). Since 2026-03-05 depth and bookTicker are served under /public, aggTrade
// and markPrice under /market ("Important WebSocket Change Notice"). The payloads also carry "ps"
// and "st", and markPrice "ap" (mark price moving average) and "P" (estimated settle price), which
// are not read.
//
// markPriceUpdate carries no funding interval. Each instrument's is set by the connector
// (set_funding_interval, from GET /fapi/v1/fundingInfo at start-up; 8 h when not listed), and the
// parser corrects it when it sees the next funding time roll over: the first frame after a
// funding, within kRolloverWindow of it, whose T moved by a whole number of hours (at most 24)
// has moved it by the interval now in force (Binance adjusts intervals of single symbols).
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

  // The funding interval reported with `id`'s mark price frames (kDefaultFundingInterval until
  // set). Startup, or on the reactor thread.
  void set_funding_interval(InstrumentId id, Duration interval) noexcept;
  [[nodiscard]] Duration funding_interval(InstrumentId id) const noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

  // Binance's standard interval; GET /fapi/v1/fundingInfo lists the symbols whose interval was
  // adjusted (fundingIntervalHours).
  static constexpr Duration kDefaultFundingInterval = seconds(std::int64_t{8} * 3600);
  // How long after a funding time a frame still counts as having seen the rollover.
  static constexpr Duration kRolloverWindow = seconds(300);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
};

}  // namespace fastmm::venues::binance_usdm
