// Live session journals replay exactly: fastmm-live's engine path (BinanceVenue on its reactor
// thread, Engine<BasicMM, TscClock, LiveTransport, RingFeed>, journal thread) against the
// in-process simulator records a session with fills; bt::replay_journal(path) replays it with
// nothing but the journal (embedded configuration, session epoch, cancel-replace, engine clock)
// and must reproduce every outbound message. The second case steps the engine's TSC mapping back
// and forth during the session, so the engine clock disagrees with the venue thread's receive
// times.
#include "integration_util.hpp"

#include "fastmm/backtest/replay.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace fastmm;
using namespace fastmm::integration;

namespace {

constexpr std::uint16_t kEpoch = 7;

std::string fresh_journal(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

bool fills_at_least(ServerFixture& fx, std::uint64_t fills, std::uint64_t orders, int timeout_ms) {
  return wait_until(
      [&] {
        const sim::server::SimServerStats s = fx.server.stats();
        return s.fills >= fills && s.orders_accepted >= orders;
      },
      timeout_ms);
}

void check_replays(const std::string& path, const LiveEngine& live) {
  const bt::JournalInfo info = bt::inspect_journal(path);
  CHECK(info.version == kJournalVersion);
  CHECK(info.strategy == "basic_mm");
  CHECK(info.has_session);
  CHECK(info.session_epoch == kEpoch);
  CHECK(info.quoting_enabled);
  CHECK(info.replace_venues == (live.supports_replace() ? 1U : 0U));
  CHECK(info.engine_time);
  CHECK_FALSE(info.config_toml.empty());
  CHECK(info.config_hash == live.config().effective_hash());
  CHECK(info.config_toml.find(kApiSecret) == std::string::npos);
  REQUIRE(info.outbound_messages > 0);

  const bt::ReplayResult r = bt::replay_journal(path);
  MESSAGE("replay: events=" << r.events << " recorded=" << r.recorded_messages << " replayed="
                            << r.outbound_messages << " first_mismatch=" << r.first_mismatch);
  INFO("expected: " << r.expected_message);
  INFO("actual:   " << r.actual_message);
  CHECK(r.session_restored);
  CHECK(r.first_mismatch == -1);
  CHECK(r.recorded_messages == info.outbound_messages - info.dropped_outbound);
  CHECK(r.outbound_messages == r.recorded_messages);
  CHECK(r.outbound_sha256 == r.recorded_sha256);
  CHECK(r.ok());
}

}  // namespace

TEST_CASE("sim_exchange replay: a live session journal replays to the identical outbound stream") {
  const std::string path = fresh_journal("live_session.fmj");
  ServerFixture fx;
  LiveEngineOptions opts;
  opts.journal_path = path;
  opts.session_epoch = kEpoch;
  LiveEngine live(sim_local_config(fx, false), opts);
  live.start();
  REQUIRE(fills_at_least(fx, 3, 6, 30000));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  live.stop();
  const EngineStats& es = live.engine().stats();
  CHECK(es.fills >= 3);
  CHECK(es.kills == 1);
  CHECK(es.journal_overflows == 0);
  CHECK(es.unknown_order_cancels == 0);
  MESSAGE("engine: events=" << es.events << " orders=" << es.orders_sent
                            << " cancels=" << es.cancels_sent << " replaces=" << es.replaces_sent
                            << " fills=" << es.fills << " risk_rejects=" << es.risk_rejects);
  check_replays(path, live);
}

TEST_CASE("sim_exchange replay: a live session with TSC recalibration steps replays exactly") {
  const std::string path = fresh_journal("live_session_tsc_steps.fmj");
  ServerFixture fx;
  LiveEngineOptions opts;
  opts.journal_path = path;
  opts.session_epoch = kEpoch;
  LiveEngine live(sim_local_config(fx, false), opts);
  live.start();
  REQUIRE(fills_at_least(fx, 1, 2, 30000));
  const std::uint64_t fills_before = fx.server.stats().fills;
  // Step the engine clock 400 ms back (requote interval, rate limiter and timers see time stand
  // still), then 700 ms forward of that.
  TscCalibration back = live.initial_tsc();
  back.ns0 -= milliseconds(400).ns;
  live.publish_tsc(back);
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  TscCalibration forward = live.initial_tsc();
  forward.ns0 += milliseconds(300).ns;
  live.publish_tsc(forward);
  CHECK(wait_until([&] { return fx.server.stats().fills >= fills_before + 2; }, 30000));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  live.stop();
  const EngineStats& es = live.engine().stats();
  if (live.initial_tsc().use_tsc) {
    CHECK(es.clock_steps == 2);
  } else {
    WARN_MESSAGE(false, "no invariant TSC: the clock reads clock_gettime and cannot be stepped");
  }
  CHECK(es.journal_overflows == 0);
  check_replays(path, live);
}
