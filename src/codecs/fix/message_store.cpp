#include "fastmm/codecs/fix/message_store.hpp"

#include <cstring>

namespace fastmm::codecs::fix {

MessageStore::MessageStore(std::size_t max_messages, std::size_t max_bytes)
    : max_messages_(max_messages),
      max_bytes_(max_bytes),
      entries_(std::make_unique<Entry[]>(max_messages)),
      arena_(std::make_unique<char[]>(max_bytes)) {}

void MessageStore::reset() noexcept {
  count_ = 0;
  used_ = 0;
}

bool MessageStore::append(std::uint64_t seq,
                          std::string_view msg_type,
                          std::int64_t sending_ns,
                          std::string_view body) noexcept {
  if (count_ >= max_messages_ || used_ + body.size() > max_bytes_ || msg_type.empty() ||
      msg_type.size() > sizeof(Entry::msg_type) ||
      (count_ > 0 && seq <= entries_[count_ - 1].seq)) {
    ++dropped_;
    return false;
  }
  Entry& e = entries_[count_];
  e.seq = seq;
  e.sending_ns = sending_ns;
  e.offset = static_cast<std::uint32_t>(used_);
  e.length = static_cast<std::uint32_t>(body.size());
  std::memcpy(e.msg_type, msg_type.data(), msg_type.size());
  e.type_len = static_cast<std::uint8_t>(msg_type.size());
  if (!body.empty()) std::memcpy(arena_.get() + used_, body.data(), body.size());
  used_ += body.size();
  ++count_;
  return true;
}

std::size_t MessageStore::lower_bound(std::uint64_t seq) const noexcept {
  std::size_t lo = 0;
  std::size_t hi = count_;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (entries_[mid].seq < seq) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

MessageStore::Stored MessageStore::at(std::size_t i) const noexcept {
  const Entry& e = entries_[i];
  return Stored{e.seq,
                e.sending_ns,
                std::string_view(e.msg_type, e.type_len),
                std::string_view(arena_.get() + e.offset, e.length)};
}

}  // namespace fastmm::codecs::fix
