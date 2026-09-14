#pragma once
// FixFramer: carves complete FIX messages out of a TCP byte stream (satisfies codecs::Framer).
//
// A frame starts at "8=FIX" and ends after the CheckSum field "10=ddd|" located exactly
// BodyLength(9) bytes after the BodyLength field's SOH. The framer checks that structure only;
// FixView validates the CheckSum value and the fields. It never copies and is re-entrant on
// partial input: call next() with everything buffered, drop `consumed` bytes, repeat.
//
//   kind == kMessage  payload is one message, consumed = leading garbage + message
//   kind == kGarbage  payload empty, consumed = bytes that cannot start a message (skip them)
//   !complete()       need more bytes
//
// Garbage handling: bytes before "8=FIX" are skipped; a candidate whose BodyLength is not a
// number, is larger than max_message, or whose CheckSum field is not where BodyLength says is
// skipped one byte at a time until the next "8=FIX". A trailing partial "8=FI" is kept.
#include "fastmm/codecs/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::codecs::fix {

class FixFramer {
 public:
  enum Kind : std::uint8_t { kMessage = 0, kGarbage = 1 };
  static constexpr std::size_t kDefaultMaxMessage = 1U << 16;

  explicit FixFramer(std::size_t max_message = kDefaultMaxMessage) noexcept
      : max_message_(max_message) {}

  FrameView next(std::span<const std::byte> in) noexcept;

  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  [[nodiscard]] std::uint64_t garbage_bytes() const noexcept { return garbage_bytes_; }

 private:
  std::size_t max_message_;
  std::uint64_t frames_ = 0;
  std::uint64_t garbage_bytes_ = 0;
};

}  // namespace fastmm::codecs::fix
