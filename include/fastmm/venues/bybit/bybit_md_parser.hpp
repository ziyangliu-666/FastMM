#pragma once
// Bybit v5 spot public-stream decoder (6.5). One WebSocket text frame from
// wss://stream[-testnet].bybit.com/v5/public/spot becomes normalised messages written back
// to back into the caller's scratch buffer:
//
//   orderbook.{50|200|1000}.SYM  snapshot/delta {s,b,a,u,seq}  -> BookDeltaMsg (last=u)
//                                (snapshot: type BookSnapshot + kSnapshot)
//   orderbook.1.SYM              snapshot/delta                -> BookTickerMsg (Bybit spot has
//                                no top-of-book ticker stream; plan 6.2)
//   publicTrade.SYM              data[] {T,s,S,v,p,i,seq}      -> one TradeMsg per trade
//                                (aggressor = S, "Taker side")
//   {"success":..,"ret_msg":..,"op":..}                          -> control (subscribe/pong)
//
// Field meanings: https://bybit-exchange.github.io/docs/v5/websocket/public/orderbook and
// .../public/trade. Level-1 deltas update the side they carry (size "0" empties it), per
// the orderbook page's "amount 0 = delete" rule.
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

  // Forgets the level-1 state (stream reconnect).
  void reset_tickers() noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Top {
    Price bid_px{};
    Qty bid_qty{};
    Price ask_px{};
    Qty ask_qty{};
  };
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
  std::array<Top, kMaxInstruments> tops_{};
};

}  // namespace fastmm::venues::bybit
