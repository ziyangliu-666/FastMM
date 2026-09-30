#pragma once
// Bybit v5 public-stream decoder (6.5). One WebSocket text frame from
// wss://stream[-testnet].bybit.com/v5/public/{spot,linear} becomes normalised messages written
// back to back into the caller's scratch buffer:
//
//   orderbook.{50|200|1000}.SYM  snapshot/delta {s,b,a,u,seq}  -> BookDeltaMsg (last=u)
//                                (snapshot: type BookSnapshot + kSnapshot)
//   orderbook.1.SYM              snapshot/delta                -> BookTickerMsg (Bybit spot has
//                                no top-of-book ticker stream; plan 6.2)
//   publicTrade.SYM              data[] {T,s,S,v,p,i,seq}      -> one TradeMsg per trade
//                                (aggressor = S, "Taker side")
//   tickers.SYM                  snapshot/delta data {markPrice, -> PerpStateMsg (derivatives)
//                                indexPrice, fundingRate, nextFundingTime, fundingIntervalHour,
//                                singleOpenInterest, openInterest}, cs, ts
//   {"success":..,"ret_msg":..,"op":..}                          -> control (subscribe/pong)
//
// Field meanings: https://bybit-exchange.github.io/docs/v5/websocket/public/orderbook,
// .../public/trade and .../public/ticker. Level-1 deltas update the side they carry (size "0"
// empties it), per the orderbook page's "amount 0 = delete" rule.
//
// Tickers (derivatives push every 100 ms): a snapshot, then deltas that carry only the fields
// that changed ("If a response param is not found in the message, then its value has not
// changed"). The parser keeps the last value of each field per instrument and writes every field
// known so far on each push, so an unchanged mark or funding rate is reported as current. The
// cache starts again at the next snapshot: a snapshot replaces it, and deltas before the first
// snapshot after reset_tickers() (reconnect) are ignored. A field present with an empty value
// ("fundingRate":"" on a dated future) is unknown.
//   markPrice, indexPrice          -> kMark, kIndex
//   fundingRate                    -> kFunding: the rate for the next settlement, decimal per
//                                     interval; nextFundingTime (Unix ms) the settlement; the
//                                     interval is fundingIntervalHour, else the instrument's
//                                     instruments-info fundingInterval (set_funding_interval)
//   singleOpenInterest             -> kOpenInterest, one side, in the base coin (linear: contracts
//                                     with multiplier 1). openInterest is both sides (ticker page:
//                                     "Open interest size (both sides)"); it is used halved only
//                                     for a symbol whose frames never carried singleOpenInterest.
//   cs, ts                         -> hdr.venue_seq, hdr.exch_ts
//
// simdjson On-Demand lives in the .cpp. Frames need kJsonPadding readable bytes behind them.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::bybit {

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t book_snapshots = 0;
  std::uint64_t book_deltas = 0;
  std::uint64_t book_tickers = 0;
  std::uint64_t trades = 0;
  std::uint64_t perp_states = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
};

enum class ControlOp : std::uint8_t { None = 0, Subscribe, Unsubscribe, Pong, Auth, Other };

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  ControlOp control = ControlOp::None;
  bool control_success = false;
  std::string_view ret_msg;  // view into the frame (control messages)
  std::string_view req_id;
};

class BybitMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 4U << 20;

  BybitMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity = kDefaultCapacity);
  ~BybitMdParser();
  BybitMdParser(const BybitMdParser&) = delete;
  BybitMdParser& operator=(const BybitMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Messages carry recv_ts/t0 from the arguments.
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;

  // Forgets the level-1 state and the tickers cache (stream reconnect).
  void reset_tickers() noexcept;
  // The funding interval of `inst` from reference data (instruments-info fundingInterval), used
  // when a tickers frame has no fundingIntervalHour.
  void set_funding_interval(InstrumentId inst, Duration interval) noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Top {
    Price bid_px{};
    Qty bid_qty{};
    Price ask_px{};
    Qty ask_qty{};
  };
  // The last tickers value of each field of one instrument.
  struct Perp {
    Price mark{};
    Price index{};
    double rate = 0.0;
    std::int64_t next_ms = 0;
    Duration interval{};
    Qty oi_single{};
    Qty oi_both{};
    std::uint8_t known = 0;  // the fields held (bits in the .cpp)
    bool live = false;       // a snapshot arrived since the last reset
  };
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
  std::array<Top, kMaxInstruments> tops_{};
  std::array<Perp, kMaxInstruments> perps_{};
  std::array<Duration, kMaxInstruments> ref_interval_{};
};

}  // namespace fastmm::venues::bybit
