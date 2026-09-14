#pragma once
// Mdp3Decoder (plan 7): CME MDP 3.0 packets -> normalised engine events.
//
//   MDIncrementalRefreshBook46          MBP entries -> one BookDeltaMsg per instrument run
//   MDIncrementalRefreshTradeSummary48  New trades -> TradeMsg (AggressorSide)
//   MDInstrumentDefinitionFuture54      -> Mdp3InstrumentTable (SecurityID -> InstrumentId)
//   ChannelReset4                       all books emptied (empty BookSnapshotMsg each)
//   SecurityStatus30                    trading status stored on the instrument
//   AdminHeartbeat12                    counted
//   templates 37/49/50/51               RptSeq accounting only (statistics are not decoded)
//   anything else                       skipped by MsgSize
//
// Books are maintained by MDPriceLevel the way CME specifies for multiple-depth MBP books
// ("MDP 3.0 - Market by Price - Multiple Depth Book", CME Client Systems Wiki): New inserts at
// the level and shifts worse levels down (the level past MarketDepth falls off), Delete removes
// the level and shifts worse levels up, Change updates the quantity at an unchanged price.
// DeleteThru, DeleteFrom and Overlay follow the FIX definitions (see mdp3_decoder.cpp; not
// described on the current wiki pages). Every change is forwarded as a price-keyed Level
// (qty 0 = delete), including levels that fall off the bottom, so an L2Book fed with the events
// holds exactly the top MarketDepth levels.
//
// RptSeq (tag 83) is tracked per instrument across every template that carries it. A gap, an
// MDPriceLevel inconsistent with the book or a full sink puts that instrument into recovery:
// its entries are skipped until apply_snapshot() (driven by Mdp3Feed). Sequencing of packets
// and A/B arbitration are Mdp3Feed's job; the decoder alone is the codecs::Decoder of one
// already-sequenced line.
//
// Hot path: decode_packet() is noexcept and allocation-free; all tables are allocated by the
// constructor.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/mdp3/generated/mdp3_schema.hpp"
#include "fastmm/codecs/mdp3/mdp3_instruments.hpp"
#include "fastmm/codecs/mdp3/mdp3_packet.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/event_sink.hpp"
#include "fastmm/venues/feed.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace fastmm::codecs::mdp3 {

// One side of an MBP book; levels[0] is the best (MDPriceLevel 1).
struct MbpSide {
  std::array<Level, kMaxMbpDepth> levels{};
  std::uint8_t count = 0;
  [[nodiscard]] std::span<const Level> view() const noexcept { return {levels.data(), count}; }
};

struct MbpBookState {
  std::array<MbpSide, 2> sides{};   // [0] bids (MDEntryType '0'), [1] offers ('1')
  std::uint32_t rpt_seq = 0;        // last applied RptSeq
  std::uint64_t transact_time = 0;  // TransactTime of the last applied entry
  bool recovering = false;          // entries skipped until a snapshot
  bool rpt_unknown = false;         // next RptSeq is taken as the baseline
  [[nodiscard]] const MbpSide& bids() const noexcept { return sides[0]; }
  [[nodiscard]] const MbpSide& asks() const noexcept { return sides[1]; }
};

struct Mdp3DecoderConfig {
  VenueId venue{};
};

// TradeMsg::pad_[0] marker for CME AggressorSide 0 (no aggressor: opening trades, trades after a
// pause, implied participation). The core TradeMsg has no "no aggressor" side, so such trades
// carry aggressor == Side::Buy plus this marker; consumers that model queue depletion should
// check it.
inline constexpr std::uint8_t kTradeNoAggressor = 1;

struct Mdp3DecoderStats {
  std::uint64_t packets = 0;
  std::uint64_t table_full = 0;  // definitions dropped: Mdp3InstrumentTable is full
  std::uint64_t messages = 0;
  std::uint64_t malformed = 0;          // truncated packets / messages, bad group bounds
  std::uint64_t unknown_templates = 0;  // skipped by MsgSize
  std::uint64_t other_schema = 0;       // schemaId != 1, skipped
  std::uint64_t unknown_security = 0;   // entries for a SecurityID without a definition
  std::uint64_t book_entries = 0;       // applied outright MBP entries
  std::uint64_t implied_ignored = 0;    // MDEntryType E / F: implied book is not maintained
  std::uint64_t other_entry_types = 0;  // w / x (EBS market best) and anything unknown
  std::uint64_t book_deltas = 0;        // BookDeltaMsg emitted
  std::uint64_t book_snapshots = 0;     // BookSnapshotMsg emitted
  std::uint64_t trades = 0;             // TradeMsg emitted
  std::uint64_t trade_adjustments = 0;  // trade summary Change / Delete: counted, not emitted
  std::uint64_t stat_entries = 0;       // entries of templates 37/49/50/51
  std::uint64_t rpt_gaps = 0;
  std::uint64_t stale_entries = 0;  // RptSeq <= last applied (replayed duplicates)
  std::uint64_t book_errors = 0;    // MDPriceLevel inconsistent with the book
  std::uint64_t price_rejects = 0;  // price not representable at 1e-8
  std::uint64_t channel_resets = 0;
  std::uint64_t definitions = 0;
  std::uint64_t security_status = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t snapshots_applied = 0;
  std::uint64_t snapshots_rejected = 0;
  std::uint64_t sink_overflows = 0;
};

// Conditions the owner has to act on, accumulated until take_events().
struct Mdp3DecodeEvents {
  bool rpt_gap = false;
  bool book_error = false;
  bool overflow = false;
  bool channel_reset = false;
  [[nodiscard]] bool needs_recovery() const noexcept { return rpt_gap || book_error || overflow; }
};

class Mdp3Decoder {
 public:
  explicit Mdp3Decoder(const Mdp3DecoderConfig& cfg = {});
  ~Mdp3Decoder();
  Mdp3Decoder(const Mdp3Decoder&) = delete;
  Mdp3Decoder& operator=(const Mdp3Decoder&) = delete;

  // codecs::Decoder: `frame.payload` is one UDP datagram (packet header + messages).
  [[nodiscard]] venues::ParseStatus decode(const FrameView& frame,
                                           std::int64_t rx_ts,
                                           venues::EventSink& sink) noexcept {
    return decode_packet(frame.payload, rx_ts, sink);
  }

  // Decodes every message of one datagram. With `only` >= 0 just the book / trade / statistics
  // entries of that table index are processed (replay after a snapshot); definitions, status
  // and channel resets are skipped.
  venues::ParseStatus decode_packet(std::span<const std::byte> datagram,
                                    std::int64_t rx_ts,
                                    venues::EventSink& sink,
                                    std::int32_t only = -1) noexcept;

  // Replaces the book of the snapshot's instrument, sets its RptSeq baseline, leaves recovery
  // and emits a BookSnapshotMsg. False, with nothing changed, for an unknown SecurityID,
  // inconsistent levels, an unrepresentable price or a full sink.
  bool apply_snapshot(const schema::SnapshotFullRefresh52& snap,
                      std::int64_t rx_ts,
                      venues::EventSink& sink) noexcept;

  // Read-only check used before replaying buffered packets: walks the entries of table index
  // `index` in `datagram`; RptSeq < next_expected is skipped, == next_expected advances it,
  // anything larger returns false.
  [[nodiscard]] bool rpt_continuous(std::span<const std::byte> datagram,
                                    std::int32_t index,
                                    std::uint32_t& next_expected) const noexcept;

  void mark_recovering(std::size_t index) noexcept;
  void mark_all_recovering() noexcept;
  // For an instrument that did not appear in a complete snapshot loop (no book activity): empty
  // book, leaves recovery, the next RptSeq becomes the baseline. Emits an empty BookSnapshotMsg.
  bool reset_empty(std::size_t index, std::int64_t rx_ts, venues::EventSink& sink) noexcept;
  // Instruments added while this is on start in recovery (late join); otherwise they start
  // synchronised at RptSeq 0 (start of the week, the first entry must be RptSeq 1).
  void set_new_instruments_recovering(bool on) noexcept { new_recovering_ = on; }

  // Registers a SecurityID without a definition message. Returns the table index or -1.
  std::int32_t add_instrument(std::int32_t security_id, std::uint8_t depth = kMaxMbpDepth) noexcept;

  [[nodiscard]] std::size_t recovering_count() const noexcept { return recovering_; }
  [[nodiscard]] Mdp3DecodeEvents take_events() noexcept {
    const Mdp3DecodeEvents e = events_;
    events_ = {};
    return e;
  }
  [[nodiscard]] const Mdp3InstrumentTable& instruments() const noexcept { return *instruments_; }
  [[nodiscard]] Mdp3InstrumentTable& instruments() noexcept { return *instruments_; }
  [[nodiscard]] const MbpBookState& book(std::size_t index) const noexcept { return books_[index]; }
  [[nodiscard]] const Mdp3DecoderStats& stats() const noexcept { return stats_; }

 private:
  struct Batch;
  enum class RptCheck : std::uint8_t { Apply, Stale, Skip };

  void decode_book(const MessageView& m,
                   std::int64_t rx_ts,
                   venues::EventSink& sink,
                   std::int32_t only) noexcept;
  void decode_trades(const MessageView& m,
                     std::int64_t rx_ts,
                     venues::EventSink& sink,
                     std::int32_t only) noexcept;
  template <class Msg>
  void account_rpt(const MessageView& m, std::int32_t only) noexcept;
  void decode_definition(const MessageView& m) noexcept;
  void decode_status(const MessageView& m) noexcept;
  void channel_reset(const MessageView& m, std::int64_t rx_ts, venues::EventSink& sink) noexcept;

  RptCheck check_rpt(std::size_t index, std::uint32_t rpt) noexcept;
  void apply_entry(std::size_t index,
                   std::size_t side,
                   const schema::MDIncrementalRefreshBook46::NoMDEntries& e) noexcept;
  void flush(venues::EventSink& sink, std::int64_t rx_ts) noexcept;
  bool emit_snapshot(std::size_t index,
                     const MbpBookState& state,
                     std::uint64_t transact_time,
                     std::int64_t rx_ts,
                     venues::EventSink& sink) noexcept;
  void book_error(std::size_t index) noexcept;
  void init_book(std::size_t index, std::uint8_t depth) noexcept;

  Mdp3DecoderConfig cfg_;
  std::unique_ptr<Mdp3InstrumentTable> instruments_;
  std::unique_ptr<MbpBookState[]> books_;
  std::unique_ptr<Batch> batch_;
  Mdp3DecoderStats stats_{};
  Mdp3DecodeEvents events_{};
  std::size_t recovering_ = 0;
  bool new_recovering_ = false;
};

}  // namespace fastmm::codecs::mdp3
