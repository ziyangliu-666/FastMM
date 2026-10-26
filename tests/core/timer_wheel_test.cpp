#include "fastmm/core/timer_wheel.hpp"

#include "test_support.hpp"

#include "fastmm/core/rng.hpp"

#include <vector>

using namespace fastmm;

TEST_CASE("core.timer_wheel: next_expiry follows adds, cancels and fires") {
  // Every timer lies within the wheel's horizon, so next_expiry_before() over the whole wheel is
  // the earliest one by a different route. Callbacks add and cancel timers and ask for
  // next_expiry() in the middle of a poll.
  Xoshiro256ss rng(7);
  TimerWheel<256> w(Timestamp{0});
  std::vector<TimerId> ids;
  Timestamp now{0};
  const Duration horizon = milliseconds(4000);
  const auto oracle = [&] {
    const Timestamp t = w.next_expiry_before(now + horizon);
    return t == now + horizon ? Timestamp::max() : t;
  };
  for (int step = 0; step < 20'000; ++step) {
    const std::uint64_t r = rng.next() % 10;
    if (r < 4 && w.size() < 200) {
      const Duration d{static_cast<std::int64_t>(1 + rng.next() % 3'000'000'000ULL)};
      ids.push_back(w.add(now, d, rng.next() % 3 == 0, 0));
    } else if (r < 6 && !ids.empty()) {
      static_cast<void>(w.cancel(ids[rng.next() % ids.size()]));
    } else {
      now = now + Duration{static_cast<std::int64_t>(rng.next() % 30'000'000)};
      static_cast<void>(w.poll(now, [&](TimerId fired, std::uint64_t) {
        static_cast<void>(w.next_expiry());
        if (rng.next() % 4 == 0) static_cast<void>(w.cancel(fired));
        if (rng.next() % 4 == 0 && w.size() < 200)
          ids.push_back(w.add(now, milliseconds(3), false, 0));
      }));
    }
    REQUIRE(w.next_expiry() == oracle());
  }
}

TEST_CASE("core.timer_wheel: next_expiry_before scans only the slots up to the limit") {
  const Timestamp now{seconds(1).ns};
  TimerWheel<64> w(now);
  CHECK(w.next_expiry_before(now + milliseconds(1)) == now + milliseconds(1));
  const TimerId a = w.add(now, microseconds(300), false);
  static_cast<void>(w.add(now, milliseconds(3), true));
  static_cast<void>(w.add(now, seconds(10), false));  // beyond the wheel: never reported
  CHECK(w.next_expiry_before(now + milliseconds(1)) == now + microseconds(300));
  CHECK(w.next_expiry_before(now + microseconds(200)) == now + microseconds(200));
  REQUIRE(w.cancel(a));
  CHECK(w.next_expiry_before(now + milliseconds(1)) == now + milliseconds(1));
  CHECK(w.next_expiry_before(now + milliseconds(5)) == now + milliseconds(3));
  CHECK(w.next_expiry_before(now + seconds(20)) == now + milliseconds(3));
}

TEST_CASE("core.timer_wheel: one-shot, repeat, cancel, overflow, order") {
  Timestamp now{seconds(1).ns};
  TimerWheel<64> w(now);
  std::vector<std::pair<std::uint32_t, std::uint64_t>> fired;
  auto cb = [&](TimerId id, std::uint64_t ud) { fired.emplace_back(id.value, ud); };

  const TimerId a = w.add(now, milliseconds(10), false, 1);
  const TimerId b = w.add(now, milliseconds(5), true, 2);
  const TimerId c = w.add(now, seconds(10), false, 3);  // beyond the 4.096 s horizon: overflow
  REQUIRE(a.valid());
  REQUIRE(b.valid());
  REQUIRE(c.valid());
  CHECK(w.size() == 3);
  CHECK(w.next_expiry() == now + milliseconds(5));

  CHECK(w.poll(now + milliseconds(4), cb) == 0);
  CHECK(w.poll(now + milliseconds(5), cb) == 1);
  CHECK(fired.back().second == 2);
  CHECK(w.poll(now + milliseconds(10), cb) == 2);  // b again (10) and a (10)
  CHECK(w.size() == 2);                            // a freed
  CHECK_FALSE(w.active(a));
  CHECK(w.active(b));
  // cancel repeating
  CHECK(w.cancel(b));
  CHECK_FALSE(w.cancel(b));
  CHECK(w.poll(now + milliseconds(100), cb) == 0);
  // large jump: overflow timer promoted and fired
  CHECK(w.poll(now + seconds(11), cb) == 1);
  CHECK(fired.back().second == 3);
  CHECK(w.size() == 0);
  CHECK(w.next_expiry() == Timestamp::max());
}

TEST_CASE("core.timer_wheel: re-entrant add/cancel inside callback and catch-up") {
  Timestamp now{0};
  TimerWheel<8> w(now);
  int hits = 0;
  TimerId self{};
  TimerId other = w.add(now, milliseconds(3), false, 9);
  self = w.add(now, milliseconds(1), true, 1);
  auto cb = [&](TimerId id, std::uint64_t) {
    ++hits;
    if (id == self && hits == 2) {
      w.cancel(self);   // cancel self while firing
      w.cancel(other);  // cancel a different pending timer
      REQUIRE(w.add(now + milliseconds(2), milliseconds(1), false, 5).valid());  // add another
    }
  };
  CHECK(w.poll(now + milliseconds(1), cb) == 1);
  CHECK(w.poll(now + milliseconds(2), cb) == 1);
  CHECK(w.size() == 1);  // only the newly added one
  CHECK(w.poll(now + milliseconds(3), cb) == 1);
  CHECK(w.size() == 0);
  // catching up after a long stall: a repeating 1 ms timer fires once per poll (no burst)
  TimerId r = w.add(now + milliseconds(3), milliseconds(1), true, 7);
  CHECK(w.poll(now + seconds(2), cb) == 1);
  CHECK(w.poll(now + seconds(2) + milliseconds(1), cb) == 1);
  CHECK(w.cancel(r));
  // pool exhaustion returns an invalid id
  for (int i = 0; i < 8; ++i) CHECK(w.add(now, seconds(1), false).valid());
  CHECK_FALSE(w.add(now, seconds(1), false).valid());
}

TEST_CASE("core.timer_wheel: the id of a timer that ended does not reach the timer in its slot") {
  const Timestamp now{seconds(1).ns};
  TimerWheel<8> w(now);
  int fired = 0;
  auto cb = [&](TimerId, std::uint64_t) { ++fired; };
  const TimerId once = w.add(now, milliseconds(1), false);
  CHECK(w.poll(now + milliseconds(1), cb) == 1);
  CHECK_FALSE(w.active(once));
  // The slot is free again and the next timer takes it: the old id is not the new timer's.
  const TimerId next = w.add(now + milliseconds(1), milliseconds(5), false);
  REQUIRE(next.valid());
  CHECK(TimerWheel<8>::index_of(next) == TimerWheel<8>::index_of(once));
  CHECK(next != once);
  CHECK_FALSE(w.cancel(once));
  CHECK(w.active(next));
  // Cancelled, the same.
  REQUIRE(w.cancel(next));
  const TimerId third = w.add(now + milliseconds(1), milliseconds(5), false);
  CHECK_FALSE(w.cancel(next));
  CHECK(w.active(third));
  CHECK(w.poll(now + milliseconds(6), cb) == 1);
  CHECK(fired == 2);
  CHECK_FALSE(w.active(TimerId{}));
  CHECK_FALSE(w.cancel(TimerId{}));
}

TEST_CASE("core.timer_wheel: a callback that cancels the next timer of its slot") {
  const Timestamp now{seconds(1).ns};
  TimerWheel<8> w(now);
  // Same expiry, so the same slot: a is walked first (the newest is the slot's head), b after it.
  const TimerId b = w.add(now, milliseconds(2), false, 2);
  const TimerId a = w.add(now, milliseconds(2), false, 1);
  const TimerId c = w.add(now, milliseconds(2), false, 3);
  std::vector<std::uint64_t> fired;
  auto cb = [&](TimerId, std::uint64_t ud) {
    fired.push_back(ud);
    if (ud == 1) CHECK(w.cancel(b));
  };
  CHECK(w.poll(now + milliseconds(2), cb) == 2);
  CHECK(fired == std::vector<std::uint64_t>{3, 1});
  CHECK(w.size() == 0);
  static_cast<void>(a);
  static_cast<void>(c);
  // Every slot is free once, not twice: eight timers fit and the ninth does not.
  for (int i = 0; i < 8; ++i) CHECK(w.add(now, seconds(1), false).valid());
  CHECK_FALSE(w.add(now, seconds(1), false).valid());
}

TEST_CASE("core.timer_wheel: a recorded fire takes the slots the session's did") {
  const Timestamp now{seconds(1).ns};
  // The session: a repeating timer and a one-shot; the one-shot's callback adds a timer while it
  // is still firing.
  TimerWheel<8> live(now);
  TimerWheel<8> replay(now);
  const TimerId rep = live.add(now, milliseconds(10), true, 1);
  const TimerId one = live.add(now, milliseconds(3), false, 2);
  REQUIRE(replay.add(now, milliseconds(10), true, 1) == rep);
  REQUIRE(replay.add(now, milliseconds(3), false, 2) == one);
  std::vector<std::pair<TimerId, std::uint64_t>> recorded;
  std::vector<TimerId> live_added;
  auto live_cb = [&](TimerId id, std::uint64_t ud) {
    recorded.emplace_back(id, ud);
    live_added.push_back(live.add(now, milliseconds(4) * (ud == 2 ? 1 : 100), false, 10 + ud));
  };
  CHECK(live.poll(now + milliseconds(3), live_cb) == 1);
  CHECK(live.poll(now + milliseconds(10), live_cb) == 2);  // the one added at 3 ms, and rep
  REQUIRE(recorded.size() == 3);
  // The replay never polls: it fires what was recorded, and its callbacks add the same timers.
  std::vector<TimerId> replay_added;
  for (const auto& [id, ud] : recorded) {
    CHECK(replay.fire_recorded(id, ud, now + milliseconds(10), [&](TimerId, std::uint64_t u) {
      replay_added.push_back(replay.add(now, milliseconds(4) * (u == 2 ? 1 : 100), false, 10 + u));
    }));
  }
  CHECK(replay_added == live_added);
  CHECK(replay.size() == live.size());
  CHECK(replay.active(rep));
  CHECK_FALSE(replay.active(one));
  // A timer the wheel does not hold still runs its callback.
  int ran = 0;
  CHECK_FALSE(replay.fire_recorded(one, 0, now, [&](TimerId, std::uint64_t) { ++ran; }));
  CHECK(ran == 1);
}
