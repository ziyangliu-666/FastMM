#pragma once
// OKX v5 order-book synchronisation for the `books` channel
// (https://www.okx.com/docs-v5/en/#order-book-trading-market-data-ws-order-book-channel, read
// 2026-09-26): the first push after subscribing is `action: snapshot`, the rest `update`.
//
// Sequencing: "prevSeqId: sequence ID of the last sent message, -1 for the snapshot"; an update
// applies when its prevSeqId equals the seqId of the last message applied. With no change for a
// while OKX pushes an update with empty `asks`/`bids` whose seqId equals its prevSeqId (a
// heartbeat), which the rule accepts. After maintenance seqId may be reset to a value lower than
// prevSeqId; the chain still holds (prevSeqId is the last seqId), so that is not a gap either.
// Anything else is a gap: ConnectionState{Resyncing}, then an unsubscribe and subscribe for a new
// snapshot, at most every 2 s per instrument (StreamBookSync).
//
// Checksum: every push carries `checksum`, the CRC-32 of the first 25 levels of the book after the
// push, as a signed 32-bit integer. The string is "bid1px:bid1sz:ask1px:ask1sz:bid2px:..." with the
// prices and sizes as the venue sent them; when one side has fewer levels the missing entries are
// left out. OkxShadowBook keeps every level's original text next to its price so that the string
// can be rebuilt byte for byte; a mismatch resyncs like a gap (SyncReason::ChecksumMismatch).
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/venues/book_sync.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace fastmm::venues::okx {

// BookDeltaMsg mapping (OkxMdParser): first_update_id = last_update_id = seqId, prev_update_id =
// prevSeqId (0 for the snapshot's -1). seqIds are positive.
struct OkxSyncTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  // Only asked when next_applies() failed. Nothing is dropped as a duplicate: seqIds may go
  // down after a reset, so an "old" seqId cannot be told from a chain that moved on without us.
  static constexpr bool is_stale(const BookDeltaMsg&, std::uint64_t) noexcept { return false; }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.prev_update_id == snap;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev) noexcept {
    return d.prev_update_id == prev;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};

using ResubscribeRequester = InstrumentCallback;
using OkxBookSync = StreamBookSync<OkxSyncTraits>;

// CRC-32 (ISO-HDLC: reflected polynomial 0xEDB88320, init and final xor 0xFFFFFFFF), the one
// zlib and Python's binascii.crc32 compute and OKX's examples use.
namespace detail {
constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
    t[i] = c;
  }
  return t;
}
inline constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();
}  // namespace detail

[[nodiscard]] constexpr std::uint32_t crc32_update(std::uint32_t crc, std::string_view s) noexcept {
  for (const char ch : s)
    crc = detail::kCrc32Table[(crc ^ static_cast<std::uint8_t>(ch)) & 0xFFU] ^ (crc >> 8U);
  return crc;
}
[[nodiscard]] constexpr std::uint32_t crc32(std::string_view s) noexcept {
  return crc32_update(0xFFFFFFFFU, s) ^ 0xFFFFFFFFU;
}

// One level of a books push as the parser saw it: views into the frame.
struct OkxLevelText {
  std::int64_t px_raw = 0;  // Price::raw, for ordering
  std::string_view px;
  std::string_view sz;
  bool remove = false;  // size zero
};

// Every level of one book with its text, so the checksum can be recomputed. Bids are kept best
// (highest) first, asks best (lowest) first. `books` holds 400 levels a side; kMaxLevels leaves
// room for updates that add levels before others fall away, and the deepest level is dropped when
// a side is full (it cannot be among the 25 the checksum covers).
class OkxShadowBook {
 public:
  static constexpr std::size_t kMaxLevels = 512;  // a power of two (a ring)
  static_assert((kMaxLevels & (kMaxLevels - 1)) == 0);
  static constexpr std::size_t kChecksumLevels = 25;
  static constexpr std::size_t kMaxText = 30;  // price or size text

  void clear() noexcept {
    bids_.n = bids_.head = 0;
    asks_.n = asks_.head = 0;
  }
  // False when a text is longer than kMaxText (the book cannot be checked; resync).
  [[nodiscard]] bool apply(bool bid, const OkxLevelText& l) noexcept {
    if (l.px.size() > kMaxText || l.sz.size() > kMaxText) return false;
    (bid ? bids_ : asks_).apply(l, bid);
    return true;
  }
  [[nodiscard]] std::size_t bid_count() const noexcept { return bids_.n; }
  [[nodiscard]] std::size_t ask_count() const noexcept { return asks_.n; }
  // Price and size text of the i-th level from the best (tests and diagnostics).
  [[nodiscard]] std::string_view level_px(bool bid, std::size_t i) const noexcept {
    return (bid ? bids_ : asks_).at(i).px();
  }
  [[nodiscard]] std::string_view level_sz(bool bid, std::size_t i) const noexcept {
    return (bid ? bids_ : asks_).at(i).sz();
  }

  // The venue's checksum of this book: CRC-32 of the first 25 levels, interleaved, as int32.
  [[nodiscard]] std::int32_t checksum() const noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    bool first = true;
    auto add = [&](const Level& lv) {
      if (!first) crc = crc32_update(crc, ":");
      first = false;
      crc = crc32_update(crc, lv.px());
      crc = crc32_update(crc, ":");
      crc = crc32_update(crc, lv.sz());
    };
    for (std::size_t i = 0; i < kChecksumLevels; ++i) {
      if (i < bids_.n) add(bids_.at(i));
      if (i < asks_.n) add(asks_.at(i));
    }
    return static_cast<std::int32_t>(crc ^ 0xFFFFFFFFU);
  }
  // The string the checksum is computed over (tests and diagnostics).
  [[nodiscard]] std::string checksum_text() const {
    std::string s;
    auto add = [&](const Level& lv) {
      if (!s.empty()) s += ':';
      s.append(lv.px()).append(":").append(lv.sz());
    };
    for (std::size_t i = 0; i < kChecksumLevels; ++i) {
      if (i < bids_.n) add(bids_.at(i));
      if (i < asks_.n) add(asks_.at(i));
    }
    return s;
  }

 private:
  struct Level {
    std::int64_t px_raw = 0;
    std::uint8_t px_len = 0;
    std::uint8_t sz_len = 0;
    char px_text[kMaxText];
    char sz_text[kMaxText];
    [[nodiscard]] std::string_view px() const noexcept { return {px_text, px_len}; }
    [[nodiscard]] std::string_view sz() const noexcept { return {sz_text, sz_len}; }
    void set_size(std::string_view s) noexcept {
      std::memcpy(sz_text, s.data(), s.size());
      sz_len = static_cast<std::uint8_t>(s.size());
    }
  };
  // A side is a ring of kMaxLevels levels, best first from `head`. A level inserted or removed
  // moves the levels on its shorter side, toward the touch or toward the deep end, so a change
  // near the touch moves the few levels better than it, and a snapshot (best first) appends.
  struct Side {
    static constexpr std::size_t kMask = kMaxLevels - 1;
    std::array<Level, kMaxLevels> levels{};
    std::size_t head = 0;
    std::size_t n = 0;
    [[nodiscard]] Level& at(std::size_t i) noexcept { return levels[(head + i) & kMask]; }
    [[nodiscard]] const Level& at(std::size_t i) const noexcept {
      return levels[(head + i) & kMask];
    }
    // Position of the first level not better than `px` for this side.
    [[nodiscard]] std::size_t lower(std::int64_t px, bool bid) const noexcept {
      std::size_t lo = 0;
      std::size_t hi = n;
      while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const std::int64_t p = at(mid).px_raw;
        if (bid ? p > px : p < px) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      return lo;
    }
    void apply(const OkxLevelText& l, bool bid) noexcept {
      const std::size_t i = lower(l.px_raw, bid);
      const bool found = i < n && at(i).px_raw == l.px_raw;
      if (l.remove) {
        if (!found) return;
        if (i < n - 1 - i) {
          for (std::size_t k = i; k > 0; --k) at(k) = at(k - 1);
          head = (head + 1) & kMask;
        } else {
          for (std::size_t k = i; k + 1 < n; ++k) at(k) = at(k + 1);
        }
        --n;
        return;
      }
      if (found) {
        at(i).set_size(l.sz);
        return;
      }
      if (i >= kMaxLevels) return;  // deeper than we keep
      if (n == kMaxLevels) --n;     // the deepest falls away
      if (i < n - i) {
        head = (head + kMaxLevels - 1) & kMask;  // the levels better than i move up by one
        for (std::size_t k = 0; k < i; ++k) at(k) = at(k + 1);
      } else {
        for (std::size_t k = n; k > i; --k) at(k) = at(k - 1);
      }
      Level& lv = at(i);
      lv.px_raw = l.px_raw;
      std::memcpy(lv.px_text, l.px.data(), l.px.size());
      lv.px_len = static_cast<std::uint8_t>(l.px.size());
      lv.set_size(l.sz);
      ++n;
    }
  };
  Side bids_;
  Side asks_;
};

}  // namespace fastmm::venues::okx
