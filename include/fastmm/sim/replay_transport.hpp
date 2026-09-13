#pragma once
// ReplayTransport: TransportLike that records nothing but a SHA-256 of the normalized
// outbound stream and, optionally, verifies each message against the Out* records of the
// original journal (5.11). A replay passes when count and hash match and no mismatch index
// was recorded.
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/sim/outbound_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace fastmm::sim {

class ReplayTransport {
 public:
  using Bytes = std::vector<std::byte>;

  ReplayTransport() = default;

  // Verification target: normalized outbound messages in journal order.
  void set_expected(std::vector<Bytes> expected) { expected_ = std::move(expected); }
  void set_supports_replace(bool v) noexcept { replace_ = v; }
  static std::vector<Bytes> load_outbound(JournalReader& reader) {
    std::vector<Bytes> out;
    reader.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) == 0) return;
      Bytes b(h->len);
      OutboundHasher::normalized_copy(*h, b.data());
      out.push_back(std::move(b));
    });
    return out;
  }

  // ---- TransportLike ------------------------------------------------------------------------
  [[nodiscard]] bool send(const EventHeader& m) noexcept {
    const std::uint64_t i = hasher_.count();
    hasher_.add(m);
    if (!expected_.empty() && first_mismatch_ < 0) {
      alignas(64) std::byte buf[OutboundHasher::kMaxOutBytes];
      const std::uint32_t len = OutboundHasher::normalized_copy(m, buf);
      if (i >= expected_.size() || expected_[i].size() != len ||
          std::memcmp(expected_[i].data(), buf, len) != 0) {
        first_mismatch_ = static_cast<std::int64_t>(i);
      }
    }
    return true;
  }
  [[nodiscard]] std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return replace_; }

  // ---- results ------------------------------------------------------------------------------
  [[nodiscard]] std::string hash_hex() const { return hasher_.hex(); }
  [[nodiscard]] std::uint64_t count() const noexcept { return hasher_.count(); }
  [[nodiscard]] std::int64_t first_mismatch() const noexcept { return first_mismatch_; }
  [[nodiscard]] std::size_t expected_count() const noexcept { return expected_.size(); }
  [[nodiscard]] bool verified_ok() const noexcept {
    return first_mismatch_ < 0 && (expected_.empty() || count() == expected_.size());
  }

 private:
  OutboundHasher hasher_;
  std::vector<Bytes> expected_;
  std::int64_t first_mismatch_ = -1;
  bool replace_ = false;
};

static_assert(TransportLike<ReplayTransport>);

}  // namespace fastmm::sim
