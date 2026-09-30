#pragma once
// Coinbase Advanced Trade public-feed decoder, wss://advanced-trade-ws.coinbase.com (AsyncAPI
// https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/advanced-trade-asyncapi.json,
// read 2026-09-30). Every message is an envelope
//   {"channel":C,"client_id":"","timestamp":T,"sequence_num":N,"events":[..]}
// with a per-connection sequence_num ("use it to detect dropped or out-of-order messages"),
// reported in MdDecodeResult for every frame. One frame becomes normalised messages written back
// to back into the caller's scratch buffer:
//
//   l2_data        events[{"type":"snapshot"|"update","product_id":P,"updates":[{"side":"bid"|
//                  "offer","event_time","price_level","new_quantity"}]}]
//                  -> one BookDelta per event (snapshot: BookSnapshot + kSnapshot). A snapshot is
//                  the whole book (22693 bids and 18957 offers, 4.6 MB for BTC-USD on
//                  2026-09-30), each side best first: the first kMaxBookLevelsPerMsg of a side are
//                  kept, a side out of price order is Malformed. An update side longer than that
//                  keeps the levels nearest the touch (LevelSpill, kTruncated*).
//   market_trades  events[{"type":"update","trades":[{"trade_id","product_id","price","size",
//                  "side","time"}]}] -> one TradeMsg per trade; `side` is "the maker's side of the
//                  trade", so the aggressor is the other. A "snapshot" event (the last trades
//                  before subscribing) produces nothing.
//   heartbeats, subscriptions -> control; {"type":"error","message"} -> Error.
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

struct AdvancedMdStats {
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

enum class AdvancedControl : std::uint8_t { None = 0, Subscriptions, Heartbeat, Error, Other };

struct AdvancedMdResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  AdvancedControl control = AdvancedControl::None;
  bool has_sequence = false;
  std::uint64_t sequence = 0;  // sequence_num of the envelope
  InstrumentId instrument{};   // a malformed snapshot's product, when known
  std::string_view msg;        // error: message (view into the frame)
};

class AdvancedMdParser {
 public:
  // A BTC-USD snapshot is about 4.6 MB of JSON.
  static constexpr std::size_t kDefaultCapacity = 32U << 20;

  AdvancedMdParser(const SymbolTable& symbols,
                   VenueId venue,
                   std::size_t capacity = kDefaultCapacity);
  ~AdvancedMdParser();
  AdvancedMdParser(const AdvancedMdParser&) = delete;
  AdvancedMdParser& operator=(const AdvancedMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Messages carry recv_ts/t0 from the arguments.
  AdvancedMdResult decode(std::string_view json,
                          Timestamp recv_ts,
                          Cycles t0,
                          std::span<std::byte> out) noexcept;

  [[nodiscard]] const AdvancedMdStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  AdvancedMdStats stats_;
};

}  // namespace fastmm::venues::coinbase
