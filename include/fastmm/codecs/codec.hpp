#pragma once
// Protocol codec concepts (plan 7). Only the shapes are defined here (M3 fills in FIX / ITCH
// / OUCH / MDP3 under codecs/<proto>/). A binary-protocol venue plugs into the same net
// stack as the JSON venues:
//
//   Connection<Stream, Pipeline<Framer, SessionLayer, Decoder>>
//
// where WsClient is "just" a Framer whose frames are text, and the venue JSON decoders are
// Decoders. Nothing in this header allocates or is virtual.
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::codecs {

// One complete application frame carved out of a byte stream. `consumed` is how many input
// bytes the framer used (frame + framing overhead); zero means "need more bytes".
struct FrameView {
  std::span<const std::byte> payload;
  std::size_t consumed = 0;
  std::uint8_t kind = 0;  // protocol-specific tag (WS opcode, SoupBin packet type, ...)
  [[nodiscard]] bool complete() const noexcept { return consumed != 0; }
};

// Splits a byte stream into frames without copying. Must be re-entrant on partial input.
template <class F>
concept Framer = requires(F& f, std::span<const std::byte> in) {
  { f.next(in) } noexcept -> std::same_as<FrameView>;
};

// Turns one frame into zero or more normalised engine events written to the sink.
template <class D>
concept Decoder =
    requires(D& d, const FrameView& frame, std::int64_t rx_ts, fastmm::venues::EventSink& sink) {
      { d.decode(frame, rx_ts, sink) } noexcept -> std::same_as<fastmm::venues::ParseStatus>;
    };

// Serialises one outbound command into wire bytes. Returns bytes written; 0 on failure.
template <class E>
concept Encoder =
    requires(E& e, const fastmm::venues::OrderCommand& cmd, std::span<std::byte> out) {
      { e.encode(cmd, out) } noexcept -> std::same_as<std::size_t>;
    };

enum class SessionState : std::uint8_t { Down = 0, LoggingOn = 1, Up = 2, Recovering = 3 };

// Session-level protocol machinery (FIX Logon/Heartbeat/Resend, SoupBinTCP login, MoldUDP
// gap requests). on_frame() consumes session frames and returns false for application
// frames the Decoder should see; on_timer() emits heartbeats / test requests.
template <class S>
concept SessionLayer = requires(S& s, const FrameView& frame, std::int64_t now_ns) {
  { s.on_frame(frame) } noexcept -> std::same_as<bool>;
  { s.on_timer(now_ns) } noexcept;
  { s.state() } noexcept -> std::same_as<SessionState>;
};

// Big/little-endian helpers for packed structs (M3): explicit-width, byte-swapped on read.
struct be16_t {
  std::uint16_t raw;
  [[nodiscard]] std::uint16_t get() const noexcept { return __builtin_bswap16(raw); }
  void set(std::uint16_t v) noexcept { raw = __builtin_bswap16(v); }
};
struct be32_t {
  std::uint32_t raw;
  [[nodiscard]] std::uint32_t get() const noexcept { return __builtin_bswap32(raw); }
  void set(std::uint32_t v) noexcept { raw = __builtin_bswap32(v); }
};
struct be64_t {
  std::uint64_t raw;
  [[nodiscard]] std::uint64_t get() const noexcept { return __builtin_bswap64(raw); }
  void set(std::uint64_t v) noexcept { raw = __builtin_bswap64(v); }
};
struct le16_t {
  std::uint16_t raw;
  [[nodiscard]] std::uint16_t get() const noexcept { return raw; }
  void set(std::uint16_t v) noexcept { raw = v; }
};
struct le32_t {
  std::uint32_t raw;
  [[nodiscard]] std::uint32_t get() const noexcept { return raw; }
  void set(std::uint32_t v) noexcept { raw = v; }
};
struct le64_t {
  std::uint64_t raw;
  [[nodiscard]] std::uint64_t get() const noexcept { return raw; }
  void set(std::uint64_t v) noexcept { raw = v; }
};
static_assert(sizeof(be16_t) == 2 && sizeof(be32_t) == 4 && sizeof(be64_t) == 8);
static_assert(sizeof(le16_t) == 2 && sizeof(le32_t) == 4 && sizeof(le64_t) == 8);

}  // namespace fastmm::codecs
