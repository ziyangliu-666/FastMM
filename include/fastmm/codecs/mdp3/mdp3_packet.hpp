#pragma once
// CME MDP 3.0 UDP packet framing (plan 7). Layout, all little-endian:
//
//   packet  := MsgSeqNum uint32 | SendingTime uint64 | message...
//   message := MsgSize uint16 | SBE message header (8) | root block | repeating groups
//
// MsgSize counts itself: the wiki's decoding example has MsgSize 56 = 2 + 8 + 11 (root block)
// + 3 (groupSize) + 32 (one entry). Packets are sequenced and recovered as a whole; one packet
// can hold several messages for several instruments.
//
// Sources (CME Group Client Systems Wiki, retrieved 2026-09-14):
//   "MDP 3.0 - SBE Technical Headers"              (MsgSeqNum uint32, SendingTime uint64)
//   "MDP 3.0 - Packet Structure with Event Based Messaging"
//   "MDP 3.0 - SBE Decoding Example"               (byte-level example, MsgSize semantics)
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/sbe/sbe.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fastmm::codecs::mdp3 {

inline constexpr std::size_t kPacketHeaderSize = 12;
inline constexpr std::size_t kMsgSizeFieldSize = 2;
inline constexpr std::size_t kMessagePrefixSize = kMsgSizeFieldSize + sbe::MessageHeader::kSize;
// Upper bound for buffered copies of a datagram (standard Ethernet MTU). CME does not publish a
// maximum in the pages above; larger datagrams are decoded but cannot be buffered for recovery.
inline constexpr std::size_t kMaxPacketBytes = 1500;
// schemaId of the MDP 3.0 market data schema (templates_FixBinary.xml, id="1").
inline constexpr std::uint16_t kMdpSchemaId = 1;

struct PacketHeader {
  std::uint32_t msg_seq_num = 0;
  std::uint64_t sending_time = 0;  // ns since Unix epoch

  [[nodiscard]] static PacketHeader load(const std::byte* p) noexcept {
    return PacketHeader{sbe::load_le<std::uint32_t>(p), sbe::load_le<std::uint64_t>(p + 4)};
  }
  void store(std::byte* p) const noexcept {
    sbe::store_le(p, msg_seq_num);
    sbe::store_le(p + 4, sending_time);
  }
};

// One SBE message inside a packet.
struct MessageView {
  sbe::MessageHeader header;
  std::span<const std::byte> body;  // after the SBE header, up to MsgSize
  std::size_t size = 0;             // MsgSize (includes the size field itself)
};

// Framer over the message area of one datagram (the bytes after the packet header). The payload
// is the SBE message without the MsgSize prefix; `consumed` is MsgSize. A zero `consumed` means
// no complete message remains: either the input is exhausted or MsgSize is invalid.
struct MessageFramer {
  [[nodiscard]] FrameView next(std::span<const std::byte> in) noexcept {
    if (in.size() < kMessagePrefixSize) return {};
    const std::size_t n = sbe::load_le<std::uint16_t>(in.data());
    if (n < kMessagePrefixSize || n > in.size()) return {};
    return FrameView{in.subspan(kMsgSizeFieldSize, n - kMsgSizeFieldSize), n, 0};
  }
};
static_assert(Framer<MessageFramer>);

// Cursor over the messages of one datagram:
//   PacketCursor c(datagram); while (c.next(m)) { ... }  if (c.malformed()) { ... }
class PacketCursor {
 public:
  explicit PacketCursor(std::span<const std::byte> datagram) noexcept : data_(datagram) {
    if (data_.size() >= kPacketHeaderSize) {
      header_ = PacketHeader::load(data_.data());
      pos_ = kPacketHeaderSize;
      valid_ = true;
    }
  }

  // False when the datagram is shorter than the packet header.
  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] const PacketHeader& header() const noexcept { return header_; }

  [[nodiscard]] bool next(MessageView& out) noexcept {
    if (!valid_ || pos_ >= data_.size()) return false;
    const FrameView f = MessageFramer{}.next(data_.subspan(pos_));
    if (!f.complete()) {
      malformed_ = true;  // trailing bytes that do not form a message
      return false;
    }
    out.header = sbe::MessageHeader::load(f.payload.data());
    out.body = f.payload.subspan(sbe::MessageHeader::kSize);
    out.size = f.consumed;
    pos_ += f.consumed;
    return true;
  }

  [[nodiscard]] bool malformed() const noexcept { return malformed_ || !valid_; }

 private:
  std::span<const std::byte> data_;
  PacketHeader header_{};
  std::size_t pos_ = 0;
  bool valid_ = false;
  bool malformed_ = false;
};

}  // namespace fastmm::codecs::mdp3
