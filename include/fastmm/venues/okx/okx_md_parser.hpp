#pragma once
// OKX v5 public-stream decoder. One WebSocket text frame from wss://ws.okx.com:8443/ws/v5/public
// becomes normalised messages written back to back into the caller's scratch buffer:
//
//   {"arg":{"channel":"books","instId":I},"action":"snapshot"|"update","data":[{"asks":[[px,sz,
//     "0",n],..],"bids":[..],"ts","checksum","prevSeqId","seqId"}]}
//                                     -> BookDeltaMsg (snapshot: BookSnapshot + kSnapshot);
//                                        first = last = seqId, prev = prevSeqId (0 for -1)
//   {"arg":{"channel":"bbo-tbt",..},"data":[{"asks":[[..]],"bids":[[..]],"ts","seqId"}]}
//                                     -> BookTickerMsg
//   {"arg":{"channel":"trades",..},"data":[{"instId","tradeId","px","sz","side","ts","count"}]}
//                                     -> one TradeMsg per trade (aggressor = side, the taker's)
//   {"event":"subscribe"|"unsubscribe"|"error",..} and the text "pong" -> control
//
// Field meanings: https://www.okx.com/docs-v5/en/#order-book-trading-market-data-ws-order-book-
// channel, ...-ws-trades-channel (read 2026-09-26). Sizes of a SWAP are in contracts, which is the
// engine's unit for it (Instrument::contract_multiplier = ctVal * ctMult).
//
// A books frame also leaves the text of every level it carried in book_texts(), views into the
// frame, for the checksum (okx_book_sync.hpp); they are valid until the next decode().
//
// simdjson On-Demand lives in the .cpp. Frames need kJsonPadding readable bytes behind them.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/okx/okx_book_sync.hpp"
#include "fastmm/venues/symbology.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace fastmm::venues::okx {

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t book_snapshots = 0;
  std::uint64_t book_deltas = 0;
  std::uint64_t book_tickers = 0;
  std::uint64_t trades = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
};

enum class ControlOp : std::uint8_t {
  None = 0,
  Subscribe,
  Unsubscribe,
  Pong,
  Login,
  Error,
  ChannelConnCount,  // {"event":"channel-conn-count",..}: informational
  Notice,            // {"event":"notice","code":"64008",..}: the connection will be closed
  Other
};

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  ControlOp control = ControlOp::None;
  bool control_success = false;
  int code = 0;               // `code` of an event (error, login, notice)
  std::string_view msg;       // view into the frame
  std::string_view channel;   // arg.channel of a subscribe / unsubscribe / error
  bool has_checksum = false;  // books: `checksum` present
  std::int32_t checksum = 0;
};

// Which channel carries the depth; all four have the same push format.
enum class OkxDepthChannel : std::uint8_t { Books = 0, Books50L2Tbt = 1, BooksL2Tbt = 2 };

[[nodiscard]] constexpr std::string_view to_string(OkxDepthChannel c) noexcept {
  switch (c) {
    case OkxDepthChannel::Books50L2Tbt:
      return "books50-l2-tbt";
    case OkxDepthChannel::BooksL2Tbt:
      return "books-l2-tbt";
    case OkxDepthChannel::Books:
      return "books";
  }
  return "books";
}

class OkxMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 4U << 20;
  static constexpr std::size_t kMaxLevelTexts = 1024;  // 512 a side

  OkxMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity = kDefaultCapacity);
  ~OkxMdParser();
  OkxMdParser(const OkxMdParser&) = delete;
  OkxMdParser& operator=(const OkxMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Messages carry recv_ts/t0 from the arguments.
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;

  // The levels of the last books frame: bids first (book_bid_count() of them), then asks.
  [[nodiscard]] std::span<const OkxLevelText> book_texts() const noexcept {
    return {texts_.data(), text_count_};
  }
  [[nodiscard]] std::uint32_t book_bid_count() const noexcept { return text_bids_; }

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
  std::array<OkxLevelText, kMaxLevelTexts> texts_{};
  std::uint32_t text_count_ = 0;
  std::uint32_t text_bids_ = 0;
};

}  // namespace fastmm::venues::okx
