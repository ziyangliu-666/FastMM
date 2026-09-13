#pragma once
// Hot-path connector concepts (6.2). A venue's market-data feed and order gateway are plain
// classes instantiated inside the venue's connection handlers (static dispatch, everything
// inlined); the Venue virtual interface (venue.hpp) is control-path only.
//
//   MarketDataFeed:  on_message(json, rx_ts) -> ParseStatus   (writes into its EventSink)
//                    on_connected() / on_disconnected()
//                    subscription_payloads() -> span<const std::string>
//   OrderGateway:    encode(cmd, now_ms, out) -> bool       (Out*Msg -> wire bytes)
//                    on_message(json, rx_ts) -> ParseStatus  (responses -> order events)
//                    can_send(now_ns) -> bool
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues {

enum class ParseStatus : std::uint8_t {
  Ok = 0,
  Ignored = 1,        // valid message we do not consume (subscription acks, pongs, ...)
  Malformed = 2,      // JSON error or missing/invalid field
  UnknownSymbol = 3,  // symbol not in the SymbolTable
  Overflow = 4,       // ring full (market data) or output buffer too small
  Error = 5,          // venue-level error payload
};
[[nodiscard]] constexpr std::string_view to_string(ParseStatus s) noexcept {
  switch (s) {
    case ParseStatus::Ok:
      return "Ok";
    case ParseStatus::Ignored:
      return "Ignored";
    case ParseStatus::Malformed:
      return "Malformed";
    case ParseStatus::UnknownSymbol:
      return "UnknownSymbol";
    case ParseStatus::Overflow:
      return "Overflow";
    case ParseStatus::Error:
      return "Error";
  }
  return "?";
}

// What a decoder produced into the caller's scratch buffer.
enum class MdKind : std::uint8_t {
  None = 0,
  BookDelta = 1,
  BookSnapshot = 2,
  BookTicker = 3,
  Trade = 4,
};

enum class OrderEventKind : std::uint8_t {
  None = 0,
  Ack = 1,
  Reject = 2,
  CancelAck = 3,
  CancelReject = 4,
  Fill = 5,
  Expired = 6,
  Position = 7,
};

struct DecodeResult {
  ParseStatus status = ParseStatus::Ignored;
  MdKind kind = MdKind::None;
  OrderEventKind order_kind = OrderEventKind::None;
  std::uint32_t len = 0;  // bytes written to the output buffer
  [[nodiscard]] bool ok() const noexcept { return status == ParseStatus::Ok; }
};

// Every decoder needs a scratch area for the largest message it can produce.
inline constexpr std::uint32_t kDecoderScratchBytes = kMaxMsgBytes;

// Parsers that hand raw text frames to simdjson need SIMDJSON_PADDING (64) readable bytes
// after the payload; RecvBuffer::kPadding guarantees it for WebSocket frames and
// PaddedJson (padded_json.hpp) for fixtures/tests.
inline constexpr std::size_t kJsonPadding = 64;

template <class F>
concept MarketDataFeed = requires(F& f, std::string_view json, std::int64_t rx_ts) {
  { f.on_message(json, rx_ts) } -> std::same_as<ParseStatus>;
  { f.on_connected() };
  { f.on_disconnected() };
  { f.subscription_payloads() } -> std::convertible_to<std::span<const std::string>>;
};

template <class G>
concept OrderGateway = requires(G& g,
                                const OrderCommand& cmd,
                                std::int64_t now_ms,
                                std::span<char> out,
                                std::string_view json,
                                std::int64_t rx_ts,
                                std::int64_t now_ns) {
  { g.encode(cmd, now_ms, out) } -> std::same_as<std::size_t>;  // 0 = failed
  { g.on_message(json, rx_ts) } -> std::same_as<ParseStatus>;
  { g.can_send(now_ns) } -> std::same_as<bool>;
};

}  // namespace fastmm::venues
