#pragma once
// Strongly typed identifiers. Mixing an InstrumentId with a VenueId is a compile error.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/hex.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace fastmm {

template <class Tag, class Rep, Rep kInvalid = std::numeric_limits<Rep>::max()>
struct StrongId {
  static_assert(std::is_integral_v<Rep>);
  using rep_type = Rep;

  Rep value = kInvalid;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep v) noexcept : value(v) {}
  [[nodiscard]] static constexpr StrongId invalid() noexcept { return StrongId{}; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value != kInvalid; }
  [[nodiscard]] constexpr Rep get() const noexcept { return value; }
  constexpr auto operator<=>(const StrongId&) const noexcept = default;
};

// Dense index into the engine's instrument table. All per-instrument state is an array
// indexed by this.
using InstrumentId = StrongId<struct InstrumentTag, std::uint32_t>;
using VenueId = StrongId<struct VenueTag, std::uint8_t>;
// 48-bit: session_epoch(16) << 32 | seq(32). 0 is never issued.
using ClientOrderId = StrongId<struct ClientOrderTag, std::uint64_t, 0>;
using VenueOrderId = FixedString<40>;
using TimerId = StrongId<struct TimerTag, std::uint32_t>;
using ExecId = FixedString<40>;

static_assert(std::is_trivially_copyable_v<InstrumentId>);
static_assert(sizeof(InstrumentId) == 4 && sizeof(VenueId) == 1 && sizeof(ClientOrderId) == 8);

// 32-bit pool index handle. Cheaper than a pointer, stable across relocation of the pool
// (journals/snapshots), and lets a slot be recycled with a generation check by the owner.
inline constexpr std::uint32_t kNullHandle = std::numeric_limits<std::uint32_t>::max();

template <class T>
struct Handle {
  std::uint32_t idx = kNullHandle;
  constexpr Handle() noexcept = default;
  constexpr explicit Handle(std::uint32_t i) noexcept : idx(i) {}
  [[nodiscard]] constexpr bool valid() const noexcept { return idx != kNullHandle; }
  constexpr auto operator<=>(const Handle&) const noexcept = default;
};

// --- ClientOrderId wire form -------------------------------------------------------------
// "fm" + 12 lowercase hex digits (48 bits) = 14 chars. Fits Binance (36) and Bybit (36).
inline constexpr std::size_t kClOrdIdChars = 14;

// Writes the kClOrdIdChars characters of the wire form to dst (no terminator) with two
// 8-byte stores.
FASTMM_FORCE_INLINE void write_cl_ord_id(char* dst, ClientOrderId id) noexcept {
  const std::uint64_t hi = hex8(static_cast<std::uint32_t>((id.value >> 32) & 0xFFFF));
  const std::uint64_t lo = hex8(static_cast<std::uint32_t>(id.value));
  // "fm" + the last 4 characters of hi (its upper 4 bytes); bytes 6 and 7 are then
  // overwritten by lo.
  const std::uint64_t head = std::uint64_t{'f'} | (std::uint64_t{'m'} << 8) | ((hi >> 32) << 16);
  std::memcpy(dst, &head, 8);
  std::memcpy(dst + 6, &lo, 8);
}

[[nodiscard]] inline FixedString<16> encode_cl_ord_id(ClientOrderId id) noexcept {
  FixedString<16> out;
  write_cl_ord_id(out.data(), id);
  out.set_size(kClOrdIdChars);
  return out;
}

[[nodiscard]] inline std::optional<ClientOrderId> decode_cl_ord_id(std::string_view s) noexcept {
  if (s.size() != kClOrdIdChars || s[0] != 'f' || s[1] != 'm') return std::nullopt;
  std::uint64_t v = 0;
  for (std::size_t i = 2; i < kClOrdIdChars; ++i) {
    const char c = s[i];
    unsigned d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<unsigned>(c - 'a') + 10U;
    } else {
      return std::nullopt;
    }
    v = (v << 4) | d;
  }
  if (v == 0) return std::nullopt;
  return ClientOrderId{v};
}

[[nodiscard]] constexpr ClientOrderId make_cl_ord_id(std::uint16_t epoch,
                                                     std::uint32_t seq) noexcept {
  return ClientOrderId{(static_cast<std::uint64_t>(epoch) << 32) | seq};
}
[[nodiscard]] constexpr std::uint16_t cl_ord_id_epoch(ClientOrderId id) noexcept {
  return static_cast<std::uint16_t>((id.value >> 32) & 0xFFFF);
}
[[nodiscard]] constexpr std::uint32_t cl_ord_id_seq(ClientOrderId id) noexcept {
  return static_cast<std::uint32_t>(id.value & 0xFFFF'FFFFULL);
}

}  // namespace fastmm

template <class Tag, class Rep, Rep I>
struct std::hash<fastmm::StrongId<Tag, Rep, I>> {
  std::size_t operator()(const fastmm::StrongId<Tag, Rep, I>& id) const noexcept {
    return std::hash<Rep>{}(id.value);
  }
};
