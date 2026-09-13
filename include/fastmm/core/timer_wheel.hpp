#pragma once
// TimerWheel: 4096 slots x 1 ms hashed wheel plus an overflow list for timers beyond the
// horizon (5.11). add/cancel are O(1); poll() walks the slots that elapsed since the last
// poll. No allocation: timers live in a Pool. Callbacks may add/cancel timers re-entrantly.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm {

template <std::size_t kMaxTimers = 1024>
class TimerWheel {
 public:
  static constexpr std::size_t kSlots = 4096;
  static constexpr std::int64_t kSlotNs = 1'000'000;  // 1 ms

  struct Timer {
    Timestamp expiry;
    Duration period;
    std::uint64_t user_data;
    std::uint32_t next;  // intrusive list within slot / overflow
    std::uint32_t prev;
    std::uint32_t list;  // slot index, kOverflow, or kUnlinked
    bool repeat;
    bool active;
  };

  explicit TimerWheel(Timestamp now = {}) noexcept : cur_ms_(now.ns / kSlotNs) {
    for (auto& s : slots_) s = kNullHandle;
  }

  // Schedules a timer `period` after `now`. Returns an invalid id when the pool is full.
  [[nodiscard]] TimerId add(Timestamp now,
                            Duration period,
                            bool repeat,
                            std::uint64_t user_data = 0) noexcept {
    const Handle<Timer> h = pool_.allocate();
    if (!h.valid()) return TimerId{};
    Timer& t = pool_.get(h);
    t.expiry = now + period;
    t.period = period.ns < kSlotNs && repeat ? Duration{kSlotNs} : period;
    t.user_data = user_data;
    t.repeat = repeat;
    t.active = true;
    t.list = kUnlinked;
    link(h.idx, t);
    return TimerId{h.idx};
  }
  bool cancel(TimerId id) noexcept {
    const Handle<Timer> h{id.value};
    if (!pool_.is_live(h)) return false;
    Timer& t = pool_.get(h);
    if (!t.active) return false;
    t.active = false;
    unlink(h.idx, t);
    if (firing_ != h.idx) pool_.free(h);  // the firing timer is freed after its callback
    return true;
  }
  [[nodiscard]] bool active(TimerId id) const noexcept {
    const Handle<Timer> h{id.value};
    return pool_.is_live(h) && pool_.get(h).active;
  }
  [[nodiscard]] std::size_t size() const noexcept { return pool_.size(); }

  // Fires every timer with expiry <= now, in slot order. F(TimerId, std::uint64_t user_data).
  template <class F>
  std::size_t poll(Timestamp now, F&& f) noexcept {
    std::size_t fired = 0;
    const std::int64_t now_ms = now.ns / kSlotNs;
    if (now_ms < cur_ms_) return 0;
    // After a stall longer than the horizon every slot is visited exactly once anyway, so
    // jump the cursor forward; this also lets promote_overflow() see the right window.
    if (now_ms - cur_ms_ > static_cast<std::int64_t>(kSlots))
      cur_ms_ = now_ms - static_cast<std::int64_t>(kSlots);
    // Bring overflow timers that are now within the horizon into the wheel.
    promote_overflow();
    const std::int64_t steps = now_ms - cur_ms_;
    // Process slots cur_ms_ .. now_ms inclusive (the current slot may have new timers).
    for (std::int64_t s = 0; s <= steps; ++s) {
      const std::int64_t ms = cur_ms_ + s;
      const std::size_t slot = static_cast<std::size_t>(ms) & (kSlots - 1);
      std::uint32_t idx = slots_[slot];
      while (idx != kNullHandle) {
        Timer& t = pool_.get(Handle<Timer>{idx});
        const std::uint32_t next = t.next;
        if (t.expiry <= now) {
          fired += fire(idx, t, now, f);
        }
        idx = next;
      }
    }
    cur_ms_ = now_ms;
    return fired;
  }

  // Earliest pending expiry (Timestamp::max() if none). O(live timers): used by the sim
  // scheduler, not the live loop.
  [[nodiscard]] Timestamp next_expiry() const noexcept {
    Timestamp best = Timestamp::max();
    pool_.for_each([&](Handle<Timer>, const Timer& t) {
      if (t.active && t.expiry < best) best = t.expiry;
    });
    return best;
  }

 private:
  static constexpr std::uint32_t kOverflow = 0xFFFF'FFFEU;
  static constexpr std::uint32_t kUnlinked = 0xFFFF'FFFDU;

  template <class F>
  std::size_t fire(std::uint32_t idx, Timer& t, Timestamp now, F& f) noexcept {
    const Handle<Timer> h{idx};
    unlink(idx, t);
    firing_ = idx;
    f(TimerId{idx}, t.user_data);
    firing_ = kNullHandle;
    Timer& tt = pool_.get(h);  // re-fetch: callback may have cancelled it
    if (!tt.active) {
      pool_.free(h);
      return 1;
    }
    if (tt.repeat) {
      // Keep the phase (no drift); catch up if we fell behind by several periods.
      tt.expiry += tt.period;
      if (tt.expiry <= now) tt.expiry = now + tt.period;
      link(idx, tt);
    } else {
      tt.active = false;
      pool_.free(h);
    }
    return 1;
  }

  void link(std::uint32_t idx, Timer& t) noexcept {
    const std::int64_t ms = t.expiry.ns / kSlotNs;
    std::uint32_t* head = nullptr;
    if (ms - cur_ms_ >= static_cast<std::int64_t>(kSlots)) {
      t.list = kOverflow;
      head = &overflow_;
    } else {
      const std::size_t slot = static_cast<std::size_t>(ms < cur_ms_ ? cur_ms_ : ms) & (kSlots - 1);
      t.list = static_cast<std::uint32_t>(slot);
      head = &slots_[slot];
    }
    t.prev = kNullHandle;
    t.next = *head;
    if (*head != kNullHandle) pool_.get(Handle<Timer>{*head}).prev = idx;
    *head = idx;
  }
  void unlink(std::uint32_t idx, Timer& t) noexcept {
    if (t.list == kUnlinked) return;
    std::uint32_t* head = t.list == kOverflow ? &overflow_ : &slots_[t.list];
    if (t.prev != kNullHandle) {
      pool_.get(Handle<Timer>{t.prev}).next = t.next;
    } else {
      *head = t.next;
    }
    if (t.next != kNullHandle) pool_.get(Handle<Timer>{t.next}).prev = t.prev;
    t.list = kUnlinked;
    static_cast<void>(idx);
  }
  void promote_overflow() noexcept {
    std::uint32_t idx = overflow_;
    while (idx != kNullHandle) {
      Timer& t = pool_.get(Handle<Timer>{idx});
      const std::uint32_t next = t.next;
      if (t.expiry.ns / kSlotNs - cur_ms_ < static_cast<std::int64_t>(kSlots)) {
        unlink(idx, t);
        link(idx, t);
      }
      idx = next;
    }
  }

  Pool<Timer, kMaxTimers> pool_;
  std::uint32_t slots_[kSlots];
  std::uint32_t overflow_ = kNullHandle;
  std::int64_t cur_ms_;
  std::uint32_t firing_ = kNullHandle;
};

}  // namespace fastmm
