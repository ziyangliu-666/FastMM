#pragma once
// Mdp3Feed (plan 7): one CME MDP 3.0 channel with A/B arbitration, gap detection and snapshot
// recovery on top of Mdp3Decoder.
//
// Arbitration. Incremental feeds A and B carry identical packets ("MDP 3.0 - Recovery
// Services": process both). The first copy of each MsgSeqNum wins; later copies are dropped.
// A packet that arrives ahead of sequence is held (reorder_window slots) because the other line
// may still deliver the missing one; the hole is declared lost when a packet beyond the window
// arrives or when the oldest held packet is gap_timeout_ns old (rx_ts or on_timer()).
//
// Recovery ("MDP 3.0 - MBP and MBOFD Market Recovery"). A packet gap makes every book suspect:
// all instruments enter recovery, the feed emits ConnectionStateMsg{Resyncing, channel 0} and
// buffers the incremental packets it keeps receiving. A per-instrument RptSeq gap (or a book
// inconsistency, or a full sink) does the same for that instrument only; the other instruments
// keep updating. For each SnapshotFullRefresh52 of a recovering instrument:
//   * the buffered packets must cover every packet after tag 369 LastMsgSeqNumProcessed,
//   * the buffered entries of the instrument with RptSeq > the snapshot's tag 83 RptSeq must be
//     contiguous from RptSeq + 1,
// otherwise the snapshot is skipped and the next loop iteration is awaited. When both hold, the
// snapshot replaces the book (BookSnapshotMsg) and the buffered entries after it are replayed
// (BookDeltaMsg / TradeMsg). This is CME's "drop cached updates with a packet sequence number <
// LastMsgSeqNumProcessed", with the TransactTime comparison replaced by a per-entry RptSeq check.
// Instruments without book activity have no snapshot; once a complete loop (snapshot packet 1
// through TotNumReports messages) has been seen, instruments that were recovering for the whole
// loop and never appeared are reset to an empty book. When no instrument is recovering the buffer
// is dropped and ConnectionStateMsg{Live} is emitted.
//
// Start of the week: MsgSeqNum restarts at 1 weekly ("SBE Technical Headers"), so a first packet
// with MsgSeqNum 1 starts Live; any other first packet is a late join and starts Resyncing.
//
// Hot path (on_incremental, in sequence or duplicate) is noexcept and allocation-free; every
// buffer is allocated by the constructor.
#include "fastmm/codecs/mdp3/mdp3_decoder.hpp"
#include "fastmm/core/enums.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace fastmm::codecs::mdp3 {

enum class FeedLine : std::uint8_t { A = 0, B = 1 };

// ConnectionStateMsg::reason_code when the feed leaves Live.
enum class ResyncReason : std::int32_t {
  None = 0,
  LateJoin = 1,
  PacketGap = 2,
  RptSeqGap = 3,
  BookError = 4,
  SinkOverflow = 5,
};

struct Mdp3FeedConfig {
  Mdp3DecoderConfig decoder{};
  // Packets held while waiting for the other line (rounded up to a power of two, <= 1024).
  std::uint32_t reorder_window = 64;
  // A missing packet is declared lost once the oldest held packet is this old. CME publishes no
  // value: it depends on the A/B latency skew at the site.
  std::int64_t gap_timeout_ns = 2'000'000;
  // Incremental packets kept while resyncing; the oldest are dropped when full.
  std::uint32_t recovery_packets = 8192;
};

struct Mdp3FeedStats {
  std::array<std::uint64_t, 2> packets{};  // per line, as received
  std::uint64_t accepted = 0;              // processed in sequence
  std::uint64_t duplicates = 0;            // copies of processed or held packets
  std::uint64_t held = 0;                  // arrived ahead of sequence
  std::uint64_t gaps = 0;
  std::uint64_t lost_packets = 0;
  std::uint64_t malformed = 0;
  std::uint64_t oversize = 0;  // larger than kMaxPacketBytes: cannot be held or buffered
  std::uint64_t resyncs = 0;
  std::uint64_t recovered_instruments = 0;
  std::uint64_t snapshot_packets = 0;
  std::uint64_t snapshot_duplicates = 0;
  std::uint64_t snapshots_used = 0;
  std::uint64_t snapshots_waiting = 0;  // older than the buffer, or an RptSeq hole: next loop
  std::uint64_t loops_completed = 0;
  std::uint64_t replayed_packets = 0;
  std::uint64_t buffer_overflows = 0;
  std::uint64_t state_msg_overflows = 0;
};

class Mdp3Feed {
 public:
  explicit Mdp3Feed(const Mdp3FeedConfig& cfg = {});
  ~Mdp3Feed();
  Mdp3Feed(const Mdp3Feed&) = delete;
  Mdp3Feed& operator=(const Mdp3Feed&) = delete;

  // One datagram of incremental line A or B.
  venues::ParseStatus on_incremental(FeedLine line,
                                     std::span<const std::byte> datagram,
                                     std::int64_t rx_ts,
                                     venues::EventSink& sink) noexcept;
  // One datagram of the Market Recovery (snapshot) feed; ignored unless resyncing. Feed it one
  // line, or both: copies of a snapshot packet already seen in the current loop are dropped.
  venues::ParseStatus on_snapshot(std::span<const std::byte> datagram,
                                  std::int64_t rx_ts,
                                  venues::EventSink& sink) noexcept;
  // Instrument definition (replay) feed. Definitions are idempotent and not sequenced here.
  venues::ParseStatus on_instrument_definition(std::span<const std::byte> datagram,
                                               std::int64_t rx_ts,
                                               venues::EventSink& sink) noexcept;
  // Declares held packets lost once they are older than gap_timeout_ns.
  void on_timer(std::int64_t now_ns, venues::EventSink& sink) noexcept;

  // codecs::Decoder over line A.
  [[nodiscard]] venues::ParseStatus decode(const FrameView& frame,
                                           std::int64_t rx_ts,
                                           venues::EventSink& sink) noexcept {
    return on_incremental(FeedLine::A, frame.payload, rx_ts, sink);
  }

  [[nodiscard]] ConnState state() const noexcept { return state_; }
  [[nodiscard]] bool needs_snapshots() const noexcept { return state_ == ConnState::Resyncing; }
  [[nodiscard]] ResyncReason last_resync_reason() const noexcept { return reason_; }
  [[nodiscard]] std::uint32_t next_expected_seq() const noexcept { return expected_; }
  [[nodiscard]] std::size_t held_packets() const noexcept { return held_count_; }
  [[nodiscard]] std::size_t buffered_packets() const noexcept { return buf_count_; }
  [[nodiscard]] Mdp3Decoder& decoder() noexcept { return decoder_; }
  [[nodiscard]] const Mdp3Decoder& decoder() const noexcept { return decoder_; }
  [[nodiscard]] const Mdp3FeedStats& stats() const noexcept { return stats_; }

 private:
  struct Slot {
    std::uint32_t seq = 0;
    std::uint16_t len = 0;
    bool used = false;
    std::int64_t rx_ts = 0;
    std::array<std::byte, kMaxPacketBytes> data{};
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data.data(), len}; }
  };

  venues::ParseStatus process(std::uint32_t seq,
                              std::span<const std::byte> datagram,
                              std::int64_t rx_ts,
                              venues::EventSink& sink) noexcept;
  [[nodiscard]] Slot* held_slot(std::uint32_t seq) noexcept;
  void process_held(Slot& slot, venues::EventSink& sink) noexcept;
  void drain(venues::EventSink& sink) noexcept;
  void advance_to(std::uint32_t target, std::int64_t rx_ts, venues::EventSink& sink) noexcept;
  void expire_held(std::int64_t now_ns, venues::EventSink& sink) noexcept;
  void on_gap(std::uint32_t lost, std::int64_t rx_ts, venues::EventSink& sink) noexcept;
  void enter_resync(ResyncReason reason, std::int64_t rx_ts, venues::EventSink& sink) noexcept;
  void maybe_live(std::int64_t rx_ts, venues::EventSink& sink) noexcept;
  void emit_state(ConnState s,
                  ResyncReason reason,
                  std::int64_t rx_ts,
                  venues::EventSink& sink) noexcept;
  void buffer_append(std::uint32_t seq,
                     std::span<const std::byte> datagram,
                     std::int64_t rx_ts) noexcept;
  // Empties the buffer; snapshots must then cover everything from `floor` on.
  void buffer_clear(std::uint32_t floor) noexcept;
  [[nodiscard]] const Slot& buffered(std::size_t i) const noexcept {
    return buffer_[(buf_head_ + i) % cfg_.recovery_packets];
  }
  void try_recover(std::int32_t index,
                   const schema::SnapshotFullRefresh52& snap,
                   std::int64_t rx_ts,
                   venues::EventSink& sink) noexcept;
  void finish_loop(std::int64_t rx_ts, venues::EventSink& sink) noexcept;

  Mdp3FeedConfig cfg_;
  Mdp3Decoder decoder_;
  std::uint32_t window_ = 64;  // power of two
  std::unique_ptr<Slot[]> held_;
  std::unique_ptr<Slot[]> buffer_;
  std::unique_ptr<std::uint8_t[]> loop_mark_;  // per instrument: 0 none, 1 seen, 2 eligible
  Mdp3FeedStats stats_{};

  ConnState state_ = ConnState::Connecting;
  ResyncReason reason_ = ResyncReason::None;
  bool started_ = false;
  std::uint32_t expected_ = 0;
  std::uint32_t last_processed_ = 0;
  std::size_t held_count_ = 0;
  std::int64_t oldest_held_rx_ = 0;
  std::size_t buf_head_ = 0;
  std::size_t buf_count_ = 0;
  std::uint32_t buffer_floor_ = 0;  // first MsgSeqNum the (possibly empty) buffer stands for

  bool loop_active_ = false;
  std::uint32_t loop_next_seq_ = 0;
  std::uint32_t loop_msgs_ = 0;
  std::uint32_t loop_total_ = 0;
};

}  // namespace fastmm::codecs::mdp3
