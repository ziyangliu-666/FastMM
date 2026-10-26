#pragma once
// RecoveryBuffer (ADR-0015 section 5): the ITCH messages a nasdaq_itch venue receives while it
// waits for a GLIMPSE snapshot, in sequence order, grouped by the datagram that carried them and
// with that datagram's receive stamp. After End of Snapshot the venue replays the messages from
// the snapshot's sequence number on.
//
// The venue keeps only the messages its books need and passes the others to skip(): the buffer
// still sees every sequence number, so gap_after() tells a message that was never received from
// one that was left out.
//
// Capacity is counted in datagrams (recovery_buffer_packets) and bytes (datagrams x 1.5 x the
// largest datagram); both are allocated by allocate(). append() fails once either is used up.
// Records are laid out back to back in one slab: a header, then each message as its sequence
// number's offset from the record's first (4 bytes), its length (2 bytes) and its bytes. Nothing
// allocates after allocate().
#include "fastmm/core/time.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>

namespace fastmm::venues::nasdaq {

// Receive metadata of a datagram, passed through moldudp::Receiver with every message.
struct ItchRxMeta {
  Cycles t0_cycles{};
  std::int64_t recv_ns = 0;    // recv_ts of the events: kernel (or NIC) time, else T0 wall clock
  std::uint64_t datagram = 0;  // per-venue datagram counter: message grouping
  std::uint8_t line = 0;       // 0 A, 1 B, 2 retransmission
};
static_assert(std::is_trivially_copyable_v<ItchRxMeta>);

class RecoveryBuffer {
 public:
  // Allocates room for `packets` datagrams of at most `max_packet_bytes` bytes each.
  void allocate(std::uint32_t packets, std::size_t max_packet_bytes) {
    packets_cap_ = packets;
    // A message's block is 4 bytes longer than in the datagram: room for 1.5 x its bytes.
    bytes_cap_ = static_cast<std::size_t>(packets) *
                 (max_packet_bytes + max_packet_bytes / 2 + sizeof(Record));
    slab_.reset(new std::byte[bytes_cap_]);  // untouched until used
    clear();
  }

  void clear() noexcept {
    used_ = 0;
    packets_ = 0;
    messages_ = 0;
    open_ = kNone;
    last_seen_ = 0;
    gap_end_ = 0;
  }

  // Appends the next message. Messages of one datagram (same meta.datagram) share a record.
  // False when the buffer is full; nothing is appended (or seen) then.
  bool append(std::uint64_t seq, std::span<const std::byte> msg, const ItchRxMeta& meta) noexcept {
    const std::size_t need = kBlockHeader + msg.size();
    if (msg.size() > 0xFFFF) return false;
    bool same = false;
    if (open_ != kNone) {
      const Record r = record_at(open_);
      same = r.meta.datagram == meta.datagram && seq - r.first_seq <= 0xFFFF'FFFFU;
    }
    if (!same) {
      if (packets_ >= packets_cap_ || bytes_cap_ - used_ < sizeof(Record) + need) return false;
      Record r{};
      r.first_seq = seq;
      r.meta = meta;
      open_ = used_;
      std::memcpy(slab_.get() + used_, &r, sizeof r);
      used_ += sizeof(Record);
      ++packets_;
    } else if (bytes_cap_ - used_ < need) {
      return false;
    }
    Record r = record_at(open_);
    const auto off = static_cast<std::uint32_t>(seq - r.first_seq);
    const auto len = static_cast<std::uint16_t>(msg.size());
    std::memcpy(slab_.get() + used_, &off, 4);
    std::memcpy(slab_.get() + used_ + 4, &len, 2);
    if (!msg.empty()) std::memcpy(slab_.get() + used_ + kBlockHeader, msg.data(), msg.size());
    used_ += need;
    ++r.count;
    r.bytes += static_cast<std::uint32_t>(need);
    std::memcpy(slab_.get() + open_, &r, sizeof r);
    ++messages_;
    seen(seq);
    return true;
  }
  // The next message was received but is not kept.
  void skip(std::uint64_t seq) noexcept { seen(seq); }

  // A message from `seq` on was not received: a sequence number at or after `seq` came after a
  // gap, or the first one seen is after `seq`. Received messages left out with skip() are no gap.
  [[nodiscard]] bool gap_after(std::uint64_t seq) const noexcept { return gap_end_ > seq; }
  // Sequence number of the last message received (kept or skipped), 0 before the first.
  [[nodiscard]] std::uint64_t last_seen() const noexcept { return last_seen_; }

  // on_message(seq, msg, meta) for every message in order, then on_datagram_end() after the last
  // message of each record.
  template <class OnMessage, class OnDatagramEnd>
  void for_each(OnMessage&& on_message, OnDatagramEnd&& on_datagram_end) const {
    std::size_t at = 0;
    while (at < used_) {
      const Record r = record_at(at);
      std::size_t p = at + sizeof(Record);
      for (std::uint32_t i = 0; i < r.count; ++i) {
        std::uint32_t off = 0;
        std::uint16_t len = 0;
        std::memcpy(&off, slab_.get() + p, 4);
        std::memcpy(&len, slab_.get() + p + 4, 2);
        on_message(r.first_seq + off,
                   std::span<const std::byte>(slab_.get() + p + kBlockHeader, len),
                   r.meta);
        p += kBlockHeader + static_cast<std::size_t>(len);
      }
      on_datagram_end();
      at += sizeof(Record) + r.bytes;
    }
  }

  [[nodiscard]] bool empty() const noexcept { return messages_ == 0; }
  [[nodiscard]] std::uint32_t packets() const noexcept { return packets_; }
  [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
  [[nodiscard]] std::uint32_t capacity_packets() const noexcept { return packets_cap_; }
  [[nodiscard]] std::size_t capacity_bytes() const noexcept { return bytes_cap_; }
  // Sequence number of the first message, 0 when empty.
  [[nodiscard]] std::uint64_t first_seq() const noexcept {
    return used_ == 0 ? 0 : record_at(0).first_seq;
  }

 private:
  static constexpr std::size_t kNone = ~std::size_t{0};
  static constexpr std::size_t kBlockHeader = 6;  // sequence offset, length
  struct Record {
    std::uint64_t first_seq;
    ItchRxMeta meta;
    std::uint32_t count;  // messages
    std::uint32_t bytes;  // message blocks after the header
  };
  static_assert(std::is_trivially_copyable_v<Record>);

  void seen(std::uint64_t seq) noexcept {
    if (seq != last_seen_ + 1 || last_seen_ == 0) gap_end_ = std::max(gap_end_, seq);
    last_seen_ = seq;
  }
  [[nodiscard]] Record record_at(std::size_t off) const noexcept {
    Record r{};
    std::memcpy(&r, slab_.get() + off, sizeof r);
    return r;
  }

  std::unique_ptr<std::byte[]> slab_;
  std::size_t bytes_cap_ = 0;
  std::size_t used_ = 0;
  std::size_t open_ = kNone;  // offset of the record being appended to
  std::uint32_t packets_cap_ = 0;
  std::uint32_t packets_ = 0;
  std::uint64_t messages_ = 0;
  std::uint64_t last_seen_ = 0;
  std::uint64_t gap_end_ = 0;  // the latest sequence number seen right after a gap, or the first
};

}  // namespace fastmm::venues::nasdaq
