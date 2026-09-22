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
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace fastmm::codecs {

[[nodiscard]] std::string_view library_name() noexcept;

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

// Big/little-endian wire fields for packed structs (M3). Each holds raw bytes (alignment 1), and
// get()/set() go through memcpy plus a byte swap, so a field at any offset of a packed layout is
// read and written without undefined behaviour (a uint32_t member at an odd offset is misaligned,
// and calling members on it is what UBSan reports). Never take the address of the value as an
// integer; use get()/set().
namespace detail {
template <class T, bool big_endian>
struct EndianField {
  unsigned char raw[sizeof(T)];
  [[nodiscard]] FASTMM_FORCE_INLINE T get() const noexcept {
    T v;
    std::memcpy(&v, raw, sizeof(T));
    return big_endian ? swap(v) : v;
  }
  FASTMM_FORCE_INLINE void set(T v) noexcept {
    const T w = big_endian ? swap(v) : v;
    std::memcpy(raw, &w, sizeof(T));
  }

 private:
  [[nodiscard]] static T swap(T v) noexcept {
    switch (sizeof(T)) {
      case 2:
        return static_cast<T>(__builtin_bswap16(static_cast<std::uint16_t>(v)));
      case 4:
        return static_cast<T>(__builtin_bswap32(static_cast<std::uint32_t>(v)));
      default:
        return static_cast<T>(__builtin_bswap64(static_cast<std::uint64_t>(v)));
    }
  }
};
}  // namespace detail

using be16_t = detail::EndianField<std::uint16_t, true>;
using be32_t = detail::EndianField<std::uint32_t, true>;
using be64_t = detail::EndianField<std::uint64_t, true>;
using le16_t = detail::EndianField<std::uint16_t, false>;
using le32_t = detail::EndianField<std::uint32_t, false>;
using le64_t = detail::EndianField<std::uint64_t, false>;
static_assert(sizeof(be16_t) == 2 && sizeof(be32_t) == 4 && sizeof(be64_t) == 8);
static_assert(sizeof(le16_t) == 2 && sizeof(le32_t) == 4 && sizeof(le64_t) == 8);
static_assert(alignof(be32_t) == 1 && alignof(be64_t) == 1 && alignof(le64_t) == 1);
static_assert(std::is_trivially_copyable_v<be64_t> && std::is_standard_layout_v<be64_t>);

}  // namespace fastmm::codecs
