#pragma once
// Binance Spot market-data decoder (6.4). One WebSocket text frame from the combined stream
// (`{"stream":"btcusdt@depth@100ms","data":{...}}`, or a raw `/ws/<stream>` payload) is
// turned into exactly one normalised message written into the caller's scratch buffer:
//
//   <symbol>@depth@100ms  depthUpdate {E,s,U,u,b,a}      -> BookDeltaMsg (first=U, last=u)
//   <symbol>@bookTicker   {u,s,b,B,a,A}                  -> BookTickerMsg
//   <symbol>@trade        trade {E,s,t,p,q,T,m,M}        -> TradeMsg (aggressor = m ? Sell : Buy)
//
// Field meanings: web-socket-streams.md ("Diff. Depth Stream", "Individual Symbol Book
// Ticker Streams", "Trade Streams"). `m` is "Is the buyer the market maker?": when true the
// buyer rested and the seller was the aggressor.
//
// simdjson On-Demand runs in the .cpp (never in a public header); the frame must have
// kJsonPadding readable bytes behind it (RecvBuffer / PaddedJson guarantee it). No
// allocation after construction: the parser capacity is reserved once.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::binance {

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t book_deltas = 0;
  std::uint64_t book_tickers = 0;
  std::uint64_t trades = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
};

class BinanceMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 4U << 20;  // largest frame we accept

  BinanceMdParser(const SymbolTable& symbols,
                  VenueId venue,
                  std::size_t capacity = kDefaultCapacity);
  ~BinanceMdParser();
  BinanceMdParser(const BinanceMdParser&) = delete;
  BinanceMdParser& operator=(const BinanceMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. On Ok, out holds one message of `len` bytes with
  // recv_ts/t0_cycles copied from the arguments and t1_delta left for the caller.
  DecodeResult decode(std::string_view json,
                      Timestamp recv_ts,
                      Cycles t0,
                      std::span<std::byte> out) noexcept;

  // REST `GET /api/v3/depth` body `{"lastUpdateId":L,"bids":[[p,q]...],"asks":[...]}` ->
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

}  // namespace fastmm::venues::binance
