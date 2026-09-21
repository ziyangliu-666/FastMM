#pragma once
// ItchL2Bridge (ADR-0015 section 5): TotalView-ITCH 5.0 messages -> one L3Book per configured
// instrument -> price-level events for the engine. Runs on the net thread.
//
// The caller passes every ITCH message of a datagram to on_itch_message() with its MoldUDP64
// (or SoupBinTCP) sequence number and the datagram's receive stamp, then calls end_datagram().
//
//   R, S            decoded for every locate (R maps the locate of a registered symbol)
//   anything else   skipped after the 11-byte header when the locate is not configured
//   A F E C X D U   applied to the instrument's L3Book
//   E, C            TradeMsg when the execution is printable (E always, C with Printable = Y):
//                   price of the resting order (E) or the Execution Price (C), aggressor
//                   opposite to the resting side, trade_id = Match Number
//   P, Q            TradeMsg as decoded by ItchDecoder
//
// Books start incomplete. An incomplete book is updated but emits nothing. mark_complete()
// (after GLIMPSE, or before sequence 1) emits a BookSnapshotMsg (EventHeader::kSnapshot) with
// the top `depth` levels per side, and ConnectionStateMsg{Live, channel 0} once every book is
// complete. mark_incomplete() clears every book and emits ConnectionStateMsg{Resyncing,
// channel 0}; the engine then clears all books of the venue.
//
// A complete book emits at most one BookDeltaMsg per datagram, from end_datagram(): the levels
// of the top `depth` per side that differ from the last emitted top, with absolute quantities
// (0 deletes). A level that enters the top because another emptied is included, and a level
// pushed out of the top is deleted, so the engine's book holds exactly the top `depth`. Changes
// strictly worse than the emitted top of a full side do not mark the book.
// first_update_id / last_update_id are the sequence numbers of the first and last message
// applied since the previous emission and prev_update_id is the previous emission's
// last_update_id; a snapshot has first = last = the last sequence applied and prev 0. A delta the
// sink has no room for is not lost: the book stays marked and the next end_datagram() emits the
// difference from what the engine last received.
//
// EventHeader: t0_cycles and recv_ts from the stamp of the message (the datagram), t1_delta
// taken after decoding and the L3 update, venue_seq = the sequence of the last message applied,
// exch_ts = midnight + the ITCH timestamp of that message.
//
// Every buffer is allocated by the constructor and add_instrument(); on_itch_message(),
// end_datagram() and the mark_*() calls do not allocate.
#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/core/book/l3_book.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fastmm::codecs::itch {

// Receive stamp of the datagram a message came in.
struct DatagramStamp {
  Cycles t0_cycles{};
  Timestamp recv_ts{};
};

struct ItchL2BridgeConfig {
  VenueId venue{};
  std::uint32_t depth = 20;  // levels per side, 1..256
  Price tick = Price::from_raw(nasdaq::kPrice4Unit);
  L3BookConfig book{};  // per instrument
};

struct ItchL2BridgeStats {
  std::uint64_t messages = 0;     // on_itch_message() calls
  std::uint64_t skipped = 0;      // unconfigured locate
  std::uint64_t book_errors = 0;  // unknown or duplicate reference, full book, bad quantity
  std::uint64_t deltas = 0;
  std::uint64_t delta_levels = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t trades = 0;
  std::uint64_t overflow = 0;  // the sink had no room
};

class ItchL2Bridge {
 public:
  static constexpr std::uint32_t kMaxDepth = 256;

  ItchL2Bridge(venues::EventSink& sink, const ItchL2BridgeConfig& cfg);
  ~ItchL2Bridge();
  ItchL2Bridge(const ItchL2Bridge&) = delete;
  ItchL2Bridge& operator=(const ItchL2Bridge&) = delete;

  // Allocates the instrument's L3Book. `symbol` (1..8 characters) is matched against Stock
  // Directory messages; empty: map the locate with map_locate(). False if `id` is invalid or
  // already added, or the symbol is invalid.
  bool add_instrument(std::string_view symbol, InstrumentId id);
  // Replay and tests: locate -> instrument without a Stock Directory message.
  bool map_locate(std::uint16_t locate, InstrumentId id) noexcept;
  // Start of a trading day: locates are reassigned by the next directory spin.
  void clear_locates() noexcept;
  void set_midnight(Timestamp midnight) noexcept { decoder_.set_midnight(midnight); }

  venues::ParseStatus on_itch_message(std::uint64_t seq,
                                      std::span<const std::byte> msg,
                                      const DatagramStamp& stamp) noexcept;
  // Emits the pending BookDeltaMsg of every book changed since the previous call.
  void end_datagram() noexcept;

  // Emits the snapshot of a book; false if `id` is unknown or the sink is full (the book stays
  // incomplete; call again).
  bool mark_complete(InstrumentId id) noexcept;
  // mark_complete() for every incomplete book; false if any failed.
  bool mark_all_complete() noexcept;
  // Every book becomes incomplete and is cleared; emits ConnectionStateMsg{Resyncing}.
  void mark_incomplete(std::int32_t reason_code = 0) noexcept;

  [[nodiscard]] bool complete(InstrumentId id) const noexcept;
  [[nodiscard]] const L3Book* book(InstrumentId id) const noexcept;
  [[nodiscard]] const ItchL2BridgeStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const ItchDecoder& decoder() const noexcept { return decoder_; }
  [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }
  // Last value of S (System Event) Event Code, 0 before the first.
  [[nodiscard]] char last_system_event() const noexcept { return system_event_; }

 private:
  struct Slot;
  static constexpr std::uint16_t kNoSlot = 0xFFFF;

  [[nodiscard]] Slot* slot_of(InstrumentId id) const noexcept;
  void touch(Slot& s, Side side, Price price, std::uint64_t seq, Timestamp exch_ts) noexcept;
  void emit_trade(const Slot& s,
                  const EventHeader& src,
                  Price price,
                  Qty qty,
                  std::uint64_t match,
                  Side aggressor) noexcept;
  bool flush(Slot& s) noexcept;
  bool emit_snapshot(Slot& s) noexcept;
  void emit_state(ConnState state, std::int32_t reason) noexcept;
  void stamp_header(EventHeader& h) const noexcept;

  venues::EventSink& sink_;
  VenueId venue_;
  std::uint32_t depth_;
  Price tick_;
  L3BookConfig book_cfg_;
  ItchDecoder decoder_;
  ScratchSink scratch_;
  DatagramStamp stamp_{};
  std::unique_ptr<std::uint16_t[]> locate_slot_;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::vector<std::uint16_t> dirty_;  // capacity = slots_.size()
  std::unique_ptr<Level[]> top_;      // 2 x depth: current top, bids then asks
  std::unique_ptr<Level[]> changes_;  // 4 x depth
  std::size_t incomplete_ = 0;
  ItchL2BridgeStats stats_{};
  char system_event_ = 0;
};

}  // namespace fastmm::codecs::itch
