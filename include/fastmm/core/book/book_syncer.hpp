#pragma once
// BookSyncer: venue-agnostic snapshot/delta synchronisation FSM (5.4), instantiated per
// instrument on the net thread. Venue rules are supplied by SyncTraits; the engine only
// ever sees normalised BookSnapshot / BookDelta events, so replay is protocol-free.
//
//   Idle --start()--> Buffering --snapshot ok--> Synced --gap--> Resync --start()--> Buffering
//
// Sink interface (a struct or lambda-holder):
//   void on_snapshot(const BookDeltaMsg&)  // kSnapshot set
//   void on_delta(const BookDeltaMsg&)
//   void on_resync(SyncReason)             // emitted before the next snapshot request
//   void request_snapshot()                // REST fetch (venues with kNeedsRestSnapshot)
//
// Buffered deltas are copied into a fixed byte arena (no allocation after construction).
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace fastmm {

enum class SyncState : std::uint8_t { Idle = 0, Buffering = 1, Synced = 2, Resync = 3 };
enum class SyncReason : std::uint8_t {
  None = 0,
  SequenceGap = 1,
  BufferOverflow = 2,
  SnapshotTooOld = 3,  // buffered deltas start after the snapshot; need a newer one
  Explicit = 4,
  SnapshotMarker = 5,  // venue signalled a reset (Bybit u == 1)
};
[[nodiscard]] constexpr const char* to_string(SyncState s) noexcept {
  switch (s) {
    case SyncState::Idle:
      return "Idle";
    case SyncState::Buffering:
      return "Buffering";
    case SyncState::Synced:
      return "Synced";
    case SyncState::Resync:
      return "Resync";
  }
  return "?";
}

// Binance Spot: snapshot via REST (lastUpdateId = L). Drop deltas with u <= L. The first
// applied delta must satisfy U <= L+1 <= u; each subsequent one U == prev_u + 1.
struct BinanceSpotSyncTraits {
  static constexpr bool kNeedsRestSnapshot = true;
  static constexpr bool kBuffersDeltas = true;
  static constexpr bool is_stale(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.last_update_id <= snap;
  }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.first_update_id <= snap + 1 && snap + 1 <= d.last_update_id;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev_u) noexcept {
    return d.first_update_id == prev_u + 1;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};

// Binance USDT-M futures: first applied delta must have U <= L and u >= L; afterwards each
// delta's pu must equal the previous delta's u.
struct BinanceFuturesSyncTraits {
  static constexpr bool kNeedsRestSnapshot = true;
  static constexpr bool kBuffersDeltas = true;
  static constexpr bool is_stale(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.last_update_id < snap;
  }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.first_update_id <= snap && d.last_update_id >= snap;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev_u) noexcept {
    return d.prev_update_id == prev_u;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg&) noexcept { return false; }
};

// Bybit v5: the stream itself sends a snapshot first (kSnapshot flag), then deltas whose
// `u` must be strictly increasing; `u == 1` means the venue restarted the book (treated as
// a snapshot marker -> resubscribe). No REST snapshot, no buffering.
struct BybitSyncTraits {
  static constexpr bool kNeedsRestSnapshot = false;
  static constexpr bool kBuffersDeltas = false;
  static constexpr bool is_stale(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.last_update_id <= snap;
  }
  static constexpr bool first_applies(const BookDeltaMsg& d, std::uint64_t snap) noexcept {
    return d.last_update_id > snap;
  }
  static constexpr bool next_applies(const BookDeltaMsg& d, std::uint64_t prev_u) noexcept {
    return d.last_update_id > prev_u;
  }
  static constexpr bool is_snapshot_marker(const BookDeltaMsg& d) noexcept {
    return d.last_update_id == 1 && !d.is_snapshot();
  }
};

template <class Traits, class Sink>
class BookSyncer {
 public:
  static constexpr std::size_t kMaxBuffered = 4096;
  static constexpr std::size_t kDefaultBufferBytes = 8U << 20;

  explicit BookSyncer(Sink& sink, std::size_t buffer_bytes = kDefaultBufferBytes)
      : sink_(sink), buf_bytes_(Traits::kBuffersDeltas ? buffer_bytes : 0) {
    if (buf_bytes_ > 0) {
      buf_.reset(new std::byte[buf_bytes_]);
      offsets_.reset(new std::uint32_t[kMaxBuffered]);
    }
  }

  [[nodiscard]] SyncState state() const noexcept { return state_; }
  [[nodiscard]] bool synced() const noexcept { return state_ == SyncState::Synced; }
  [[nodiscard]] std::uint64_t last_update_id() const noexcept { return prev_u_; }
  [[nodiscard]] std::uint32_t resync_count() const noexcept { return resyncs_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return count_; }

  // Begin (or restart) synchronisation.
  void start() noexcept {
    reset_buffer();
    awaiting_first_ = false;
    state_ = SyncState::Buffering;
    if constexpr (Traits::kNeedsRestSnapshot) sink_.request_snapshot();
  }

  // Force a resync (e.g. crossed book beyond grace, or a disconnect).
  void resync(SyncReason reason) noexcept {
    ++resyncs_;
    state_ = SyncState::Resync;
    sink_.on_resync(reason);
    start();
  }

  // Delta from the stream. For venues whose stream carries snapshots (Bybit) a message
  // flagged kSnapshot is routed to on_snapshot().
  void on_delta(const BookDeltaMsg& d) noexcept {
    if (d.is_snapshot()) {
      on_snapshot(d);
      return;
    }
    if (Traits::is_snapshot_marker(d)) {
      resync(SyncReason::SnapshotMarker);
      return;
    }
    switch (state_) {
      case SyncState::Idle:
      case SyncState::Resync:
        return;  // not started / waiting for start()
      case SyncState::Buffering:
        if constexpr (Traits::kBuffersDeltas) {
          if (!buffer(d)) resync(SyncReason::BufferOverflow);
        }
        return;
      case SyncState::Synced:
        if constexpr (Traits::kBuffersDeltas) {
          // No buffered delta was newer than the snapshot, so the first live one must be checked
          // with the first-delta rule: it may overlap the snapshot (U <= L+1 <= u on Binance
          // spot) rather than follow it exactly, and must not be mistaken for a gap.
          if (FASTMM_UNLIKELY(awaiting_first_)) {
            if (Traits::is_stale(d, prev_u_)) return;
            if (!Traits::first_applies(d, prev_u_)) {
              resync(SyncReason::SequenceGap);
              return;
            }
            awaiting_first_ = false;
            prev_u_ = d.last_update_id;
            sink_.on_delta(d);
            return;
          }
        }
        if (FASTMM_LIKELY(Traits::next_applies(d, prev_u_))) {
          prev_u_ = d.last_update_id;
          sink_.on_delta(d);
        } else if (!Traits::is_stale(d, prev_u_)) {
          resync(SyncReason::SequenceGap);
        }
        // stale duplicates are silently dropped
        return;
    }
  }

  // Snapshot (REST response or in-stream snapshot). Replays the buffer.
  void on_snapshot(const BookDeltaMsg& snap) noexcept {
    if (state_ == SyncState::Idle) return;
    const std::uint64_t L = snap.last_update_id;
    if constexpr (Traits::kBuffersDeltas) {
      // Find the first non-stale buffered delta; it must bracket L.
      std::size_t i = 0;
      while (i < count_ && Traits::is_stale(*at(i), L)) ++i;
      if (i < count_ && !Traits::first_applies(*at(i), L)) {
        // Buffered stream starts after the snapshot: the snapshot is too old. Keep
        // buffering and ask again.
        ++resyncs_;
        sink_.on_resync(SyncReason::SnapshotTooOld);
        // keep only the deltas we still need
        drop_front(i);
        sink_.request_snapshot();
        return;
      }
      sink_.on_snapshot(snap);
      prev_u_ = L;
      state_ = SyncState::Synced;
      bool first = true;
      for (; i < count_; ++i) {
        const BookDeltaMsg& d = *at(i);
        const bool ok = first ? Traits::first_applies(d, L) : Traits::next_applies(d, prev_u_);
        if (!ok) {
          reset_buffer();
          resync(SyncReason::SequenceGap);
          return;
        }
        first = false;
        prev_u_ = d.last_update_id;
        sink_.on_delta(d);
      }
      awaiting_first_ = first;  // nothing newer than the snapshot was buffered
      reset_buffer();
    } else {
      sink_.on_snapshot(snap);
      prev_u_ = L;
      state_ = SyncState::Synced;
    }
  }

 private:
  bool buffer(const BookDeltaMsg& d) noexcept {
    if (count_ >= kMaxBuffered || used_ + d.hdr.len > buf_bytes_) return false;
    std::memcpy(buf_.get() + used_, &d, d.hdr.len);
    offsets_[count_++] = static_cast<std::uint32_t>(used_);
    used_ += d.hdr.len;
    return true;
  }
  [[nodiscard]] const BookDeltaMsg* at(std::size_t i) const noexcept {
    return reinterpret_cast<const BookDeltaMsg*>(buf_.get() + offsets_[i]);
  }
  void drop_front(std::size_t n) noexcept {
    if (n == 0) return;
    if (n >= count_) {
      reset_buffer();
      return;
    }
    const std::uint32_t base = offsets_[n];
    std::memmove(buf_.get(), buf_.get() + base, used_ - base);
    for (std::size_t i = n; i < count_; ++i) offsets_[i - n] = offsets_[i] - base;
    count_ -= n;
    used_ -= base;
  }
  void reset_buffer() noexcept {
    count_ = 0;
    used_ = 0;
  }

  Sink& sink_;
  SyncState state_ = SyncState::Idle;
  bool awaiting_first_ = false;  // Synced, but the first-delta rule has not been applied yet
  std::uint64_t prev_u_ = 0;
  std::uint32_t resyncs_ = 0;
  std::unique_ptr<std::byte[]> buf_;
  std::unique_ptr<std::uint32_t[]> offsets_;
  std::size_t buf_bytes_;
  std::size_t used_ = 0;
  std::size_t count_ = 0;
};

}  // namespace fastmm
