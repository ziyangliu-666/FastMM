// MoldUDP64 1.00 packets, builders and transmitter (see moldudp64.hpp).
#include "fastmm/codecs/moldudp/moldudp64.hpp"

#include <cstring>

namespace fastmm::codecs::moldudp {

namespace {
void write_header(std::byte* p,
                  const SessionId& session,
                  std::uint64_t seq,
                  std::uint16_t count) noexcept {
  std::memcpy(p, session.data(), kSessionLength);
  nasdaq::store_be64(p + 10, seq);
  nasdaq::store_be16(p + 18, count);
}
}  // namespace

bool parse_packet(std::span<const std::byte> datagram, PacketView& out) noexcept {
  if (FASTMM_UNLIKELY(datagram.size() < kHeaderLength)) return false;
  const std::byte* p = datagram.data();
  std::memcpy(out.session.data(), p, kSessionLength);
  out.sequence = nasdaq::load_be64(p + 10);
  const std::uint16_t raw_count = nasdaq::load_be16(p + 18);
  const std::span<const std::byte> blocks = datagram.subspan(kHeaderLength);
  if (raw_count == kHeartbeatCount || raw_count == kEndOfSessionCount) {
    out.kind = raw_count == kHeartbeatCount ? PacketKind::Heartbeat : PacketKind::EndOfSession;
    out.count = 0;
    out.blocks = {};
    return blocks.empty();
  }
  std::size_t off = 0;
  for (std::uint16_t i = 0; i < raw_count; ++i) {
    if (FASTMM_UNLIKELY(blocks.size() - off < 2)) return false;
    const std::size_t n = nasdaq::load_be16(blocks.data() + off);
    if (FASTMM_UNLIKELY(blocks.size() - off - 2 < n)) return false;
    off += 2 + n;
  }
  if (FASTMM_UNLIKELY(off != blocks.size())) return false;
  out.kind = PacketKind::Data;
  out.count = raw_count;
  out.blocks = blocks;
  return true;
}

bool parse_request(std::span<const std::byte> datagram, RequestView& out) noexcept {
  if (datagram.size() != kRequestLength) return false;
  const std::byte* p = datagram.data();
  std::memcpy(out.session.data(), p, kSessionLength);
  out.sequence = nasdaq::load_be64(p + 10);
  out.count = nasdaq::load_be16(p + 18);
  return true;
}

PacketBuilder::PacketBuilder(std::span<std::byte> buf,
                             const SessionId& session,
                             std::uint64_t first_sequence) noexcept
    : buf_(buf), first_(first_sequence) {
  if (buf_.size() >= kHeaderLength) {
    write_header(buf_.data(), session, first_sequence, 0);
    size_ = kHeaderLength;
  }
}

bool PacketBuilder::add(std::span<const std::byte> msg) noexcept {
  if (size_ == 0 || msg.size() > kMaxMessageLength || count_ >= kMaxMessagesPerPacket ||
      buf_.size() - size_ < 2 + msg.size())
    return false;
  nasdaq::store_be16(buf_.data() + size_, static_cast<std::uint16_t>(msg.size()));
  if (!msg.empty()) std::memcpy(buf_.data() + size_ + 2, msg.data(), msg.size());
  size_ += 2 + msg.size();
  ++count_;
  return true;
}

std::size_t PacketBuilder::finish() noexcept {
  if (size_ == 0) return 0;
  nasdaq::store_be16(buf_.data() + 18, count_);
  return size_;
}

std::size_t write_heartbeat(std::span<std::byte> out,
                            const SessionId& session,
                            std::uint64_t next_sequence) noexcept {
  if (out.size() < kHeaderLength) return 0;
  write_header(out.data(), session, next_sequence, kHeartbeatCount);
  return kHeaderLength;
}

std::size_t write_end_of_session(std::span<std::byte> out,
                                 const SessionId& session,
                                 std::uint64_t next_sequence) noexcept {
  if (out.size() < kHeaderLength) return 0;
  write_header(out.data(), session, next_sequence, kEndOfSessionCount);
  return kHeaderLength;
}

std::size_t write_request(std::span<std::byte> out,
                          const SessionId& session,
                          std::uint64_t sequence,
                          std::uint16_t count) noexcept {
  if (out.size() < kRequestLength) return 0;
  write_header(out.data(), session, sequence, count);
  return kRequestLength;
}

// ---- transmitter ---------------------------------------------------------------------------

Transmitter::Transmitter(std::string_view session, const TransmitterConfig& cfg)
    : session_(make_session(session)),
      cfg_(cfg),
      bytes_(new std::byte[cfg.history_bytes]),
      offsets_(new std::uint64_t[cfg.history_messages + 1]()) {}

std::uint64_t Transmitter::publish(std::span<const std::byte> msg) noexcept {
  if (msg.size() > kMaxMessageLength || count_ >= cfg_.history_messages ||
      cfg_.history_bytes - used_ < msg.size())
    return 0;
  if (!msg.empty()) std::memcpy(bytes_.get() + used_, msg.data(), msg.size());
  used_ += msg.size();
  ++count_;
  offsets_[count_] = used_;
  return count_;
}

std::size_t Transmitter::packet_at(std::span<std::byte> out,
                                   std::uint64_t seq,
                                   std::uint16_t max_count) const noexcept {
  if (seq == 0 || seq > count_) return 0;
  const std::size_t limit = out.size() < cfg_.max_datagram ? out.size() : cfg_.max_datagram;
  PacketBuilder b(out.first(limit), session_, seq);
  for (std::uint64_t s = seq; s <= count_ && b.count() < max_count; ++s) {
    const std::uint64_t begin = offsets_[s - 1];
    const std::uint64_t end = offsets_[s];
    if (!b.add({bytes_.get() + begin, end - begin})) break;
  }
  if (b.count() == 0) return 0;  // first message larger than a datagram
  return b.finish();
}

std::size_t Transmitter::next_packet(std::span<std::byte> out) noexcept {
  if (next_unsent_ > count_) return 0;
  const std::size_t n = packet_at(out, next_unsent_, kMaxMessagesPerPacket);
  if (n == 0) return 0;
  next_unsent_ += nasdaq::load_be16(out.data() + 18);
  return n;
}

std::size_t Transmitter::answer_request(std::span<const std::byte> request,
                                        std::span<std::byte> out) const noexcept {
  RequestView r;
  if (!parse_request(request, r) || r.session != session_ || r.count == 0) return 0;
  return packet_at(out, r.sequence, r.count);
}

std::size_t Transmitter::heartbeat(std::span<std::byte> out) const noexcept {
  return write_heartbeat(out, session_, next_unsent_);
}

std::size_t Transmitter::end_of_session(std::span<std::byte> out) const noexcept {
  return write_end_of_session(out, session_, next_unsent_);
}

}  // namespace fastmm::codecs::moldudp
