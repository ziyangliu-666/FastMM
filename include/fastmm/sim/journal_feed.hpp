#pragma once
// JournalFeed: FeedLike over a JournalReader for deterministic replay (5.11). Yields every
// INBOUND record (outbound copies and latency samples are skipped) in journal sequence
// order, one per Engine::step(): the driver arms the feed, sets the SimClock to the event's
// time (peek_ts) and steps, so each event is processed at exactly the virtual time of the
// original run.
//
// ParamUpdate records are replayed like any other input. With set_param_schema() their field
// indices are mapped by name and type from the journal's parameter table (format v3) to the
// replaying strategy's schema; set_param_updates(false) skips them (a what-if run).
#include "fastmm/core/journal.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/strategies/params.hpp"

#include <array>
#include <cstddef>
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
    if (FASTMM_UNLIKELY(remap_params_ && cur_->type == EventType::ParamUpdate))
      return remap(msg_cast<ParamUpdateMsg>(cur_));
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

  // false: ParamUpdate records are skipped (counted in skipped()), the engine keeps its parameters.
  void set_param_updates(bool replay) noexcept {
    skip_params_ = !replay;
    if (skip_params_ && cur_ != nullptr && cur_->type == EventType::ParamUpdate) {
      ++skipped_;
      advance();
    }
  }
  // Maps recorded field indices to `schema` by name and type. A recorded field the schema does not
  // have is dropped from the update (counted in dropped_param_fields()). Without a parameter table
  // in the journal the indices are used as recorded.
  void set_param_schema(const ParamSchema& schema) noexcept {
    const std::span<const JournalParam> table = reader_.params();
    param_table_size_ = table.size();
    remap_params_ = false;
    for (std::size_t i = 0; i < table.size(); ++i) {
      std::int32_t to = -1;
      for (std::size_t j = 0; j < schema.size(); ++j) {
        const ParamDesc& d = schema.begin()[j];
        if (table[i].name == d.name && table[i].type == static_cast<std::uint8_t>(d.type)) {
          to = static_cast<std::int32_t>(j);
          break;
        }
      }
      param_map_[i] = to;
      if (to != static_cast<std::int32_t>(i)) remap_params_ = true;
    }
  }
  [[nodiscard]] std::uint64_t dropped_param_fields() const noexcept { return dropped_fields_; }

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
      if (is_inbound(*cur_) && !(skip_params_ && cur_->type == EventType::ParamUpdate)) return;
      ++skipped_;
    }
  }

  const EventHeader* remap(const ParamUpdateMsg& m) noexcept {
    scratch_ = m;
    const std::uint32_t n =
        m.count < ParamUpdateMsg::kMaxFields ? m.count : ParamUpdateMsg::kMaxFields;
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::int32_t to = m.field[i] < param_table_size_ ? param_map_[m.field[i]] : -1;
      if (to < 0) {
        ++dropped_fields_;
        continue;
      }
      scratch_.field[kept] = static_cast<std::uint16_t>(to);
      scratch_.value[kept] = m.value[i];
      ++kept;
    }
    scratch_.count = kept;
    return &scratch_.hdr;
  }

  JournalReader& reader_;
  const EventHeader* cur_ = nullptr;
  ParamUpdateMsg scratch_{};
  std::array<std::int32_t, kJournalMaxParams> param_map_{};
  std::size_t param_table_size_ = 0;
  std::uint64_t dropped_fields_ = 0;
  bool remap_params_ = false;
  bool skip_params_ = false;
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
