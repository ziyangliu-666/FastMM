#pragma once
// JournalFeed: FeedLike over a JournalReader for deterministic replay (5.11). Yields every
// INBOUND record (outbound copies and latency samples are skipped) in journal sequence
// order, one per Engine::step(): the driver arms the feed, sets the SimClock to the event's
// time (peek_ts) and steps, so each event is processed at exactly the virtual time of the
// original run.
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"

#include <cstdint>

namespace fastmm::sim {

class JournalFeed {
 public:
  explicit JournalFeed(JournalReader& reader) noexcept : reader_(reader) {
    reader_.reset();
    advance();
  }

  // ---- FeedLike -----------------------------------------------------------------------------
  [[nodiscard]] const EventHeader* next() noexcept {
    if (!armed_) return nullptr;
    armed_ = false;
    return cur_;
  }
  void release() noexcept {
    ++delivered_;
    advance();
  }

  // ---- driver API ---------------------------------------------------------------------------
  [[nodiscard]] bool has_next() const noexcept { return cur_ != nullptr; }
  [[nodiscard]] const EventHeader* peek() const noexcept { return cur_; }
  // Virtual time at which the current event was processed originally.
  [[nodiscard]] Timestamp peek_ts() const noexcept {
    if (cur_ == nullptr) return Timestamp::max();
    if (cur_->type == EventType::Timer) return msg_cast<TimerMsg>(cur_).fire_ts;
    return cur_->recv_ts;
  }
  void arm() noexcept { armed_ = cur_ != nullptr; }
  [[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_; }
  [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

  // Inbound == consumed by the engine in the original run.
  [[nodiscard]] static bool is_inbound(const EventHeader& h) noexcept {
    return (h.flags & EventHeader::kOutbound) == 0 && h.type != EventType::LatencySample &&
           h.type != EventType::OutNewOrder && h.type != EventType::OutCancel &&
           h.type != EventType::OutReplace && h.type != EventType::Padding;
  }

 private:
  void advance() noexcept {
    armed_ = false;
    for (;;) {
      cur_ = reader_.next();
      if (cur_ == nullptr || is_inbound(*cur_)) return;
      ++skipped_;
    }
  }

  JournalReader& reader_;
  const EventHeader* cur_ = nullptr;
  bool armed_ = false;
  std::uint64_t delivered_ = 0;
  std::uint64_t skipped_ = 0;
};

static_assert(FeedLike<JournalFeed>);

}  // namespace fastmm::sim
