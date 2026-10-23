#pragma once
// TimerWheel: 4096 slots x 1 ms hashed wheel plus an overflow list for timers beyond the
// horizon (5.11). add/cancel are O(1); poll() walks the slots that elapsed since the last
// poll. No allocation: timers live in a Pool. Callbacks may add or cancel any timer, the one firing
// and the ones after it in its slot included.
//
// A TimerId is (generation << kIndexBits) | pool slot: the id of a timer that fired or was
// cancelled matches nothing once its slot holds another timer, so cancelling it is a no-op that
// returns false rather than cancelling the newer timer. A slot's first timer has generation 0.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace fastmm {

template <std::size_t kMaxTimers = 1024>
class TimerWheel {
 public:
  static constexpr std::size_t kCapacity = kMaxTimers;
  static constexpr std::size_t kSlots = 4096;
  static constexpr std::int64_t kSlotNs = 1'000'000;  // 1 ms
  static constexpr unsigned kIndexBits = std::bit_width(kMaxTimers - 1);
  static_assert(kIndexBits <= 24, "a TimerId keeps at least 8 bits of generation");

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

  // The id of the timer in pool slot `idx` now, and the slot of an id.
  [[nodiscard]] TimerId id_of(std::uint32_t idx) const noexcept {
    return TimerId{(gen_[idx] << kIndexBits) | idx};
  }
  [[nodiscard]] static constexpr std::uint32_t index_of(TimerId id) noexcept {
    return id.value & kIndexMask;
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
    return id_of(h.idx);
  }
  bool cancel(TimerId id) noexcept {
    if (!live(id)) return false;
    const Handle<Timer> h{index_of(id)};
    Timer& t = pool_.get(h);
    if (!t.active) return false;
    t.active = false;
    unlink(h.idx, t);
    if (firing_ != h.idx) release(h);  // the firing timer is freed after its callback
    return true;
  }
  [[nodiscard]] bool active(TimerId id) const noexcept {
    return live(id) && pool_.get(Handle<Timer>{index_of(id)}).active;
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
    if (now_ms + static_cast<std::int64_t>(kSlots) > overflow_min_ms_) promote_overflow();
    const std::int64_t steps = now_ms - cur_ms_;
    // Process slots cur_ms_ .. now_ms inclusive (the current slot may have new timers). The walk's
    // next timer is walk_next_, which unlink() moves on when a callback cancels that timer.
    for (std::int64_t s = 0; s <= steps; ++s) {
      const std::int64_t ms = cur_ms_ + s;
      const std::size_t slot = static_cast<std::size_t>(ms) & (kSlots - 1);
      for (std::uint32_t idx = slots_[slot]; idx != kNullHandle; idx = walk_next_) {
        Timer& t = pool_.get(Handle<Timer>{idx});
        walk_next_ = t.next;
        if (t.expiry <= now) fired += fire(idx, t, now, f);
      }
    }
    walk_next_ = kNullHandle;
    cur_ms_ = now_ms;
    return fired;
  }

  // A replay: the journal says timer `id` fired at `now`. It ends or repeats as poll() would have
  // made it, around the call of F(TimerId, user_data), so the timers the callback adds and cancels
  // take the same slots, and get the same ids, as in the session. F runs even when the wheel does
  // not hold the timer (a replay that started from a checkpoint). Returns whether it held it.
  template <class F>
  bool fire_recorded(TimerId id, std::uint64_t user_data, Timestamp now, F&& f) noexcept {
    if (!active(id)) {
      f(id, user_data);
      return false;
    }
    const std::uint32_t idx = index_of(id);
    fire(idx, pool_.get(Handle<Timer>{idx}), now, f);
    return true;
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
  // Earliest pending expiry before `limit`, or `limit` if there is none. Scans the wheel slots from
  // the last poll up to `limit` (one per ms), so it is cheap for a limit a few ms ahead: the live
  // engine bounds its idle wait with it. Overflow timers lie beyond the wheel and are not scanned.
  [[nodiscard]] Timestamp next_expiry_before(Timestamp limit) const noexcept {
    Timestamp best = limit;
    const std::int64_t span = limit.ns / kSlotNs - cur_ms_;
    const std::int64_t steps =
        span < static_cast<std::int64_t>(kSlots) ? span : static_cast<std::int64_t>(kSlots) - 1;
    for (std::int64_t s = 0; s <= steps; ++s) {
      const std::size_t slot = static_cast<std::size_t>(cur_ms_ + s) & (kSlots - 1);
      for (std::uint32_t idx = slots_[slot]; idx != kNullHandle;) {
        const Timer& t = pool_.get(Handle<Timer>{idx});
        if (t.active && t.expiry < best) best = t.expiry;
        idx = t.next;
      }
    }
    return best;
  }

 private:
  static constexpr std::uint32_t kOverflow = 0xFFFF'FFFEU;
  static constexpr std::uint32_t kUnlinked = 0xFFFF'FFFDU;

  static constexpr std::uint32_t kIndexMask = (1U << kIndexBits) - 1;
  // Generations wrap below the one whose id with the last slot would be TimerId's invalid value.
  static constexpr std::uint32_t kGenerations = (~0U >> kIndexBits);

  [[nodiscard]] bool live(TimerId id) const noexcept {
    const std::uint32_t idx = index_of(id);
    return id.valid() && idx < kMaxTimers && pool_.is_live(Handle<Timer>{idx}) &&
           (id.value >> kIndexBits) == gen_[idx];
  }
  void release(Handle<Timer> h) noexcept {
    const std::uint32_t g = gen_[h.idx] + 1;
    gen_[h.idx] = g == kGenerations ? 0 : g;
    pool_.free(h);
  }

  template <class F>
  std::size_t fire(std::uint32_t idx, Timer& t, Timestamp now, F& f) noexcept {
    const Handle<Timer> h{idx};
    unlink(idx, t);
    firing_ = idx;
    f(id_of(idx), t.user_data);
    firing_ = kNullHandle;
    Timer& tt = pool_.get(h);  // re-fetch: callback may have cancelled it
    if (!tt.active) {
      release(h);
      return 1;
    }
    if (tt.repeat) {
      // Keep the phase (no drift); catch up if we fell behind by several periods.
      tt.expiry += tt.period;
      if (tt.expiry <= now) tt.expiry = now + tt.period;
      link(idx, tt);
    } else {
      tt.active = false;
      release(h);
    }
    return 1;
  }

  void link(std::uint32_t idx, Timer& t) noexcept {
    const std::int64_t ms = t.expiry.ns / kSlotNs;
    std::uint32_t* head = nullptr;
    if (ms - cur_ms_ >= static_cast<std::int64_t>(kSlots)) {
      t.list = kOverflow;
      head = &overflow_;
      if (ms < overflow_min_ms_) overflow_min_ms_ = ms;
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
    if (idx == walk_next_) walk_next_ = t.next;
    std::uint32_t* head = t.list == kOverflow ? &overflow_ : &slots_[t.list];
    if (t.prev != kNullHandle) {
      pool_.get(Handle<Timer>{t.prev}).next = t.next;
    } else {
      *head = t.next;
    }
    if (t.next != kNullHandle) pool_.get(Handle<Timer>{t.next}).prev = t.prev;
    t.list = kUnlinked;
  }
  // Moves the overflow timers now within the horizon into the wheel; overflow_min_ms_ becomes the
  // earliest expiry of those left, so poll() walks the list only once one of them is due to move.
  void promote_overflow() noexcept {
    std::int64_t min_ms = kNever;
    std::uint32_t idx = overflow_;
    while (idx != kNullHandle) {
      Timer& t = pool_.get(Handle<Timer>{idx});
      const std::uint32_t next = t.next;
      const std::int64_t ms = t.expiry.ns / kSlotNs;
      if (ms - cur_ms_ < static_cast<std::int64_t>(kSlots)) {
        unlink(idx, t);
        link(idx, t);
      } else if (ms < min_ms) {
        min_ms = ms;
      }
      idx = next;
    }
    overflow_min_ms_ = min_ms;
  }

  static constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

  Pool<Timer, kMaxTimers> pool_;
  // Each pool slot's generation (the Timer itself shares its storage with the free list).
  std::unique_ptr<std::uint32_t[]> gen_ = std::make_unique<std::uint32_t[]>(kMaxTimers);
  std::uint32_t slots_[kSlots];
  std::uint32_t overflow_ = kNullHandle;
  // A lower bound of the overflow timers' expiries, in ms (kNever with none).
  std::int64_t overflow_min_ms_ = kNever;
  std::int64_t cur_ms_;
  std::uint32_t firing_ = kNullHandle;
  std::uint32_t walk_next_ = kNullHandle;  // poll()'s next timer in the slot it walks
};

}  // namespace fastmm
