#pragma once
// ReplayTransport: TransportLike that records nothing but a SHA-256 of the normalized
// outbound stream and, optionally, verifies each message against the Out* records of the
// original journal (5.11). A replay passes when count and hash match and no mismatch index
// was recorded.
//
// Outbound copies the original transport did not accept (kDropped, format v2) are refused again
// at the same position, so the engine sees the same failure (and trips the same kill switch).
// Indices count send attempts, dropped ones included; the hash and count() cover accepted
// messages only, like the journal's recorded hash.
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

  // Verification target: normalized outbound messages in journal order (dropped ones included).
  void set_expected(std::vector<Bytes> expected) { expected_ = std::move(expected); }
  // Attempt indices the original transport refused (from load_dropped()).
  void set_dropped(std::vector<std::uint64_t> dropped) { dropped_ = std::move(dropped); }
  void set_supports_replace(bool v) noexcept {
    for (bool& r : replace_) r = v;
  }
  void set_supports_replace(VenueId v, bool on) noexcept {
    if (v.value < kMaxVenues) replace_[v.value] = on;
  }
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
  static std::vector<std::uint64_t> load_dropped(JournalReader& reader) {
    std::vector<std::uint64_t> out;
    std::uint64_t i = 0;
    reader.for_each([&](const EventHeader* h) {
      if ((h->flags & EventHeader::kOutbound) == 0) return;
      if ((h->flags & EventHeader::kDropped) != 0) out.push_back(i);
      ++i;
    });
    return out;
  }

  // ---- TransportLike ------------------------------------------------------------------------
  [[nodiscard]] bool send(const EventHeader& m) noexcept {
    const EventHeader* one[1] = {&m};
    return send(std::span<const EventHeader* const>(one, 1)) == 1;
  }
  // Like LiveTransport: the accepted messages are a prefix of the batch.
  [[nodiscard]] std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    std::size_t accepted = 0;
    bool refusing = false;
    for (const EventHeader* m : batch) {
      const std::uint64_t i = attempts_++;
      verify(i, *m);
      if (!refusing && next_drop_ < dropped_.size() && dropped_[next_drop_] == i) refusing = true;
      while (next_drop_ < dropped_.size() && dropped_[next_drop_] <= i) ++next_drop_;
      if (refusing) continue;
      hasher_.add(*m);
      ++accepted;
    }
    return accepted;
  }
  [[nodiscard]] bool supports_replace(VenueId v) const noexcept {
    return v.value < kMaxVenues && replace_[v.value];
  }

  // ---- results ------------------------------------------------------------------------------
  [[nodiscard]] std::string hash_hex() const { return hasher_.hex(); }
  [[nodiscard]] std::uint64_t count() const noexcept { return hasher_.count(); }
  [[nodiscard]] std::uint64_t attempts() const noexcept { return attempts_; }
  [[nodiscard]] std::int64_t first_mismatch() const noexcept { return first_mismatch_; }
  // The replayed message at first_mismatch() (normalized); empty when none differed.
  [[nodiscard]] const Bytes& mismatch_actual() const noexcept { return mismatch_actual_; }
  [[nodiscard]] const std::vector<Bytes>& expected() const noexcept { return expected_; }
  [[nodiscard]] std::size_t expected_count() const noexcept { return expected_.size(); }
  [[nodiscard]] bool verified_ok() const noexcept {
    return first_mismatch_ < 0 && (expected_.empty() || attempts_ == expected_.size());
  }

 private:
  void verify(std::uint64_t i, const EventHeader& m) {
    if (expected_.empty() || first_mismatch_ >= 0) return;
    alignas(64) std::byte buf[OutboundHasher::kMaxOutBytes];
    const std::uint32_t len = OutboundHasher::normalized_copy(m, buf);
    if (i >= expected_.size() || expected_[i].size() != len ||
        std::memcmp(expected_[i].data(), buf, len) != 0) {
      first_mismatch_ = static_cast<std::int64_t>(i);
      mismatch_actual_.assign(buf, buf + len);
    }
  }

  OutboundHasher hasher_;
  std::vector<Bytes> expected_;
  std::vector<std::uint64_t> dropped_;
  std::size_t next_drop_ = 0;
  std::uint64_t attempts_ = 0;
  std::int64_t first_mismatch_ = -1;
  Bytes mismatch_actual_;
  bool replace_[kMaxVenues] = {};
};

static_assert(TransportLike<ReplayTransport>);

}  // namespace fastmm::sim
