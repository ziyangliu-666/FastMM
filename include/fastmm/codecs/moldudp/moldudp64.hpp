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
//   Receiver<H>      SessionLayer that delivers messages to H in sequence order, detects gaps
//                    (sequence jumps, heartbeats / end-of-session announcing a higher next
//                    sequence) and recovers them with Request Packets (re-sent on on_timer()
//                    until the gap closes). Packets ahead of the next expected sequence are
//                    dropped rather than buffered: the retransmission request covers them.
//   Transmitter      a sender with a fixed-capacity history that answers Request Packets
//                    (simulation, tests, replay).
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"
#include "fastmm/core/config_macros.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

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

// on_message: next message in sequence order. send_request: a Request Packet to transmit to
// the re-request server. on_end_of_session: the session ended and every message was delivered.
// The handler must not feed packets back into the receiver from inside these callbacks.
template <class H>
concept ReceiverHandler = requires(H& h, std::uint64_t seq, std::span<const std::byte> bytes) {
  { h.on_message(seq, bytes) } noexcept;
  { h.send_request(bytes) } noexcept;
  { h.on_end_of_session() } noexcept;
};

struct ReceiverConfig {
  SessionId session = kBlankSession;  // blank: adopt the session of the first packet
  std::uint64_t next_sequence = 0;    // 0: start at the first packet received (live join)
  std::uint16_t max_request_count = 1024;
  std::int64_t request_timeout_ns = 250'000'000;  // re-send an unanswered request
};

struct ReceiverStats {
  std::uint64_t packets = 0;
  std::uint64_t messages = 0;  // delivered
  std::uint64_t heartbeats = 0;
  std::uint64_t end_of_session = 0;
  std::uint64_t duplicate_packets = 0;  // entirely below the next expected sequence
  std::uint64_t ahead_packets = 0;      // dropped: start beyond the next expected sequence
  std::uint64_t gaps = 0;               // recoveries started
  std::uint64_t requests = 0;           // Request Packets sent (including re-sends)
  std::uint64_t session_mismatch = 0;
  std::uint64_t malformed = 0;
};

template <ReceiverHandler H>
class Receiver {
 public:
  explicit Receiver(H& handler, const ReceiverConfig& cfg = {}) noexcept
      : h_(handler), cfg_(cfg), session_(cfg.session), next_(cfg.next_sequence) {}

  // SessionLayer: the frame payload is one datagram. Always consumed.
  bool on_frame(const FrameView& frame) noexcept {
    on_packet(frame.payload);
    return true;
  }

  void on_packet(std::span<const std::byte> datagram) noexcept {
    PacketView p;
    if (FASTMM_UNLIKELY(!parse_packet(datagram, p))) {
      ++stats_.malformed;
      return;
    }
    if (!have_session_) {
      if (!is_blank(session_) && p.session != session_) {
        ++stats_.session_mismatch;
        return;
      }
      session_ = p.session;
      have_session_ = true;
    } else if (FASTMM_UNLIKELY(p.session != session_)) {
      ++stats_.session_mismatch;
      return;
    }
    ++stats_.packets;
    if (next_ == 0) next_ = p.sequence == 0 ? 1 : p.sequence;
    if (state_ == SessionState::Down) state_ = SessionState::Up;
    const std::uint64_t end = p.next_sequence();
    if (end > highest_) highest_ = end;

    bool progressed = false;
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
          ++stats_.duplicate_packets;
        } else if (p.sequence > next_) {
          ++stats_.ahead_packets;
        } else {
          deliver(p);
          progressed = true;
        }
        break;
    }

    if (next_ < highest_) {
      if (state_ != SessionState::Recovering) {
        state_ = SessionState::Recovering;
        ++stats_.gaps;
        request(now_ns_);
      } else if (progressed) {
        request(now_ns_);  // a retransmission arrived: ask for the next chunk right away
      }
    } else {
      state_ = SessionState::Up;
      if (eos_ && !eos_notified_) {
        eos_notified_ = true;
        h_.on_end_of_session();
      }
    }
  }

  void on_timer(std::int64_t now_ns) noexcept {
    now_ns_ = now_ns;
    if (state_ == SessionState::Recovering && now_ns - last_request_ns_ >= cfg_.request_timeout_ns)
      request(now_ns);
  }

  [[nodiscard]] SessionState state() const noexcept { return state_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_; }
  [[nodiscard]] std::uint64_t highest_known() const noexcept { return highest_; }
  [[nodiscard]] bool end_of_session() const noexcept { return eos_; }
  [[nodiscard]] const SessionId& session() const noexcept { return session_; }
  [[nodiscard]] const ReceiverStats& stats() const noexcept { return stats_; }

 private:
  void deliver(const PacketView& p) noexcept {
    MessageIterator it(p);
    std::uint64_t seq = 0;
    std::span<const std::byte> msg;
    while (it.next(seq, msg)) {
      if (seq < next_) continue;
      h_.on_message(seq, msg);
      next_ = seq + 1;
      ++stats_.messages;
    }
  }

  void request(std::int64_t now_ns) noexcept {
    const std::uint64_t missing = highest_ - next_;
    const std::uint16_t n = missing < cfg_.max_request_count ? static_cast<std::uint16_t>(missing)
                                                             : cfg_.max_request_count;
    std::array<std::byte, kRequestLength> buf{};
    write_request(buf, session_, next_, n);
    last_request_ns_ = now_ns;
    ++stats_.requests;
    h_.send_request(buf);
  }

  H& h_;
  ReceiverConfig cfg_;
  SessionId session_;
  bool have_session_ = false;
  bool eos_ = false;
  bool eos_notified_ = false;
  SessionState state_ = SessionState::Down;
  std::uint64_t next_ = 0;
  std::uint64_t highest_ = 0;
  std::int64_t now_ns_ = 0;
  std::int64_t last_request_ns_ = 0;
  ReceiverStats stats_{};
};

// ---- transmitter ---------------------------------------------------------------------------

struct TransmitterConfig {
  std::size_t history_bytes = 1U << 22;
  std::size_t history_messages = 1U << 18;
  std::size_t max_datagram = kDefaultMaxDatagram;
};

class Transmitter {
 public:
  explicit Transmitter(std::string_view session, const TransmitterConfig& cfg = {});

  // Stores one message and returns its sequence number (from 1); 0 when the history is full.
  std::uint64_t publish(std::span<const std::byte> msg) noexcept;
  // The next downstream packet of messages not sent yet; 0 when none are pending.
  std::size_t next_packet(std::span<std::byte> out) noexcept;
  // A packet starting at `seq` with at most `max_count` messages that fit max_datagram.
  // 0 when `seq` has not been published (or the output buffer is too small).
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

 private:
  SessionId session_;
  TransmitterConfig cfg_;
  std::unique_ptr<std::byte[]> bytes_;
  std::unique_ptr<std::uint64_t[]> offsets_;  // offsets_[i] = start of message i+1
  std::uint64_t count_ = 0;
  std::uint64_t used_ = 0;
  std::uint64_t next_unsent_ = 1;
};

}  // namespace fastmm::codecs::moldudp
