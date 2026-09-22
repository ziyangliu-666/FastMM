#pragma once
// ItchDecoder: Nasdaq TotalView-ITCH 5.0 -> normalised engine events (satisfies Decoder).
//
// One FrameView holds exactly one ITCH message, as delivered by the MoldUDP64 message framer
// or a SoupBinTCP Sequenced Data packet. Dispatch is on the first byte (spec section):
//
//   A / F  Add Order (1.3)                   -> OrderAddL3Msg
//   E      Order Executed (1.4.1)            -> OrderExecL3Msg, exec_price 0 (= the order price)
//   C      Order Executed With Price (1.4.2) -> OrderExecL3Msg, exec_price = Execution Price,
//                                               kNonPrintable unless Printable is 'Y'
//   X      Order Cancel (1.4.3)              -> OrderCancelL3Msg, canceled_qty = Cancelled Shares
//   D      Order Delete (1.4.4)              -> OrderCancelL3Msg, canceled_qty 0 (= delete)
//   U      Order Replace (1.4.5)             -> OrderReplaceL3Msg
//   P      Trade, non-cross (1.5.1)          -> TradeMsg
//   Q      Cross Trade (1.5.2)               -> TradeMsg (only when Shares > 0)
//   R      Stock Directory (1.2.1)           -> stock locate -> InstrumentId table, no event
//   S H Y L V W K J h B I N O                -> validated and ignored
//
// Stock locate codes are assigned per day by the Stock Directory spin. Register the symbols
// the engine trades with add_symbol() (or map locates directly with map_locate() for replay);
// messages for any other locate are ignored and counted in stats().unknown_locate. The locate
// table is a flat 65 536-entry array and the symbol table a fixed OpenHashMap, both allocated
// in the constructor: decode() never allocates.
//
// EventHeader: exch_ts = set_midnight() + ITCH timestamp (the caller supplies midnight of the
// trading day, US/Eastern, as Unix nanoseconds); recv_ts = rx_ts; venue_seq = the value last
// passed to set_venue_seq() (the MoldUDP64 / SoupBinTCP sequence number of the message).
//
// decode_into() writes into any sink with EventSink's reserve<M>() / commit(); ScratchSink holds
// the one event of a call for callers that consume it in place (ItchL2Bridge).
//
// Not carried into the engine messages (no field for them): the Attribution of F, the Cross
// Type of Q and the Buy/Sell Indicator semantics of P (Nasdaq sends 'B' for every P message
// since 2014-07-14, so TradeMsg::aggressor is not meaningful).
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/itch/itch_messages.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace fastmm::codecs::itch {

using venues::ParseStatus;

struct DecoderStats {
  std::uint64_t messages = 0;          // frames passed to decode()
  std::uint64_t events = 0;            // engine events committed to the sink
  std::uint64_t ignored = 0;           // valid messages that produce no event
  std::uint64_t directory = 0;         // Stock Directory messages
  std::uint64_t directory_mapped = 0;  // ... whose symbol was registered
  std::uint64_t unknown_locate = 0;    // order / trade messages for an unmapped locate
  std::uint64_t unknown_type = 0;      // first byte is not an ITCH 5.0 message type
  std::uint64_t malformed = 0;         // shorter than the type's length, bad side, ...
  std::uint64_t overflow = 0;          // the sink had no room
};

// Room for the one event a single ITCH message produces.
class ScratchSink {
 public:
  static constexpr std::size_t kBytes = 128;

  template <MessageLike M>
  [[nodiscard]] M* reserve() noexcept {
    static_assert(sizeof(M) <= kBytes && alignof(M) <= 64);
    return reinterpret_cast<M*>(buf_);
  }
  void commit() noexcept { committed_ = true; }
  // The committed event, or nullptr.
  [[nodiscard]] const EventHeader* event() const noexcept {
    return committed_ ? reinterpret_cast<const EventHeader*>(buf_) : nullptr;
  }
  [[nodiscard]] EventHeader* event() noexcept {
    return committed_ ? reinterpret_cast<EventHeader*>(buf_) : nullptr;
  }
  void reset() noexcept { committed_ = false; }

 private:
  alignas(64) std::byte buf_[kBytes];
  bool committed_ = false;
};

class ItchDecoder {
 public:
  static constexpr std::size_t kLocateSlots = 65'536;
  static constexpr std::size_t kMaxSymbols = 1U << 14;

  explicit ItchDecoder(VenueId venue = VenueId{0});
  ItchDecoder(const ItchDecoder&) = delete;
  ItchDecoder& operator=(const ItchDecoder&) = delete;

  // Symbols (1..8 characters) whose Stock Directory message maps the day's locate code.
  bool add_symbol(std::string_view symbol, InstrumentId id) noexcept;
  void map_locate(std::uint16_t locate, InstrumentId id) noexcept { locate_[locate] = id; }
  [[nodiscard]] InstrumentId instrument(std::uint16_t locate) const noexcept {
    return locate_[locate];
  }
  // Start of a new trading day: locate codes are reassigned by the next directory spin.
  void clear_locates() noexcept;
  void set_midnight(Timestamp midnight) noexcept { midnight_ns_ = midnight.ns; }
  void set_venue_seq(std::uint64_t seq) noexcept { venue_seq_ = seq; }

  ParseStatus decode(const FrameView& frame, std::int64_t rx_ts, venues::EventSink& sink) noexcept;
  // Same as decode() into venues::EventSink or ScratchSink (explicitly instantiated).
  template <class Sink>
  ParseStatus decode_into(const FrameView& frame, std::int64_t rx_ts, Sink& sink) noexcept;

  [[nodiscard]] const DecoderStats& stats() const noexcept { return stats_; }
  [[nodiscard]] VenueId venue() const noexcept { return venue_; }

 private:
  template <class M, class Sink>
  M* start(Sink& sink,
           EventType type,
           InstrumentId inst,
           const MessageHeader& h,
           std::int64_t rx_ts) noexcept;
  template <class Sink>
  ParseStatus commit(Sink& sink) noexcept;
  bool lookup(const MessageHeader& h, InstrumentId& out) noexcept;

  template <class Sink>
  ParseStatus add(const MessageHeader& h,
                  std::uint64_t ref,
                  char side,
                  std::uint32_t shares,
                  std::uint32_t price4,
                  std::int64_t rx_ts,
                  Sink& sink) noexcept;
  template <class Sink>
  ParseStatus execute(const MessageHeader& h,
                      std::uint64_t ref,
                      std::uint32_t shares,
                      std::uint64_t match,
                      Price exec_price,
                      std::uint8_t flags,
                      std::int64_t rx_ts,
                      Sink& sink) noexcept;
  template <class Sink>
  ParseStatus cancel(const MessageHeader& h,
                     std::uint64_t ref,
                     Qty canceled,
                     std::int64_t rx_ts,
                     Sink& sink) noexcept;
  template <class Sink>
  ParseStatus trade(const MessageHeader& h,
                    Price price,
                    Qty qty,
                    std::uint64_t match,
                    Side aggressor,
                    std::int64_t rx_ts,
                    Sink& sink) noexcept;

  VenueId venue_;
  std::int64_t midnight_ns_ = 0;
  std::uint64_t venue_seq_ = 0;
  std::unique_ptr<InstrumentId[]> locate_;
  OpenHashMap<std::uint64_t, InstrumentId, kMaxSymbols> symbols_;
  DecoderStats stats_{};
};

static_assert(Decoder<ItchDecoder>);

}  // namespace fastmm::codecs::itch
