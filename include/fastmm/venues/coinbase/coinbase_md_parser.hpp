#pragma once
// Coinbase Exchange public-feed decoder, wss://ws-feed.exchange.coinbase.com
// (https://docs.cdp.coinbase.com/exchange/websocket-feed/channels, read 2026-09-30). One text frame
// becomes normalised messages written into the caller's scratch buffer:
//
//   {"type":"snapshot","product_id":P,"asks":[[px,sz],..],"bids":[..],"time":T}
//        -> BookSnapshot (kSnapshot). The venue sends the whole book (22753 bids and 18947 asks
//           for BTC-USD on 2026-09-30), best level first on each side; the first
//           kMaxBookLevelsPerMsg of each side are kept, and a side that is not in strict price
//           order is Malformed (the book is resubscribed).
//   {"type":"l2update","product_id":P,"changes":[["buy"|"sell",px,sz],..],"time":T}
//        -> BookDelta; sz is the level's new size, "0" removes it. A side changing more levels than
//           a message carries keeps the ones nearest the touch (LevelSpill, kTruncated*).
//   {"type":"match"|"last_match","trade_id":N,"side":S,"size","price","product_id",..}
//        -> TradeMsg for match (the aggressor is the other side: `side` is the maker's);
//           last_match (the one sent on subscribing) produces nothing but its trade_id.
//   {"type":"heartbeat","last_trade_id":N,"product_id":P,..}, "subscriptions", "error" -> control.
//
// The level2 channels carry no sequence number (neither the documented examples nor the frames
// recorded on 2026-09-30); the update ids of the book messages are assigned by the feed
// (coinbase_md_feed.hpp). trade_id, which increases by one per trade of a product, is reported
// in MdDecodeResult for the feed's loss check.
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

namespace fastmm::venues::coinbase {

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t book_snapshots = 0;
  std::uint64_t book_deltas = 0;
  std::uint64_t trades = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
  std::uint64_t snapshot_cut = 0;  // snapshots with a side deeper than a message carries
  std::uint64_t truncated = 0;     // updates with a side cut to the levels nearest the touch
};

enum class ControlOp : std::uint8_t {
  None = 0,
  Subscriptions,  // {"type":"subscriptions",..}: the channels now subscribed
  Heartbeat,      // {"type":"heartbeat",..}: trade_id = last_trade_id
  LastMatch,      // {"type":"last_match",..}: trade_id
  Error,          // {"type":"error","message","reason"}
  Other
};

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  ControlOp control = ControlOp::None;
  InstrumentId instrument{};   // of the frame (book, trade, heartbeat), when known
  std::uint64_t trade_id = 0;  // match / last_match: its id; heartbeat: last_trade_id
  std::string_view msg;        // error: message (view into the frame)
  std::string_view reason;     // error: reason
};

class CoinbaseMdParser {
 public:
  // A BTC-USD snapshot is about 1.3 MB of JSON.
  static constexpr std::size_t kDefaultCapacity = 16U << 20;

  CoinbaseMdParser(const SymbolTable& symbols,
                   VenueId venue,
                   std::size_t capacity = kDefaultCapacity);
  ~CoinbaseMdParser();
  CoinbaseMdParser(const CoinbaseMdParser&) = delete;
  CoinbaseMdParser& operator=(const CoinbaseMdParser&) = delete;

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

}  // namespace fastmm::venues::coinbase
