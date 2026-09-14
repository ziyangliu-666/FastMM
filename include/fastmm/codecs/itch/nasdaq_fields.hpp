#pragma once
// Field encodings shared by the Nasdaq protocol family (fastmm::codecs::nasdaq):
//
//   * TotalView-ITCH 5.0 ("Data Types"): big-endian unsigned integers, 6-byte timestamps in
//     nanoseconds since midnight, alpha fields left-justified and padded on the right with
//     spaces, Price(4) / Price(8) integers with 4 / 8 implied decimals.
//   * OUCH 4.2 / OUCH 5.0 ("Data Types", October 2025): prices with 4 implied decimals
//     (4 bytes in 4.2, 8 bytes in 5.0), 8-byte nanosecond timestamps.
//   * SoupBinTCP 3.00 / 4.00 / 4.10: numeric login fields are ASCII, left padded with spaces.
//
// Every conversion to the engine's 1e-8 fixed point is exact integer arithmetic: a value that
// does not fit (negative, too many decimals, overflow) is refused instead of being rounded.
#include "fastmm/core/fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace fastmm::codecs::nasdaq {

// 6-byte big-endian unsigned integer (ITCH 5.0 Timestamp: nanoseconds since midnight).
struct be48_t {
  std::uint8_t b[6];
  [[nodiscard]] constexpr std::uint64_t get() const noexcept {
    return (std::uint64_t{b[0]} << 40U) | (std::uint64_t{b[1]} << 32U) |
           (std::uint64_t{b[2]} << 24U) | (std::uint64_t{b[3]} << 16U) |
           (std::uint64_t{b[4]} << 8U) | std::uint64_t{b[5]};
  }
  constexpr void set(std::uint64_t v) noexcept {
    b[0] = static_cast<std::uint8_t>(v >> 40U);
    b[1] = static_cast<std::uint8_t>(v >> 32U);
    b[2] = static_cast<std::uint8_t>(v >> 24U);
    b[3] = static_cast<std::uint8_t>(v >> 16U);
    b[4] = static_cast<std::uint8_t>(v >> 8U);
    b[5] = static_cast<std::uint8_t>(v);
  }
};
static_assert(sizeof(be48_t) == 6);
inline constexpr std::uint64_t kMaxBe48 = (std::uint64_t{1} << 48U) - 1U;
inline constexpr std::int64_t kNanosPerDay = 86'400'000'000'000;

// Big-endian loads / stores on raw byte pointers (framing headers read in place).
[[nodiscard]] inline std::uint16_t load_be16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>((std::to_integer<unsigned>(p[0]) << 8U) |
                                    std::to_integer<unsigned>(p[1]));
}
[[nodiscard]] inline std::uint64_t load_be64(const std::byte* p) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v = (v << 8U) | std::to_integer<std::uint64_t>(p[i]);
  return v;
}
inline void store_be16(std::byte* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::byte>(v >> 8U);
  p[1] = static_cast<std::byte>(v);
}
inline void store_be64(std::byte* p, std::uint64_t v) noexcept {
  for (std::size_t i = 0; i < 8; ++i) p[i] = static_cast<std::byte>(v >> (56U - 8U * i));
}

// ---- prices ------------------------------------------------------------------------------

// One Price(4) unit (0.0001) expressed in the engine's 1e-8 fixed point.
inline constexpr std::int64_t kPrice4Unit = kFixedScale / 10'000;
static_assert(kPrice4Unit == 10'000);

// Price(4), 4-byte (ITCH 5.0, OUCH 4.2). Always exact: 0xFFFFFFFF * 1e4 fits in int64.
[[nodiscard]] constexpr Price price4_to_price(std::uint32_t p) noexcept {
  return Price::from_raw(static_cast<std::int64_t>(p) * kPrice4Unit);
}
// Price with 4 implied decimals in an 8-byte field (OUCH 5.0). False on overflow.
[[nodiscard]] constexpr bool price4_to_price(std::uint64_t p, Price& out) noexcept {
  if (p > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / kPrice4Unit))
    return false;
  out = Price::from_raw(static_cast<std::int64_t>(p) * kPrice4Unit);
  return true;
}
// Price(8) (ITCH 5.0 MWCB levels): 8 implied decimals == the engine scale. False on overflow.
[[nodiscard]] constexpr bool price8_to_price(std::uint64_t p, Price& out) noexcept {
  if (p > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
  out = Price::from_raw(static_cast<std::int64_t>(p));
  return true;
}
// Engine price -> 4-byte Price(4). False if negative, not a multiple of 0.0001 or too large.
[[nodiscard]] constexpr bool price_to_price4(Price p, std::uint32_t& out) noexcept {
  if (p.raw < 0 || p.raw % kPrice4Unit != 0) return false;
  const std::int64_t v = p.raw / kPrice4Unit;
  if (v > std::int64_t{std::numeric_limits<std::uint32_t>::max()}) return false;
  out = static_cast<std::uint32_t>(v);
  return true;
}
// Engine price -> 8-byte price with 4 implied decimals (OUCH 5.0).
[[nodiscard]] constexpr bool price_to_price4(Price p, std::uint64_t& out) noexcept {
  if (p.raw < 0 || p.raw % kPrice4Unit != 0) return false;
  out = static_cast<std::uint64_t>(p.raw / kPrice4Unit);
  return true;
}
// Engine price -> Price(8).
[[nodiscard]] constexpr bool price_to_price8(Price p, std::uint64_t& out) noexcept {
  if (p.raw < 0) return false;
  out = static_cast<std::uint64_t>(p.raw);
  return true;
}

// ---- share quantities --------------------------------------------------------------------

// Largest share count representable as a Qty (whole units at 1e-8).
inline constexpr std::uint64_t kMaxShares =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / kFixedScale);

[[nodiscard]] constexpr Qty shares_to_qty(std::uint32_t n) noexcept {
  return Qty::from_int(static_cast<std::int64_t>(n));
}
[[nodiscard]] constexpr bool shares_to_qty(std::uint64_t n, Qty& out) noexcept {
  if (n > kMaxShares) return false;
  out = Qty::from_int(static_cast<std::int64_t>(n));
  return true;
}
// Engine quantity -> whole shares. False if negative, fractional or wider than 32 bits.
[[nodiscard]] constexpr bool qty_to_shares(Qty q, std::uint32_t& out) noexcept {
  if (q.raw < 0 || q.raw % kFixedScale != 0) return false;
  const std::int64_t v = q.raw / kFixedScale;
  if (v > std::int64_t{std::numeric_limits<std::uint32_t>::max()}) return false;
  out = static_cast<std::uint32_t>(v);
  return true;
}
[[nodiscard]] constexpr bool qty_to_shares(Qty q, std::uint64_t& out) noexcept {
  if (q.raw < 0 || q.raw % kFixedScale != 0) return false;
  out = static_cast<std::uint64_t>(q.raw / kFixedScale);
  return true;
}

// ---- alpha fields ------------------------------------------------------------------------

// Left-justified, right-padded with spaces; longer input is truncated to `width`.
inline void put_alpha(char* dst, std::size_t width, std::string_view s) noexcept {
  const std::size_t n = s.size() < width ? s.size() : width;
  if (n != 0) std::memcpy(dst, s.data(), n);
  if (width > n) std::memset(dst + n, ' ', width - n);
}
// The field without its right padding.
[[nodiscard]] inline std::string_view get_alpha(const char* src, std::size_t width) noexcept {
  std::size_t n = width;
  while (n > 0 && src[n - 1] == ' ') --n;
  return {src, n};
}
// The field without padding on either side. SoupBinTCP documents the Login Accepted session
// as "left padded with spaces" but alpha fields elsewhere as right padded, so accept both.
[[nodiscard]] inline std::string_view get_alpha_trimmed(const char* src,
                                                        std::size_t width) noexcept {
  std::string_view v = get_alpha(src, width);
  while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
  return v;
}

// An 8-byte space-padded wire field packed into an integer (hash key for symbol tables). Reads
// exactly 8 bytes: pass the field of a message, never a shorter string.
[[nodiscard]] inline std::uint64_t symbol_key8(const char* field8) noexcept {
  std::uint64_t k = 0;
  for (std::size_t i = 0; i < 8; ++i) k = (k << 8U) | static_cast<unsigned char>(field8[i]);
  return k;
}
// The same key for a symbol of 1..8 characters (padded with spaces first).
[[nodiscard]] inline std::uint64_t symbol_key(std::string_view symbol) noexcept {
  char buf[8];
  put_alpha(buf, sizeof buf, symbol);
  return symbol_key8(buf);
}

// ---- ASCII numeric fields (SoupBinTCP login fields) --------------------------------------

// Right-aligned decimal, left padded with spaces. False if it does not fit.
inline bool put_numeric(char* dst, std::size_t width, std::uint64_t v) noexcept {
  std::size_t i = width;
  do {
    if (i == 0) return false;
    dst[--i] = static_cast<char>('0' + static_cast<int>(v % 10U));
    v /= 10U;
  } while (v != 0);
  if (i != 0) std::memset(dst, ' ', i);
  return true;
}
// Decimal with optional leading / trailing spaces. An all-space field parses as 0.
[[nodiscard]] inline bool get_numeric(const char* src,
                                      std::size_t width,
                                      std::uint64_t& out) noexcept {
  std::size_t i = 0;
  while (i < width && src[i] == ' ') ++i;
  std::uint64_t v = 0;
  for (; i < width && src[i] != ' '; ++i) {
    const char c = src[i];
    if (c < '0' || c > '9') return false;
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10U) return false;
    v = v * 10U + d;
  }
  for (; i < width; ++i) {
    if (src[i] != ' ') return false;
  }
  out = v;
  return true;
}

// Decimal rendering into a caller buffer of at least 20 bytes. Returns the length.
inline std::size_t format_decimal(char* dst, std::uint64_t v) noexcept {
  char tmp[20];
  std::size_t n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10U));
    v /= 10U;
  } while (v != 0);
  for (std::size_t i = 0; i < n; ++i) dst[i] = tmp[n - 1 - i];
  return n;
}

}  // namespace fastmm::codecs::nasdaq
