// MoldUDP64 1.00 packets, builders and transmitter (see moldudp64.hpp).
#include "fastmm/codecs/moldudp/moldudp64.hpp"

#include <cstring>
#include <limits>
#include <memory>

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

// ---- reorder buffer ------------------------------------------------------------------------

ReorderBuffer::ReorderBuffer(std::uint32_t capacity, std::size_t max_packet_bytes)
    : capacity_(capacity),
      stride_(max_packet_bytes > kHeaderLength ? max_packet_bytes - kHeaderLength : 0) {
  if (capacity_ == 0) return;
  entries_ = std::make_unique<Entry[]>(capacity_);
  if (stride_ != 0) data_ = std::make_unique<std::byte[]>(capacity_ * stride_);
}

std::uint32_t ReorderBuffer::insert(const PacketView& p, std::int64_t rx_ns) noexcept {
  if (full() || !fits(p)) return kNone;
  std::uint32_t i = 0;
  while (entries_[i].used) ++i;
  Entry& e = entries_[i];
  e.seq = p.sequence;
  e.end = p.next_sequence();
  e.rx_ns = rx_ns;
  e.len = static_cast<std::uint32_t>(p.blocks.size());
  e.count = p.count;
  e.used = true;
  if (!p.blocks.empty()) std::memcpy(data_.get() + i * stride_, p.blocks.data(), p.blocks.size());
  ++size_;
  return i;
}

bool ReorderBuffer::covers(std::uint64_t seq, std::uint64_t end) const noexcept {
  if (size_ == 0) return false;
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    const Entry& e = entries_[i];
    if (e.used && e.seq <= seq && e.end >= end) return true;
  }
  return false;
}

std::uint32_t ReorderBuffer::find_at_or_below(std::uint64_t seq) const noexcept {
  if (size_ == 0) return kNone;
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    if (entries_[i].used && entries_[i].seq <= seq) return i;
  }
  return kNone;
}

PacketView ReorderBuffer::view(std::uint32_t slot, const SessionId& session) const noexcept {
  const Entry& e = entries_[slot];
  PacketView v;
  v.session = session;
  v.sequence = e.seq;
  v.count = e.count;
  v.kind = PacketKind::Data;
  v.blocks = {data_.get() + slot * stride_, e.len};
  return v;
}

void ReorderBuffer::erase(std::uint32_t slot) noexcept {
  entries_[slot].used = false;
  --size_;
}

void ReorderBuffer::clear() noexcept {
  for (std::uint32_t i = 0; i < capacity_; ++i) entries_[i].used = false;
  size_ = 0;
}

std::uint64_t ReorderBuffer::lowest() const noexcept {
  std::uint64_t lo = std::numeric_limits<std::uint64_t>::max();
  if (size_ == 0) return lo;
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    if (entries_[i].used && entries_[i].seq < lo) lo = entries_[i].seq;
  }
  return lo;
}

std::int64_t ReorderBuffer::oldest_rx() const noexcept {
  std::int64_t t = std::numeric_limits<std::int64_t>::max();
  if (size_ == 0) return t;
  for (std::uint32_t i = 0; i < capacity_; ++i) {
    if (entries_[i].used && entries_[i].rx_ns < t) t = entries_[i].rx_ns;
  }
  return t;
}

// ---- transmitter ---------------------------------------------------------------------------

Transmitter::Transmitter(std::string_view session, const TransmitterConfig& cfg)
    : session_(make_session(session)),
      cfg_(cfg),
      bytes_(new std::byte[cfg.history_bytes]),
      starts_(new std::uint64_t[cfg.history_messages == 0 ? 1 : cfg.history_messages]()),
      lens_(new std::uint16_t[cfg.history_messages == 0 ? 1 : cfg.history_messages]()) {}

void Transmitter::evict_oldest() noexcept {
  ++first_;
  ++evicted_;
}

std::uint64_t Transmitter::publish(std::span<const std::byte> msg) noexcept {
  const std::size_t n = msg.size();
  if (n > kMaxMessageLength || n > cfg_.history_bytes || cfg_.history_messages == 0) return 0;
  if (!cfg_.overwrite_oldest) {
    if (held() >= cfg_.history_messages || cfg_.history_bytes - head_ < n) return 0;
  } else {
    if (held() >= cfg_.history_messages) evict_oldest();
    if (cfg_.history_bytes - head_ < n) {
      // Wrap: the messages of the previous lap that lie behind head_ go first (they are the
      // oldest), then the write restarts at offset 0.
      while (held() != 0 && start_of(first_) >= head_) evict_oldest();
      head_ = 0;
    }
    while (held() != 0 && start_of(first_) >= head_ && start_of(first_) < head_ + n) evict_oldest();
  }
  if (n != 0) std::memcpy(bytes_.get() + head_, msg.data(), n);
  ++count_;
  const std::size_t slot = count_ % cfg_.history_messages;
  starts_[slot] = head_;
  lens_[slot] = static_cast<std::uint16_t>(n);
  head_ += n;
  return count_;
}

std::size_t Transmitter::packet_at(std::span<std::byte> out,
                                   std::uint64_t seq,
                                   std::uint16_t max_count) const noexcept {
  if (seq < first_ || seq > count_) return 0;
  const std::size_t limit = out.size() < cfg_.max_datagram ? out.size() : cfg_.max_datagram;
  PacketBuilder b(out.first(limit), session_, seq);
  for (std::uint64_t s = seq; s <= count_ && b.count() < max_count; ++s) {
    const std::size_t slot = s % cfg_.history_messages;
    if (!b.add({bytes_.get() + starts_[slot], lens_[slot]})) break;
  }
  if (b.count() == 0) return 0;  // first message larger than a datagram
  return b.finish();
}

std::size_t Transmitter::next_packet(std::span<std::byte> out, std::uint16_t max_count) noexcept {
  if (next_unsent_ < first_) next_unsent_ = first_;  // evicted before it was sent
  if (next_unsent_ > count_) return 0;
  const std::size_t n = packet_at(out, next_unsent_, max_count);
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
