// SlowChannel (ADR-0013, sections 1 and 4): fills ring, recent window, snapshot, watchdog and
// publish; ParamSchedule's inbox.
#include "fastmm/strategies/slow_channel.hpp"

#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/param_schedule.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace fastmm;

namespace {

SlowChannelConfig small(std::size_t fills, std::size_t rows) {
  SlowChannelConfig c;
  c.instruments = 2;
  c.fills_capacity = fills;
  c.recent_rows = rows;
  return c;
}

SlowRecentRow row(std::int64_t ts) {
  SlowRecentRow r{};
  r.ts_ns = ts;
  return r;
}

InstrumentId instrument(std::uint32_t k) {
  InstrumentId id{};
  id.value = k;
  return id;
}

}  // namespace

TEST_CASE("slow.fills: numbered in order, and a full ring records FillsOverflow") {
  SlowChannel ch(small(4, 8));
  for (int i = 0; i < 4; ++i) {
    SlowFill f{};
    f.qty_raw = i;
    CHECK(ch.push_fill(f));
  }
  CHECK(ch.failure() == SlowFailure::None);
  CHECK_FALSE(ch.push_fill(SlowFill{}));
  CHECK(ch.failure() == SlowFailure::FillsOverflow);
  std::array<SlowFill, 8> out{};
  REQUIRE(ch.drain_fills(out) == 4);
  for (std::uint64_t i = 0; i < 4; ++i) CHECK(out[i].seq == i + 1);
  CHECK(ch.push_fill(SlowFill{}));
  REQUIRE(ch.drain_fills(out) == 1);
  CHECK(out[0].seq == 6);  // the lost fill's number stays skipped
  CHECK(ch.fills_pending() == 0);
}

TEST_CASE("slow.recent: the newest rows, oldest first, with the rows that left the window") {
  SlowChannel ch(small(4, 8));
  const InstrumentId inst = instrument(1);
  std::vector<SlowRecentRow> out(8);
  std::uint64_t cursor = 0;

  for (int i = 0; i < 20; ++i) ch.record(inst, row(i));
  SlowChannel::RecentRead r = ch.recent(inst, out, cursor);
  REQUIRE(r.rows == 8);
  CHECK(out[0].ts_ns == 12);
  CHECK(out[7].ts_ns == 19);
  CHECK(r.dropped == 12);

  for (int i = 20; i < 23; ++i) ch.record(inst, row(i));
  r = ch.recent(inst, out, cursor);
  REQUIRE(r.rows == 8);
  CHECK(out[0].ts_ns == 15);
  CHECK(r.dropped == 0);  // rows 15..19 were returned before

  for (int i = 23; i < 33; ++i) ch.record(inst, row(i));
  r = ch.recent(inst, out, cursor);
  CHECK(out[0].ts_ns == 25);
  CHECK(r.dropped == 2);  // rows 23 and 24

  std::uint64_t other = 0;
  CHECK(ch.recent(instrument(0), out, other).rows == 0);
  CHECK(ch.recent_written(inst) == 33);
}

TEST_CASE("slow.snapshot: readers see the last published header and instruments") {
  SlowChannel ch(small(4, 8));
  SlowSnapshotHeader h{};
  std::vector<SlowInstrumentState> states(2);
  REQUIRE(ch.try_snapshot(h, states));
  CHECK(h.version == 0);
  CHECK(h.instruments == 2);
  ch.write_snapshot(Timestamp{123}, true, false, [](SlowInstrumentState* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) s[i].mid_raw = static_cast<std::int64_t>(100 + i);
  });
  ch.snapshot(h, states);
  CHECK(h.ts_ns == 123);
  CHECK(h.version == 1);
  CHECK(h.quoting_enabled == 1);
  CHECK(h.killed == 0);
  CHECK(states[1].mid_raw == 101);
}

TEST_CASE("slow.watchdog: a call past its timeout, and the first failure is kept") {
  SlowChannel ch(small(4, 8));
  ch.begin_call(1'000, Duration{500});
  CHECK(ch.call_deadline() == 1'500);
  CHECK(ch.check(1'400) == SlowFailure::None);
  ch.end_call(1'450);
  CHECK(ch.check(2'000) == SlowFailure::None);
  CHECK(ch.last_heartbeat() == 1'450);
  ch.begin_call(3'000, Duration{500});
  CHECK(ch.check(3'501) == SlowFailure::Timeout);
  CHECK_FALSE(ch.fail(SlowFailure::Exception));
  CHECK(ch.failure() == SlowFailure::Timeout);
}

TEST_CASE("slow.publish: updates reach the ring numbered, and none after close") {
  SlowChannel ch(small(4, 8));
  const std::array<std::uint16_t, 2> fields{1, 0};
  const std::array<std::int64_t, 2> values{7, -3};
  const InstrumentId inst = instrument(1);
  CHECK(ch.publish(inst, fields, values));
  CHECK(ch.publish(ParamUpdateMsg::kAllInstruments, {}, {}));
  CHECK(ch.published() == 2);

  MsgRing& ring = ch.param_ring();
  const auto* h = reinterpret_cast<const EventHeader*>(ring.try_peek());
  REQUIRE(h != nullptr);
  REQUIRE(h->type == EventType::ParamUpdate);
  const auto& m = msg_cast<ParamUpdateMsg>(h);
  CHECK(m.publish_seq == 1);
  CHECK(m.count == 2);
  CHECK(m.field[0] == 1);
  CHECK(m.value[1] == -3);
  CHECK(m.hdr.instrument.value == 1);
  ring.release();

  const std::vector<std::uint16_t> too_many(ParamUpdateMsg::kMaxFields + 1, 0);
  const std::vector<std::int64_t> too_many_values(ParamUpdateMsg::kMaxFields + 1, 0);
  CHECK_THROWS_AS(ch.publish(inst, too_many, too_many_values), std::invalid_argument);
  ch.close();
  CHECK(ch.closed());
  CHECK_FALSE(ch.publish(inst, fields, values));
}

TEST_CASE("slow.fills_capacity: from the order rate limit and the longest undrained time") {
  CHECK(slow_fills_capacity(0, seconds(10)) == 65'536);  // 4 * 1000 * 11
  CHECK(slow_fills_capacity(5, seconds(1)) == 4'096);
  CHECK(slow_fills_capacity(1'000'000, seconds(3'600)) == (1U << 24));
}

TEST_CASE("slow.param_schedule: updates from another thread wait in the inbox") {
  SimClock clock(Timestamp{1'000});
  sim::ParamSchedule schedule;
  schedule.set_delay(Duration{5});
  const ParamSink sink = schedule.threaded_sink(clock);
  ParamUpdateMsg m{};
  init_header(m, EventType::ParamUpdate);
  CHECK(sink.push(sink.ctx, m));
  CHECK(schedule.next_ts() == Timestamp{1'005});
  std::thread other([&] { CHECK(sink.push(sink.ctx, m)); });
  other.join();
  CHECK(schedule.size() == 1);
  clock.set(Timestamp{2'000});
  schedule.collect();
  CHECK(schedule.size() == 2);
  static_cast<void>(schedule.pop());
  CHECK(schedule.next_ts() == Timestamp{2'005});
}
