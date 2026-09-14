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
  // Time at which the current event was processed originally: the journaled engine clock (format
  // v2), else its recv_ts, or fire_ts for a timer (v1: an approximation that mixes the venue
  // threads' receive clock with the engine clock).
  [[nodiscard]] Timestamp peek_ts() const noexcept {
    if (cur_ == nullptr) return Timestamp::max();
    if (has_engine_ts()) return engine_ts_;
    if (cur_->type == EventType::Timer) return msg_cast<TimerMsg>(cur_).fire_ts;
    return cur_->recv_ts;
  }
  // The current event carries the engine clock it was processed at; peek_ts() returns it.
  [[nodiscard]] bool has_engine_ts() const noexcept {
    return cur_ != nullptr && (cur_->flags & EventHeader::kEngineTime) != 0;
  }
  // Engine clock at start() / finish() when the journal recorded them (finish_ts() is known once
  // the last event has been released).
  [[nodiscard]] bool has_start_ts() const noexcept { return has_start_; }
  [[nodiscard]] Timestamp start_ts() const noexcept { return start_ts_; }
  [[nodiscard]] bool has_finish_ts() const noexcept { return has_finish_; }
  [[nodiscard]] Timestamp finish_ts() const noexcept { return finish_ts_; }
  void arm() noexcept { armed_ = cur_ != nullptr; }
  [[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_; }
  [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

  // Inbound == consumed by the engine in the original run.
  [[nodiscard]] static bool is_inbound(const EventHeader& h) noexcept {
    return (h.flags & EventHeader::kOutbound) == 0 && h.type != EventType::LatencySample &&
           h.type != EventType::OutNewOrder && h.type != EventType::OutCancel &&
           h.type != EventType::OutReplace && h.type != EventType::Padding &&
           h.type != EventType::EngineTime;
  }

 private:
  // Walks to the next inbound event, following the engine clock chain (EngineTimeMsg absolute
  // values, kEngineTime deltas) through every record on the way.
  void advance() noexcept {
    armed_ = false;
    for (;;) {
      cur_ = reader_.next();
      if (cur_ == nullptr) return;
      if (cur_->type == EventType::EngineTime) {
        const auto& t = msg_cast<EngineTimeMsg>(cur_);
        engine_ts_ = t.engine_ts;
        if (t.kind == EngineTimeMsg::Kind::Start && !has_start_) {
          has_start_ = true;
          start_ts_ = t.engine_ts;
        } else if (t.kind == EngineTimeMsg::Kind::Finish) {
          has_finish_ = true;
          finish_ts_ = t.engine_ts;
        }
      } else if ((cur_->flags & EventHeader::kEngineTime) != 0) {
        engine_ts_.ns += static_cast<std::int32_t>(cur_->reserved0);
      }
      if (is_inbound(*cur_)) return;
      ++skipped_;
    }
  }

  JournalReader& reader_;
  const EventHeader* cur_ = nullptr;
  Timestamp engine_ts_{};
  Timestamp start_ts_{};
  Timestamp finish_ts_{};
  bool has_start_ = false;
  bool has_finish_ = false;
  bool armed_ = false;
  std::uint64_t delivered_ = 0;
  std::uint64_t skipped_ = 0;
};

static_assert(FeedLike<JournalFeed>);

}  // namespace fastmm::sim
