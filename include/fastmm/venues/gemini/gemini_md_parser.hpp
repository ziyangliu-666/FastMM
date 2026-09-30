#pragma once
// Gemini public-stream decoder for the WebSocket API at wss://ws.gemini.com
// (https://developer.gemini.com/websocket/streams.md, message-format.md and the AsyncAPI spec
// https://developer.gemini.com/specs/asyncapi/websocket.yaml, read 2026-09-30). One text frame
// becomes normalised messages written back to back into the caller's scratch buffer:
//
//   {"e":"depthUpdate","E":ns,"s":sym,"U":first,"u":last,"b":[[px,qty],..],"a":[..]}
//                         -> BookDeltaMsg (BookDelta; the feed turns a symbol's first frame into
//                            the snapshot), first = U, last = u, prev = U, exch_ts = E
//   {"u":id,"E":ns,"s":sym,"b":px,"B":qty,"a":px,"A":qty[,"c","C"]}   (`@bookTicker`)
//                         -> BookTickerMsg
//   {"E":ns,"s":sym,"t":id,"p":px,"q":qty,"m":buyer_is_maker}          (`@trade`)
//                         -> TradeMsg, aggressor Sell when the buyer made, Buy otherwise
//   {"id":..,"status":200[,"result":{..}]} and {"id","status","error":{"code","msg"}}
//                         -> control: the request id, the status, the error, `serverTime` of a
//                            `time` reply
//
// Only depthUpdate carries an `e`; a bookTicker has `B`, a trade has `t` and `m`. Symbols are
// lowercase on this API ("btcgusdperp"); the symbol table matches without regard to case.
// Quantities are base units (1 BTC per perpetual contract): the engine's unit.
//
// simdjson On-Demand lives in the .cpp. Frames need kJsonPadding readable bytes behind them.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::gemini {

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t book_deltas = 0;
  std::uint64_t book_tickers = 0;
  std::uint64_t trades = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
  // depth frames with a side longer than half a BookDeltaMsg, cut to the levels nearest the touch
  // (LevelSpill) rather than refused: a full-book snapshot of a deep spot book.
  std::uint64_t truncated = 0;
  // Levels priced past the 8-decimal fixed point (spot asks at 1e10 and more), left out.
  std::uint64_t out_of_range = 0;
};

// A reply to a request ({"id","status",..}).
struct MdControl {
  bool present = false;
  std::string_view id;              // a string id as sent back
  std::int64_t id_number = -1;      // a number id
  int status = 0;                   // HTTP-like status
  int error_code = 0;               // error.code
  std::string_view msg;             // error.msg
  std::int64_t server_time_ms = 0;  // result.serverTime of a `time` reply
};

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  MdControl control;
};

class GeminiMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 4U << 20;

  GeminiMdParser(const SymbolTable& symbols,
                 VenueId venue,
                 std::size_t capacity = kDefaultCapacity);
  ~GeminiMdParser();
  GeminiMdParser(const GeminiMdParser&) = delete;
  GeminiMdParser& operator=(const GeminiMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Messages carry recv_ts/t0 from the arguments.
  MdDecodeResult decode(std::string_view json,
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

}  // namespace fastmm::venues::gemini
