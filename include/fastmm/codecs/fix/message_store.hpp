#pragma once
// MessageStore: the sender side of FIX message recovery. Keeps the application messages a session
// sent (MsgType, original SendingTime, body bytes after the standard header) so a ResendRequest
// can retransmit them with PossDupFlag(43)=Y and OrigSendingTime(122).
//
// Preallocated at construction (entry table + byte arena), append-only, bounded: once either is
// full further messages are not stored (dropped() counts them) and a resend covers them with a
// SequenceReset-GapFill instead. Administrative messages are never stored: FIX gap-fills them on
// resend. reset() empties the store (sequence reset, ResetSeqNumFlag logon).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace fastmm::codecs::fix {

class MessageStore {
 public:
  struct Stored {
    std::uint64_t seq = 0;
    std::int64_t sending_ns = 0;
    std::string_view msg_type;
    std::string_view body;  // "tag=value|..." between the standard header and CheckSum
  };

  MessageStore(std::size_t max_messages, std::size_t max_bytes);
  MessageStore(const MessageStore&) = delete;
  MessageStore& operator=(const MessageStore&) = delete;

  void reset() noexcept;
  // `seq` must be greater than the last stored seq. False when full or out of order.
  bool append(std::uint64_t seq,
              std::string_view msg_type,
              std::int64_t sending_ns,
              std::string_view body) noexcept;

  // Index of the first stored message with seq >= `seq` (size() when none).
  [[nodiscard]] std::size_t lower_bound(std::uint64_t seq) const noexcept;
  [[nodiscard]] Stored at(std::size_t i) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::size_t bytes_used() const noexcept { return used_; }
  [[nodiscard]] std::size_t max_messages() const noexcept { return max_messages_; }
  [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

 private:
  struct Entry {
    std::uint64_t seq;
    std::int64_t sending_ns;
    std::uint32_t offset;
    std::uint32_t length;
    char msg_type[3];
    std::uint8_t type_len;
  };

  std::size_t max_messages_;
  std::size_t max_bytes_;
  std::unique_ptr<Entry[]> entries_;
  std::unique_ptr<char[]> arena_;
  std::size_t count_ = 0;
  std::size_t used_ = 0;
  std::uint64_t dropped_ = 0;
};

}  // namespace fastmm::codecs::fix
