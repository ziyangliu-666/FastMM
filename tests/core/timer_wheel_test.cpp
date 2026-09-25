#include "fastmm/core/timer_wheel.hpp"

#include "test_support.hpp"

#include <vector>

using namespace fastmm;

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
