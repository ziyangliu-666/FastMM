#pragma once
// Pieces shared by the OUCH 4.2 and OUCH 5.0 codecs (fastmm::codecs::ouch):
//
//   * OrderTable: the per-order state the decoders keep (client order id, Nasdaq order
//     reference number, leaves, cumulative executed quantity, side, instrument), keyed by the
//     order's wire identifier. OUCH executions and cancels carry only that identifier and an
//     incremental share count, while the engine's fill / cancel events need cum and leaves.
//   * SymbolMap: 8-character stock symbol <-> InstrumentId.
//   * liquidity_from_flag(): Nasdaq liquidity flags (OUCH 4.2 section 3.7.1, OUCH 5.0
//     "Liquidity Flags" appendix) -> Maker / Taker / Unknown (crosses and unlisted flags).
//   * emit_*(): write normalised order events into an EventSink.
//
// Everything is allocated at construction; lookups, inserts and emits never allocate.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

namespace fastmm::codecs::ouch {

using venues::ParseStatus;

template <class M>
[[nodiscard]] inline const M& view_as(const std::byte* p) noexcept {
  return *reinterpret_cast<const M*>(p);
}
// A zeroed M at the start of `out` (the caller checked the size) for the encoder to fill in
// place. Building M on the stack and copying it out would load right after a run of narrow
// stores to the same bytes, which store-to-load forwarding cannot serve.
template <class M>
[[nodiscard]] FASTMM_FORCE_INLINE M& emplace(std::span<std::byte> out) noexcept {
  static_assert(alignof(M) == 1 && std::is_trivially_copyable_v<M>);
  std::memset(out.data(), 0, sizeof(M));
  return *reinterpret_cast<M*>(out.data());
}
template <class M>
inline std::size_t put(std::span<std::byte> out, const M& m) noexcept {
  if (out.size() < sizeof(M)) return 0;
  std::memcpy(out.data(), &m, sizeof(M));
  return sizeof(M);
}

// Buy/Sell Indicator: B buy, S sell, T sell short, E sell short exempt (both versions).
[[nodiscard]] constexpr bool side_from_code(char c, Side& out) noexcept {
  switch (c) {
    case 'B':
      out = Side::Buy;
      return true;
    case 'S':
    case 'T':
    case 'E':
      out = Side::Sell;
      return true;
    default:
      return false;
  }
}
[[nodiscard]] constexpr char side_code(Side s) noexcept {
  return s == Side::Buy ? 'B' : 'S';
}
[[nodiscard]] Liquidity liquidity_from_flag(char flag) noexcept;

struct OrderEntry {
  ClientOrderId cl_ord_id{};
  std::uint64_t reference_number = 0;
  Qty leaves{};
  Qty cum{};
  InstrumentId instrument{};
  Side side = Side::Buy;
  std::uint8_t pad_[3] = {};
};
// 2^16 slots: up to 57 344 simultaneously open orders (OpenHashMap caps load at 7/8).
inline constexpr std::size_t kOrderSlots = 1U << 16;
using OrderTable = OpenHashMap<std::uint64_t, OrderEntry, kOrderSlots>;

class SymbolMap {
 public:
  static constexpr std::size_t kMaxInstruments = 4096;
  SymbolMap();
  // 1..8 characters; id.value < kMaxInstruments.
  bool add(std::string_view symbol, InstrumentId id) noexcept;
  // The 8-byte space-padded field, nullptr when the instrument has no symbol.
  [[nodiscard]] const char* symbol8(InstrumentId id) const noexcept;
  // Invalid id when the symbol is not registered.
  [[nodiscard]] InstrumentId find(const char* field8) const noexcept;

 private:
  std::unique_ptr<char[]> names_;
  std::unique_ptr<std::uint8_t[]> known_;
  OpenHashMap<std::uint64_t, InstrumentId, 8192> by_symbol_;
};

struct DecoderStats {
  std::uint64_t messages = 0;
  std::uint64_t events = 0;           // engine events committed
  std::uint64_t ignored = 0;          // valid messages without an engine event
  std::uint64_t unknown_order = 0;    // execution / cancel for an order not in the table
  std::uint64_t foreign_id = 0;       // identifier that does not map to a ClientOrderId
  std::uint64_t partial_cancels = 0;  // Canceled leaving shares open (no engine event)
  std::uint64_t broken_trades = 0;
  std::uint64_t system_events = 0;
  std::uint64_t unknown_type = 0;
  std::uint64_t malformed = 0;
  std::uint64_t overflow = 0;    // sink full
  std::uint64_t table_full = 0;  // order table at capacity
  char last_system_event = ' ';
};

struct EncoderStats {
  std::uint64_t encoded = 0;
  std::uint64_t unsupported = 0;         // market orders (no continuous-market OUCH form)
  std::uint64_t bad_value = 0;           // price / quantity not representable
  std::uint64_t unknown_instrument = 0;  // no symbol registered
  std::uint64_t unknown_order = 0;       // cancel / replace of an id without a wire mapping
  std::uint64_t buffer_too_small = 0;
  std::uint64_t id_table_full = 0;
};

// Header values for the emitted engine event.
struct EventStamp {
  VenueId venue{};
  InstrumentId instrument{};
  std::uint64_t venue_seq = 0;
  std::int64_t exch_ns = 0;
  std::int64_t rx_ns = 0;
};

// Each returns false when the sink has no room (nothing was committed).
bool emit_ack(venues::EventSink& sink,
              const EventStamp& st,
              ClientOrderId id,
              std::uint64_t reference) noexcept;
bool emit_reject(venues::EventSink& sink,
                 const EventStamp& st,
                 ClientOrderId id,
                 std::int32_t venue_code,
                 std::string_view text) noexcept;
bool emit_cancel_ack(venues::EventSink& sink, const EventStamp& st, const OrderEntry& e) noexcept;
bool emit_cancel_reject(venues::EventSink& sink,
                        const EventStamp& st,
                        ClientOrderId id,
                        std::int32_t venue_code,
                        std::string_view text) noexcept;
bool emit_expired(venues::EventSink& sink, const EventStamp& st, const OrderEntry& e) noexcept;
// `e` already includes this execution (cum and leaves after it).
bool emit_fill(venues::EventSink& sink,
               const EventStamp& st,
               const OrderEntry& e,
               std::uint64_t match_number,
               Price price,
               Qty qty,
               Liquidity liquidity) noexcept;

}  // namespace fastmm::codecs::ouch
