#include "fastmm/codecs/mdp3/mdp3_feed.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace fastmm::codecs::mdp3 {

using venues::EventSink;
using venues::ParseStatus;

Mdp3Feed::Mdp3Feed(const Mdp3FeedConfig& cfg)
    : cfg_(cfg),
      decoder_(cfg.decoder),
      window_(std::bit_ceil(std::clamp<std::uint32_t>(cfg.reorder_window, 1, 1024))),
      held_(std::make_unique<Slot[]>(window_)),
      buffer_(std::make_unique<Slot[]>(std::max<std::uint32_t>(cfg.recovery_packets, 1))),
      loop_mark_(std::make_unique<std::uint8_t[]>(kMaxMdp3Instruments)) {
  cfg_.recovery_packets = std::max<std::uint32_t>(cfg.recovery_packets, 1);
}

Mdp3Feed::~Mdp3Feed() = default;

// ---- incremental lines --------------------------------------------------------------------------

ParseStatus Mdp3Feed::on_incremental(FeedLine line,
                                     std::span<const std::byte> datagram,
                                     std::int64_t rx_ts,
                                     EventSink& sink) noexcept {
  ++stats_.packets[static_cast<std::size_t>(line)];
  if (FASTMM_UNLIKELY(datagram.size() < kPacketHeaderSize)) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const std::uint32_t seq = sbe::load_le<std::uint32_t>(datagram.data());
  if (FASTMM_UNLIKELY(!started_)) {
    started_ = true;
    expected_ = seq;
    buffer_floor_ = seq;
    if (seq == 1) {
      state_ = ConnState::Live;
      emit_state(ConnState::Live, ResyncReason::None, rx_ts, sink);
    } else {
      decoder_.set_new_instruments_recovering(true);
      decoder_.mark_all_recovering();
      enter_resync(ResyncReason::LateJoin, rx_ts, sink);
    }
  }
  if (held_count_ != 0) expire_held(rx_ts, sink);

  if (FASTMM_LIKELY(seq == expected_)) {
    const ParseStatus st = process(seq, datagram, rx_ts, sink);
    ++expected_;
    if (held_count_ != 0) drain(sink);
    return st;
  }
  if (seq < expected_) {
    ++stats_.duplicates;
    return ParseStatus::Ignored;
  }
  // Ahead of sequence: hold it for the other line, unless it cannot be held.
  if (seq - expected_ >= window_ || datagram.size() > kMaxPacketBytes) {
    if (datagram.size() > kMaxPacketBytes) ++stats_.oversize;
    advance_to(seq, rx_ts, sink);  // everything still missing below seq is lost
    const ParseStatus st = process(seq, datagram, rx_ts, sink);
    ++expected_;
    if (held_count_ != 0) drain(sink);
    return st;
  }
  Slot& slot = held_[seq & (window_ - 1)];
  if (slot.used && slot.seq == seq) {
    ++stats_.duplicates;
    return ParseStatus::Ignored;
  }
  slot.used = true;
  slot.seq = seq;
  slot.len = static_cast<std::uint16_t>(datagram.size());
  slot.rx_ts = rx_ts;
  std::memcpy(slot.data.data(), datagram.data(), datagram.size());
  oldest_held_rx_ = held_count_ == 0 ? rx_ts : std::min(oldest_held_rx_, rx_ts);
  ++held_count_;
  ++stats_.held;
  return ParseStatus::Ok;
}

ParseStatus Mdp3Feed::process(std::uint32_t seq,
                              std::span<const std::byte> datagram,
                              std::int64_t rx_ts,
                              EventSink& sink) noexcept {
  ++stats_.accepted;
  last_processed_ = seq;
  if (state_ == ConnState::Resyncing) buffer_append(seq, datagram, rx_ts);
  const ParseStatus st = decoder_.decode_packet(datagram, rx_ts, sink);
  const Mdp3DecodeEvents ev = decoder_.take_events();
  if (FASTMM_UNLIKELY(ev.channel_reset || ev.needs_recovery())) {
    if (ev.channel_reset) {
      // Books were emptied and are rebuilt from the incremental feed; older packets are useless.
      buffer_clear(seq + 1);
      decoder_.set_new_instruments_recovering(false);
    }
    if (ev.needs_recovery() && state_ != ConnState::Resyncing) {
      buffer_clear(seq);
      buffer_append(seq, datagram, rx_ts);
      const ResyncReason why = ev.rpt_gap      ? ResyncReason::RptSeqGap
                               : ev.book_error ? ResyncReason::BookError
                                               : ResyncReason::SinkOverflow;
      enter_resync(why, rx_ts, sink);
    }
    maybe_live(rx_ts, sink);
  }
  return st;
}

Mdp3Feed::Slot* Mdp3Feed::held_slot(std::uint32_t seq) noexcept {
  if (held_count_ == 0) return nullptr;
  Slot& s = held_[seq & (window_ - 1)];
  return s.used && s.seq == seq ? &s : nullptr;
}

void Mdp3Feed::process_held(Slot& slot, EventSink& sink) noexcept {
  slot.used = false;
  --held_count_;
  static_cast<void>(process(slot.seq, slot.bytes(), slot.rx_ts, sink));
}

void Mdp3Feed::drain(EventSink& sink) noexcept {
  while (Slot* s = held_slot(expected_)) {
    process_held(*s, sink);
    ++expected_;
  }
  if (held_count_ != 0) {
    oldest_held_rx_ = std::numeric_limits<std::int64_t>::max();
    for (std::uint32_t i = 0; i < window_; ++i) {
      if (held_[i].used) oldest_held_rx_ = std::min(oldest_held_rx_, held_[i].rx_ts);
    }
  }
}

// Moves expected_ up to `target`: held packets below it are processed in order and the holes
// between them are lost.
void Mdp3Feed::advance_to(std::uint32_t target, std::int64_t rx_ts, EventSink& sink) noexcept {
  while (expected_ < target) {
    if (Slot* s = held_slot(expected_)) {
      process_held(*s, sink);
      ++expected_;
      continue;
    }
    const std::uint32_t first = expected_;
    while (expected_ < target && held_slot(expected_) == nullptr) {
      if (held_count_ == 0) {
        expected_ = target;
        break;
      }
      ++expected_;
    }
    on_gap(expected_ - first, rx_ts, sink);
  }
}

void Mdp3Feed::expire_held(std::int64_t now_ns, EventSink& sink) noexcept {
  if (held_count_ == 0 || now_ns - oldest_held_rx_ < cfg_.gap_timeout_ns) return;
  std::uint32_t lowest = std::numeric_limits<std::uint32_t>::max();
  for (std::uint32_t i = 0; i < window_; ++i) {
    if (held_[i].used) lowest = std::min(lowest, held_[i].seq);
  }
  advance_to(lowest, now_ns, sink);
  drain(sink);
}

void Mdp3Feed::on_timer(std::int64_t now_ns, EventSink& sink) noexcept {
  expire_held(now_ns, sink);
}

void Mdp3Feed::on_gap(std::uint32_t lost, std::int64_t rx_ts, EventSink& sink) noexcept {
  ++stats_.gaps;
  stats_.lost_packets += lost;
  // Every book may be wrong; only snapshots taken after the hole can repair them.
  buffer_clear(expected_);
  decoder_.mark_all_recovering();
  decoder_.set_new_instruments_recovering(true);
  loop_active_ = false;  // loop marks are retaken at the next snapshot packet 1
  enter_resync(ResyncReason::PacketGap, rx_ts, sink);
  maybe_live(rx_ts, sink);
}

void Mdp3Feed::enter_resync(ResyncReason reason, std::int64_t rx_ts, EventSink& sink) noexcept {
  if (state_ == ConnState::Resyncing) return;
  state_ = ConnState::Resyncing;
  reason_ = reason;
  loop_active_ = false;
  ++stats_.resyncs;
  emit_state(ConnState::Resyncing, reason, rx_ts, sink);
}

void Mdp3Feed::maybe_live(std::int64_t rx_ts, EventSink& sink) noexcept {
  if (state_ != ConnState::Resyncing || decoder_.recovering_count() != 0) return;
  state_ = ConnState::Live;
  buffer_clear(expected_);
  loop_active_ = false;
  decoder_.set_new_instruments_recovering(false);
  emit_state(ConnState::Live, ResyncReason::None, rx_ts, sink);
}

void Mdp3Feed::emit_state(ConnState s,
                          ResyncReason reason,
                          std::int64_t rx_ts,
                          EventSink& sink) noexcept {
  auto* m = sink.reserve<ConnectionStateMsg>();
  if (m == nullptr) {
    ++stats_.state_msg_overflows;
    return;
  }
  init_header(*m, EventType::ConnectionState, InstrumentId{}, cfg_.decoder.venue);
  m->hdr.recv_ts = Timestamp{rx_ts};
  m->state = s;
  m->channel = 0;
  std::memset(m->pad0_, 0, sizeof(m->pad0_));
  m->reason_code = static_cast<std::int32_t>(reason);
  std::memset(m->pad_, 0, sizeof(m->pad_));
  sink.commit();
}

// ---- recovery buffer ----------------------------------------------------------------------------

void Mdp3Feed::buffer_append(std::uint32_t seq,
                             std::span<const std::byte> datagram,
                             std::int64_t rx_ts) noexcept {
  if (datagram.size() > kMaxPacketBytes) {
    buffer_clear(seq + 1);  // no copy possible: snapshots must be at least this recent
    return;
  }
  const std::size_t cap = cfg_.recovery_packets;  // >= 1, clamped by the constructor
  if (FASTMM_UNLIKELY(cap == 0)) return;
  if (buf_count_ == cap) {
    buf_head_ = (buf_head_ + 1) % cap;
    --buf_count_;
    ++stats_.buffer_overflows;
  }
  Slot& s = buffer_[(buf_head_ + buf_count_) % cap];
  s.used = true;
  s.seq = seq;
  s.len = static_cast<std::uint16_t>(datagram.size());
  s.rx_ts = rx_ts;
  std::memcpy(s.data.data(), datagram.data(), datagram.size());
  ++buf_count_;
}

void Mdp3Feed::buffer_clear(std::uint32_t floor) noexcept {
  buf_head_ = 0;
  buf_count_ = 0;
  buffer_floor_ = floor;
}

// ---- snapshot feed ------------------------------------------------------------------------------

ParseStatus Mdp3Feed::on_snapshot(std::span<const std::byte> datagram,
                                  std::int64_t rx_ts,
                                  EventSink& sink) noexcept {
  ++stats_.snapshot_packets;
  if (state_ != ConnState::Resyncing) return ParseStatus::Ignored;
  PacketCursor cur(datagram);
  if (!cur.valid()) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const std::uint32_t seq = cur.header().msg_seq_num;
  // Loop accounting: an iteration starts at snapshot packet 1 and is complete once TotNumReports
  // snapshot messages arrived in consecutive packets. A second copy of packet 1 simply restarts
  // the count.
  if (seq == 1) {
    loop_active_ = true;
    loop_next_seq_ = 1;
    loop_msgs_ = 0;
    loop_total_ = 0;
    for (std::size_t i = 0; i < decoder_.instruments().size(); ++i) {
      loop_mark_[i] = decoder_.book(i).recovering ? 2 : 0;
    }
  }
  if (loop_active_) {
    if (seq < loop_next_seq_) {
      ++stats_.snapshot_duplicates;
      return ParseStatus::Ignored;
    }
    if (seq > loop_next_seq_) loop_active_ = false;  // a snapshot packet was missed
    loop_next_seq_ = seq + 1;
  }

  bool any = false;
  MessageView m;
  while (cur.next(m)) {
    if (m.header.schema_id != kMdpSchemaId ||
        m.header.template_id != schema::SnapshotFullRefresh52::kTemplateId) {
      continue;
    }
    const schema::SnapshotFullRefresh52 snap(m.body, m.header.block_length, m.header.version);
    if (!snap.valid()) {
      ++stats_.malformed;
      continue;
    }
    any = true;
    ++loop_msgs_;
    loop_total_ = snap.tot_num_reports();
    const std::int32_t index = decoder_.instruments().index_of(snap.security_id());
    if (index < 0) continue;
    const auto i = static_cast<std::size_t>(index);
    if (loop_mark_[i] != 0) loop_mark_[i] = 1;
    if (decoder_.book(i).recovering) try_recover(index, snap, rx_ts, sink);
  }
  if (cur.malformed()) ++stats_.malformed;
  if (loop_active_ && loop_total_ != 0 && loop_msgs_ >= loop_total_) finish_loop(rx_ts, sink);
  maybe_live(rx_ts, sink);
  return any ? ParseStatus::Ok : ParseStatus::Ignored;
}

void Mdp3Feed::try_recover(std::int32_t index,
                           const schema::SnapshotFullRefresh52& snap,
                           std::int64_t rx_ts,
                           EventSink& sink) noexcept {
  // The buffer must hold every incremental packet after LastMsgSeqNumProcessed. A snapshot older
  // than that waits for the next loop iteration.
  const std::uint64_t last_seq = snap.last_msg_seq_num_processed();
  const std::uint64_t first_covered =
      buf_count_ != 0 ? buffered(0).seq
                      : std::max<std::uint64_t>(buffer_floor_, std::uint64_t{last_processed_} + 1);
  if (last_seq + 1 < first_covered) {
    ++stats_.snapshots_waiting;
    return;
  }
  std::uint32_t next = snap.rpt_seq() + 1;
  for (std::size_t k = 0; k < buf_count_; ++k) {
    if (!decoder_.rpt_continuous(buffered(k).bytes(), index, next)) {
      ++stats_.snapshots_waiting;
      return;
    }
  }
  if (!decoder_.apply_snapshot(snap, rx_ts, sink)) return;
  for (std::size_t k = 0; k < buf_count_; ++k) {
    const Slot& s = buffered(k);
    static_cast<void>(decoder_.decode_packet(s.bytes(), s.rx_ts, sink, index));
    ++stats_.replayed_packets;
  }
  static_cast<void>(decoder_.take_events());  // a failed replay re-marks the instrument itself
  if (!decoder_.book(static_cast<std::size_t>(index)).recovering) {
    ++stats_.snapshots_used;
    ++stats_.recovered_instruments;
  }
}

void Mdp3Feed::finish_loop(std::int64_t rx_ts, EventSink& sink) noexcept {
  ++stats_.loops_completed;
  loop_active_ = false;
  for (std::size_t i = 0; i < decoder_.instruments().size(); ++i) {
    // Recovering since before the loop started and absent from it: no book activity this week.
    if (loop_mark_[i] == 2 && decoder_.book(i).recovering && decoder_.reset_empty(i, rx_ts, sink)) {
      ++stats_.recovered_instruments;
    }
    loop_mark_[i] = 0;
  }
}

ParseStatus Mdp3Feed::on_instrument_definition(std::span<const std::byte> datagram,
                                               std::int64_t rx_ts,
                                               EventSink& sink) noexcept {
  const ParseStatus st = decoder_.decode_packet(datagram, rx_ts, sink);
  static_cast<void>(decoder_.take_events());
  return st;
}

}  // namespace fastmm::codecs::mdp3
