#pragma once
// Gate futures public-stream decoder (wss://fx-ws.gateio.ws/v4/ws/usdt). One text frame becomes
// normalised messages written back to back into the caller's scratch buffer. Channels (futures WS
// docs, read 2026-10-03):
//
//   futures.obu          result {t, full?, s "ob.SYM.L", U?, u, b [[p,s]..], a [[p,s]..]}
//                        -> BookDeltaMsg: full -> BookSnapshot + kSnapshot (first=last=u),
//                           else a delta with first=U, last=u (gate_book_sync.hpp)
//   futures.book_ticker  result {t, u, s, b, B, a, A}  -> BookTickerMsg (empty b/a = empty side)
//   futures.trades       result [{id, create_time_ms, price, size, contract, is_internal?}]
//                        -> one TradeMsg per trade; size > 0: the taker bought, < 0: sold
//   futures.tickers      result [{contract, mark_price, index_price, funding_rate,
//                        funding_interval (s), funding_next_apply (s), t, ...}]
//                        -> PerpStateMsg per contract (the contract table's interval and next
//                           settlement, set_funding, for a frame without them)
//   {"channel":..,"event":"subscribe"|"unsubscribe","error":..?,"result":{"status":..}}
//   {"channel":"futures.pong"} / {"channel":"futures.system",...}      -> control
//
// Sizes arrive as JSON numbers (integers) or, with the "X-Gate-Size-Decimal: 1" upgrade header,
// as strings that may carry decimals; both are read. Prices are strings. Quantities are contracts
// (Instrument::contract_multiplier = quanto_multiplier). simdjson On-Demand lives in the .cpp;
// frames need kJsonPadding readable bytes behind them.
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

namespace fastmm::venues::gate {

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
  std::uint64_t truncated = 0;  // obu pushes with more levels on a side than a message holds
};

enum class ControlOp : std::uint8_t { None = 0, Subscribe, Unsubscribe, Pong, Api, System, Other };

struct MdDecodeResult : DecodeResult {
  std::uint32_t count = 0;  // messages written back to back
  ControlOp control = ControlOp::None;
  bool control_success = false;
  std::string_view channel;  // view into the frame (control messages)
  std::string_view error;    // error.message of a failed request
  std::int64_t error_code = 0;
};

class GateMdParser {
 public:
  static constexpr std::size_t kDefaultCapacity = 4U << 20;

  GateMdParser(const SymbolTable& symbols, VenueId venue, std::size_t capacity = kDefaultCapacity);
  ~GateMdParser();
  GateMdParser(const GateMdParser&) = delete;
  GateMdParser& operator=(const GateMdParser&) = delete;

  // `out` must hold kDecoderScratchBytes. Messages carry recv_ts/t0 from the arguments.
  MdDecodeResult decode(std::string_view json,
                        Timestamp recv_ts,
                        Cycles t0,
                        std::span<std::byte> out) noexcept;

  // The contract's funding interval and the next settlement (Unix ms; 0 unknown), from reference
  // data: the tickers channel carries the rate only.
  void set_funding(InstrumentId inst, Duration interval, std::int64_t next_ms) noexcept;

  [[nodiscard]] const MdParserStats& stats() const noexcept { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  const SymbolTable& symbols_;
  VenueId venue_;
  MdParserStats stats_;
  std::array<Duration, kMaxInstruments> interval_{};
  std::array<std::int64_t, kMaxInstruments> next_funding_ms_{};
};

}  // namespace fastmm::venues::gate
