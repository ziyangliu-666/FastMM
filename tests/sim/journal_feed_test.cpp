// JournalFeed and ReplayDriver clock source: a v2 journal replays at the recorded engine clock
// (including a backward step, start and finish); a journal without engine time falls back to the
// receive / fire times.
#include "fastmm/sim/journal_feed.hpp"

#include "test_support.hpp"

#include "fastmm/core/journal.hpp"
#include "fastmm/sim/sim_driver.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;
using fastmm::test::tmp_dir;

namespace {

const Timestamp kT0{1'789'000'000'000'000'000LL};

TradeMsg trade(Timestamp recv) {
  TradeMsg t{};
  init_header(t, EventType::Trade, InstrumentId{0}, VenueId{0});
  t.hdr.recv_ts = recv;
  return t;
}
TimerMsg timer(Timestamp fire) {
  TimerMsg t{};
  init_header(t, EventType::Timer);
  t.fire_ts = fire;
  t.hdr.flags = EventHeader::kSynthetic;
  return t;
}

// start, trade, out, timer, trade (clock stepped back), latency sample, finish
std::string write_journal(const char* name, bool engine_time) {
  const auto path = (tmp_dir() / name).string();
  MsgRing ring(1 << 16);
  JournalFileWriter fw(ring, path, JournalSessionInfo{});
  REQUIRE(fw.ok());
  JournalWriter w(&ring);
  const TradeMsg t1 = trade(kT0 + milliseconds(5));
  const TimerMsg tm = timer(kT0 + milliseconds(9));
  const TradeMsg t2 = trade(kT0 + milliseconds(3));
  OutCancelMsg out{};
  init_header(out, EventType::OutCancel);
  LatencySampleMsg lat{};
  init_header(lat, EventType::LatencySample);
  if (engine_time) {
    REQUIRE(w.record_clock(EngineTimeMsg::Kind::Start, kT0 + milliseconds(1)));
    REQUIRE(w.record_at(t1.hdr, kT0 + milliseconds(2)));
    REQUIRE(w.record_outbound(out.hdr));
    REQUIRE(w.record_at(tm.hdr, kT0 + milliseconds(25) / 10));
    REQUIRE(w.record_at(t2.hdr, kT0 + milliseconds(15) / 10));  // stepped back by 1 ms
    REQUIRE(w.record(lat.hdr));
    REQUIRE(w.record_clock(EngineTimeMsg::Kind::Finish, kT0 + milliseconds(4)));
  } else {
    REQUIRE(w.record(t1.hdr));
    REQUIRE(w.record_outbound(out.hdr));
    REQUIRE(w.record(tm.hdr));
    REQUIRE(w.record(t2.hdr));
    REQUIRE(w.record(lat.hdr));
  }
  fw.drain_once();
  fw.stop();
  return path;
}

// Fake engine: consumes one feed event per step and remembers the clock it saw.
struct Probe {
  SimClock* clock = nullptr;
  JournalFeed* feed = nullptr;
  std::vector<Timestamp> steps;
  Timestamp started{};
  Timestamp finished{};
};
EngineHooks probe_hooks(Probe& p) {
  EngineHooks h;
  h.ctx = &p;
  h.step = [](void* c) -> std::size_t {
    auto* self = static_cast<Probe*>(c);
    if (self->feed->next() == nullptr) return 0;
    self->steps.push_back(self->clock->now());
    self->feed->release();
    return 1;
  };
  h.next_timer = [](void*) { return Timestamp::max(); };
  h.warm_up = [](void*) {};
  h.start = [](void* c) { static_cast<Probe*>(c)->started = static_cast<Probe*>(c)->clock->now(); };
  h.finish = [](void* c) {
    static_cast<Probe*>(c)->finished = static_cast<Probe*>(c)->clock->now();
  };
  h.cancel_timers = [](void*) {};
  return h;
}

}  // namespace

TEST_CASE("sim.journal_feed: v2 journals give the recorded engine clock") {
  const std::string path = write_journal("feed_engine_time.fmj", true);
  JournalReader reader;
  REQUIRE(reader.open(path));
  JournalFeed feed(reader);
  CHECK(feed.has_start_ts());
  CHECK(feed.start_ts() == kT0 + milliseconds(1));
  CHECK_FALSE(feed.has_finish_ts());

  REQUIRE(feed.has_next());
  CHECK(feed.peek()->type == EventType::Trade);
  CHECK(feed.has_engine_ts());
  CHECK(feed.peek_ts() == kT0 + milliseconds(2));  // not the receive time
  feed.arm();
  REQUIRE(feed.next() != nullptr);
  feed.release();

  CHECK(feed.peek()->type == EventType::Timer);
  CHECK(feed.peek_ts() == kT0 + milliseconds(25) / 10);  // not fire_ts
  feed.arm();
  static_cast<void>(feed.next());
  feed.release();

  CHECK(feed.peek()->type == EventType::Trade);
  CHECK(feed.peek_ts() == kT0 + milliseconds(15) / 10);
  feed.arm();
  static_cast<void>(feed.next());
  feed.release();

  CHECK_FALSE(feed.has_next());
  CHECK(feed.has_finish_ts());
  CHECK(feed.finish_ts() == kT0 + milliseconds(4));
  CHECK(feed.delivered() == 3);
  CHECK(feed.skipped() == 4);  // start, out, latency sample, finish
}

TEST_CASE("sim.journal_feed: without engine time the receive and fire times are used") {
  const std::string path = write_journal("feed_legacy.fmj", false);
  JournalReader reader;
  REQUIRE(reader.open(path));
  JournalFeed feed(reader);
  CHECK_FALSE(feed.has_start_ts());
  REQUIRE(feed.has_next());
  CHECK_FALSE(feed.has_engine_ts());
  CHECK(feed.peek_ts() == kT0 + milliseconds(5));
  feed.arm();
  static_cast<void>(feed.next());
  feed.release();
  CHECK(feed.peek_ts() == kT0 + milliseconds(9));
  feed.arm();
  static_cast<void>(feed.next());
  feed.release();
  CHECK(feed.peek_ts() == kT0 + milliseconds(3));
}

TEST_CASE("sim.replay_driver: the clock follows the journal, backwards too") {
  {
    const std::string path = write_journal("driver_engine_time.fmj", true);
    JournalReader reader;
    REQUIRE(reader.open(path));
    JournalFeed feed(reader);
    SimClock clock(kT0);
    Probe probe;
    probe.clock = &clock;
    probe.feed = &feed;
    ReplayDriver driver(clock, feed, probe_hooks(probe));
    CHECK(driver.run_all() == 3);
    driver.finish();
    CHECK(probe.started == kT0 + milliseconds(1));
    REQUIRE(probe.steps.size() == 3);
    CHECK(probe.steps[0] == kT0 + milliseconds(2));
    CHECK(probe.steps[1] == kT0 + milliseconds(25) / 10);
    CHECK(probe.steps[2] == kT0 + milliseconds(15) / 10);
    CHECK(probe.finished == kT0 + milliseconds(4));
  }
  {
    const std::string path = write_journal("driver_legacy.fmj", false);
    JournalReader reader;
    REQUIRE(reader.open(path));
    JournalFeed feed(reader);
    SimClock clock(kT0);
    Probe probe;
    probe.clock = &clock;
    probe.feed = &feed;
    ReplayDriver driver(clock, feed, probe_hooks(probe));
    CHECK(driver.run_all() == 3);
    driver.finish();
    REQUIRE(probe.steps.size() == 3);
    CHECK(probe.steps[0] == kT0 + milliseconds(5));
    CHECK(probe.steps[1] == kT0 + milliseconds(9));
    CHECK(probe.steps[2] == kT0 + milliseconds(9));  // a v1 clock never moves backwards
    CHECK(probe.finished == kT0 + milliseconds(9));
  }
}
