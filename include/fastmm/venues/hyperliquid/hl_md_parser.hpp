#pragma once
// Hyperliquid public-stream decoder (wss://api.hyperliquid.xyz/ws, "Subscriptions" in the
// WebSocket docs, read 2026-10-12; frames recorded from production, tests/fixtures/hyperliquid).
// One text frame becomes normalised messages written back to back into the caller's scratch
// buffer:
//
//   l2Book   {"channel":"l2Book","data":{"coin","time","levels":[[{px,sz,n}..],[{px,sz,n}..]]}}
//            -> BookSnapshot (kSnapshot) of up to 20 levels a side: every push is a whole book,
//               update id = time (the block's, ms)
//   bbo      {"channel":"bbo","data":{"coin","time","bbo":[{px,sz,n}|null, {px,sz,n}|null]}}
//            -> BookTickerMsg (null: an empty side)
//   trades   {"channel":"trades","data":[{coin, side "B"|"A" (the taker's), px, sz, time, hash,
//            tid, users}]} -> one TradeMsg per trade; users = [buyer, seller] (0x wallet
//            addresses) -> TradeMsg::parties() as PartyIdKind::Address (20 bytes); a trade
//            without a readable pair carries none
//   subscriptionResponse, pong, error -> control
//
// Coins are the venue's names ("BTC", "xyz:NVDA" on the xyz builder dex). An instrument may be
// configured with the dex prefix or, when the connector names a dex, without it.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::hyperliquid {

struct MdParserStats {
  std::uint64_t frames = 0;
  std::uint64_t book_snapshots = 0;
  std::uint64_t book_tickers = 0;
  std::uint64_t trades = 0;
  std::uint64_t control = 0;
  std::uint64_t ignored = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown_symbol = 0;
  std::uint64_t overflow = 0;
};

enum class ControlOp : std::uint8_t { None = 0, Subscribe, Unsubscribe, Pong, Error, Post, Other };

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  ControlOp control = ControlOp::None;
  std::string_view channel;  // view into the frame
  std::string_view error;    // the "error" channel's text
};

// The instrument of a coin, with or without the configured dex's prefix.
class CoinLookup {
 public:
  CoinLookup(const SymbolTable& symbols, VenueId venue, std::string dex = {})
      : symbols_(symbols), venue_(venue), dex_(std::move(dex)) {}
  [[nodiscard]] InstrumentId find(std::string_view coin) const noexcept {
    const InstrumentId id = symbols_.find(venue_, coin);
    if (id.valid() || dex_.empty()) return id;
    if (coin.size() > dex_.size() && coin[dex_.size()] == ':' && coin.starts_with(dex_))
      return symbols_.find(venue_, coin.substr(dex_.size() + 1));
    return id;
  }

 private:
  const SymbolTable& symbols_;
  VenueId venue_;
  std::string dex_;
};

class HlMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 1U << 20;

  HlMdParser(const SymbolTable& symbols,
             VenueId venue,
             std::string dex = {},
             std::size_t capacity = kDefaultCapacity);
  ~HlMdParser();
  HlMdParser(const HlMdParser&) = delete;
  HlMdParser& operator=(const HlMdParser&) = delete;

  // `json` needs kJsonPadding readable bytes behind it; `out` must hold kDecoderScratchBytes.
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  CoinLookup coins_;
  VenueId venue_;
  MdParserStats stats_;
};

}  // namespace fastmm::venues::hyperliquid
