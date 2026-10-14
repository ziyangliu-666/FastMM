#pragma once
// Lighter public-stream decoder (wss://mainnet.zklighter.elliot.ai/stream, the WebSocket reference
// read 2026-10-13; frames recorded from production, tests/fixtures/lighter). Frames carry their
// keys in alphabetical order, "type" last; one text frame becomes normalised messages written back
// to back into the caller's scratch buffer:
//
//   order_book/{market}  {"channel":"order_book:110","order_book":{"asks":[{price,size}..],
//                        "bids":[..],"begin_nonce","nonce","offset",..},"type":..}
//                        subscribed/ -> BookSnapshot, update/ -> BookDelta (size 0 removes the
//                        level); update ids: last = nonce, prev = begin_nonce, which equals the
//                        nonce before it (the venue batches every 50 ms)
//   ticker/{market}      {"ticker":{"s","a":{price,size},"b":{price,size}}} -> BookTickerMsg
//   trade/{market}       {"trades":[{trade_id, market_id, price, size, is_maker_ask, timestamp
//                        (ms)..}],"liquidation_trades":[..]} -> one TradeMsg per trade (the
//                        aggressor buys when the maker is the ask); bid_account_id (the
//                        buyer) and ask_account_id (the seller) -> TradeMsg::parties() as
//                        PartyIdKind::Account; the history sent on subscribing is skipped
//   connected, pong, {"error":{code,message}} -> control
//
// Prices and sizes are decimal strings. exch_ts is the venue's last_updated_at (us) where the
// frame has it, else its timestamp (ms).
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/feed.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fastmm::venues::lighter {

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
  Connected,
  Subscribed,
  Pong,
  Error,
  TxReply,
  Other
};

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  ControlOp control = ControlOp::None;
  std::string_view channel;  // view into the frame
  std::string_view error;    // an error frame's message
  std::int64_t error_code = 0;
};

// Market index -> instrument.
class MarketMap {
 public:
  void add(std::uint32_t market, InstrumentId id) {
    if (market >= by_market_.size()) by_market_.resize(market + 1, InstrumentId::invalid());
    by_market_[market] = id;
  }
  [[nodiscard]] InstrumentId find(std::int64_t market) const noexcept {
    if (market < 0 || static_cast<std::uint64_t>(market) >= by_market_.size())
      return InstrumentId::invalid();
    return by_market_[static_cast<std::size_t>(market)];
  }

 private:
  std::vector<InstrumentId> by_market_;
};

// "order_book:110" -> 110; -1 when the channel has no market.
[[nodiscard]] std::int64_t market_of_channel(std::string_view channel) noexcept;

class LighterMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 1U << 20;

  LighterMdParser(const MarketMap& markets, VenueId venue, std::size_t capacity = kDefaultCapacity);
  ~LighterMdParser();
  LighterMdParser(const LighterMdParser&) = delete;
  LighterMdParser& operator=(const LighterMdParser&) = delete;

  // `json` needs kJsonPadding readable bytes behind it; `out` must hold kDecoderScratchBytes.
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const MarketMap& markets_;
  VenueId venue_;
  MdParserStats stats_;
};

}  // namespace fastmm::venues::lighter
