// GLIMPSE 5.0 End of Snapshot message (see glimpse.hpp).
#include "fastmm/codecs/itch/glimpse.hpp"

#include "fastmm/codecs/itch/nasdaq_fields.hpp"

#include <cstring>

namespace fastmm::codecs::itch::glimpse {

std::size_t write_end_of_snapshot(std::span<std::byte> out, std::uint64_t next_sequence) noexcept {
  if (out.size() < kEndOfSnapshotLength) return 0;
  EndOfSnapshot m{};
  m.type = kEndOfSnapshot;
  if (!nasdaq::put_numeric(m.sequence_number, kSequenceChars, next_sequence)) return 0;
  std::memcpy(out.data(), &m, sizeof m);
  return sizeof m;
}

bool parse_end_of_snapshot(std::span<const std::byte> msg, std::uint64_t& next_sequence) noexcept {
  if (msg.size() != kEndOfSnapshotLength || static_cast<char>(msg[0]) != kEndOfSnapshot)
    return false;
  const auto* p = reinterpret_cast<const char*>(msg.data()) + 1;
  std::uint64_t v = 0;
  if (!nasdaq::get_numeric(p, kSequenceChars, v)) return false;
  next_sequence = v;
  return true;
}

}  // namespace fastmm::codecs::itch::glimpse
