#pragma once
// MoldUDP64 1.00 (fastmm::codecs::moldudp).
//
// Source: "MoldUDP64 Protocol Specification V 1.00", Nasdaq (moldudp64.pdf, formatting
// update 8/2/24). All MoldUDP64 numbers are big-endian.
//
//   Downstream Packet   Session(10, alnum) | Sequence Number(8) | Message Count(2) | blocks
//   Message Block       Message Length(2, excludes itself) | Message Data
//   Heartbeat           Message Count == 0,      Sequence Number = next expected
//   End of Session      Message Count == 0xFFFF, Sequence Number = next expected
//   Request Packet      Session(10) | Sequence Number(8) | Requested Message Count(2)
//
// Pieces:
//   parse_packet()   validates one datagram (header + exactly `count` message blocks)
//   MessageFramer    Framer over the message blocks (one FrameView per message)
//   PacketBuilder    downstream packets; write_heartbeat / write_end_of_session / write_request
//   Receiver<H>      SessionLayer that delivers messages to H in sequence order from one or
//                    more lines (A/B arbitration), holds packets ahead of a gap in a reorder
//                    buffer, declares a gap after gap_timeout_ns and recovers it with Request
//                    Packets (re-sent on on_timer() until the gap closes) or, without a
//                    re-request server, skips it and reports it to H.
//   Transmitter      a sender with a fixed-capacity history (optionally a ring) that answers
//                    Request Packets (simulation, tests, replay).
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/core/config_macros.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

namespace fastmm::codecs::moldudp {

inline constexpr std::size_t kSessionLength = 10;
inline constexpr std::size_t kHeaderLength = 20;
inline constexpr std::size_t kRequestLength = 20;
inline constexpr std::uint16_t kHeartbeatCount = 0;
inline constexpr std::uint16_t kEndOfSessionCount = 0xFFFF;
inline constexpr std::size_t kMaxMessageLength = 0xFFFF;
// Largest data count in one packet (0xFFFF is the end-of-session marker).
inline constexpr std::uint16_t kMaxMessagesPerPacket = 0xFFFE;
// Ethernet MTU 1500 - IPv4 header 20 - UDP header 8.
inline constexpr std::size_t kDefaultMaxDatagram = 1472;

using SessionId = std::array<char, kSessionLength>;
inline constexpr SessionId kBlankSession = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

[[nodiscard]] inline SessionId make_session(std::string_view s) noexcept {
  SessionId id{};
  nasdaq::put_alpha(id.data(), id.size(), s);
  return id;
}
[[nodiscard]] inline bool is_blank(const SessionId& s) noexcept {
  return s == kBlankSession;
}

#pragma pack(push, 1)
struct DownstreamHeader {
  char session[10];
  be64_t sequence_number;
  be16_t message_count;
};
struct RequestPacket {
  char session[10];
  be64_t sequence_number;
  be16_t requested_message_count;
};
#pragma pack(pop)
static_assert(sizeof(DownstreamHeader) == kHeaderLength);
static_assert(offsetof(DownstreamHeader, sequence_number) == 10 &&
              offsetof(DownstreamHeader, message_count) == 18);
static_assert(sizeof(RequestPacket) == kRequestLength);
static_assert(offsetof(RequestPacket, sequence_number) == 10 &&
              offsetof(RequestPacket, requested_message_count) == 18);

enum class PacketKind : std::uint8_t { Data = 0, Heartbeat = 1, EndOfSession = 2 };

struct PacketView {
  SessionId session = kBlankSession;
  std::uint64_t sequence = 0;  // first message (Data) or next expected (Heartbeat / EOS)
  std::uint16_t count = 0;     // message blocks (0 for Heartbeat / EOS)
  PacketKind kind = PacketKind::Data;
  std::span<const std::byte> blocks;
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return sequence + count; }
};

// False when the datagram is shorter than the header, a block is truncated or bytes follow
// the last announced block.
[[nodiscard]] bool parse_packet(std::span<const std::byte> datagram, PacketView& out) noexcept;

struct RequestView {
  SessionId session = kBlankSession;
  std::uint64_t sequence = 0;
  std::uint16_t count = 0;
};
[[nodiscard]] bool parse_request(std::span<const std::byte> datagram, RequestView& out) noexcept;

// Splits message blocks. kind is always 0; a zero-length message is a valid frame.
struct MessageFramer {
  [[nodiscard]] FrameView next(std::span<const std::byte> in) const noexcept {
    if (in.size() < 2) return {};
    const std::size_t n = nasdaq::load_be16(in.data());
    if (in.size() < 2 + n) return {};
    return FrameView{in.subspan(2, n), 2 + n, 0};
  }
};
static_assert(Framer<MessageFramer>);

// Walks the messages of a parsed packet with their implied sequence numbers.
class MessageIterator {
 public:
  explicit MessageIterator(const PacketView& p) noexcept
      : rest_(p.blocks), seq_(p.sequence), left_(p.count) {}
  bool next(std::uint64_t& seq, std::span<const std::byte>& msg) noexcept {
    if (left_ == 0) return false;
    const FrameView f = MessageFramer{}.next(rest_);
    if (!f.complete()) return false;
    rest_ = rest_.subspan(f.consumed);
    msg = f.payload;
    seq = seq_++;
    --left_;
    return true;
  }

 private:
  std::span<const std::byte> rest_;
  std::uint64_t seq_;
  std::uint16_t left_;
};

// Builds one downstream packet in a caller buffer.
class PacketBuilder {
 public:
  PacketBuilder(std::span<std::byte> buf,
                const SessionId& session,
                std::uint64_t first_sequence) noexcept;
  // False when the message does not fit in the buffer (or the packet already holds 0xFFFE).
  bool add(std::span<const std::byte> msg) noexcept;
  // Writes the message count; returns the datagram length (0 if the buffer is < 20 bytes).
  std::size_t finish() noexcept;
  [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return first_ + count_; }

 private:
  std::span<std::byte> buf_;
  std::uint64_t first_;
  std::size_t size_ = 0;
  std::uint16_t count_ = 0;
};

std::size_t write_heartbeat(std::span<std::byte> out,
                            const SessionId& session,
                            std::uint64_t next_sequence) noexcept;
std::size_t write_end_of_session(std::span<std::byte> out,
                                 const SessionId& session,
                                 std::uint64_t next_sequence) noexcept;
std::size_t write_request(std::span<std::byte> out,
                          const SessionId& session,
                          std::uint64_t sequence,
                          std::uint16_t count) noexcept;

// ---- receiver ------------------------------------------------------------------------------

// Lines a Receiver keeps statistics for (A, B, a retransmission socket, one spare).
inline constexpr std::size_t kMaxLines = 4;

// Default per-packet metadata: nothing. A receive path passes its own trivially copyable type
// (receive timestamps, line) through Receiver<H, Meta>::on_packet() to H::on_message().
struct NoRxMeta {};

// on_message: next message in sequence order, as on_message(seq, bytes, meta) with the
// metadata of the packet that carried it, or on_message(seq, bytes). send_request: a Request
// Packet to transmit to the re-request server. on_end_of_session: the session ended and every
// message was delivered. The handler must not feed packets back into the receiver from inside
// these callbacks.
template <class H, class Meta = NoRxMeta>
concept ReceiverHandler =
    requires(H& h, std::span<const std::byte> bytes) {
      { h.send_request(bytes) } noexcept;
      { h.on_end_of_session() } noexcept;
    } &&
    (requires(H& h, std::uint64_t seq, std::span<const std::byte> bytes, const Meta& meta) {
      { h.on_message(seq, bytes, meta) } noexcept;
    } || requires(H& h, std::uint64_t seq, std::span<const std::byte> bytes) {
      { h.on_message(seq, bytes) } noexcept;
    });

// Optional: `count` messages from `from_seq` on will never be delivered. Delivery continues
// after them.
template <class H>
concept UnrecoverableGapHandler = requires(H& h, std::uint64_t seq) {
  { h.on_gap_unrecoverable(seq, seq) } noexcept;
};

struct ReceiverConfig {
  SessionId session = kBlankSession;  // blank: adopt the session of the first packet
  std::uint64_t next_sequence = 0;    // 0: start at the first packet received (live join)
  std::uint16_t max_request_count = 1024;
  std::int64_t request_timeout_ns = 250'000'000;  // re-send an unanswered request
  // A request for the same sequence unanswered this many times makes the gap unrecoverable
  // (0: never).
  std::uint32_t max_request_attempts = 4;
  // A gap is declared once the missing sequence has been awaited this long on every line (0:
  // at once); until then packets ahead of it are held and nothing is requested. It covers the
  // A/B arrival skew of the site (ReceiverLineStats::skew_*), as Mdp3FeedConfig::gap_timeout_ns.
  std::int64_t gap_timeout_ns = 2'000'000;
  // Packets held ahead of a gap, each up to max_packet_bytes; allocated at construction.
  std::uint32_t reorder_packets = 256;
  std::size_t max_packet_bytes = kDefaultMaxDatagram;
  // false: no re-request server; a declared gap is unrecoverable.
  bool can_request = true;
  // After End of Session (every message delivered), adopt the session of the next packet with
  // another session id instead of counting it as a mismatch.
  bool follow_session = false;
};

struct ReceiverLineStats {
  std::uint64_t packets = 0;
  std::uint64_t duplicate_packets = 0;  // already delivered or already held
  // Arrival of this line's duplicate copies after the first copy of the same packet (any line).
  std::uint64_t skew_samples = 0;
  std::int64_t skew_last_ns = 0;
  std::int64_t skew_max_ns = 0;
  std::int64_t skew_sum_ns = 0;
};

struct ReceiverStats {
  std::uint64_t packets = 0;
  std::uint64_t messages = 0;  // delivered
  std::uint64_t heartbeats = 0;
  std::uint64_t end_of_session = 0;
  std::uint64_t duplicate_packets = 0;   // already delivered or already held (all lines)
  std::uint64_t ahead_packets = 0;       // started beyond the next expected sequence
  std::uint64_t held_packets = 0;        // ahead packets copied into the reorder buffer
  std::uint64_t reorder_overflow = 0;    // ahead packets dropped: buffer full or too large
  std::uint64_t reorder_high_water = 0;  // most packets held at once
  std::uint64_t gaps = 0;                // gaps declared
  std::uint64_t requests = 0;            // Request Packets sent (including re-sends)
  std::uint64_t unrecoverable_gaps = 0;
  std::uint64_t lost_messages = 0;  // sequences given up
  std::uint64_t sessions = 0;       // sessions adopted after End of Session
  std::uint64_t session_mismatch = 0;
  std::uint64_t malformed = 0;
  std::array<ReceiverLineStats, kMaxLines> lines{};
};

// Fixed-capacity store of data packets that arrived ahead of the next expected sequence. The
// message blocks are copied into a slab allocated at construction. Lookups scan the slots: they
// only run while a gap is open.
class ReorderBuffer {
 public:
  static constexpr std::uint32_t kNone = 0xFFFF'FFFFU;

  // capacity slots of max_packet_bytes - 20 bytes of message blocks.
  ReorderBuffer(std::uint32_t capacity, std::size_t max_packet_bytes);

  // Copies a data packet; returns its slot, or kNone when full or larger than a slot.
  std::uint32_t insert(const PacketView& p, std::int64_t rx_ns) noexcept;
  // A held packet covers every sequence of [seq, end).
  [[nodiscard]] bool covers(std::uint64_t seq, std::uint64_t end) const noexcept;
  // A held packet starting at or below `seq`; kNone if there is none.
  [[nodiscard]] std::uint32_t find_at_or_below(std::uint64_t seq) const noexcept;
  [[nodiscard]] PacketView view(std::uint32_t slot, const SessionId& session) const noexcept;
  void erase(std::uint32_t slot) noexcept;
  void clear() noexcept;
  // Lowest held start sequence (UINT64_MAX when empty); earliest arrival (INT64_MAX when empty).
  [[nodiscard]] std::uint64_t lowest() const noexcept;
  [[nodiscard]] std::int64_t oldest_rx() const noexcept;
  [[nodiscard]] bool fits(const PacketView& p) const noexcept {
    return capacity_ != 0 && p.blocks.size() <= stride_;
  }
  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] bool full() const noexcept { return size_ == capacity_; }

 private:
  struct Entry {
    std::uint64_t seq = 0;
    std::uint64_t end = 0;
    std::int64_t rx_ns = 0;
    std::uint32_t len = 0;
    std::uint16_t count = 0;
    bool used = false;
  };
  std::unique_ptr<Entry[]> entries_;
  std::unique_ptr<std::byte[]> data_;
  std::uint32_t capacity_ = 0;
  std::uint32_t size_ = 0;
  std::size_t stride_ = 0;  // message block bytes per slot
};

// Delivers messages in sequence order from one or more lines carrying the same packets.
//
// Arbitration: the first copy of a sequence wins; later copies count as duplicates of their
// line, and their delay behind the first copy is recorded per line (a direct-mapped table of
// the last kSkewSlots packets' arrival times).
//
// Gaps: a data packet starting beyond the next expected sequence, or a heartbeat / End of
// Session announcing a higher next sequence, opens a gap. Packets ahead of it are copied into
// the reorder buffer with their metadata and delivered when it closes. The gap is declared
// after gap_timeout_ns without the missing sequence on any line, or at once when an ahead
// packet cannot be held (buffer full or packet too large: it is dropped and counted).
//
// Recovery: with can_request, a declared gap is requested one hole at a time (next expected
// sequence up to the first held packet, at most max_request_count), and re-sent from
// on_timer() after request_timeout_ns. Once a retransmission advances the stream, the next
// hole is requested when it is gap_timeout_ns old, or at once if it holds dropped packets. A
// hole requested max_request_attempts times without progress, and every declared hole without
// can_request, is unrecoverable: it is reported through on_gap_unrecoverable() and delivery
// resumes at the first held packet. Without can_request an overflowing packet does the same.
//
// Time comes from on_packet() and on_timer(); on_frame() uses the latest time seen.
template <class H, class Meta = NoRxMeta>
  requires ReceiverHandler<H, Meta>
class Receiver {
 public:
  static_assert(std::is_trivially_copyable_v<Meta>);
  static constexpr std::size_t kSkewSlots = 4096;

  // Allocates the reorder buffer and the skew table.
  explicit Receiver(H& handler, const ReceiverConfig& cfg = {})
      : h_(handler),
        cfg_(cfg),
        held_(cfg.reorder_packets, cfg.max_packet_bytes),
        skew_(std::make_unique<SkewSlot[]>(kSkewSlots)),
        session_(cfg.session),
        next_(cfg.next_sequence) {
    if constexpr (!std::is_empty_v<Meta>) {
      if (cfg.reorder_packets != 0) metas_ = std::make_unique<Meta[]>(cfg.reorder_packets);
    }
  }

  // SessionLayer: the frame payload is one datagram of line 0. Always consumed.
  bool on_frame(const FrameView& frame) noexcept {
    on_packet(0, frame.payload, now_ns_);
    return true;
  }

  void on_packet(std::span<const std::byte> datagram, std::int64_t now_ns) noexcept {
    on_packet(0, datagram, now_ns);
  }

  // One datagram received on `line` (lines >= kMaxLines are counted as the last one). `meta`
  // reaches on_message() with every message of the packet, also when it is delivered later from
  // the reorder buffer.
  void on_packet(std::size_t line,
                 std::span<const std::byte> datagram,
                 std::int64_t now_ns,
                 const Meta& meta = Meta{}) noexcept {
    now_ns_ = now_ns;
    ReceiverLineStats& ls = stats_.lines[line < kMaxLines ? line : kMaxLines - 1];
    PacketView p;
    if (FASTMM_UNLIKELY(!parse_packet(datagram, p))) {
      ++stats_.malformed;
      return;
    }
    if (FASTMM_UNLIKELY(!accept_session(p))) return;
    ++stats_.packets;
    ++ls.packets;
    if (next_ == 0) next_ = p.sequence == 0 ? 1 : p.sequence;
    if (state_ == SessionState::Down) state_ = SessionState::Up;
    const std::uint64_t end = p.next_sequence();
    if (end > highest_) {
      highest_ = end;
      highest_ns_ = now_ns;
    }

    const std::uint64_t before = next_;
    switch (p.kind) {
      case PacketKind::Heartbeat:
        ++stats_.heartbeats;
        break;
      case PacketKind::EndOfSession:
        ++stats_.end_of_session;
        eos_ = true;
        break;
      case PacketKind::Data:
        if (end <= next_) {
          duplicate(p, now_ns, ls);
        } else if (p.sequence <= next_) {
          first_copy(p, now_ns);
          deliver(p, meta);
          drain();
        } else {
          on_ahead(p, now_ns, meta, ls);
        }
        break;
    }
    if (next_ != before) on_progress();
    update(now_ns);
  }

  // Declares gaps whose timeout expired and re-sends (or gives up) unanswered requests.
  void on_timer(std::int64_t now_ns) noexcept {
    now_ns_ = now_ns;
    if (next_ < highest_) update(now_ns);
  }

  // Delivery continues at `next_sequence` (a snapshot said where the stream must resume): held
  // packets are dropped and request attempts start over. Sequences from there up to the highest
  // one seen are a gap, declared and requested as usual; with next_sequence 0 the next packet
  // received sets the position, as at construction. Not from inside a handler callback.
  void reset(std::uint64_t next_sequence) noexcept {
    next_ = next_sequence;
    held_.clear();
    requested_next_ = 0;
    attempts_ = 0;
    overflow_end_ = 0;
    gap_since_ = kNoGap;
    if (state_ == SessionState::Recovering) state_ = SessionState::Up;
    if (next_ != 0 && highest_ < next_) {
      highest_ = next_;
      highest_ns_ = now_ns_;
    }
    if (next_ != 0 && next_ < highest_) gap_since_ = now_ns_;
  }

  [[nodiscard]] SessionState state() const noexcept { return state_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_; }
  [[nodiscard]] std::uint64_t highest_known() const noexcept { return highest_; }
  [[nodiscard]] bool end_of_session() const noexcept { return eos_; }
  [[nodiscard]] const SessionId& session() const noexcept { return session_; }
  [[nodiscard]] std::uint32_t held() const noexcept { return held_.size(); }
  [[nodiscard]] const ReceiverStats& stats() const noexcept { return stats_; }

 private:
  static constexpr std::int64_t kNoGap = std::numeric_limits<std::int64_t>::min();
  struct SkewSlot {
    std::uint64_t seq = 0;
    std::int64_t rx_ns = 0;
  };

  bool accept_session(const PacketView& p) noexcept {
    if (!have_session_) {
      if (!is_blank(session_) && p.session != session_) {
        ++stats_.session_mismatch;
        return false;
      }
      session_ = p.session;
      have_session_ = true;
      return true;
    }
    if (FASTMM_LIKELY(p.session == session_)) return true;
    if (cfg_.follow_session && eos_notified_) {
      start_session(p.session);
      return true;
    }
    ++stats_.session_mismatch;
    return false;
  }

  // A new session numbers its messages from 1; with next_sequence 0 (live join) it is joined at
  // its first packet.
  void start_session(const SessionId& s) noexcept {
    session_ = s;
    next_ = cfg_.next_sequence == 0 ? 0 : 1;
    highest_ = 0;
    highest_ns_ = 0;
    requested_next_ = 0;
    attempts_ = 0;
    overflow_end_ = 0;
    eos_ = false;
    eos_notified_ = false;
    gap_since_ = kNoGap;
    held_.clear();
    for (std::size_t i = 0; i < kSkewSlots; ++i) skew_[i] = SkewSlot{};
    state_ = SessionState::Up;
    ++stats_.sessions;
  }

  void first_copy(const PacketView& p, std::int64_t now_ns) noexcept {
    skew_[p.sequence & (kSkewSlots - 1)] = SkewSlot{p.sequence, now_ns};
  }

  void duplicate(const PacketView& p, std::int64_t now_ns, ReceiverLineStats& ls) noexcept {
    ++stats_.duplicate_packets;
    ++ls.duplicate_packets;
    const SkewSlot& s = skew_[p.sequence & (kSkewSlots - 1)];
    if (s.seq != p.sequence) return;
    const std::int64_t d = now_ns - s.rx_ns;
    ++ls.skew_samples;
    ls.skew_last_ns = d;
    ls.skew_sum_ns += d;
    if (d > ls.skew_max_ns) ls.skew_max_ns = d;
  }

  void on_ahead(const PacketView& p,
                std::int64_t now_ns,
                const Meta& meta,
                ReceiverLineStats& ls) noexcept {
    ++stats_.ahead_packets;
    if (held_.covers(p.sequence, p.next_sequence())) {
      duplicate(p, now_ns, ls);
      return;
    }
    first_copy(p, now_ns);
    if (hold(p, now_ns, meta)) return;
    ++stats_.reorder_overflow;
    if (cfg_.can_request) {
      // Dropped: highest_ includes it, so the requests cover it.
      if (p.next_sequence() > overflow_end_) overflow_end_ = p.next_sequence();
      if (state_ != SessionState::Recovering) {
        state_ = SessionState::Recovering;
        ++stats_.gaps;
        request(now_ns);
      }
      return;
    }
    // No retransmission: give up holes until the packet is in sequence or can be held.
    do {
      ++stats_.gaps;
      skip_hole(p.sequence);
    } while (p.sequence > next_ && (held_.full() || !held_.fits(p)));
    if (p.sequence > next_) {
      static_cast<void>(hold(p, now_ns, meta));
    } else if (p.next_sequence() > next_) {
      deliver(p, meta);
      drain();
    }
  }

  bool hold(const PacketView& p, std::int64_t now_ns, const Meta& meta) noexcept {
    const std::uint32_t slot = held_.insert(p, now_ns);
    if (slot == ReorderBuffer::kNone) return false;
    if constexpr (!std::is_empty_v<Meta>) metas_[slot] = meta;
    ++stats_.held_packets;
    if (held_.size() > stats_.reorder_high_water) stats_.reorder_high_water = held_.size();
    return true;
  }

  // The first hole has been known at least since the earliest held packet (all lie beyond it)
  // arrived, and since highest_ was last raised (it was undelivered then).
  [[nodiscard]] std::int64_t hole_known_since() const noexcept {
    const std::int64_t held = held_.oldest_rx();
    return held < highest_ns_ ? held : highest_ns_;
  }

  void on_progress() noexcept { gap_since_ = next_ < highest_ ? hole_known_since() : kNoGap; }

  [[nodiscard]] bool aged(std::int64_t now_ns) const noexcept {
    return now_ns - gap_since_ >= cfg_.gap_timeout_ns;
  }

  void update(std::int64_t now_ns) noexcept {
    while (next_ < highest_) {
      if (gap_since_ == kNoGap) gap_since_ = hole_known_since();
      if (state_ != SessionState::Recovering) {
        if (!aged(now_ns)) return;
        ++stats_.gaps;
        if (!cfg_.can_request) {
          skip_hole(highest_);
          continue;
        }
        state_ = SessionState::Recovering;
        request(now_ns);
        return;
      }
      if (requested_next_ != next_) {  // a retransmission made progress: the next hole
        if (aged(now_ns) || next_ < overflow_end_) request(now_ns);
        return;
      }
      if (now_ns - last_request_ns_ < cfg_.request_timeout_ns) return;
      if (cfg_.max_request_attempts == 0 || attempts_ < cfg_.max_request_attempts) {
        request(now_ns);
        return;
      }
      skip_hole(highest_);
    }
    gap_since_ = kNoGap;
    if (state_ != SessionState::Down) state_ = SessionState::Up;
    if (eos_ && !eos_notified_) {
      eos_notified_ = true;
      h_.on_end_of_session();
    }
  }

  // Gives up the sequences from next_ to the first held packet (at most to `limit`).
  void skip_hole(std::uint64_t limit) noexcept {
    const std::uint64_t lowest = held_.lowest();
    const std::uint64_t target = lowest < limit ? lowest : limit;
    const std::uint64_t from = next_;
    ++stats_.unrecoverable_gaps;
    stats_.lost_messages += target - from;
    next_ = target;
    if constexpr (UnrecoverableGapHandler<H>) h_.on_gap_unrecoverable(from, target - from);
    drain();
    on_progress();
  }

  void deliver(const PacketView& p, const Meta& meta) noexcept {
    MessageIterator it(p);
    std::uint64_t seq = 0;
    std::span<const std::byte> msg;
    while (it.next(seq, msg)) {
      if (seq < next_) continue;
      if constexpr (requires { h_.on_message(seq, msg, meta); }) {
        h_.on_message(seq, msg, meta);
      } else {
        h_.on_message(seq, msg);
      }
      next_ = seq + 1;
      ++stats_.messages;
    }
  }

  // Delivers the held packets that next_ reaches and frees the ones already covered.
  void drain() noexcept {
    if (FASTMM_LIKELY(held_.empty())) return;
    for (std::uint32_t i = held_.find_at_or_below(next_); i != ReorderBuffer::kNone;
         i = held_.find_at_or_below(next_)) {
      const PacketView v = held_.view(i, session_);
      if (v.next_sequence() > next_) {
        if constexpr (std::is_empty_v<Meta>) {
          deliver(v, Meta{});
        } else {
          deliver(v, metas_[i]);
        }
      }
      held_.erase(i);
    }
  }

  // Requests the first hole: from next_ up to the first held packet.
  void request(std::int64_t now_ns) noexcept {
    const std::uint64_t lowest = held_.lowest();
    const std::uint64_t hole_end = lowest < highest_ ? lowest : highest_;
    const std::uint64_t missing = hole_end - next_;
    const std::uint16_t n = missing < cfg_.max_request_count ? static_cast<std::uint16_t>(missing)
                                                             : cfg_.max_request_count;
    std::array<std::byte, kRequestLength> buf{};
    write_request(buf, session_, next_, n);
    attempts_ = requested_next_ == next_ ? attempts_ + 1 : 1;
    requested_next_ = next_;
    last_request_ns_ = now_ns;
    ++stats_.requests;
    h_.send_request(buf);
  }

  H& h_;
  ReceiverConfig cfg_;
  ReorderBuffer held_;
  std::unique_ptr<Meta[]> metas_;  // per reorder slot; none for an empty Meta
  std::unique_ptr<SkewSlot[]> skew_;
  SessionId session_;
  bool have_session_ = false;
  bool eos_ = false;
  bool eos_notified_ = false;
  SessionState state_ = SessionState::Down;
  std::uint64_t next_ = 0;
  std::uint64_t highest_ = 0;
  std::uint64_t requested_next_ = 0;  // next_ when the last request was sent
  std::uint64_t overflow_end_ = 0;    // end of the highest packet dropped on overflow
  std::uint32_t attempts_ = 0;        // requests sent for requested_next_
  std::int64_t now_ns_ = 0;           // latest time from on_packet() / on_timer()
  std::int64_t highest_ns_ = 0;       // when highest_ was last raised
  std::int64_t last_request_ns_ = 0;
  std::int64_t gap_since_ = kNoGap;  // when the first missing sequence became known
  ReceiverStats stats_{};
};

// ---- transmitter ---------------------------------------------------------------------------

struct TransmitterConfig {
  std::size_t history_bytes = 1U << 22;
  std::size_t history_messages = 1U << 18;
  std::size_t max_datagram = kDefaultMaxDatagram;
  // false: publish() fails once the history is full. true: the history is a ring; the oldest
  // messages are dropped to make room and requests for them are not answered.
  bool overwrite_oldest = false;
};

class Transmitter {
 public:
  explicit Transmitter(std::string_view session, const TransmitterConfig& cfg = {});

  // Stores one message and returns its sequence number (from 1); 0 when the history is full
  // (without overwrite_oldest) or the message is larger than the history.
  std::uint64_t publish(std::span<const std::byte> msg) noexcept;
  // The next downstream packet of messages not sent yet (at most max_count of them); 0 when none
  // are pending. Messages evicted before they were sent are skipped.
  std::size_t next_packet(std::span<std::byte> out,
                          std::uint16_t max_count = kMaxMessagesPerPacket) noexcept;
  // A packet starting at `seq` with at most `max_count` messages that fit max_datagram.
  // 0 when `seq` is not in the history (or the output buffer is too small).
  std::size_t packet_at(std::span<std::byte> out,
                        std::uint64_t seq,
                        std::uint16_t max_count) const noexcept;
  // Answers a Request Packet (unicast retransmission); 0 for a bad request.
  std::size_t answer_request(std::span<const std::byte> request,
                             std::span<std::byte> out) const noexcept;
  // Heartbeat / End of Session carrying the next sequence number to be sent.
  std::size_t heartbeat(std::span<std::byte> out) const noexcept;
  std::size_t end_of_session(std::span<std::byte> out) const noexcept;

  [[nodiscard]] const SessionId& session() const noexcept { return session_; }
  [[nodiscard]] std::uint64_t published() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t next_unsent() const noexcept { return next_unsent_; }
  // Oldest sequence number still in the history (published() + 1 when it is empty).
  [[nodiscard]] std::uint64_t oldest() const noexcept { return first_; }
  [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }

 private:
  [[nodiscard]] std::uint64_t held() const noexcept { return count_ + 1 - first_; }
  [[nodiscard]] std::uint64_t start_of(std::uint64_t seq) const noexcept {
    return starts_[seq % cfg_.history_messages];
  }
  void evict_oldest() noexcept;

  SessionId session_;
  TransmitterConfig cfg_;
  std::unique_ptr<std::byte[]> bytes_;
  std::unique_ptr<std::uint64_t[]> starts_;  // byte offset of message s at [s % history_messages]
  std::unique_ptr<std::uint16_t[]> lens_;
  std::uint64_t count_ = 0;  // last sequence number published
  std::uint64_t first_ = 1;  // oldest sequence number held
  std::uint64_t head_ = 0;   // next write offset in bytes_
  std::uint64_t evicted_ = 0;
  std::uint64_t next_unsent_ = 1;
};

}  // namespace fastmm::codecs::moldudp
